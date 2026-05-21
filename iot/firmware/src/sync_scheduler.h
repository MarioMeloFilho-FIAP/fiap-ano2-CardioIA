// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sync_scheduler.h
// Finalidade:
//   Declara o módulo ``cardioia::SyncScheduler`` — orquestrador de
//   sincronização de amostras pendentes no ``EdgeBuffer`` após
//   reconexão (Requisito 7). O scheduler implementa:
//
//     * Replay do buffer com atraso inicial de até 5 s (R7.1) e
//       intervalo mínimo de 100 ms entre envios consecutivos.
//     * Padrão "in-flight record": ``pop_front`` só ocorre após
//       confirmação de publicação bem-sucedida; em falha, a amostra
//       permanece em ``in_flight_`` para reenvio sem duplicação (R7.2,
//       R7.4).
//     * Política de retry: 5 s entre tentativas após falha, até 3
//       falhas consecutivas antes de suspender a sincronização até a
//       próxima transição de ``Connectivity_Flag`` (R7.5).
//     * Roteamento direto: enquanto online e buffer vazio, novas
//       amostras vão direto ao ``MqttClient.publicar`` (R7.3).
//
//   Propriedade alvo (reference_model.py): P13 — conservação do
//   multiconjunto ``publicados ⊎ buffer_final == B0`` e preservação
//   da ordem cronológica nos publicados.
//
//   Trade-off documentado:
//     Ao desconectar durante replay, o registro ``in_flight_`` é
//     devolvido ao FINAL do buffer (via ``push``) e não ao início.
//     Isso é aceitável porque: (a) a Property 13 exige conservação de
//     multiconjunto (não de ordem no buffer_final após aborto), (b) o
//     ``EdgeBuffer`` não expõe ``push_front``, e (c) na próxima
//     reconexão o replay retomará do início do buffer, restaurando a
//     ordem cronológica de entrega.
//
//   Requisitos atendidos: R7.1, R7.2, R7.3, R7.4, R7.5, R12.2, R12.4.
// =============================================================================

#ifndef CARDIOIA_SYNC_SCHEDULER_H
#define CARDIOIA_SYNC_SCHEDULER_H

#include <cstdint>   // std::uint32_t
#include <optional>  // std::optional

#include "edge_buffer.h"     // cardioia::EdgeBuffer
#include "mqtt_client.h"     // cardioia::MqttClient, cardioia::topico_telemetria
#include "logger.h"          // cardioia::Logger
#include "config.h"          // cardioia::CARDIOIA_JSON_MAX
#include "sample_builder.h"  // cardioia::SampleRecord, cardioia::serializar

namespace cardioia {

// ---------------------------------------------------------------------------
// Classe ``SyncScheduler``
// ---------------------------------------------------------------------------
// Orquestra o reenvio de amostras pendentes no ``EdgeBuffer`` após
// reconexão e o roteamento direto de novas amostras quando online e
// buffer vazio. Não possui threads internas — depende de chamadas
// periódicas a ``tick()`` pelo ``loop()`` do firmware.
class SyncScheduler {
public:
    // Número máximo de falhas consecutivas de publicação antes de
    // suspender a sincronização (R7.5).
    static constexpr int kMaxConsecutiveFailures = 3;

    // Intervalo mínimo, em ms, entre envios consecutivos durante o
    // replay do buffer (R7.1).
    static constexpr std::uint32_t kReplayIntervalMs = 100;

    // Tempo de espera, em ms, antes de iniciar o replay após
    // reconexão (R7.1) e entre tentativas após falha (R7.5).
    static constexpr std::uint32_t kRetryDelayMs = 5000;

    // -----------------------------------------------------------------------
    // Construtor: SyncScheduler::SyncScheduler
    // Finalidade: Inicializa o scheduler com referências ao buffer e ao
    //             cliente MQTT. O scheduler não assume posse dos objetos
    //             injetados — eles devem viver pelo menos tanto quanto o
    //             próprio scheduler.
    // Parâmetros:
    //   - ``buffer``: referência ao ``EdgeBuffer`` compartilhado.
    //   - ``mqtt``:   referência ao ``MqttClient`` compartilhado.
    //   - ``logger``: ponteiro opcional para o Logger (default nullptr).
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    SyncScheduler(EdgeBuffer& buffer, MqttClient& mqtt,
                  Logger* logger = nullptr);

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::onReconectar
    // Finalidade: Chamado quando ``Connectivity_Flag`` transita de
    //             ``false`` para ``true`` (R7.1). Inicia o ciclo de
    //             replay: marca ``replaying_ = true``, registra o
    //             timestamp de início para aguardar 5 s antes de
    //             começar os envios, e reinicia contadores de falha.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void onReconectar();

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::onDesconectar
    // Finalidade: Chamado quando ``Connectivity_Flag`` transita para
    //             ``false`` durante a sincronização (R7.4). Aborta o
    //             replay imediatamente: se houver registro in-flight,
    //             devolve-o ao buffer (ao final — trade-off documentado)
    //             para preservar o multiconjunto sem duplicação.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void onDesconectar();

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::enviarOuEnfileirar
    // Finalidade: Roteia uma nova amostra gerada pelo firmware (R7.3).
    //             Se online, buffer vazio, sem in-flight e sem replay
    //             ativo, publica diretamente via ``MqttClient``. Caso
    //             contrário, enfileira no ``EdgeBuffer``.
    // Parâmetros:
    //   - ``record``:           amostra a rotear.
    //   - ``connectivity_flag``: estado atual da conectividade.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void enviarOuEnfileirar(const SampleRecord& record,
                            bool connectivity_flag);

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::tick
    // Finalidade: Chamado periodicamente pelo ``loop()`` para processar
    //             o replay pendente. Implementa a máquina de estados:
    //             aguarda 5 s após reconexão, envia registros com
    //             intervalo de 100 ms, trata falhas com retry de 5 s e
    //             suspende após 3 falhas consecutivas (R7.5).
    // Parâmetros:
    //   - ``now_ms``:           timestamp atual em ms (``millis()``).
    //   - ``connectivity_flag``: estado atual da conectividade.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void tick(std::uint32_t now_ms, bool connectivity_flag);

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::suspended
    // Finalidade: Indica se a sincronização está suspensa por excesso
    //             de falhas consecutivas (R7.5). A suspensão é desfeita
    //             na próxima chamada a ``onReconectar()``.
    // Parâmetros: (nenhum).
    // Retorno:    ``true`` se suspensa; ``false`` caso contrário.
    // -----------------------------------------------------------------------
    bool suspended() const { return suspended_; }

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::replaying
    // Finalidade: Indica se o scheduler está em modo replay (enviando
    //             registros pendentes do buffer).
    // Parâmetros: (nenhum).
    // Retorno:    ``true`` se em replay; ``false`` caso contrário.
    // -----------------------------------------------------------------------
    bool replaying() const { return replaying_; }

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::reset
    // Finalidade: Reinicia todos os contadores e flags internos. Usado
    //             em testes e em cenários de reset externo.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void reset();

private:
    EdgeBuffer& buffer_;
    MqttClient& mqtt_;
    Logger* logger_;

    // Contador de falhas consecutivas de publicação durante replay.
    int consecutive_failures_ = 0;

    // Sincronização suspensa por excesso de falhas (R7.5).
    bool suspended_ = false;

    // Indica que o scheduler está em modo replay (enviando pendentes).
    bool replaying_ = false;

    // Registro atualmente em trânsito — retirado do buffer mas ainda
    // não confirmado pelo broker. Padrão "in-flight record" para
    // evitar perda sem duplicação (R7.2, R7.4).
    std::optional<SampleRecord> in_flight_;

    // Timestamp (ms) do último envio bem-sucedido — usado para
    // respeitar o intervalo mínimo de 100 ms entre envios (R7.1).
    std::uint32_t last_send_ms_ = 0;

    // Timestamp (ms) em que ``onReconectar()`` foi chamado — usado
    // para aguardar 5 s antes de iniciar o replay (R7.1).
    std::uint32_t replay_start_ms_ = 0;

    // Indica que o atraso inicial de 5 s já foi cumprido e o replay
    // pode prosseguir com os envios.
    bool replay_started_ = false;

    // Timestamp (ms) da última falha — usado para aguardar 5 s antes
    // de tentar novamente (R7.5).
    std::uint32_t last_failure_ms_ = 0;

    // -----------------------------------------------------------------------
    // Método:     SyncScheduler::tentarPublicar (privado)
    // Finalidade: Serializa o ``SampleRecord`` e tenta publicá-lo via
    //             ``MqttClient.publicar``. Encapsula a lógica de
    //             serialização + montagem do tópico para manter os
    //             métodos públicos legíveis.
    // Parâmetros:
    //   - ``record``: amostra a publicar.
    // Retorno:    ``true`` se publicação confirmada; ``false`` caso
    //             contrário.
    // -----------------------------------------------------------------------
    bool tentarPublicar(const SampleRecord& record);
};

}  // namespace cardioia

#endif  // CARDIOIA_SYNC_SCHEDULER_H
