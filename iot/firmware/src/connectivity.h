// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/connectivity.h
// Finalidade:
//   Declara a máquina de estados finita (FSM) da ``Connectivity_Flag`` do
//   firmware CardioIA. O controlador expõe:
//
//     * a flag global ``Connectivity_Flag`` (R6.1), inicializada com
//       ``true`` no boot e controlável em tempo de execução;
//     * o parser puro :func:`parse_cmd`, que aceita apenas os comandos
//       canônicos ``{"ONLINE", "OFFLINE", "STATUS", "CONFIG_SHOW"}``
//       (case-insensitive após ``trim``) e preserva o estado atual em
//       qualquer entrada inválida, registrando log de rejeição (R6.5);
//     * a função pura :func:`transition_event`, que monta o evento
//       publicado a cada transição efetiva de ``Connectivity_Flag``,
//       contendo direção, tamanho do ``EdgeBuffer`` e texto distintivo
//       por direção (R6.3, R6.4);
//     * o ISR do botão dedicado (GPIO específico, distinto do
//       ``Pulse_Simulator``) com debounce mínimo de
//       ``DEBOUNCE_CONECTIVIDADE_MS`` (50 ms) via ``micros()`` (R6.2);
//     * a classe :class:`ConnectivityController`, que encapsula o lado
//       mutável da FSM, o ``Logger`` e a notificação ao
//       ``SyncScheduler`` em toda transição ``false → true`` (R6.3).
//
//   A implementação é "dual build":
//
//     * Em builds Arduino/PlatformIO (``ARDUINO`` definido) o ISR é
//       marcado com ``IRAM_ATTR`` e o ``ConnectivityController::iniciar``
//       configura o pino como ``INPUT_PULLUP`` e aciona
//       ``attachInterrupt`` na borda de descida.
//     * Em builds nativos Unity (env ``native``) o ISR é apenas uma
//       função regular que manipula os mesmos símbolos ``volatile`` —
//       isto permite que a suíte Unity simule o clique do botão sem
//       arrastar ``Arduino.h``.
//
//   Requisitos atendidos: R6.1, R6.2, R6.3, R6.4, R6.5, R12.2, R12.4.
//   Propriedades alvo (reference_model.py): P11, P12.
// =============================================================================

#ifndef CARDIOIA_CONNECTIVITY_H
#define CARDIOIA_CONNECTIVITY_H

#include <cstddef>  // std::size_t
#include <cstdint>  // std::uint32_t, std::uint8_t
#include <string>   // std::string — evitamos a classe ``String`` do Arduino

#include "config.h"
#include "logger.h"

namespace cardioia {

// ---------------------------------------------------------------------------
// FSM da ``Connectivity_Flag`` (R6.1)
// ---------------------------------------------------------------------------
// A FSM tem apenas dois estados — ``ONLINE`` e ``OFFLINE`` — que espelham
// o valor booleano de ``Connectivity_Flag``. O mapeamento canônico
// usado pelo ``parse_cmd`` e pelos testes PBT é:
//
//     Connectivity_Flag == true   <=>   ConnectivityState::ONLINE
//     Connectivity_Flag == false  <=>   ConnectivityState::OFFLINE
//
// Os nomes foram mantidos em inglês para casar 1:1 com o Enum
// ``ConnectivityState`` do ``reference_model.py`` (Property 12).
enum class ConnectivityState {
    ONLINE,
    OFFLINE
};

// Conversões utilitárias entre o booleano da ``Connectivity_Flag`` e
// ``ConnectivityState`` — declaradas aqui (e implementadas em
// ``connectivity.cpp``) para evitar duplicação em main.cpp / testes.

// -----------------------------------------------------------------------------
// Função:     state_from_flag
// Finalidade: Converte o booleano global ``Connectivity_Flag`` em
//             ``ConnectivityState`` sem alocar memória.
// Parâmetros:
//   - ``flag``: valor atual da ``Connectivity_Flag``.
// Retorno:    ``ConnectivityState::ONLINE`` se ``flag`` for ``true``,
//             ``ConnectivityState::OFFLINE`` caso contrário.
// -----------------------------------------------------------------------------
ConnectivityState state_from_flag(bool flag);

// -----------------------------------------------------------------------------
// Função:     flag_from_state
// Finalidade: Conversão inversa de :func:`state_from_flag`.
// Parâmetros:
//   - ``state``: estado lógico da FSM.
// Retorno:    ``true`` para ``ONLINE``, ``false`` para ``OFFLINE``.
// -----------------------------------------------------------------------------
bool flag_from_state(ConnectivityState state);

// ---------------------------------------------------------------------------
// parse_cmd (R6.5, Property 12)
// ---------------------------------------------------------------------------
// Resultado do parser puro de comandos Serial. O campo ``new_state`` é
// sempre igual a ``state`` quando ``event_type`` é ``"rejected"``,
// ``"status"`` ou ``"config_show"`` — apenas ``"online"`` e ``"offline"``
// podem alterar o estado.

struct ParseResult {
    // Novo estado da FSM após interpretar ``raw``. Em inputs inválidos,
    // ``new_state`` preserva ``state`` (R6.5).
    ConnectivityState new_state;

    // Tipo canônico do evento. Valores possíveis:
    //   * ``"online"``      — transição para ``ONLINE``.
    //   * ``"offline"``     — transição para ``OFFLINE``.
    //   * ``"status"``      — não muda estado; o chamador imprime
    //                         diagnóstico.
    //   * ``"config_show"`` — não muda estado; dump da configuração.
    //   * ``"rejected"``    — comando inválido ou malformado (R6.5).
    const char* event_type;

    // String original recebida — preservada literalmente para log de
    // rejeição conforme R6.5 ("contendo a string original"). Copiada
    // por valor para evitar *dangling reference* quando o buffer Serial
    // for reutilizado pelo chamador.
    std::string raw;
};

// -----------------------------------------------------------------------------
// Função:     parse_cmd
// Finalidade: Interpreta puramente uma string recebida pelo Monitor
//             Serial e devolve o ``ConnectivityState`` resultante e o
//             evento correspondente — sem tocar no ``Logger`` ou na
//             flag global. Isto permite testes PBT determinísticos e
//             paridade direta com :func:`reference_model.parse_cmd`
//             (Property 12).
// Parâmetros:
//   - ``raw``:   string bruta recebida pelo Serial (``readStringUntil``,
//                ``available/read`` etc.). Pode conter espaços, ``\r``,
//                ``\n`` e/ou diferença de caixa — todos são normalizados
//                internamente via ``trim`` + ``upper``.
//   - ``state``: estado atual da FSM; preservado em qualquer entrada
//                inválida (R6.5).
// Retorno:    estrutura :class:`ParseResult` com o novo estado, o tipo
//             do evento e a string original para log.
// -----------------------------------------------------------------------------
ParseResult parse_cmd(const std::string& raw, ConnectivityState state);

// ---------------------------------------------------------------------------
// transition_event (R6.3, R6.4, Property 11)
// ---------------------------------------------------------------------------
// Descrição compacta da transição ``old_flag → new_flag``. O campo
// ``text`` é a frase em pt-BR que o ``Logger`` vai emitir — distintiva
// por direção (vide ``reference_model.transition_event``).

struct TransitionEvent {
    // Literal curta identificando a direção da transição:
    //   * ``"false->true"`` (reconexão — R6.3).
    //   * ``"true->false"`` (perda de conexão — R6.4).
    const char* direction;

    // Número de ``Sample_Record`` pendentes no ``Local_Buffer`` no
    // momento da transição. Inteiro não negativo (R6.3).
    std::size_t buffer_size;

    // Texto distintivo por direção; redigido em pt-BR para o Monitor
    // Serial. Em ``false->true`` inclui ``buffer_size``; em
    // ``true->false`` inclui o aviso de armazenamento local.
    std::string text;
};

// -----------------------------------------------------------------------------
// Função:     transition_event
// Finalidade: Monta, de forma pura, o :class:`TransitionEvent` a ser
//             publicado pelo ``ConnectivityController`` em toda
//             transição efetiva da ``Connectivity_Flag`` (R6.3, R6.4).
//             Esta função NÃO toca em ``Serial``, ``Logger`` ou globais
//             — permitindo paridade direta com
//             :func:`reference_model.transition_event` (Property 11).
// Parâmetros:
//   - ``old_flag``:   valor anterior da ``Connectivity_Flag``.
//   - ``new_flag``:   novo valor da ``Connectivity_Flag``; DEVE ser
//                     diferente de ``old_flag`` (pré-condição do
//                     evento de transição).
//   - ``buffer_size``: quantidade atual de ``Sample_Record`` no
//                     ``EdgeBuffer``.
// Retorno:    :class:`TransitionEvent` com ``direction``, ``buffer_size``
//             e ``text`` preenchidos. Em ``old_flag == new_flag``, o
//             texto resultante fica vazio e ``direction`` aponta para
//             ``"noop"`` — a violação é tratada no chamador (asserção
//             no build nativo, log de erro no build Arduino) para não
//             propagar exceções pelo firmware.
// -----------------------------------------------------------------------------
TransitionEvent transition_event(bool old_flag, bool new_flag,
                                 std::size_t buffer_size);

// ---------------------------------------------------------------------------
// ISR do botão dedicado (R6.2)
// ---------------------------------------------------------------------------
// A ISR vive em RAM (``IRAM_ATTR``) para que possa ser servida mesmo
// durante acessos à flash. Ela NÃO toca em estruturas complexas — apenas
// sinaliza, via ``volatile bool``, que o ``loop()`` deve alternar a flag
// no próximo ciclo. Esta separação entre "detectar em ISR" e "tratar no
// loop" preserva o deadline de 500 ms exigido por R6.3/R6.4 sem fazer
// trabalho pesado dentro da interrupção.

// Intervalo mínimo (micros) entre duas bordas aceitas pela ISR. Vem de
// ``config.h`` (R6.2 fixa 50 ms).
constexpr std::uint32_t kDebounceConectividadeUs =
    static_cast<std::uint32_t>(DEBOUNCE_CONECTIVIDADE_MS) * 1000u;

// Sinal levantado pela ISR para informar ao ``loop()`` que há um
// toggle pendente. O ``ConnectivityController::processar_pendencias``
// consome este sinal. ``volatile`` garante visibilidade entre ISR e
// contexto regular (compartilhamento de memória sem barreiras).
extern volatile bool g_connectivity_toggle_pending;

// Timestamp (em ``micros()``) da última borda aceita pela ISR. Usado
// exclusivamente pelo debounce de ``DEBOUNCE_CONECTIVIDADE_MS``. Em
// builds nativos pode ser manipulado diretamente pelos testes Unity
// para simular rajadas de cliques.
extern volatile std::uint32_t g_connectivity_last_press_us;

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     isr_connectivity_button
// Finalidade: ISR do botão dedicado à ``Connectivity_Flag`` (R6.2).
//             Descarta bordas espúrias pelo critério ``delta <
//             DEBOUNCE_CONECTIVIDADE_MS`` e, quando aceita, sinaliza um
//             toggle pendente via ``g_connectivity_toggle_pending``.
//             Marcada com ``IRAM_ATTR`` para residir em RAM no ESP32.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_connectivity_button();

#else  // !ARDUINO — build nativo (Unity). ISR vira função regular.

// -----------------------------------------------------------------------------
// Função:     isr_connectivity_button
// Finalidade: Versão "nativa" da ISR, compilada nos testes Unity.
//             Semânticamente idêntica à versão Arduino; apenas não
//             carrega o atributo ``IRAM_ATTR``.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void isr_connectivity_button();

#endif  // ARDUINO

// -----------------------------------------------------------------------------
// Função:     set_micros_source (apenas para testes nativos)
// Finalidade: Permite aos testes Unity injetar uma fonte de ``micros()``
//             determinística para exercitar o debounce da ISR sem
//             depender do clock real do ESP32. Em builds Arduino a
//             função é um ``no-op`` silencioso (definido como
//             ``inline``).
// Parâmetros:
//   - ``fn``: ponteiro para função que retorna ``std::uint32_t``
//             simulando ``micros()``. ``nullptr`` restaura o default.
// Retorno:    (void).
// -----------------------------------------------------------------------------
#ifdef ARDUINO
inline void set_micros_source(std::uint32_t (* /*fn*/)()) { /* no-op */ }
#else
void set_micros_source(std::uint32_t (*fn)());
#endif

// ---------------------------------------------------------------------------
// Hook de notificação do SyncScheduler (R6.3 — "notificar em toda
// transição para ``true``")
// ---------------------------------------------------------------------------
// Definimos a dependência como um ponteiro de função registrável para
// evitar um ``#include "sync_scheduler.h"`` circular. O
// ``ConnectivityController`` chama este callback sempre que a flag
// transita de ``false`` para ``true`` — o ``SyncScheduler`` é quem de
// fato dispara o replay do ``EdgeBuffer``. Em testes Unity basta
// injetar um ``std::function``-like espião.
using SyncSchedulerReconectHook = void (*)(std::size_t buffer_size);

// -----------------------------------------------------------------------------
// Função:     set_sync_scheduler_hook
// Finalidade: Registra um callback invocado pela FSM em toda transição
//             ``false → true`` (R6.3). Caso ``hook == nullptr``, o
//             controlador não dispara replay — útil em testes.
// Parâmetros:
//   - ``hook``: ponteiro para função ``void(size_t)``. ``size_t`` é o
//               tamanho atual do ``EdgeBuffer`` no momento da
//               transição.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void set_sync_scheduler_hook(SyncSchedulerReconectHook hook);

// ---------------------------------------------------------------------------
// Flag global da conectividade (R6.1)
// ---------------------------------------------------------------------------
// Declarada aqui como ``extern`` e definida no ``.cpp``. É inicializada
// em ``true`` no boot conforme R6.1. Dashboards / testes externos podem
// ler esta flag; a escrita SHALL passar sempre por
// :class:`ConnectivityController` ou pelo :func:`parse_cmd` (para
// manter o log e a notificação do SyncScheduler coerentes).
extern bool Connectivity_Flag;

// ---------------------------------------------------------------------------
// ConnectivityController — fachada com estado mutável (R6.1–R6.4)
// ---------------------------------------------------------------------------
// Encapsula a única cópia da ``Connectivity_Flag``, o ``Logger`` e a
// notificação do ``SyncScheduler``. A instância é de posse do
// ``main.cpp`` — os módulos de I/O (``MqttClient``, ``SyncScheduler``)
// recebem uma referência const para LEITURA apenas.
class ConnectivityController {
public:
    // -----------------------------------------------------------------------
    // Construtor: ConnectivityController::ConnectivityController
    // Finalidade: Mantém referência ao ``Logger`` recebido (injeção de
    //             dependência). O ``logger`` pode ser ``nullptr`` em
    //             testes puros de FSM; nesse caso os métodos emitem no
    //             ``Serial`` apenas se ``Logger`` estiver definido.
    // Parâmetros:
    //   - ``logger``: ponteiro para o ``Logger`` compartilhado. O
    //                 controlador não assume posse do ponteiro.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit ConnectivityController(Logger* logger = nullptr);

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::iniciar
    // Finalidade: Inicializa a ``Connectivity_Flag`` em ``true`` (R6.1)
    //             e, em builds Arduino, configura o pino do botão como
    //             ``INPUT_PULLUP`` e anexa a ISR
    //             :func:`isr_connectivity_button` à borda de descida.
    //             Em builds nativos apenas reinicializa os ``volatile``
    //             da ISR, sem mexer em hardware.
    // Parâmetros:
    //   - ``pino_botao``: GPIO dedicado à ``Connectivity_Flag`` — DEVE
    //                     ser distinto do ``Pulse_Simulator`` (R6.2).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void iniciar(std::uint8_t pino_botao);

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::handle_serial_command
    // Finalidade: Aplica um comando textual vindo do Monitor Serial
    //             (R6.5). Normaliza via :func:`parse_cmd`, aplica a
    //             transição resultante (se houver) e registra no
    //             ``Logger`` — incluindo a rejeição explícita para
    //             qualquer comando inválido (R6.5).
    // Parâmetros:
    //   - ``raw``:         string bruta recebida do Serial.
    //   - ``buffer_size``: quantidade atual de ``Sample_Record``
    //                     pendentes no ``EdgeBuffer`` — repassada ao
    //                     :func:`transition_event` quando há
    //                     transição (R6.3).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void handle_serial_command(const std::string& raw, std::size_t buffer_size);

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::processar_pendencias
    // Finalidade: Consumido pelo ``loop()`` em todo ciclo. Se a ISR
    //             sinalizou um toggle (``g_connectivity_toggle_pending
    //             == true``), inverte a flag, monta o
    //             :class:`TransitionEvent`, emite log e notifica o
    //             ``SyncScheduler``. Caso não haja pendência, retorna
    //             imediatamente (caminho quente — chamada barata).
    // Parâmetros:
    //   - ``buffer_size``: tamanho atual do ``EdgeBuffer`` — usado no
    //                     :func:`transition_event` e na notificação ao
    //                     ``SyncScheduler``.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void processar_pendencias(std::size_t buffer_size);

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::set_flag
    // Finalidade: Ponto único de escrita da ``Connectivity_Flag``.
    //             Se houver transição real (``flag_`` != ``new_flag``)
    //             chama :func:`transition_event`, registra a mensagem
    //             em pt-BR no ``Logger`` e, quando
    //             ``new_flag == true``, dispara o callback do
    //             ``SyncScheduler`` registrado em
    //             :func:`set_sync_scheduler_hook`.
    // Parâmetros:
    //   - ``new_flag``:    novo valor desejado.
    //   - ``buffer_size``: tamanho atual do ``EdgeBuffer`` no momento
    //                     da transição.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void set_flag(bool new_flag, std::size_t buffer_size);

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::flag
    // Finalidade: Acessor read-only da ``Connectivity_Flag``. Usado por
    //             ``MqttClient``, ``SyncScheduler`` e pelo
    //             ``main.cpp`` para decidir se a amostra deve ir para
    //             o buffer ou ser publicada imediatamente (R7.3).
    // Parâmetros: (nenhum).
    // Retorno:    valor booleano atual de ``Connectivity_Flag``.
    // -----------------------------------------------------------------------
    bool flag() const { return flag_; }

    // -----------------------------------------------------------------------
    // Método:     ConnectivityController::state
    // Finalidade: Versão enum da flag (``ConnectivityState``). Facilita
    //             integração com ``parse_cmd`` que opera sobre
    //             ``ConnectivityState`` para ter paridade com o
    //             reference model Python.
    // Parâmetros: (nenhum).
    // Retorno:    ``ConnectivityState::ONLINE`` ou ``::OFFLINE``.
    // -----------------------------------------------------------------------
    ConnectivityState state() const { return state_from_flag(flag_); }

private:
    Logger* logger_;

    // R6.1: inicializada em ``true`` no boot. A flag global
    // ``Connectivity_Flag`` é mantida em sincronia por ``set_flag``.
    bool flag_ = true;
};

}  // namespace cardioia

#endif  // CARDIOIA_CONNECTIVITY_H
