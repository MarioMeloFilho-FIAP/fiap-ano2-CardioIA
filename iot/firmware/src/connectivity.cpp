// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/connectivity.cpp
// Finalidade:
//   Implementação da FSM da ``Connectivity_Flag`` declarada em
//   ``connectivity.h``. Esta TU cobre:
//
//     * o parser puro :func:`parse_cmd` (Property 12, R6.5), com
//       paridade de semântica com :func:`reference_model.parse_cmd`;
//     * :func:`transition_event` (Property 11, R6.3/R6.4), também puro;
//     * o ISR do botão dedicado com debounce de
//       ``DEBOUNCE_CONECTIVIDADE_MS`` (50 ms) via ``micros()`` (R6.2);
//     * :class:`ConnectivityController`, que integra flag global,
//       Logger e notificação ao ``SyncScheduler``.
//
//   Não usamos a classe ``String`` do Arduino em nenhum ponto: todas
//   as conversões passam por ``std::string`` + ``snprintf`` — mesma
//   convenção do ``Logger`` (task 19) para evitar fragmentação de heap
//   no ESP32.
//
//   Requisitos atendidos: R6.1, R6.2, R6.3, R6.4, R6.5, R12.2, R12.4.
// =============================================================================

#include "connectivity.h"

#include <cctype>   // std::toupper, std::isspace
#include <cstddef>  // std::size_t
#include <cstdint>  // std::uint32_t
#include <cstdio>   // std::snprintf
#include <cstring>  // std::strcmp
#include <string>   // std::string

#ifdef ARDUINO
#  include <Arduino.h>  // micros, digitalRead, pinMode, attachInterrupt, etc.
#endif

namespace cardioia {

// ---------------------------------------------------------------------------
// Definição da flag global (R6.1)
// ---------------------------------------------------------------------------
// Inicializada em ``true`` — o ESP32 nasce "online" e só cai para
// ``false`` mediante comando Serial ``OFFLINE``, pressão do botão
// dedicado (R6.2), falha de autenticação MQTT (R8.6) ou 3 reconexões
// MQTT falhas consecutivas (R8.4).
bool Connectivity_Flag = true;

// ---------------------------------------------------------------------------
// Estado compartilhado com a ISR (R6.2)
// ---------------------------------------------------------------------------
// ``volatile`` sinaliza ao compilador que estes símbolos podem mudar
// fora do fluxo regular — evitando que otimizações eliminem as leituras
// feitas pelo ``loop()`` que consome o toggle pendente.
volatile bool g_connectivity_toggle_pending = false;
volatile std::uint32_t g_connectivity_last_press_us = 0;

// ---------------------------------------------------------------------------
// Hook do ``SyncScheduler`` (R6.3)
// ---------------------------------------------------------------------------
// Inicialmente nulo; ``main.cpp`` registra via
// :func:`set_sync_scheduler_hook` após construir o scheduler.
namespace {
SyncSchedulerReconectHook g_sync_hook = nullptr;
}  // namespace

// -----------------------------------------------------------------------------
// Função:     set_sync_scheduler_hook
// Finalidade: Armazena o callback a ser invocado em toda transição
//             ``false → true`` da ``Connectivity_Flag`` (R6.3).
// Parâmetros:
//   - ``hook``: ponteiro para ``void(size_t)`` ou ``nullptr`` para
//               desabilitar.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void set_sync_scheduler_hook(SyncSchedulerReconectHook hook) {
    g_sync_hook = hook;
}

// ---------------------------------------------------------------------------
// Fonte de ``micros()`` (build Arduino vs build nativo)
// ---------------------------------------------------------------------------
// Em produção usamos ``::micros()`` do Arduino core. Em testes Unity,
// injetamos uma função determinística via :func:`set_micros_source`.
#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     micros_now
// Finalidade: Wrapper para ``::micros()`` do Arduino core, usado pelo
//             debounce da ISR.
// Parâmetros: (nenhum).
// Retorno:    microssegundos desde o boot do ESP32 (wraparound em ~71 min).
// -----------------------------------------------------------------------------
static std::uint32_t micros_now() {
    return static_cast<std::uint32_t>(::micros());
}

#else  // !ARDUINO — build nativo (testes Unity).

namespace {

// -----------------------------------------------------------------------------
// Função:     micros_default
// Finalidade: Implementação *stub* de ``micros`` para o caminho nativo.
//             Retorna zero para que os testes Unity tenham uma origem
//             determinística até injetarem a própria fonte via
//             :func:`set_micros_source`.
// Parâmetros: (nenhum).
// Retorno:    sempre ``0``.
// -----------------------------------------------------------------------------
std::uint32_t micros_default() {
    return 0u;
}

// Ponteiro para a fonte ativa de ``micros()``. Testes Unity trocam
// este ponteiro chamando :func:`set_micros_source`.
std::uint32_t (*g_micros_source)() = &micros_default;

}  // namespace

// -----------------------------------------------------------------------------
// Função:     micros_now (build nativo)
// Finalidade: Indireção configurável para ``micros()`` — permite testes
//             Unity injetarem uma sequência determinística.
// Parâmetros: (nenhum).
// Retorno:    valor retornado pela fonte registrada (ou ``0`` por default).
// -----------------------------------------------------------------------------
static std::uint32_t micros_now() {
    return g_micros_source();
}

// -----------------------------------------------------------------------------
// Função:     set_micros_source
// Finalidade: Registra uma fonte alternativa de ``micros()`` para o
//             build nativo. ``nullptr`` restaura o default de zero.
// Parâmetros:
//   - ``fn``: ponteiro para função ``std::uint32_t()``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void set_micros_source(std::uint32_t (*fn)()) {
    // Condição avaliada: ``fn`` ausente ⇒ restaurar default; caso
    // contrário, adotar a fonte injetada pelos testes.
    if (fn == nullptr) {
        g_micros_source = &micros_default;
    } else {
        g_micros_source = fn;
    }
}

#endif  // ARDUINO

// ---------------------------------------------------------------------------
// Conversões state <-> flag (utilitárias)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     state_from_flag
// Finalidade: Converte o booleano global em ``ConnectivityState``.
// Parâmetros:
//   - ``flag``: valor atual da ``Connectivity_Flag``.
// Retorno:    ``ConnectivityState::ONLINE`` se ``flag``, caso contrário
//             ``ConnectivityState::OFFLINE``.
// -----------------------------------------------------------------------------
ConnectivityState state_from_flag(bool flag) {
    return flag ? ConnectivityState::ONLINE : ConnectivityState::OFFLINE;
}

// -----------------------------------------------------------------------------
// Função:     flag_from_state
// Finalidade: Conversão inversa de :func:`state_from_flag`.
// Parâmetros:
//   - ``state``: estado da FSM.
// Retorno:    ``true`` para ``ONLINE``, ``false`` para ``OFFLINE``.
// -----------------------------------------------------------------------------
bool flag_from_state(ConnectivityState state) {
    return state == ConnectivityState::ONLINE;
}

// ---------------------------------------------------------------------------
// Normalização puramente textual usada por ``parse_cmd`` (R6.5)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     trim_upper
// Finalidade: Remove ``whitespace`` (tab, LF, CR, espaço, VT, FF) das
//             extremidades da string e promove todos os caracteres ASCII
//             restantes para maiúsculo — mesma normalização aplicada por
//             :func:`reference_model.parse_cmd` (``raw.strip().upper()``).
//             Como o ``config.h`` e a suíte PBT restringem comandos a
//             ASCII puro, usamos ``std::toupper`` com ``unsigned char``
//             (único uso seguro, portável e livre de UB).
// Parâmetros:
//   - ``raw``: string original recebida do Serial.
// Retorno:    nova :class:`std::string` normalizada; pode ser vazia.
// -----------------------------------------------------------------------------
static std::string trim_upper(const std::string& raw) {
    // Condição avaliada: string vazia ⇒ retorno imediato para poupar
    // alocações. Este atalho é dominante no caminho quente do parser.
    if (raw.empty()) {
        return std::string();
    }

    // Localiza o primeiro caractere não-``whitespace`` a partir da
    // esquerda. ``isspace`` cobre os mesmos brancos que ``str.strip()``
    // em Python (space, tab, LF, VT, FF, CR) — paridade com o oráculo.
    std::size_t start = 0;
    while (start < raw.size() &&
           std::isspace(static_cast<unsigned char>(raw[start]))) {
        ++start;
    }

    // Localiza o último caractere não-``whitespace`` a partir da direita.
    std::size_t end = raw.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }

    // Promove cada caractere a maiúsculo e devolve uma nova string.
    std::string out;
    out.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        const unsigned char ch = static_cast<unsigned char>(raw[i]);
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// parse_cmd (R6.5, Property 12)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     parse_cmd
// Finalidade: Parser puro dos comandos Serial — paridade estrita com
//             :func:`reference_model.parse_cmd` (R6.5, Property 12).
//             Retorna o novo estado da FSM (igual ao atual em qualquer
//             entrada inválida), o tipo canônico do evento (um literal
//             curto) e a string original para log de rejeição.
// Parâmetros:
//   - ``raw``:   string bruta recebida do Serial (``readStringUntil``
//                geralmente entrega com ``\r``/``\n`` no final).
//   - ``state``: estado atual da FSM; preservado em qualquer comando
//                inválido (R6.5).
// Retorno:    :class:`ParseResult` com ``new_state``, ``event_type`` e
//             cópia de ``raw``.
// -----------------------------------------------------------------------------
ParseResult parse_cmd(const std::string& raw, ConnectivityState state) {
    const std::string normalized = trim_upper(raw);

    ParseResult result;
    result.new_state = state;
    result.raw = raw;

    // Switch textual contra o conjunto canônico de comandos Serial
    // (R6.5). A sequência de ``strcmp`` substitui o ``switch``
    // tradicional (C++ não aceita ``switch`` em ``std::string``) sem
    // perder clareza. Qualquer outra entrada cai no caminho de
    // rejeição e preserva o estado atual.
    const char* norm = normalized.c_str();

    if (std::strcmp(norm, "ONLINE") == 0) {
        result.new_state = ConnectivityState::ONLINE;
        result.event_type = "online";
        return result;
    }
    if (std::strcmp(norm, "OFFLINE") == 0) {
        result.new_state = ConnectivityState::OFFLINE;
        result.event_type = "offline";
        return result;
    }
    if (std::strcmp(norm, "STATUS") == 0) {
        // STATUS não altera a flag; apenas sinaliza ao chamador que
        // ele deve imprimir o estado atual.
        result.event_type = "status";
        return result;
    }
    if (std::strcmp(norm, "CONFIG_SHOW") == 0) {
        // CONFIG_SHOW também é um "getter"; não altera a flag.
        result.event_type = "config_show";
        return result;
    }

    // Caminho de rejeição (R6.5): entrada vazia, só espaços, comando
    // próximo ("ONLINNE"), etc. Preserva o estado e devolve a string
    // original para que o Logger a inclua na mensagem de rejeição.
    result.event_type = "rejected";
    return result;
}

// ---------------------------------------------------------------------------
// transition_event (R6.3, R6.4, Property 11)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     transition_event
// Finalidade: Monta o :class:`TransitionEvent` puro para publicação pelo
//             ``ConnectivityController``. Paridade estrita com
//             :func:`reference_model.transition_event` (Property 11).
// Parâmetros:
//   - ``old_flag``:   valor anterior da ``Connectivity_Flag``.
//   - ``new_flag``:   novo valor da ``Connectivity_Flag``.
//   - ``buffer_size``: tamanho atual do ``EdgeBuffer`` (R6.3).
// Retorno:    :class:`TransitionEvent` com ``direction``, ``buffer_size``
//             e ``text`` preenchidos. Em ``old_flag == new_flag`` o
//             evento é degenerado (``direction == "noop"``, texto
//             vazio) — a decisão de publicar ou não é do chamador.
// -----------------------------------------------------------------------------
TransitionEvent transition_event(bool old_flag, bool new_flag,
                                 std::size_t buffer_size) {
    TransitionEvent event;
    event.buffer_size = buffer_size;

    // Condição avaliada: sem transição efetiva ⇒ evento degenerado.
    // O firmware NÃO lança exceções (ESP32 normalmente compila com
    // ``-fno-exceptions``); no lugar emitimos ``direction = "noop"`` e
    // texto vazio. O ``ConnectivityController`` evita emissão de log
    // neste caso (vide ``set_flag``).
    if (old_flag == new_flag) {
        event.direction = "noop";
        event.text = std::string();
        return event;
    }

    // Transição efetiva: escolhe direção e monta o texto distintivo
    // em pt-BR (mesmas strings do reference model Python).
    char buf[128];
    if (!old_flag && new_flag) {
        // Reconexão (R6.3) — inclui o número de amostras pendentes.
        event.direction = "false->true";
        std::snprintf(
            buf, sizeof(buf),
            "Conectividade restaurada; %lu amostra(s) pendente(s) no Local_Buffer.",
            static_cast<unsigned long>(buffer_size));
        event.text = std::string(buf);
    } else {
        // Perda de conexão (R6.4). O texto NÃO menciona o buffer_size
        // — mas mantemos o valor no evento por simetria de auditoria.
        event.direction = "true->false";
        event.text = std::string(
            "Conectividade perdida; amostras serao armazenadas localmente.");
    }
    return event;
}

// ---------------------------------------------------------------------------
// ISR do botão dedicado (R6.2)
// ---------------------------------------------------------------------------

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     isr_connectivity_button (build Arduino)
// Finalidade: ISR da borda de descida do botão dedicado à
//             ``Connectivity_Flag``. Aplica debounce de 50 ms (R6.2) e
//             sinaliza um toggle pendente ao ``loop()``. NÃO registra
//             log nem altera a flag diretamente — o tratamento
//             complexo é feito fora da ISR para preservar o deadline
//             de 500 ms (R6.3/R6.4) e evitar chamadas bloqueantes em
//             contexto de interrupção.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void IRAM_ATTR isr_connectivity_button() {
    const std::uint32_t now = micros_now();
    // Condição avaliada: intervalo desde a última borda aceita.
    // Se estiver abaixo do limiar de debounce (``DEBOUNCE_CONECTIVIDADE_MS``
    // * 1000), a borda é considerada ruído e descartada — sem pendurar
    // o toggle.
    if ((now - g_connectivity_last_press_us) < kDebounceConectividadeUs) {
        return;
    }
    g_connectivity_last_press_us = now;
    g_connectivity_toggle_pending = true;
}

#else  // !ARDUINO — build nativo (Unity).

// -----------------------------------------------------------------------------
// Função:     isr_connectivity_button (build nativo)
// Finalidade: Versão nativa da ISR — idêntica em semântica à versão
//             Arduino, porém sem ``IRAM_ATTR``. Permite que testes
//             Unity exercitem o debounce sem flashing.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_connectivity_button() {
    const std::uint32_t now = micros_now();
    // Mesma condição avaliada pela versão Arduino; duplicada aqui
    // deliberadamente para manter a simetria entre os dois builds
    // (não compartilhamos corpo via macro para preservar legibilidade).
    if ((now - g_connectivity_last_press_us) < kDebounceConectividadeUs) {
        return;
    }
    g_connectivity_last_press_us = now;
    g_connectivity_toggle_pending = true;
}

#endif  // ARDUINO

// ---------------------------------------------------------------------------
// ConnectivityController — implementação
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Construtor: ConnectivityController::ConnectivityController
// Finalidade: Armazena o ``Logger`` injetado e inicializa ``flag_`` em
//             ``true`` (R6.1) — a flag global ``Connectivity_Flag``
//             também já está definida como ``true`` neste arquivo.
// Parâmetros:
//   - ``logger``: ponteiro para o ``Logger`` compartilhado (pode ser
//                 ``nullptr`` em testes puros de FSM).
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
ConnectivityController::ConnectivityController(Logger* logger)
    : logger_(logger), flag_(true) {
    Connectivity_Flag = true;
}

// -----------------------------------------------------------------------------
// Método:     ConnectivityController::iniciar
// Finalidade: Garante o estado inicial (R6.1) e, em builds Arduino,
//             configura o pino do botão como ``INPUT_PULLUP`` e anexa
//             a ISR :func:`isr_connectivity_button` à borda de descida
//             (R6.2). Em builds nativos apenas zera as variáveis de
//             controle da ISR, sem tocar em hardware.
// Parâmetros:
//   - ``pino_botao``: GPIO dedicado à ``Connectivity_Flag`` — DEVE ser
//                     distinto do ``Pulse_Simulator`` (R6.2).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConnectivityController::iniciar(std::uint8_t pino_botao) {
    flag_ = true;
    Connectivity_Flag = true;
    g_connectivity_toggle_pending = false;
    g_connectivity_last_press_us = 0;

#ifdef ARDUINO
    // Configura pino com ``INPUT_PULLUP`` (botão fecha para GND quando
    // pressionado, coerente com a convenção do Wokwi).
    pinMode(static_cast<uint8_t>(pino_botao), INPUT_PULLUP);
    attachInterrupt(
        digitalPinToInterrupt(static_cast<uint8_t>(pino_botao)),
        isr_connectivity_button,
        FALLING);
#else
    // Build nativo: o parâmetro ``pino_botao`` é irrelevante — apenas
    // consumido para evitar warning ``-Wunused-parameter``.
    (void)pino_botao;
#endif

    if (logger_ != nullptr) {
        logger_->info("ConnectivityController",
                      "Inicializacao concluida; Connectivity_Flag=true no boot.");
    }
}

// -----------------------------------------------------------------------------
// Método:     ConnectivityController::handle_serial_command
// Finalidade: Aplica um comando textual do Monitor Serial (R6.5).
//             Delega a análise pura ao :func:`parse_cmd`, emite log em
//             pt-BR e, quando o comando implica transição, chama
//             :func:`set_flag`.
// Parâmetros:
//   - ``raw``:         string bruta recebida do Serial.
//   - ``buffer_size``: tamanho atual do ``EdgeBuffer``, repassado ao
//                     :func:`transition_event`.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConnectivityController::handle_serial_command(const std::string& raw,
                                                   std::size_t buffer_size) {
    const ParseResult result = parse_cmd(raw, state_from_flag(flag_));

    // Switch contra o tipo de evento retornado pelo parser (R6.5).
    // Cada ramo tem um comportamento próprio quanto a transição,
    // logging e notificação do SyncScheduler.
    if (std::strcmp(result.event_type, "online") == 0) {
        set_flag(true, buffer_size);
        return;
    }
    if (std::strcmp(result.event_type, "offline") == 0) {
        set_flag(false, buffer_size);
        return;
    }
    if (std::strcmp(result.event_type, "status") == 0) {
        if (logger_ != nullptr) {
            char extras[64];
            std::snprintf(extras, sizeof(extras), "state=%s buffer=%lu",
                          flag_ ? "ONLINE" : "OFFLINE",
                          static_cast<unsigned long>(buffer_size));
            logger_->event(LogLevel::INFO, "ConnectivityController",
                           "Comando STATUS", extras);
        }
        return;
    }
    if (std::strcmp(result.event_type, "config_show") == 0) {
        if (logger_ != nullptr) {
            char extras[64];
            std::snprintf(extras, sizeof(extras), "state=%s",
                          flag_ ? "ONLINE" : "OFFLINE");
            logger_->event(LogLevel::INFO, "ConnectivityController",
                           "Comando CONFIG_SHOW", extras);
        }
        return;
    }

    // Ramo final: rejeição (R6.5). Registra no ``Logger`` a string
    // original recebida do Serial, dentro do bloco de ``extras`` do
    // formato ``(raw="...")``, preservando o estado atual da flag.
    if (logger_ != nullptr) {
        char extras[128];
        std::snprintf(extras, sizeof(extras), "raw=\"%.80s\"",
                      result.raw.c_str());
        logger_->event(LogLevel::WARN, "ConnectivityController",
                       "Comando invalido rejeitado", extras);
    }
}

// -----------------------------------------------------------------------------
// Método:     ConnectivityController::processar_pendencias
// Finalidade: Ponto de consumo do sinal ``g_connectivity_toggle_pending``
//             levantado pela ISR (R6.2). Se houver toggle, inverte a
//             flag via :func:`set_flag` — o que já cobre o log e o
//             disparo do ``SyncScheduler``. Caso contrário, retorna
//             imediatamente.
// Parâmetros:
//   - ``buffer_size``: tamanho atual do ``EdgeBuffer``; repassado ao
//                     :func:`set_flag` para compor o
//                     :class:`TransitionEvent`.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConnectivityController::processar_pendencias(std::size_t buffer_size) {
    // Condição avaliada: há toggle pendente emitido pela ISR?
    // Evitamos barreiras explícitas porque ``volatile bool`` já
    // garante leitura direta em ambos os builds; o risco de
    // race-condition é mitigado porque a ISR só escreve ``true`` e o
    // loop só escreve ``false`` após consumir.
    if (!g_connectivity_toggle_pending) {
        return;
    }
    g_connectivity_toggle_pending = false;

    // Inversão da flag é o único comportamento do botão (R6.2).
    set_flag(!flag_, buffer_size);
}

// -----------------------------------------------------------------------------
// Método:     ConnectivityController::set_flag
// Finalidade: Ponto único de escrita da ``Connectivity_Flag``. Se
//             ``new_flag != flag_``, monta o :class:`TransitionEvent`
//             via :func:`transition_event`, registra log em pt-BR e,
//             quando a transição for para ``true``, dispara o hook do
//             ``SyncScheduler`` registrado em
//             :func:`set_sync_scheduler_hook`.
// Parâmetros:
//   - ``new_flag``:    novo valor desejado.
//   - ``buffer_size``: tamanho atual do ``EdgeBuffer`` no momento da
//                     transição.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConnectivityController::set_flag(bool new_flag, std::size_t buffer_size) {
    const bool old_flag = flag_;

    // Condição avaliada: transição efetiva? Escritas idempotentes são
    // silenciosamente ignoradas para não poluir o log — o comando
    // ``ONLINE`` recebido enquanto já ``ONLINE`` é um no-op.
    if (old_flag == new_flag) {
        return;
    }

    flag_ = new_flag;
    Connectivity_Flag = new_flag;

    const TransitionEvent event =
        transition_event(old_flag, new_flag, buffer_size);

    if (logger_ != nullptr) {
        // ``extras`` agrega direção e tamanho do buffer para facilitar
        // auditoria no Monitor Serial. Mantemos as mesmas literais
        // usadas pelo reference model Python para permitir diff textual
        // entre execução real e suíte de PBT.
        char extras[96];
        std::snprintf(extras, sizeof(extras),
                      "direction=%s buffer_size=%lu",
                      event.direction,
                      static_cast<unsigned long>(event.buffer_size));

        // Condição avaliada: direção da transição escolhe o nível do
        // log — ``INFO`` em reconexões (evento positivo) e ``WARN`` em
        // perdas de conexão (situação anômala mas esperada).
        const LogLevel level =
            new_flag ? LogLevel::INFO : LogLevel::WARN;
        logger_->event(level, "ConnectivityController",
                       event.text.c_str(), extras);
    }

    // R6.3: em toda transição ``false → true`` notifica o
    // ``SyncScheduler`` (se registrado) para drenar o buffer.
    if (new_flag && g_sync_hook != nullptr) {
        g_sync_hook(buffer_size);
    }
}

}  // namespace cardioia
