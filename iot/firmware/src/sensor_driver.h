// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sensor_driver.h
// Finalidade:
//   Declara o driver de I/O ``SensorDriver`` — camada física responsável
//   por (a) ler o ``DHT22_Sensor`` a cada Sampling_Interval, (b) validar
//   as leituras contra as faixas operacionais (R1.3 / P5), (c) contar
//   falhas consecutivas e emitir alerta persistente após três rejeições
//   (R1.5 / P6), (d) formatar a linha de leitura para o Monitor Serial
//   no formato canônico ``[DHT22_Sensor] ts=... temperatura=XX.X°C
//   umidade=YY%`` (R1.2, R1.4 / P7) e (e) alimentar o ``BpmWindow`` com
//   timestamps de pulso vindos da ISR com debounce de
//   ``DEBOUNCE_PULSO_MS`` (150 ms) aplicado via ``micros()`` (R2.1,
//   R2.4 / P4).
//
//   O arquivo também expõe três helpers **puros** (``is_valid_reading``,
//   ``build_invalid_reading_log``, ``format_reading``) e a classe
//   ``PersistentFailureCounter`` — espelhos diretos das funções homônimas
//   do ``reference_model.py``. Manter a paridade desses oráculos em C++
//   é o que permite à suíte ``iot/tests/test_sensor_driver_pbt.py``
//   validar, por co-simulação, as propriedades P5/P6/P7 do firmware.
//
//   Estratégia de build dual (produção × testes):
//     * Em builds Arduino/PlatformIO (``ARDUINO`` definido) o ISR do
//       pulso é marcado com ``IRAM_ATTR`` e o método ``iniciar`` aciona
//       ``attachInterrupt`` na borda de subida do pino do
//       ``Pulse_Simulator``. A leitura do DHT22 usa a biblioteca
//       ``DHT sensor library`` (``DHT.h``) via compilação condicional
//       em ``sensor_driver.cpp``.
//     * Em builds nativos Unity (env ``native``) não há ``Arduino.h``:
//       a ISR vira função regular (sem ``IRAM_ATTR``) e ``ler()``
//       retorna uma leitura *stub* injetável — permitindo aos testes
//       exercitar as transições P5/P6/P7 de forma determinística.
//
//   Requisitos atendidos: R1.1, R1.2, R1.3, R1.4, R1.5, R2.1, R2.4,
//                         R12.2, R12.4.
//   Propriedades alvo (reference_model.py): P4, P5, P6, P7.
// =============================================================================

#ifndef CARDIOIA_SENSOR_DRIVER_H
#define CARDIOIA_SENSOR_DRIVER_H

#include <cstdint>    // std::uint32_t, std::uint8_t — tipos fixos sem Arduino.h
#include <cstddef>    // std::size_t
#include <optional>   // std::optional, std::nullopt — paridade com Python None
#include <string>     // std::string — evita a classe ``String`` do Arduino

#include "logger.h"         // cardioia::Logger
#include "sample_builder.h" // cardioia::SensorReading (struct compartilhada)
#include "bpm_window.h"     // cardioia::BpmWindow::registrar_pulso
#include "config.h"         // DEBOUNCE_PULSO_MS

namespace cardioia {

// ---------------------------------------------------------------------------
// Helper puro: is_valid_reading (R1.3, Property 5)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     is_valid_reading
// Finalidade: Bicondicional de validação do DHT22 — espelho direto de
//             :func:`reference_model.is_valid_reading`. Retorna ``true``
//             se e somente se ambos os campos estão presentes (não
//             ``std::nullopt``), são finitos (não ``NaN``/``Inf``),
//             ``temperatura ∈ [-40,0; 80,0]`` °C e ``umidade ∈ [0; 100]``
//             (como ``float``, a faixa é ``[0.0, 100.0]``, coerente com
//             a variante do reference model que aceita *floats* em
//             umidade). Função pura — sem efeitos colaterais.
// Parâmetros:
//   - ``temp``: temperatura opcional em °C lida do DHT22.
//   - ``hum``:  umidade opcional em % lida do DHT22 (aceita ``float``
//               para casar com o retorno típico da biblioteca DHT).
// Retorno:    ``true`` se a leitura é válida; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool is_valid_reading(std::optional<float> temp, std::optional<float> hum);

// -----------------------------------------------------------------------------
// Função:     build_invalid_reading_log
// Finalidade: Monta a mensagem de log para uma leitura rejeitada por
//             :func:`is_valid_reading`. A string DEVE conter, como
//             substrings: ``"DHT22_Sensor"`` (identificador do sensor),
//             o tipo canônico de falha (``"NaN"`` se algum valor está
//             ausente ou é ``NaN``; ``"out_of_range"`` caso contrário)
//             e a representação numérica dos valores rejeitados — em
//             paridade com :func:`reference_model.build_invalid_reading_log`
//             usado como oráculo da suíte PBT (R1.3 / P5).
// Parâmetros:
//   - ``temp``: temperatura rejeitada (pode ser ``std::nullopt`` ou ``NaN``).
//   - ``hum``:  umidade rejeitada (pode ser ``std::nullopt`` ou ``NaN``).
// Retorno:    :class:`std::string` com a mensagem pronta para o Logger.
// -----------------------------------------------------------------------------
std::string build_invalid_reading_log(std::optional<float> temp,
                                      std::optional<float> hum);

// -----------------------------------------------------------------------------
// Função:     format_reading
// Finalidade: Formata a linha do Monitor Serial para uma leitura
//             **válida**. A string retornada DEVE conter, como
//             substrings: ``"DHT22_Sensor"``, a representação decimal
//             de ``ts_ms``, ``temperatura`` com uma casa decimal seguida
//             de ``"°C"`` e ``umidade`` inteira seguida de ``"%"``
//             (R1.2, R1.4 / P7). Espelho direto de
//             :func:`reference_model.format_reading`. O chamador é
//             responsável por chamar :func:`is_valid_reading` antes.
// Parâmetros:
//   - ``ts_ms``:       timestamp da leitura em milissegundos (``>= 0``).
//   - ``temperatura``: temperatura válida em °C.
//   - ``umidade``:     umidade válida em %, inteiro ``[0; 100]``.
// Retorno:    :class:`std::string` no formato
//             ``"[DHT22_Sensor] ts=<ts> temperatura=<T>°C umidade=<H>%"``.
// -----------------------------------------------------------------------------
std::string format_reading(std::uint32_t ts_ms, float temperatura, int umidade);

// ---------------------------------------------------------------------------
// Classe PersistentFailureCounter (R1.5, Property 6)
// ---------------------------------------------------------------------------
// Contador de falhas consecutivas. Espelho do :class:`PersistentFailureCounter`
// de ``reference_model.py`` — conta leituras inválidas em sequência e
// retorna ``true`` **exatamente uma vez** por cada transição estrita
// ``2 → 3``; qualquer leitura válida reinicia o contador. Quatro
// inválidos consecutivos disparam apenas um alerta (a transição ``3→4``
// não re-emite).
class PersistentFailureCounter {
public:
    // -----------------------------------------------------------------------
    // Método:     PersistentFailureCounter::record
    // Finalidade: Registra o resultado de uma leitura do DHT22 e decide
    //             se o Logger deve emitir o alerta persistente (R1.5).
    // Parâmetros:
    //   - ``is_valid``: ``true`` se a leitura foi aceita por
    //                    :func:`is_valid_reading`; ``false`` caso tenha
    //                    sido rejeitada.
    // Retorno:    ``true`` se a chamada atual corresponde à transição
    //             ``2 → 3`` (disparar alerta); ``false`` caso contrário.
    // -----------------------------------------------------------------------
    bool record(bool is_valid);

    // -----------------------------------------------------------------------
    // Método:     PersistentFailureCounter::alerts_fired
    // Finalidade: Devolve o número total de alertas persistentes
    //             emitidos desde a construção — exposto para diagnóstico
    //             e asserts em testes.
    // Parâmetros: (nenhum).
    // Retorno:    inteiro não negativo.
    // -----------------------------------------------------------------------
    int alerts_fired() const noexcept { return alerts_fired_; }

    // -----------------------------------------------------------------------
    // Método:     PersistentFailureCounter::consecutive_invalid
    // Finalidade: Devolve a contagem atual de inválidos consecutivos —
    //             exposto para diagnóstico (não use para tomada de
    //             decisão clínica).
    // Parâmetros: (nenhum).
    // Retorno:    inteiro não negativo.
    // -----------------------------------------------------------------------
    int consecutive_invalid() const noexcept { return consecutive_; }

private:
    int consecutive_ = 0;
    int alerts_fired_ = 0;
};

// ---------------------------------------------------------------------------
// Classe SensorDriver (camada de I/O)
// ---------------------------------------------------------------------------
// Orquestra a leitura do DHT22, o contador de falhas persistentes e o
// registro de pulsos no ``BpmWindow``. O driver NÃO é dono do Logger
// nem do BpmWindow — ambos são injetados por referência para facilitar
// testes determinísticos.
class SensorDriver {
public:
    // -----------------------------------------------------------------------
    // Construtor: SensorDriver::SensorDriver
    // Finalidade: Mantém referências fracas ao ``Logger`` e à
    //             ``BpmWindow`` injetados. Ambos podem ser ``nullptr``
    //             em testes que exercitem apenas parte do fluxo.
    // Parâmetros:
    //   - ``logger``:      ponteiro para o Logger compartilhado.
    //   - ``bpm_window``:  ponteiro para a BpmWindow compartilhada.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit SensorDriver(Logger* logger = nullptr,
                          BpmWindow* bpm_window = nullptr) noexcept;

    // -----------------------------------------------------------------------
    // Método:     SensorDriver::iniciar
    // Finalidade: Em builds Arduino, inicializa o DHT22 (``dht.begin()``),
    //             configura o pino do ``Pulse_Simulator`` como
    //             ``INPUT_PULLUP`` e anexa a ISR :func:`isr_pulse_button`
    //             à borda de subida (R2.1). Em builds nativos apenas
    //             memoriza os pinos — nenhum hardware é tocado.
    // Parâmetros:
    //   - ``dht_pin``:   GPIO ao qual o DHT22 está conectado.
    //   - ``pulse_pin``: GPIO do botão do ``Pulse_Simulator``.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void iniciar(int dht_pin, int pulse_pin);

    // -----------------------------------------------------------------------
    // Método:     SensorDriver::ler
    // Finalidade: Executa um ciclo de leitura do DHT22. Em builds
    //             Arduino delega a ``DHT::readTemperature`` /
    //             ``DHT::readHumidity``; em builds nativos consome a
    //             leitura injetada por :func:`injetar_leitura_stub`
    //             (default: ``std::nullopt`` para ambos os campos,
    //             simulando falha).
    //             Aplica :func:`is_valid_reading`, loga no Logger a
    //             rejeição (com mensagem canônica de
    //             :func:`build_invalid_reading_log`), atualiza o
    //             ``PersistentFailureCounter`` e emite alerta de falha
    //             persistente via Logger quando houver transição 2→3
    //             (R1.5). Em leitura válida, o contador é zerado e a
    //             linha formatada por :func:`format_reading` é
    //             enviada ao Logger (R1.2, R1.4).
    // Parâmetros:
    //   - ``now_ms``: timestamp do ciclo em ms (``millis()`` em produção,
    //                 injetado em testes). Usado apenas no log.
    // Retorno:    :class:`SensorReading` com ``std::nullopt`` em caso
    //             de rejeição (P5), ou valores válidos caso contrário.
    // -----------------------------------------------------------------------
    SensorReading ler(std::uint32_t now_ms = 0u);

    // -----------------------------------------------------------------------
    // Método:     SensorDriver::registrar_pulso
    // Finalidade: Encaminha um timestamp de pulso (já aceito pelo
    //             debounce da ISR) à ``BpmWindow`` injetada. Em testes
    //             nativos permite simular a ISR sem hardware.
    // Parâmetros:
    //   - ``ts_ms``: timestamp do pulso em milissegundos.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void registrar_pulso(std::uint32_t ts_ms);

    // -----------------------------------------------------------------------
    // Método:     SensorDriver::injetar_leitura_stub
    // Finalidade: Disponível apenas em builds nativos — define a
    //             próxima leitura devolvida por :func:`ler`. Nos
    //             builds Arduino é compilado como *no-op* inline para
    //             manter a ABI consistente sem impacto em runtime.
    // Parâmetros:
    //   - ``leitura``: :class:`SensorReading` a devolver na próxima
    //                  chamada de :func:`ler`.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void injetar_leitura_stub(const SensorReading& leitura) noexcept;

    // -----------------------------------------------------------------------
    // Método:     SensorDriver::alerts_fired
    // Finalidade: Atalho para ``failure_counter_.alerts_fired()`` —
    //             exposto para que os testes verifiquem a contagem
    //             acumulada de alertas persistentes sem vazar o
    //             estado interno.
    // Parâmetros: (nenhum).
    // Retorno:    inteiro não negativo.
    // -----------------------------------------------------------------------
    int alerts_fired() const noexcept { return failure_counter_.alerts_fired(); }

private:
    Logger* logger_;
    BpmWindow* bpm_window_;
    PersistentFailureCounter failure_counter_;
    int dht_pin_ = -1;
    int pulse_pin_ = -1;
    SensorReading stub_leitura_{};  // usada somente em builds nativos
};

// ---------------------------------------------------------------------------
// ISR do pulso (R2.1, R2.4, Property 4)
// ---------------------------------------------------------------------------
// Mesmo padrão usado em ``connectivity.h``: a ISR NÃO toca em ``Logger``,
// ``BpmWindow`` ou ``std::string``. Ela apenas aplica o debounce de
// ``DEBOUNCE_PULSO_MS`` (150 ms) comparando ``micros()`` com o último
// pulso aceito e, quando válido, incrementa um contador ``volatile`` e
// armazena o ``ts_ms`` do último pulso. O ``loop()`` principal
// (``main.cpp``) drena essas pendências chamando
// :func:`SensorDriver::registrar_pulso` com o timestamp correspondente.

// Intervalo mínimo (microssegundos) entre duas bordas aceitas pela ISR.
// Deriva de ``config.h`` (R2.4 fixa 150 ms).
constexpr std::uint32_t kDebouncePulsoUs =
    static_cast<std::uint32_t>(DEBOUNCE_PULSO_MS) * 1000u;

// Pendências levantadas pela ISR e consumidas pelo ``loop()``:
//   * ``g_pulse_pending_count``: número de pulsos aceitos desde o
//     último consumo. Incremento atômico na ISR, decremento no loop.
//   * ``g_pulse_last_accepted_us``: timestamp (micros) do último pulso
//     aceito — base do debounce.
extern volatile std::uint32_t g_pulse_pending_count;
extern volatile std::uint32_t g_pulse_last_accepted_us;

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     isr_pulse_button
// Finalidade: ISR do botão ``Pulse_Simulator`` (R2.1, R2.4). Aplica o
//             debounce de 150 ms via ``micros()`` e, quando aceita,
//             incrementa ``g_pulse_pending_count``. Marcada com
//             ``IRAM_ATTR`` para residir em RAM no ESP32 (exigência
//             do core para ISRs anexadas via ``attachInterrupt``).
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_pulse_button();

#else  // !ARDUINO — build nativo (Unity). ISR vira função regular.

// -----------------------------------------------------------------------------
// Função:     isr_pulse_button
// Finalidade: Versão nativa da ISR — idêntica em semântica à versão
//             Arduino, porém sem ``IRAM_ATTR``. Usada pelos testes
//             Unity para exercitar o debounce sem flashing.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_pulse_button();

#endif  // ARDUINO

// -----------------------------------------------------------------------------
// Função:     set_pulse_micros_source (apenas em builds nativos)
// Finalidade: Permite aos testes Unity injetar uma fonte de ``micros()``
//             determinística para exercitar o debounce da ISR de pulso
//             sem depender do relógio real.
// Parâmetros:
//   - ``fn``: ponteiro para função ``std::uint32_t()``. ``nullptr``
//             restaura o default (retorna ``0``).
// Retorno:    (void).
// -----------------------------------------------------------------------------
#ifdef ARDUINO
inline void set_pulse_micros_source(std::uint32_t (* /*fn*/)()) { /* no-op */ }
#else
void set_pulse_micros_source(std::uint32_t (*fn)());
#endif

}  // namespace cardioia

#endif  // CARDIOIA_SENSOR_DRIVER_H
