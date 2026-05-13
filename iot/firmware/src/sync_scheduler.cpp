// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sync_scheduler.cpp
// Finalidade:
//   Implementação do ``cardioia::SyncScheduler`` declarado em
//   ``sync_scheduler.h``. Orquestra o reenvio de amostras pendentes no
//   ``EdgeBuffer`` após reconexão e o roteamento direto de novas
//   amostras quando online e buffer vazio.
//
//   Padrão "in-flight record":
//     O scheduler retira um registro do buffer (``pop_front``) e o
//     mantém em ``in_flight_`` até que a publicação seja confirmada.
//     Somente após confirmação o registro é descartado (considerado
//     entregue). Em caso de falha, o registro permanece em
//     ``in_flight_`` para reenvio na próxima tentativa — sem
//     duplicação e sem perda (R7.2, R7.4, Property 13).
//
//   Trade-off de ordem ao desconectar:
//     Quando ``Connectivity_Flag`` transita para ``false`` durante o
//     replay e há um registro in-flight, ele é devolvido ao FINAL do
//     buffer (via ``push``) porque o ``EdgeBuffer`` não expõe
//     ``push_front``. Isso é aceitável: a Property 13 exige
//     conservação de multiconjunto (sem perda nem duplicação), e a
//     ordem no ``buffer_final`` após aborto é implementation-defined.
//     Na próxima reconexão, o replay retomará do início do buffer.
//
//   Requisitos atendidos: R7.1, R7.2, R7.3, R7.4, R7.5, R12.2, R12.4.
//   Propriedade alvo: P13.
// =============================================================================

#include "sync_scheduler.h"

#include <cstring>  // std::strlen

#ifdef ARDUINO
#  include <Arduino.h>  // millis()
#endif

namespace cardioia {

// ---------------------------------------------------------------------------
// Utilitário interno — timestamp portátil
// ---------------------------------------------------------------------------
// No ESP32 usamos ``millis()``; em build nativo retornamos zero para
// manter os testes determinísticos (o ``tick`` recebe ``now_ms`` como
// parâmetro, então o relógio real só importa em ``onReconectar``).

#ifdef ARDUINO
static std::uint32_t agora_ms() {
    return static_cast<std::uint32_t>(millis());
}
#else
static std::uint32_t agora_ms() {
    return 0U;
}
#endif

// ---------------------------------------------------------------------------
// Utilitário interno — log seguro
// ---------------------------------------------------------------------------
// Encaminha mensagens ao Logger quando ele existe; evita testes de
// nullptr repetidos no corpo dos métodos públicos.

[[maybe_unused]]
static void log_info(Logger* logger, const char* msg) {
    if (logger != nullptr) {
        logger->info("SyncScheduler", msg);
    }
}

[[maybe_unused]]
static void log_warn(Logger* logger, const char* msg) {
    if (logger != nullptr) {
        logger->warn("SyncScheduler", msg);
    }
}

[[maybe_unused]]
static void log_error(Logger* logger, const char* msg) {
    if (logger != nullptr) {
        logger->error("SyncScheduler", msg);
    }
}

// =============================================================================
// Implementação dos métodos públicos
// =============================================================================

// -----------------------------------------------------------------------------
// Construtor: SyncScheduler::SyncScheduler
// Finalidade: Inicializa o scheduler com referências ao buffer e ao
//             cliente MQTT. Todos os contadores e flags começam em
//             estado "inativo" (sem replay, sem suspensão).
// Parâmetros:
//   - ``buffer``: referência ao EdgeBuffer compartilhado.
//   - ``mqtt``:   referência ao MqttClient compartilhado.
//   - ``logger``: ponteiro opcional para o Logger.
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
SyncScheduler::SyncScheduler(EdgeBuffer& buffer, MqttClient& mqtt,
                             Logger* logger)
    : buffer_(buffer), mqtt_(mqtt), logger_(logger) {}

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::onReconectar
// Finalidade: Inicia o ciclo de replay ao reconectar (R7.1). Registra
//             o timestamp de início para aguardar 5 s antes de começar
//             os envios e reinicia contadores de falha e suspensão.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SyncScheduler::onReconectar() {
    replaying_ = true;
    replay_started_ = false;
    replay_start_ms_ = agora_ms();
    consecutive_failures_ = 0;
    suspended_ = false;
    last_failure_ms_ = 0;

    log_info(logger_, "reconexao detectada: replay agendado em ate 5 s");
}

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::onDesconectar
// Finalidade: Aborta o replay quando a conectividade é perdida (R7.4).
//             Se houver registro in-flight, devolve-o ao buffer para
//             preservar o multiconjunto sem duplicação. O registro vai
//             ao final do buffer (trade-off documentado no cabeçalho).
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SyncScheduler::onDesconectar() {
    // Se há um registro em trânsito que ainda não foi confirmado,
    // devolvemos ao buffer para não perdê-lo (R7.4, Property 13).
    if (in_flight_.has_value()) {
        buffer_.push(in_flight_.value());
        in_flight_ = std::nullopt;
        log_warn(logger_,
                 "desconexao durante replay: in-flight devolvido ao buffer");
    }

    replaying_ = false;
    replay_started_ = false;

    log_info(logger_, "replay abortado por desconexao");
}

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::enviarOuEnfileirar
// Finalidade: Roteia uma nova amostra (R7.3). Se online, buffer vazio,
//             sem in-flight e sem replay ativo, publica diretamente.
//             Caso contrário, enfileira no EdgeBuffer.
// Parâmetros:
//   - ``record``:           amostra a rotear.
//   - ``connectivity_flag``: estado atual da conectividade.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SyncScheduler::enviarOuEnfileirar(const SampleRecord& record,
                                       bool connectivity_flag) {
    // Se online, buffer vazio, sem registro in-flight pendente e sem
    // replay ativo, encaminha diretamente ao MqttClient (R7.3).
    if (connectivity_flag &&
        buffer_.is_empty() &&
        !in_flight_.has_value() &&
        !replaying_) {

        // Tenta publicar diretamente; se falhar, enfileira para
        // reenvio posterior.
        if (tentarPublicar(record)) {
            return;
        }
        // Publicação direta falhou — enfileira para não perder a
        // amostra (conservação do multiconjunto, P13).
        buffer_.push(record);
        log_warn(logger_,
                 "publicacao direta falhou: amostra enfileirada");
        return;
    }

    // Condição não atendida para envio direto — enfileira no buffer.
    buffer_.push(record);
}

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::tick
// Finalidade: Processamento periódico do replay. Implementa a máquina
//             de estados completa: atraso inicial de 5 s, envio com
//             throttle de 100 ms, retry com backoff de 5 s e suspensão
//             após 3 falhas consecutivas (R7.1, R7.5).
// Parâmetros:
//   - ``now_ms``:           timestamp atual em ms.
//   - ``connectivity_flag``: estado atual da conectividade.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SyncScheduler::tick(std::uint32_t now_ms, bool connectivity_flag) {
    // Se a conectividade foi perdida, aborta o replay imediatamente
    // (R7.4).
    if (!connectivity_flag) {
        // Só chama onDesconectar se estávamos em replay ou temos
        // in-flight — evita chamadas redundantes.
        if (replaying_ || in_flight_.has_value()) {
            onDesconectar();
        }
        return;
    }

    // Se a sincronização está suspensa por excesso de falhas (R7.5),
    // não faz nada até a próxima transição de Connectivity_Flag.
    if (suspended_) {
        return;
    }

    // Se não estamos em modo replay, não há nada a processar.
    if (!replaying_) {
        return;
    }

    // Fase 1: aguardar 5 s após reconexão antes de iniciar o replay
    // (R7.1). O atraso garante que a conexão MQTT esteja estabilizada.
    if (!replay_started_) {
        if ((now_ms - replay_start_ms_) < kRetryDelayMs) {
            return;
        }
        // Atraso de 5 s cumprido — replay pode prosseguir.
        replay_started_ = true;
        log_info(logger_, "atraso de 5 s cumprido: iniciando replay");
    }

    // Fase 2: se houve falha recente, aguardar 5 s antes de tentar
    // novamente (R7.5).
    if (last_failure_ms_ != 0 &&
        (now_ms - last_failure_ms_) < kRetryDelayMs) {
        return;
    }

    // Fase 3: se há registro in-flight (falha anterior), tenta
    // reenviar antes de pegar o próximo do buffer.
    if (in_flight_.has_value()) {
        // Tenta reenviar o registro que falhou anteriormente.
        if (tentarPublicar(in_flight_.value())) {
            // Publicação confirmada — descarta o in-flight (R7.2).
            in_flight_ = std::nullopt;
            consecutive_failures_ = 0;
            last_send_ms_ = now_ms;
            last_failure_ms_ = 0;
        } else {
            // Falha no reenvio — incrementa contador e verifica
            // suspensão (R7.5).
            consecutive_failures_++;
            last_failure_ms_ = now_ms;

            // Se atingiu 3 falhas consecutivas, suspende a
            // sincronização até a próxima transição de
            // Connectivity_Flag (R7.5).
            if (consecutive_failures_ >= kMaxConsecutiveFailures) {
                suspended_ = true;
                log_error(logger_,
                          "3 falhas consecutivas: sync suspensa (R7.5)");
            }
        }
        return;
    }

    // Fase 4: sem in-flight — tenta pegar o próximo registro do buffer.
    if (!buffer_.is_empty()) {
        // Respeita o intervalo mínimo de 100 ms entre envios
        // consecutivos (R7.1).
        if (last_send_ms_ != 0 &&
            (now_ms - last_send_ms_) < kReplayIntervalMs) {
            return;
        }

        // Retira o registro mais antigo do buffer para tentativa de
        // envio. O registro fica em in_flight_ até confirmação.
        auto record_opt = buffer_.pop_front();

        // Se pop_front retornou vazio (condição de corrida improvável
        // mas defensiva), encerra o replay.
        if (!record_opt.has_value()) {
            replaying_ = false;
            return;
        }

        in_flight_ = record_opt.value();

        // Tenta publicar o registro retirado do buffer.
        if (tentarPublicar(in_flight_.value())) {
            // Publicação confirmada — descarta o in-flight (R7.2).
            in_flight_ = std::nullopt;
            consecutive_failures_ = 0;
            last_send_ms_ = now_ms;
            last_failure_ms_ = 0;
        } else {
            // Falha na publicação — o registro permanece em
            // in_flight_ para reenvio (R7.2, R7.4). Incrementa
            // contador de falhas consecutivas.
            consecutive_failures_++;
            last_failure_ms_ = now_ms;

            // Se atingiu 3 falhas consecutivas, suspende (R7.5).
            if (consecutive_failures_ >= kMaxConsecutiveFailures) {
                suspended_ = true;
                log_error(logger_,
                          "3 falhas consecutivas: sync suspensa (R7.5)");
            }
        }
        return;
    }

    // Buffer vazio e sem in-flight — replay concluído com sucesso.
    replaying_ = false;
    log_info(logger_, "replay concluido: buffer vazio");
}

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::reset
// Finalidade: Reinicia todos os contadores e flags internos. Usado em
//             testes e em cenários de reset externo.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SyncScheduler::reset() {
    consecutive_failures_ = 0;
    suspended_ = false;
    replaying_ = false;
    replay_started_ = false;
    in_flight_ = std::nullopt;
    last_send_ms_ = 0;
    replay_start_ms_ = 0;
    last_failure_ms_ = 0;
}

// =============================================================================
// Implementação de métodos privados
// =============================================================================

// -----------------------------------------------------------------------------
// Método:     SyncScheduler::tentarPublicar (privado)
// Finalidade: Serializa o SampleRecord em JSON compacto e tenta
//             publicá-lo via MqttClient no tópico de telemetria do
//             paciente. Encapsula serialização + montagem do tópico.
// Parâmetros:
//   - ``record``: amostra a publicar.
// Retorno:    ``true`` se publicação confirmada; ``false`` caso
//             contrário (payload inválido, tópico inválido ou falha
//             de rede).
// -----------------------------------------------------------------------------
bool SyncScheduler::tentarPublicar(const SampleRecord& record) {
    // Serializa o registro em JSON compacto (R3.2, R3.3).
    char payload[CARDIOIA_JSON_MAX];

    // Se a serialização falhar (paciente_id inválido ou JSON > 256),
    // não há como publicar — retorna false sem tentar rede.
    if (!serializar(record, payload, sizeof(payload))) {
        log_error(logger_, "falha ao serializar SampleRecord para JSON");
        return false;
    }

    // Monta o tópico canônico de telemetria para o paciente (R8.2).
    std::string topico;
    try {
        topico = topico_telemetria(record.paciente_id);
    } catch (...) {
        // paciente_id inválido para o padrão do tópico — não publica.
        log_error(logger_, "paciente_id invalido para topico MQTT");
        return false;
    }

    // Tenta publicar via MqttClient com QoS 1 (R8.3). O retorno
    // indica se o PUBACK foi recebido dentro do timeout (R8.7).
    const std::size_t len = std::strlen(payload);
    return mqtt_.publicar(topico, payload, len);
}

}  // namespace cardioia
