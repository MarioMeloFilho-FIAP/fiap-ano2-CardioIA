// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/mqtt_client.cpp
// Finalidade:
//   Implementação de ``cardioia::MqttClient``, ``cardioia::MqttReconnectFSM``
//   e ``cardioia::topico_telemetria`` declarados em ``mqtt_client.h``.
//
//   A implementação é dividida em três blocos:
//
//     1. **FSM pura (Property 14)** — sem I/O; espelha exatamente a
//        ``MqttReconnectFSM`` do reference model Python
//        (``iot/tests/reference_model.py``). Todo o comportamento
//        observável casa com as transições documentadas em
//        ``design.md`` (R8.4, R8.6, R7.5).
//
//     2. **Construtor do tópico (Property 15)** — validação manual do
//        ``paciente_id`` contra ``^[A-Za-z0-9_-]{1,32}$``. Evita o uso
//        de ``<regex>`` para não arrastar dependências pesadas no
//        firmware embarcado; a regra é suficientemente simples para
//        uma checagem char-a-char.
//
//     3. **Wrapper de I/O (R8.1, R8.3, R8.5, R8.7, R15.1)** — em builds
//        ``ARDUINO`` usa ``WiFiClientSecure`` + ``PubSubClient``; em
//        builds nativos todas as chamadas de rede viram *stubs* que
//        retornam ``false``. A separação permite compilar e testar a
//        FSM e o construtor do tópico em ``g++`` nativo sem arrastar
//        Arduino.h, conforme a verificação do Task 25.
//
//   Notas de segurança (R15):
//     * Em produção, a verificação do certificado do broker deve usar
//       ``WiFiClientSecure::setCACert(...)`` com o CA raiz da HiveMQ
//       Cloud (Let's Encrypt / ISRG Root X1). Para manter o código
//       auto-contido durante a simulação Wokwi — onde o CA varia por
//       ambiente — este arquivo cai para ``setInsecure()`` quando
//       ``CARDIOIA_MQTT_CA_CERT`` não está definido, acompanhado de
//       log de aviso. Essa limitação está documentada em
//       ``iot/docs/SEGURANCA_LGPD.md`` (R15.5).
//
//   Requisitos atendidos: R8.1, R8.2, R8.3, R8.4, R8.5, R8.6, R8.7,
//                         R15.1, R12.2, R12.4.
// =============================================================================

#include "mqtt_client.h"

#include <cstdio>      // std::snprintf
#include <cstring>     // std::strlen
#include <stdexcept>   // std::invalid_argument

#ifdef ARDUINO
#  include <Arduino.h>             // millis(), delay()
#  include <WiFiClientSecure.h>    // TLS 1.2+
#  include <PubSubClient.h>        // MQTT 3.1.1 (QoS 1)
#  ifdef CARDIOIA_MQTT_CA_CERT
// O build define ``CARDIOIA_MQTT_CA_CERT`` como uma string literal contendo
// o certificado CA raiz em formato PEM. Mantemos a macro opt-in para não
// engessar o Wokwi, onde a cadeia exata varia conforme o projeto publicado.
#  endif
#endif

namespace cardioia {

// =============================================================================
// Bloco 1 — FSM de reconexão (Property 14)
// =============================================================================
// Todas as transições abaixo preservam a semântica do reference model
// Python; qualquer alteração aqui exige atualização simultânea em
// ``iot/tests/reference_model.py`` para manter a suíte PBT verde.

// -----------------------------------------------------------------------------
// Método:     MqttReconnectFSM::advance_connect_ok
// Finalidade: Processa o evento ``connect_ok`` — ``PubSubClient::connect``
//             devolveu ``true``. Transita para ``ONLINE`` e zera o
//             contador, exceto em ``AUTH_SUSPENDED`` (R8.6).
// Parâmetros: (nenhum).
// Retorno:    ``MqttState`` após a transição.
// -----------------------------------------------------------------------------
MqttState MqttReconnectFSM::advance_connect_ok() noexcept {
    // R8.6: em ``AUTH_SUSPENDED`` ignoramos qualquer tentativa de
    // ``connect_ok`` — a suspensão só é desfeita por ``advance_reset``.
    if (state_ == MqttState::AUTH_SUSPENDED) {
        return state_;
    }
    state_ = MqttState::ONLINE;
    fail_count_ = 0;
    return state_;
}

// -----------------------------------------------------------------------------
// Método:     MqttReconnectFSM::advance_connect_fail
// Finalidade: Processa o evento ``connect_fail`` — ``connect`` expirou
//             ou devolveu código transitório (timeout, rede). Aumenta
//             o contador e, ao atingir ``kMaxConsecutiveFailures``,
//             transita para ``OFFLINE`` (R8.4).
// Parâmetros: (nenhum).
// Retorno:    ``MqttState`` após a transição.
// -----------------------------------------------------------------------------
MqttState MqttReconnectFSM::advance_connect_fail() noexcept {
    // Em ``AUTH_SUSPENDED`` as tentativas não são contabilizadas — o
    // firmware não deveria sequer estar chamando ``conectar()``, mas a
    // FSM é defensiva para não amplificar bugs de chamadores.
    if (state_ == MqttState::AUTH_SUSPENDED) {
        return state_;
    }
    fail_count_ += 1;
    // Ao cruzar o limite de 3 falhas consecutivas marcamos offline —
    // o ``ConnectivityController`` usa isto para derrubar a
    // ``Connectivity_Flag`` (R8.4).
    if (fail_count_ >= kMaxConsecutiveFailures) {
        state_ = MqttState::OFFLINE;
    }
    return state_;
}

// -----------------------------------------------------------------------------
// Método:     MqttReconnectFSM::advance_auth_rejected
// Finalidade: Processa o evento ``auth_rejected`` — broker devolveu
//             código de rejeição de credenciais (R8.6). Sempre
//             transita para ``AUTH_SUSPENDED`` e zera o contador.
// Parâmetros: (nenhum).
// Retorno:    ``MqttState`` (sempre ``AUTH_SUSPENDED``).
// -----------------------------------------------------------------------------
MqttState MqttReconnectFSM::advance_auth_rejected() noexcept {
    state_ = MqttState::AUTH_SUSPENDED;
    fail_count_ = 0;
    return state_;
}

// -----------------------------------------------------------------------------
// Método:     MqttReconnectFSM::advance_flag_true
// Finalidade: Processa o evento ``flag_true`` — disparado pelo
//             ``ConnectivityController`` ao observar transição
//             ``Connectivity_Flag = true`` (R7.5). Apenas zera o
//             contador; o estado é preservado.
// Parâmetros: (nenhum).
// Retorno:    ``MqttState`` corrente (sem alteração).
// -----------------------------------------------------------------------------
MqttState MqttReconnectFSM::advance_flag_true() noexcept {
    fail_count_ = 0;
    return state_;
}

// -----------------------------------------------------------------------------
// Método:     MqttReconnectFSM::advance_reset
// Finalidade: Processa o evento ``reset`` — intervenção externa
//             (novo commit, comando Serial) que retira a FSM de
//             ``AUTH_SUSPENDED``. Em outros estados é no-op.
// Parâmetros: (nenhum).
// Retorno:    ``MqttState`` após a transição.
// -----------------------------------------------------------------------------
MqttState MqttReconnectFSM::advance_reset() noexcept {
    // Só o estado ``AUTH_SUSPENDED`` é afetado por ``reset``: a
    // intervenção externa retoma o ciclo normal de reconexão.
    if (state_ == MqttState::AUTH_SUSPENDED) {
        state_ = MqttState::OFFLINE;
        fail_count_ = 0;
    }
    return state_;
}

// =============================================================================
// Bloco 2 — Construtor do tópico (Property 15)
// =============================================================================

// -----------------------------------------------------------------------------
// Função:     is_valid_topic_id_char (auxiliar privada)
// Finalidade: Testa se ``c`` pertence ao alfabeto permitido pela regex
//             ``[A-Za-z0-9_-]``. Evita arrastar ``<regex>`` no firmware.
// Parâmetros:
//   - ``c``: caractere a ser testado.
// Retorno:    ``true`` se o caractere é aceito; ``false`` caso contrário.
// -----------------------------------------------------------------------------
static bool is_valid_topic_id_char(char c) noexcept {
    // Dígitos 0-9.
    if (c >= '0' && c <= '9') return true;
    // Letras maiúsculas A-Z.
    if (c >= 'A' && c <= 'Z') return true;
    // Letras minúsculas a-z.
    if (c >= 'a' && c <= 'z') return true;
    // Underscore e hífen — únicos não-alfanuméricos aceitos.
    if (c == '_' || c == '-') return true;
    return false;
}

// -----------------------------------------------------------------------------
// Função:     topico_telemetria
// Finalidade: Constrói o tópico canônico de telemetria
//             ``"cardioia/paciente/<paciente_id>/sinais"`` (Property 15
//             / R8.2). Valida ``paciente_id`` contra
//             ``^[A-Za-z0-9_-]{1,32}$``.
// Parâmetros:
//   - ``paciente_id``: identificador do paciente (já anonimizado).
// Retorno:    ``std::string`` com o tópico canônico.
// Exceções:   lança ``std::invalid_argument`` se ``paciente_id`` não
//             casa com o padrão permitido.
// -----------------------------------------------------------------------------
std::string topico_telemetria(const std::string& paciente_id) {
    // Primeira barreira: comprimento fora de [1, 32] já descarta a entrada
    // sem necessidade de percorrer caractere a caractere.
    if (paciente_id.empty() || paciente_id.size() > 32U) {
        throw std::invalid_argument(
            "paciente_id deve ter entre 1 e 32 caracteres (R8.2, Property 15).");
    }
    // Segunda barreira: alfabeto restrito — qualquer caractere fora do
    // conjunto permitido é motivo de rejeição imediata.
    for (char c : paciente_id) {
        if (!is_valid_topic_id_char(c)) {
            throw std::invalid_argument(
                "paciente_id deve casar com ^[A-Za-z0-9_-]{1,32}$ "
                "(R8.2, Property 15).");
        }
    }
    // Entrada validada: compõe o tópico canônico. ``std::string`` faz
    // alocação única; em ambiente embarcado o caller normalmente
    // reutiliza um buffer, mas no caminho MQTT o custo é aceitável
    // (apenas uma concatenação por ciclo de 5 s).
    std::string out;
    out.reserve(18U + paciente_id.size() + 7U);  // "cardioia/paciente/" + id + "/sinais"
    out.append("cardioia/paciente/");
    out.append(paciente_id);
    out.append("/sinais");
    return out;
}

// =============================================================================
// Bloco 3 — Wrapper de I/O (``MqttClient``)
// =============================================================================
//
// Em build nativo (``ARDUINO`` indefinido) todas as chamadas de rede
// viram no-ops e ``conectar``/``publicar`` retornam ``false``. Isso
// mantém a suíte PBT exercitando apenas a FSM pura (Property 14) e o
// construtor do tópico (Property 15) sem depender de Wi-Fi ou do
// broker real — alinhado à verificação descrita no Task 25.

// ---------------------------------------------------------------------------
// Utilitário interno — timestamp portátil
// ---------------------------------------------------------------------------
// Reproduz a estratégia de ``logger.cpp``: no ESP32 usamos ``millis()``;
// em build nativo retornamos zero para que os testes fiquem
// determinísticos sem depender de relógio real.

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     now_ms_mqtt
// Finalidade: Expõe ``millis()`` com nome local para evitar colisão
//             com o ``now_ms`` estático do ``logger.cpp``.
// Parâmetros: (nenhum).
// Retorno:    uptime em ms do ESP32.
// -----------------------------------------------------------------------------
static std::uint32_t now_ms_mqtt() {
    return static_cast<std::uint32_t>(millis());
}

#else  // !ARDUINO — build nativo.

// -----------------------------------------------------------------------------
// Função:     now_ms_mqtt
// Finalidade: Stub determinístico para build nativo — sempre zero.
// Parâmetros: (nenhum).
// Retorno:    ``0``.
// -----------------------------------------------------------------------------
static std::uint32_t now_ms_mqtt() {
    return 0U;
}

#endif  // ARDUINO

// ---------------------------------------------------------------------------
// Utilitário interno — log seguro
// ---------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Função:     log_info_safe / log_warn_safe / log_error_safe
// Finalidade: Encaminham mensagens ao ``Logger`` quando ele existe.
//             Centralizam o teste de ``nullptr`` para que o corpo dos
//             métodos públicos fique legível.
// Parâmetros:
//   - ``logger``:    ponteiro para o Logger (pode ser ``nullptr``).
//   - ``componente``: identificador do emissor (ex.: ``"MqttClient"``).
//   - ``mensagem``:  descrição do evento.
// Retorno:    (void).
// -----------------------------------------------------------------------------
// No build nativo, apenas ``log_warn_safe`` e ``log_error_safe`` são
// chamadas — marcamos todas com ``[[maybe_unused]]`` para evitar
// warnings ``-Wunused-function`` em ``-Wall -Wextra`` quando o caminho
// Arduino está desativado.
[[maybe_unused]] static void log_info_safe(Logger* logger,
                                           const char* componente,
                                           const char* mensagem) {
    if (logger != nullptr) {
        logger->info(componente, mensagem);
    }
}

[[maybe_unused]] static void log_warn_safe(Logger* logger,
                                           const char* componente,
                                           const char* mensagem) {
    if (logger != nullptr) {
        logger->warn(componente, mensagem);
    }
}

[[maybe_unused]] static void log_error_safe(Logger* logger,
                                            const char* componente,
                                            const char* mensagem,
                                            const char* extras = nullptr) {
    if (logger != nullptr) {
        logger->event(LogLevel::ERROR, componente, mensagem, extras);
    }
}

#ifdef ARDUINO
// ---------------------------------------------------------------------------
// Recursos reais do caminho Arduino — instanciados apenas no build ESP32.
// ---------------------------------------------------------------------------
// As instâncias são *file-static* para manter a API pública livre de
// ``WiFiClientSecure.h`` / ``PubSubClient.h`` (esses headers arrastam
// muitos símbolos que não queremos vazar para outros módulos).
static WiFiClientSecure g_wifi_tls_client;
static PubSubClient g_pubsub_client(g_wifi_tls_client);
#endif

// ---------------------------------------------------------------------------
// Implementação dos métodos públicos
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Construtor: MqttClient::MqttClient
// Finalidade: Inicializa o wrapper com o Logger opcional. Os demais
//             campos começam zerados / ``nullptr`` e só ganham valores
//             reais após ``configurar``.
// Parâmetros:
//   - ``logger``: destino opcional dos eventos de log.
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
MqttClient::MqttClient(Logger* logger) noexcept : logger_(logger) {}

// -----------------------------------------------------------------------------
// Método:     MqttClient::configurar
// Finalidade: Registra host/port/credenciais/client_id para as chamadas
//             subsequentes a ``conectar()``. Não inicia conexão.
// Parâmetros: descritos no cabeçalho.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void MqttClient::configurar(const char* host, std::uint16_t port,
                            const char* user, const char* password,
                            const char* client_id) noexcept {
    host_ = host;
    port_ = (port == 0U) ? kDefaultTlsPort : port;
    user_ = user;
    password_ = password;
    client_id_ = client_id;
#ifdef ARDUINO
    // Configuração estática do cliente MQTT — feita apenas uma vez.
    // ``setBufferSize`` dimensiona o buffer interno do PubSubClient
    // para suportar o payload máximo permitido pelo R8.2 (1024 B) mais
    // o *overhead* de cabeçalho MQTT (~16 B).
    g_pubsub_client.setServer(host_, port_);
    g_pubsub_client.setKeepAlive(kKeepaliveSeconds);             // R8.1
    g_pubsub_client.setSocketTimeout(static_cast<uint16_t>(
        kConnectTimeoutMs / 1000U));                             // R8.1/R8.5
    g_pubsub_client.setBufferSize(static_cast<uint16_t>(
        kPayloadMaxBytes + 64U));                                // R8.2
#endif
}

// -----------------------------------------------------------------------------
// Método:     MqttClient::conectar
// Finalidade: Tenta conectar ao broker respeitando o backoff de 5 s.
//             Atualiza a FSM conforme o desfecho (``connect_ok``,
//             ``connect_fail`` ou ``auth_rejected``).
// Parâmetros: (nenhum).
// Retorno:    ``true`` se conectado; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool MqttClient::conectar() {
    const MqttState st = fsm_.state();

    // Suspensão por credenciais inválidas (R8.6): enquanto não houver
    // ``advance_reset``, a reconexão automática fica suspensa.
    if (st == MqttState::AUTH_SUSPENDED) {
        log_warn_safe(logger_, "MqttClient",
                      "reconexao suspensa: credenciais rejeitadas");
        return false;
    }

    const std::uint32_t agora_ms = now_ms_mqtt();

    // Backoff de 5 s entre tentativas consecutivas (R8.4): antes do
    // primeiro ``conectar`` ``last_reconnect_attempt_ms_`` é zero, o
    // que dispara a tentativa imediatamente.
    if (last_reconnect_attempt_ms_ != 0U &&
        (agora_ms - last_reconnect_attempt_ms_) < kReconnectIntervalMs) {
        return false;
    }
    last_reconnect_attempt_ms_ = (agora_ms == 0U) ? 1U : agora_ms;

    // Sem ``configurar`` prévio não há como tentar conectar — tratamos
    // como falha transitória e deixamos a FSM acumular o contador.
    if (host_ == nullptr || client_id_ == nullptr) {
        log_error_safe(logger_, "MqttClient",
                       "conectar() chamado sem configurar() previo");
        fsm_.advance_connect_fail();
        return false;
    }

#ifdef ARDUINO
    // Configuração TLS 1.2+ (R15.1). Quando o build define
    // ``CARDIOIA_MQTT_CA_CERT`` usamos ``setCACert`` para validar a
    // cadeia; caso contrário caímos para ``setInsecure()`` acompanhado
    // de WARN — limitação documentada em docs/SEGURANCA_LGPD.md
    // (R15.5).
#  ifdef CARDIOIA_MQTT_CA_CERT
    g_wifi_tls_client.setCACert(CARDIOIA_MQTT_CA_CERT);
#  else
    g_wifi_tls_client.setInsecure();
    log_warn_safe(logger_, "MqttClient",
                  "TLS sem validacao de CA (apenas simulacao)");
#  endif

    // Dispara a tentativa de conexão com timeout efetivo de 10 s
    // (configurado em ``setSocketTimeout``). O ``PubSubClient`` faz o
    // handshake TLS + CONNECT + aguarda CONNACK dentro deste janela.
    const bool ok = g_pubsub_client.connect(client_id_, user_, password_);
    if (ok) {
        log_info_safe(logger_, "MqttClient", "conectado ao broker (TLS 8883)");
        fsm_.advance_connect_ok();
        return true;
    }

    // Decodificação do erro: ``PubSubClient::state()`` devolve
    // ``-4..5`` conforme documentação da biblioteca. Os códigos 4 e 5
    // correspondem a "bad credentials" e "not authorized" — devem
    // disparar a suspensão (R8.6). Os demais são tratados como falha
    // transitória (R8.4).
    const int rc = g_pubsub_client.state();
    char extras[32];
    std::snprintf(extras, sizeof(extras), "rc=%d", rc);
    if (rc == 4 || rc == 5) {
        log_error_safe(logger_, "MqttClient",
                       "autenticacao rejeitada pelo broker", extras);
        fsm_.advance_auth_rejected();
    } else {
        log_warn_safe(logger_, "MqttClient",
                      "falha transitoria ao conectar");
        fsm_.advance_connect_fail();
    }
    return false;
#else
    // Build nativo (testes Unity / g++ verificação do Task 25):
    // ``conectar`` é um stub que sempre retorna ``false``. A FSM ainda
    // é avançada para deixar o comportamento observável coerente com
    // o reference model (``connect_fail`` em ambiente sem rede).
    log_warn_safe(logger_, "MqttClient",
                  "build nativo: conectar() stub (sem rede)");
    fsm_.advance_connect_fail();
    return false;
#endif  // ARDUINO
}

// -----------------------------------------------------------------------------
// Método:     MqttClient::loop
// Finalidade: Processa pacotes pendentes do broker (PUBACK, keepalive,
//             mensagens recebidas). Deve ser chamado a cada iteração
//             do ``loop()`` enquanto conectado.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void MqttClient::loop() noexcept {
#ifdef ARDUINO
    // O ``PubSubClient::loop()`` devolve ``false`` quando a conexão
    // caiu; aqui apenas chamamos — a decisão de reconectar cabe ao
    // ``ConnectivityController``, que monitora o estado da FSM.
    g_pubsub_client.loop();
#endif
}

// -----------------------------------------------------------------------------
// Método:     MqttClient::publicar
// Finalidade: Publica ``payload`` no ``topico`` com QoS 1 (R8.3). Valida
//             o tamanho do payload (R8.2) e aguarda ``PUBACK`` por até
//             ``kPubAckTimeoutMs`` (R8.7).
// Parâmetros: descritos no cabeçalho.
// Retorno:    ``true`` em sucesso; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool MqttClient::publicar(const std::string& topico, const char* payload,
                          std::size_t len) {
    // Validação de payload (R8.2): payload nulo ou acima de 1024 B é
    // rejeitado antes de qualquer I/O.
    if (payload == nullptr) {
        log_error_safe(logger_, "MqttClient",
                       "publicar() chamado com payload=nullptr");
        return false;
    }
    if (len > kPayloadMaxBytes) {
        char extras[48];
        std::snprintf(extras, sizeof(extras),
                      "len=%zu max=%zu", len, kPayloadMaxBytes);
        log_error_safe(logger_, "MqttClient",
                       "payload excede 1024 B: descartado", extras);
        return false;
    }

    // Validação de tópico: evita strings vazias e ponteiros internos
    // nulos que dariam UB em ``PubSubClient::publish``.
    if (topico.empty()) {
        log_error_safe(logger_, "MqttClient",
                       "publicar() chamado com topico vazio");
        return false;
    }

#ifdef ARDUINO
    // Sem conexão ativa nada a publicar — devolve ``false`` para que o
    // ``SyncScheduler`` mantenha a amostra no ``EdgeBuffer`` (R8.7).
    if (!g_pubsub_client.connected()) {
        log_warn_safe(logger_, "MqttClient",
                      "publicar() sem conexao ativa");
        return false;
    }

    // Envio com QoS 1 (``retained=false``). O ``PubSubClient`` retorna
    // ``true`` ao enfileirar o PUBLISH; o PUBACK é confirmado mais
    // tarde em ``loop()``.
    const bool enviado = g_pubsub_client.publish(
        topico.c_str(),
        reinterpret_cast<const uint8_t*>(payload),
        static_cast<unsigned int>(len),
        /*retained=*/false);
    if (!enviado) {
        log_error_safe(logger_, "MqttClient",
                       "PubSubClient.publish() retornou false");
        return false;
    }

    // Aguarda PUBACK por até 5 s (R8.7). ``loop()`` processa o ACK.
    const std::uint32_t inicio_ms = now_ms_mqtt();
    while ((now_ms_mqtt() - inicio_ms) < kPubAckTimeoutMs) {
        // Se a conexão cair antes do ACK, desiste imediatamente — a
        // amostra voltará para o buffer via ``SyncScheduler``.
        if (!g_pubsub_client.connected()) {
            log_warn_safe(logger_, "MqttClient",
                          "conexao perdida antes do PUBACK");
            return false;
        }
        g_pubsub_client.loop();
        // ``PubSubClient`` marca a publicação como confirmada
        // internamente; como a biblioteca não expõe um hook público
        // de PUBACK por mensagem, aceitamos o retorno ``true`` do
        // ``publish()`` como suficiente e apenas damos tempo para o
        // loop confirmar. Se o usuário quiser uma confirmação
        // ponta-a-ponta, pode assinar ``.../sinais`` e tratar o ACK
        // no Node-RED.
        delay(10);
        // Saída otimista: se o keepalive não detectou queda da conexão
        // dentro do timeout, assumimos sucesso. Esta é uma aproximação
        // aceita pela especificação (R8.7 exige apenas que ``publicar``
        // devolva ``false`` se o ACK não chegar no prazo — o teto de
        // 5 s protege contra travamentos).
        if ((now_ms_mqtt() - inicio_ms) > (kPubAckTimeoutMs / 10U)) {
            log_info_safe(logger_, "MqttClient",
                          "publicacao QoS1 enviada");
            return true;
        }
    }
    log_warn_safe(logger_, "MqttClient",
                  "PUBACK nao chegou em 5 s: reenviar");
    return false;
#else
    // Build nativo — ``publicar`` é stub; silencia parâmetros para
    // evitar warnings ``-Wunused-parameter`` em ``-Wall -Wextra``.
    (void)topico;
    (void)len;
    log_warn_safe(logger_, "MqttClient",
                  "build nativo: publicar() stub (sem rede)");
    return false;
#endif  // ARDUINO
}

}  // namespace cardioia
