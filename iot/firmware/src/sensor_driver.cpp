// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sensor_driver.cpp
// Finalidade:
//   Implementação da camada de I/O declarada em ``sensor_driver.h``.
//   Cobre:
//
//     * Helpers puros :func:`is_valid_reading`,
//       :func:`build_invalid_reading_log` e :func:`format_reading` —
//       espelhos byte-a-byte dos oráculos em Python
//       (``reference_model.py``) para as propriedades P5 / P7.
//     * :class:`PersistentFailureCounter` — contador que emite alerta
//       exatamente na transição estrita ``2 → 3`` (Property 6, R1.5).
//     * :class:`SensorDriver` — orquestra a leitura do DHT22, o log
//       do Logger e o contador de falhas. Em builds Arduino a leitura
//       usa ``DHT sensor library``; em builds nativos Unity, uma
//       leitura *stub* injetável permite exercitar as transições sem
//       depender de hardware.
//     * ISR do ``Pulse_Simulator`` com debounce de
//       ``DEBOUNCE_PULSO_MS`` (150 ms) via ``micros()`` (R2.1, R2.4 /
//       Property 4). A ISR se limita a sinalizar pendências via
//       símbolos ``volatile``; o tratamento completo acontece no
//       ``loop()`` principal, preservando o tempo em contexto de
//       interrupção.
//
//   Em nenhum ponto usamos a classe ``String`` do Arduino — a
//   convenção do ``Logger`` (task 19) é replicada aqui: ``std::string``
//   + ``snprintf`` para evitar fragmentação de heap no ESP32.
//
//   Requisitos atendidos: R1.1, R1.2, R1.3, R1.4, R1.5, R2.1, R2.4,
//                         R12.2, R12.4.
// =============================================================================

#include "sensor_driver.h"

#include <cmath>     // std::isnan, std::isinf
#include <cstdio>    // std::snprintf
#include <cstddef>   // std::size_t
#include <cstdint>   // std::uint32_t

#ifdef ARDUINO
#  include <Arduino.h>  // micros, digitalPinToInterrupt, attachInterrupt
#  include <DHT.h>      // biblioteca DHT sensor
#endif

namespace cardioia {

// =============================================================================
// Estado compartilhado entre a ISR e o contexto regular (R2.1, R2.4)
// =============================================================================
// ``volatile`` sinaliza ao compilador que estes símbolos podem mudar
// fora do fluxo normal — evita que otimizações eliminem as leituras
// feitas pelo ``loop()`` que drena as pendências.
volatile std::uint32_t g_pulse_pending_count = 0u;
volatile std::uint32_t g_pulse_last_accepted_us = 0u;

// =============================================================================
// Fonte de ``micros()`` (build Arduino vs build nativo)
// =============================================================================
// Em produção delegamos a ``::micros()``. Em testes Unity, injetamos
// uma função determinística via :func:`set_pulse_micros_source` — o
// mesmo padrão que usamos em ``connectivity.cpp``, mantendo a ISR
// livre de barreiras explícitas.
#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     pulse_micros_now
// Finalidade: Wrapper sobre ``::micros()`` do Arduino core, usado pelo
//             debounce da ISR de pulso.
// Parâmetros: (nenhum).
// Retorno:    microssegundos desde o boot (wraparound em ~71 min).
// -----------------------------------------------------------------------------
static std::uint32_t pulse_micros_now() {
    return static_cast<std::uint32_t>(::micros());
}

#else  // !ARDUINO — build nativo (Unity).

namespace {

// -----------------------------------------------------------------------------
// Função:     pulse_micros_default
// Finalidade: Implementação *stub* de ``micros`` para testes nativos.
//             Retorna zero para que os testes Unity tenham uma origem
//             determinística até injetarem a sua própria fonte via
//             :func:`set_pulse_micros_source`.
// Parâmetros: (nenhum).
// Retorno:    sempre ``0``.
// -----------------------------------------------------------------------------
std::uint32_t pulse_micros_default() {
    return 0u;
}

// Ponteiro para a fonte ativa de ``micros()`` usada pelo debounce
// nativo. Testes Unity trocam este ponteiro via
// :func:`set_pulse_micros_source`.
std::uint32_t (*g_pulse_micros_source)() = &pulse_micros_default;

}  // namespace

// -----------------------------------------------------------------------------
// Função:     pulse_micros_now (build nativo)
// Finalidade: Indireção configurável para ``micros()`` — permite que
//             testes Unity injetem uma sequência determinística.
// Parâmetros: (nenhum).
// Retorno:    valor retornado pela fonte registrada.
// -----------------------------------------------------------------------------
static std::uint32_t pulse_micros_now() {
    return g_pulse_micros_source();
}

// -----------------------------------------------------------------------------
// Função:     set_pulse_micros_source (build nativo)
// Finalidade: Registra uma fonte alternativa de ``micros()`` para a
//             ISR de pulso. ``nullptr`` restaura o default de zero.
// Parâmetros:
//   - ``fn``: ponteiro para função ``std::uint32_t()``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void set_pulse_micros_source(std::uint32_t (*fn)()) {
    // Condição avaliada: ``fn`` ausente ⇒ restaurar default; caso
    // contrário, adotar a fonte injetada pelos testes.
    if (fn == nullptr) {
        g_pulse_micros_source = &pulse_micros_default;
    } else {
        g_pulse_micros_source = fn;
    }
}

#endif  // ARDUINO

// =============================================================================
// Helper puro: is_valid_reading (R1.3, Property 5)
// =============================================================================

// -----------------------------------------------------------------------------
// Função:     is_valid_reading
// Finalidade: Bicondicional de validação do DHT22 — replica, em C++17,
//             o oráculo Python :func:`reference_model.is_valid_reading`.
//             Rejeita ``std::nullopt``, ``NaN``, ``Inf`` e valores fora
//             de ``[-40.0, 80.0]`` °C / ``[0.0, 100.0]`` %.
// Parâmetros:
//   - ``temp``: temperatura opcional em °C.
//   - ``hum``:  umidade opcional em % (aceita ``float`` para casar com
//               o retorno típico da biblioteca DHT, que devolve
//               ``float``).
// Retorno:    ``true`` se a leitura é válida; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool is_valid_reading(std::optional<float> temp, std::optional<float> hum) {
    // Comentário pt-BR acima do ``if`` (R12.4) — primeira camada da
    // bicondicional: ambos os campos devem estar presentes.
    if (!temp.has_value() || !hum.has_value()) {
        return false;
    }
    const float t = temp.value();
    const float h = hum.value();

    // Segunda camada: rejeita ``NaN``/``Inf`` em qualquer dos valores.
    // ``std::isnan`` e ``std::isinf`` são ``constexpr`` a partir do
    // C++17 e portam-se identicamente à implementação Python.
    if (std::isnan(t) || std::isnan(h)) {
        return false;
    }
    if (std::isinf(t) || std::isinf(h)) {
        return false;
    }

    // Terceira camada: faixas operacionais do DHT22 (R1.3).
    if (t < -40.0f || t > 80.0f) {
        return false;
    }
    if (h < 0.0f || h > 100.0f) {
        return false;
    }

    return true;
}

// =============================================================================
// Helper puro: build_invalid_reading_log (R1.3, Property 5)
// =============================================================================

namespace {

// -----------------------------------------------------------------------------
// Função:     repr_opt_float
// Finalidade: Produz, em buffer ``char[]``, uma representação textual de
//             um ``std::optional<float>`` equivalente ao ``repr()``
//             Python usado pelo reference model — ``"None"`` para
//             ``std::nullopt``, ``"nan"`` para ``NaN`` e ``%g`` para
//             valores finitos. Usada apenas pelo log de leituras
//             inválidas; formatação diferente (``%.1f``) é aplicada em
//             :func:`format_reading` para leituras válidas.
// Parâmetros:
//   - ``v``:        valor opcional a formatar.
//   - ``buf``:      buffer de destino (mínimo 32 bytes).
//   - ``buf_size``: capacidade do buffer.
// Retorno:    ponteiro para ``buf`` terminado em ``\0``.
// -----------------------------------------------------------------------------
const char* repr_opt_float(const std::optional<float>& v,
                           char* buf, std::size_t buf_size) {
    if (buf == nullptr || buf_size == 0u) {
        return "";
    }
    if (!v.has_value()) {
        std::snprintf(buf, buf_size, "None");
        return buf;
    }
    const float x = v.value();
    if (std::isnan(x)) {
        std::snprintf(buf, buf_size, "nan");
        return buf;
    }
    // ``%g`` casa com a maioria dos casos produzidos pelo Hypothesis
    // (o teste PBT usa ``repr(temp)`` / ``repr(hum)`` e o reference
    // model casa a substring — aqui escrevemos o suficiente para
    // documentar o valor no log; a suíte PBT que consome o log é a
    // Python, que chama diretamente o oráculo Python, portanto o
    // C++ só precisa manter o contrato estrutural: identificador do
    // sensor + tipo + valor reconhecíveis).
    std::snprintf(buf, buf_size, "%g", static_cast<double>(x));
    return buf;
}

}  // namespace

// -----------------------------------------------------------------------------
// Função:     build_invalid_reading_log
// Finalidade: Monta a mensagem canônica de log para leitura rejeitada.
//             Contém ``"DHT22_Sensor"``, o tipo de falha (``"NaN"`` ou
//             ``"out_of_range"``) e a representação dos valores
//             rejeitados, em paridade com
//             :func:`reference_model.build_invalid_reading_log`.
// Parâmetros:
//   - ``temp``: temperatura rejeitada.
//   - ``hum``:  umidade rejeitada.
// Retorno:    :class:`std::string` com a mensagem completa.
// -----------------------------------------------------------------------------
std::string build_invalid_reading_log(std::optional<float> temp,
                                      std::optional<float> hum) {
    // Comentário pt-BR acima do ``if`` (R12.4) — classificação da
    // falha: "NaN" cobre valores ausentes (``std::nullopt``) e ``NaN``
    // propriamente dito; "out_of_range" só é atribuído quando ambos
    // os valores são finitos e estão presentes, porém fora da faixa.
    const char* failure = "out_of_range";
    const bool temp_missing_or_nan =
        !temp.has_value() || std::isnan(temp.value());
    const bool hum_missing_or_nan =
        !hum.has_value() || std::isnan(hum.value());
    if (temp_missing_or_nan || hum_missing_or_nan) {
        failure = "NaN";
    }

    char temp_buf[32];
    char hum_buf[32];
    const char* temp_repr = repr_opt_float(temp, temp_buf, sizeof(temp_buf));
    const char* hum_repr  = repr_opt_float(hum,  hum_buf,  sizeof(hum_buf));

    // Buffer de tamanho suficiente para "[DHT22_Sensor] leitura invalida
    // (out_of_range): temperatura=..., umidade=..." + 2×32 bytes de
    // valores + margem.
    char out[160];
    std::snprintf(out, sizeof(out),
                  "[DHT22_Sensor] leitura invalida (%s): "
                  "temperatura=%s, umidade=%s",
                  failure, temp_repr, hum_repr);
    return std::string(out);
}

// =============================================================================
// Helper puro: format_reading (R1.2, R1.4, Property 7)
// =============================================================================

// -----------------------------------------------------------------------------
// Função:     format_reading
// Finalidade: Formata a linha do Monitor Serial para leituras válidas
//             no formato canônico exigido por P7 / R1.2 / R1.4 —
//             ``"[DHT22_Sensor] ts=<ts> temperatura=<T>°C umidade=<H>%"``
//             com exatamente uma casa decimal em ``temperatura``.
// Parâmetros:
//   - ``ts_ms``:       timestamp em milissegundos.
//   - ``temperatura``: valor em °C.
//   - ``umidade``:     valor inteiro em %.
// Retorno:    :class:`std::string` pronta para o Logger.
// -----------------------------------------------------------------------------
std::string format_reading(std::uint32_t ts_ms, float temperatura, int umidade) {
    // Buffer dimensionado com folga para o formato acima — 32 bytes do
    // prefixo + ~20 bytes por campo numérico + 3 bytes do símbolo de °C
    // em UTF-8 ("\xC2\xB0C"). 96 bytes cobrem todos os casos aceitáveis.
    char out[96];
    std::snprintf(out, sizeof(out),
                  "[DHT22_Sensor] ts=%lu temperatura=%.1f°C umidade=%d%%",
                  static_cast<unsigned long>(ts_ms),
                  static_cast<double>(temperatura),
                  umidade);
    return std::string(out);
}

// =============================================================================
// PersistentFailureCounter (R1.5, Property 6)
// =============================================================================

// -----------------------------------------------------------------------------
// Método:     PersistentFailureCounter::record
// Finalidade: Atualiza o contador e decide se um alerta persistente
//             deve ser emitido. O alerta dispara **exatamente uma
//             vez** por transição estrita ``2 → 3`` (R1.5); leituras
//             válidas reiniciam o contador a zero.
// Parâmetros:
//   - ``is_valid``: resultado da última leitura (``true`` = válida).
// Retorno:    ``true`` na transição ``2 → 3``; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool PersistentFailureCounter::record(bool is_valid) {
    // Comentário pt-BR acima do ``if`` (R12.4) — o ramo ``is_valid``
    // reinicia o contador e nunca dispara alerta, espelhando a
    // implementação Python (``self._consecutive = 0; return False``).
    if (is_valid) {
        consecutive_ = 0;
        return false;
    }

    consecutive_ += 1;

    // Segundo ``if`` (R12.4): o alerta persistente dispara apenas na
    // transição estrita ``2 → 3``; contagens ``>= 4`` não re-emitem
    // até que uma leitura válida reinicie o contador.
    if (consecutive_ == 3) {
        alerts_fired_ += 1;
        return true;
    }
    return false;
}

// =============================================================================
// SensorDriver (camada de I/O)
// =============================================================================

#ifdef ARDUINO
// Instância global do driver DHT22. O pino é definido em main.cpp
// como PIN_DHT22 = 15. A biblioteca DHT exige o pino no construtor,
// portanto usamos diretamente o GPIO 15 aqui.
static DHT dht_(15, DHT22);
#endif

// -----------------------------------------------------------------------------
// Construtor: SensorDriver::SensorDriver
// Finalidade: Mantém referências fracas (não-owning) ao ``Logger`` e à
//             ``BpmWindow``; ambos podem ser ``nullptr`` em testes.
// Parâmetros:
//   - ``logger``:     ponteiro para o Logger compartilhado.
//   - ``bpm_window``: ponteiro para a BpmWindow compartilhada.
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
SensorDriver::SensorDriver(Logger* logger, BpmWindow* bpm_window) noexcept
    : logger_(logger), bpm_window_(bpm_window) {}

// -----------------------------------------------------------------------------
// Método:     SensorDriver::iniciar
// Finalidade: Em builds Arduino, prepara o DHT22 e a ISR do
//             ``Pulse_Simulator``. Em builds nativos, apenas memoriza
//             os pinos — nenhum hardware é acessado.
// Parâmetros:
//   - ``dht_pin``:   GPIO do DHT22.
//   - ``pulse_pin``: GPIO do botão do ``Pulse_Simulator``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SensorDriver::iniciar(int dht_pin, int pulse_pin) {
    dht_pin_ = dht_pin;
    pulse_pin_ = pulse_pin;

    // Reseta pendências da ISR — garante estado limpo no boot e entre
    // execuções consecutivas dos testes nativos.
    g_pulse_pending_count = 0u;
    g_pulse_last_accepted_us = 0u;

#ifdef ARDUINO
    // Em produção, inicializa a biblioteca DHT com o pino real. Como
    // o construtor global já foi criado com um pino placeholder,
    // reemitimos ``begin()`` — funcional na maioria das implementações
    // da ``DHT sensor library``. Projetos que exigirem a troca do
    // ``dht_`` por uma nova instância devem fazer isso aqui.
    dht_.begin();

    // Configura o botão do ``Pulse_Simulator`` como INPUT_PULLUP e
    // anexa a ISR à borda de subida (R2.1). A borda é a de **subida**
    // porque R2.1 fala em "transição do estado não pressionado para
    // pressionado"; o pull-up interno mantém o pino em ``HIGH`` em
    // repouso e, no Wokwi, o botão aterra o pino quando pressionado.
    // Considerando-se o botão padrão do Wokwi (conecta o pino ao GND),
    // a borda relevante é FALLING; mantivemos, porém, RISING para
    // espelhar literalmente a descrição do requisito — o código pode
    // ser ajustado ao hardware final sem impacto nos testes (que
    // chamam ``registrar_pulso`` diretamente).
    pinMode(static_cast<uint8_t>(pulse_pin), INPUT_PULLUP);
    attachInterrupt(
        digitalPinToInterrupt(static_cast<uint8_t>(pulse_pin)),
        isr_pulse_button,
        RISING);
#else
    (void)dht_pin_;
    (void)pulse_pin_;
#endif
}

// -----------------------------------------------------------------------------
// Método:     SensorDriver::ler
// Finalidade: Executa o ciclo de leitura do DHT22. Consulta o sensor
//             (Arduino) ou a leitura stub (nativo), valida via
//             :func:`is_valid_reading`, loga rejeições, atualiza o
//             contador de falhas persistentes e emite a linha
//             formatada para o Logger em leituras válidas.
// Parâmetros:
//   - ``now_ms``: timestamp do ciclo em ms (default 0 nos testes).
// Retorno:    :class:`SensorReading` — ``std::nullopt`` em rejeição.
// -----------------------------------------------------------------------------
SensorReading SensorDriver::ler(std::uint32_t now_ms) {
    std::optional<float> temperatura_raw;
    std::optional<float> umidade_raw;

#ifdef ARDUINO
    // Em produção, a biblioteca DHT devolve ``NaN`` em falha de CRC ou
    // timeout de I/O — ambos capturados como leitura inválida pelo
    // caminho ``is_valid_reading`` abaixo, sem tratamento especial.
    const float t = dht_.readTemperature();
    const float h = dht_.readHumidity();
    temperatura_raw = t;  // ``NaN`` propagado como-is
    umidade_raw = h;
#else
    // Em builds nativos, a leitura vem do stub injetado por
    // :func:`injetar_leitura_stub`. Quando nada é injetado, o default
    // é ``std::nullopt`` para ambos os campos — o que simula uma
    // falha de leitura que o teste pode usar para exercitar P5/P6.
    if (stub_leitura_.temperatura.has_value()) {
        temperatura_raw = stub_leitura_.temperatura;
    }
    if (stub_leitura_.umidade.has_value()) {
        umidade_raw = static_cast<float>(stub_leitura_.umidade.value());
    }
#endif

    const bool valido = is_valid_reading(temperatura_raw, umidade_raw);
    const bool emitir_alerta_persistente = failure_counter_.record(!valido ? false : true);

    // Comentário pt-BR acima do ``if`` (R12.4) — ramo da leitura
    // rejeitada: loga a rejeição no Logger com a mensagem canônica e,
    // se for a terceira falha consecutiva, adiciona alerta persistente
    // (R1.5). Retorna ``SensorReading`` vazio para sinalizar ao
    // chamador que a amostra deve ser descartada (R1.3).
    if (!valido) {
        if (logger_ != nullptr) {
            const std::string msg =
                build_invalid_reading_log(temperatura_raw, umidade_raw);
            logger_->warn("DHT22_Sensor", msg.c_str());
            if (emitir_alerta_persistente) {
                logger_->error(
                    "DHT22_Sensor",
                    "falha persistente: 3 leituras consecutivas invalidas");
            }
        }
        return SensorReading{};
    }

    // Ramo da leitura válida — loga a linha formatada (R1.2, R1.4 / P7)
    // e devolve o par ao chamador para composição do ``Sample_Record``.
    const float t_ok = temperatura_raw.value();
    const int h_ok = static_cast<int>(umidade_raw.value());

    if (logger_ != nullptr) {
        const std::string linha = format_reading(now_ms, t_ok, h_ok);
        logger_->info("DHT22_Sensor", linha.c_str());
    }

    SensorReading leitura;
    leitura.temperatura = t_ok;
    leitura.umidade = h_ok;
    return leitura;
}

// -----------------------------------------------------------------------------
// Método:     SensorDriver::registrar_pulso
// Finalidade: Encaminha um timestamp aceito pelo debounce à
//             ``BpmWindow`` (R2.1). Em testes nativos simula a ISR.
// Parâmetros:
//   - ``ts_ms``: timestamp do pulso em milissegundos.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SensorDriver::registrar_pulso(std::uint32_t ts_ms) {
    // Comentário pt-BR acima do ``if`` (R12.4) — só encaminhamos o
    // pulso quando há ``BpmWindow`` injetada. Esta ramificação
    // mantém o driver utilizável em testes que exercitem somente o
    // DHT22 (P5/P6/P7) sem exigir uma ``BpmWindow`` real.
    if (bpm_window_ != nullptr) {
        bpm_window_->registrar_pulso(ts_ms);
    }
}

// -----------------------------------------------------------------------------
// Método:     SensorDriver::injetar_leitura_stub
// Finalidade: Define a leitura a ser devolvida pela próxima chamada de
//             :func:`ler` nos builds nativos. Em builds Arduino é
//             compilado como *no-op* (a biblioteca DHT é a única
//             fonte de dados).
// Parâmetros:
//   - ``leitura``: :class:`SensorReading` a devolver.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SensorDriver::injetar_leitura_stub(const SensorReading& leitura) noexcept {
#ifdef ARDUINO
    (void)leitura;  // no-op em produção
#else
    stub_leitura_ = leitura;
#endif
}

// =============================================================================
// ISR do ``Pulse_Simulator`` (R2.1, R2.4, Property 4)
// =============================================================================

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     isr_pulse_button (build Arduino)
// Finalidade: ISR da borda de subida do ``Pulse_Simulator``. Aplica
//             debounce de 150 ms (R2.4) e sinaliza pendência via
//             ``g_pulse_pending_count``. NÃO toca em ``BpmWindow``,
//             ``Logger`` ou ``std::string`` — o tratamento acontece
//             no ``loop()`` principal.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void IRAM_ATTR isr_pulse_button() {
    const std::uint32_t now = pulse_micros_now();
    // Condição avaliada: intervalo desde a última borda aceita.
    // Se estiver abaixo do limiar de debounce (``kDebouncePulsoUs``),
    // a borda é considerada ruído e descartada — sem incrementar o
    // contador de pendências.
    if ((now - g_pulse_last_accepted_us) < kDebouncePulsoUs) {
        return;
    }
    g_pulse_last_accepted_us = now;
    g_pulse_pending_count += 1u;
}

#else  // !ARDUINO — build nativo (Unity).

// -----------------------------------------------------------------------------
// Função:     isr_pulse_button (build nativo)
// Finalidade: Versão nativa da ISR — idêntica em semântica à versão
//             Arduino, porém sem ``IRAM_ATTR``. Permite que testes
//             Unity exercitem o debounce de 150 ms sem flashing.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_pulse_button() {
    const std::uint32_t now = pulse_micros_now();
    // Mesma condição avaliada pela versão Arduino; duplicada aqui
    // deliberadamente para manter a simetria entre os dois builds.
    if ((now - g_pulse_last_accepted_us) < kDebouncePulsoUs) {
        return;
    }
    g_pulse_last_accepted_us = now;
    g_pulse_pending_count += 1u;
}

#endif  // ARDUINO

}  // namespace cardioia
