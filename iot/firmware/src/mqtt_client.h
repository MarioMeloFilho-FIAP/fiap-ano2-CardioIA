// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/mqtt_client.h
// Finalidade:
//   Declara o módulo ``cardioia::MqttClient`` — wrapper fino sobre
//   ``WiFiClientSecure`` (TLS 1.2+) e ``PubSubClient`` (MQTT 3.1.1) que
//   encapsula, em um único ponto, toda a lógica de conexão, publicação
//   QoS 1 e reconexão com HiveMQ Cloud exigida pelo Requisito 8 da
//   especificação. O cabeçalho também expõe duas peças puras usadas
//   pelos testes de propriedade (PBT) em ``iot/tests``:
//
//     * ``cardioia::MqttReconnectFSM`` — espelha, em C++ 17, a
//       ``MqttReconnectFSM`` do reference model Python
//       (``iot/tests/reference_model.py``) e satisfaz a **Property 14**
//       (R8.4, R8.6, R7.5).
//     * ``cardioia::topico_telemetria`` — constrói o tópico canônico
//       ``"cardioia/paciente/<paciente_id>/sinais"`` e satisfaz a
//       **Property 15** (R8.2, validação ``[A-Za-z0-9_-]{1,32}``).
//
//   Estratégia de build dual (produção × testes):
//     * Em builds Arduino/PlatformIO (macro ``ARDUINO`` definida), o
//       ``MqttClient`` arrasta ``WiFiClientSecure.h`` e ``PubSubClient.h``,
//       configura TLS 1.2+ na porta 8883 (R15.1, R8.1) e executa as
//       chamadas reais de ``connect()`` / ``publish()`` contra o broker.
//     * Em builds nativos Unity (env PlatformIO ``native``), ``ARDUINO``
//       está indefinido; ``conectar()`` e ``publicar()`` viram *stubs*
//       que retornam ``false`` sem efeitos colaterais — o suficiente
//       para que os testes de propriedade exercitem a FSM e o tópico
//       sem dependência de rede, Wi-Fi ou ``Arduino.h``.
//
//   Requisitos atendidos:
//     * R8.1 — porta TLS 8883, ``keepalive`` 60 s, timeout de conexão
//              10 s (``kConnectTimeoutMs``).
//     * R8.2 — payload ≤ 1024 bytes e tópico canônico produzido por
//              ``topico_telemetria``.
//     * R8.3 — ``publicar`` envia com QoS 1 (``at least once``).
//     * R8.4 — FSM de reconexão: 3 falhas consecutivas marcam
//              ``OFFLINE`` (``kMaxConsecutiveFailures``); intervalo de
//              5 s entre tentativas controlado por
//              ``kReconnectIntervalMs``.
//     * R8.5 — usuário/senha lidos de ``secrets.h`` no caminho
//              Arduino; timeout de 10 s para resposta do broker.
//     * R8.6 — estado ``AUTH_SUSPENDED`` ao receber ``auth_rejected``;
//              reconexões automáticas ficam suspensas até um evento
//              externo de ``reset``.
//     * R8.7 — ``publicar`` retorna ``false`` se ``PUBACK`` não chegar
//              em 5 s (``kPubAckTimeoutMs``); o ``SyncScheduler`` é o
//              responsável por devolver a amostra ao ``EdgeBuffer``.
//     * R15.1 — conexão com o broker sempre em TLS 1.2+.
//     * R12.2, R12.4 — cada função pública carrega bloco de comentário
//              em pt-BR e cada ``if``/``while`` de decisão sobre
//              conexão ou publicação tem comentário adjacente.
// =============================================================================

#ifndef CARDIOIA_MQTT_CLIENT_H
#define CARDIOIA_MQTT_CLIENT_H

#include <cstddef>  // std::size_t
#include <cstdint>  // std::uint16_t, std::uint32_t
#include <string>   // std::string — usado apenas em interfaces puras (FSM/tópico).

#include "logger.h"

namespace cardioia {

// ---------------------------------------------------------------------------
// Enumeração ``MqttState``
// ---------------------------------------------------------------------------
// Espelho exato do enum Python ``MqttState`` em
// ``iot/tests/reference_model.py``. A ordem dos valores é estável e faz
// parte do contrato PBT da Property 14.
enum class MqttState {
    ONLINE,          // Conectado ao broker; ``publicar`` pode ser chamado.
    OFFLINE,         // Desconectado; reconexões automáticas em curso.
    AUTH_SUSPENDED   // Broker rejeitou credenciais; suspende reconexões
                     // automáticas até intervenção externa (R8.6).
};

// ---------------------------------------------------------------------------
// Classe ``MqttReconnectFSM`` (pura, sem I/O)
// ---------------------------------------------------------------------------
// FSM determinística de reconexão MQTT — espelha bit-a-bit a classe
// homônima do reference model Python. Não depende de ``Arduino.h`` nem
// de ``PubSubClient``, o que permite exercitá-la tanto pelos testes
// nativos Unity quanto indiretamente pelos testes de propriedade em
// ``iot/tests/test_sync_and_mqtt_pbt.py``.
//
// Transições aceitas (alinhadas à Property 14):
//   * ``ONLINE + connect_fail``  → fail_count += 1; se ≥ 3 → OFFLINE.
//   * ``OFFLINE + connect_fail`` → idem, permanece em OFFLINE.
//   * ``* + connect_ok`` (exceto ``AUTH_SUSPENDED``) → ``ONLINE``,
//     fail_count = 0.
//   * ``AUTH_SUSPENDED + connect_ok`` → evento ignorado (R8.6).
//   * ``* + auth_rejected`` → ``AUTH_SUSPENDED``, fail_count = 0.
//   * ``AUTH_SUSPENDED + reset`` → ``OFFLINE``, fail_count = 0.
//   * ``* + flag_true`` → fail_count = 0 (estado preservado; R7.5).
class MqttReconnectFSM {
public:
    // Limite de falhas consecutivas antes de marcar ``OFFLINE`` (R8.4).
    // Valor idêntico a ``MqttReconnectFSM.MAX_CONSECUTIVE_FAILURES`` do
    // reference model.
    static constexpr int kMaxConsecutiveFailures = 3;

    // -----------------------------------------------------------------------
    // Construtor: MqttReconnectFSM::MqttReconnectFSM
    // Finalidade: Inicializa a FSM em ``OFFLINE`` com ``fail_count = 0``.
    //             Ponto de partida padrão no boot do firmware — antes da
    //             primeira tentativa de ``conectar()``.
    // Parâmetros: (nenhum).
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    MqttReconnectFSM() noexcept = default;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::advance_connect_ok
    // Finalidade: Aplica o evento ``connect_ok`` — ocorre quando
    //             ``PubSubClient::connect(...)`` retorna ``true``.
    //             Transita para ``ONLINE`` e zera ``fail_count``, exceto
    //             quando a FSM está em ``AUTH_SUSPENDED`` (R8.6: broker
    //             rejeitou credenciais; novas conexões são ignoradas).
    // Parâmetros: (nenhum).
    // Retorno:    novo ``MqttState``.
    // -----------------------------------------------------------------------
    MqttState advance_connect_ok() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::advance_connect_fail
    // Finalidade: Aplica o evento ``connect_fail`` — ocorre quando a
    //             tentativa de ``connect`` expira ou retorna erro
    //             transitório. Incrementa ``fail_count``; ao atingir
    //             ``kMaxConsecutiveFailures`` transita para ``OFFLINE``
    //             (R8.4). Ignorado quando a FSM já está em
    //             ``AUTH_SUSPENDED``.
    // Parâmetros: (nenhum).
    // Retorno:    novo ``MqttState``.
    // -----------------------------------------------------------------------
    MqttState advance_connect_fail() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::advance_auth_rejected
    // Finalidade: Aplica o evento ``auth_rejected`` — broker devolveu
    //             código de rejeição de credenciais (``rc == 4``/``5``
    //             no PubSubClient). Transita para ``AUTH_SUSPENDED`` e
    //             zera ``fail_count`` (R8.6). Novas reconexões ficam
    //             suspensas até ``advance_reset()``.
    // Parâmetros: (nenhum).
    // Retorno:    novo ``MqttState`` (sempre ``AUTH_SUSPENDED``).
    // -----------------------------------------------------------------------
    MqttState advance_auth_rejected() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::advance_flag_true
    // Finalidade: Aplica o evento ``flag_true`` — disparado pelo
    //             ``ConnectivityController`` ao observar
    //             ``Connectivity_Flag`` transitando para ``true`` (R7.5).
    //             Apenas zera ``fail_count``; o estado é preservado.
    // Parâmetros: (nenhum).
    // Retorno:    ``MqttState`` atual (inalterado).
    // -----------------------------------------------------------------------
    MqttState advance_flag_true() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::advance_reset
    // Finalidade: Aplica o evento ``reset`` — intervenção externa
    //             (novo commit, deploy manual, comando Serial) que tira
    //             a FSM de ``AUTH_SUSPENDED``. Em ``AUTH_SUSPENDED``
    //             transita para ``OFFLINE`` e zera ``fail_count``; em
    //             qualquer outro estado é no-op.
    // Parâmetros: (nenhum).
    // Retorno:    novo ``MqttState``.
    // -----------------------------------------------------------------------
    MqttState advance_reset() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::state
    // Finalidade: Getter do estado atual — usado pelo ``SyncScheduler``
    //             para decidir se pode iniciar o replay do ``EdgeBuffer``.
    // Parâmetros: (nenhum).
    // Retorno:    ``MqttState`` corrente.
    // -----------------------------------------------------------------------
    MqttState state() const noexcept { return state_; }

    // -----------------------------------------------------------------------
    // Método:     MqttReconnectFSM::fail_count
    // Finalidade: Getter do contador de falhas consecutivas — exposto
    //             apenas para diagnóstico e testes.
    // Parâmetros: (nenhum).
    // Retorno:    inteiro em ``[0, kMaxConsecutiveFailures]``.
    // -----------------------------------------------------------------------
    int fail_count() const noexcept { return fail_count_; }

private:
    MqttState state_ = MqttState::OFFLINE;
    int fail_count_ = 0;
};

// ---------------------------------------------------------------------------
// Função livre ``topico_telemetria`` (pura)
// ---------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Função:     topico_telemetria
// Finalidade: Constrói o tópico MQTT canônico de telemetria de um
//             paciente (Property 15 / R8.2). A string produzida é
//             ``"cardioia/paciente/<paciente_id>/sinais"``.
// Parâmetros:
//   - ``paciente_id``: identificador anonimizado do paciente. Deve casar
//                      com o padrão ``^[A-Za-z0-9_-]{1,32}$``; a
//                      conformidade adicional com LGPD
//                      (``^PAC-\\d{1,27}$``) é responsabilidade do
//                      chamador via ``is_anonymized_paciente_id``.
// Retorno:    string C++ com o tópico canônico.
// Exceções:   lança ``std::invalid_argument`` se ``paciente_id`` não
//             casa com o padrão permitido.
// -----------------------------------------------------------------------------
std::string topico_telemetria(const std::string& paciente_id);

// ---------------------------------------------------------------------------
// Classe ``MqttClient``
// ---------------------------------------------------------------------------
// Wrapper do cliente MQTT real. Centraliza:
//   1. Configuração de ``WiFiClientSecure`` (TLS 1.2+, R15.1) e
//      ``PubSubClient`` (keepalive 60 s, buffer de payload).
//   2. Ciclo de reconexão regido pela ``MqttReconnectFSM``.
//   3. Publicação com QoS 1 e espera por ``PUBACK`` dentro do timeout de
//      5 s (R8.7).
//
// A classe é intencionalmente simples: o ``SyncScheduler`` decide
// *quando* chamar ``publicar()`` e o ``ConnectivityController`` decide
// *quando* chamar ``conectar()``. O ``MqttClient`` apenas executa e
// comunica resultado via ``bool``.
class MqttClient {
public:
    // Intervalo mínimo, em ms, entre duas tentativas de reconexão (R8.4).
    static constexpr std::uint32_t kReconnectIntervalMs = 5000U;

    // Timeout, em ms, aguardando resposta do broker durante ``connect``
    // (R8.1, R8.5).
    static constexpr std::uint32_t kConnectTimeoutMs = 10000U;

    // Timeout, em ms, aguardando ``PUBACK`` após uma publicação QoS 1
    // (R8.7).
    static constexpr std::uint32_t kPubAckTimeoutMs = 5000U;

    // Keepalive, em segundos, enviado ao broker no ``connect`` (R8.1).
    static constexpr std::uint16_t kKeepaliveSeconds = 60U;

    // Porta TLS padrão do HiveMQ Cloud (R8.1, R15.1).
    static constexpr std::uint16_t kDefaultTlsPort = 8883U;

    // Tamanho máximo, em bytes, do payload aceito por ``publicar`` (R8.2).
    // Mesmo valor de ``MQTT_PAYLOAD_MAX`` em ``config.h`` e no reference
    // model Python (``iot/tests/reference_model.py``).
    static constexpr std::size_t kPayloadMaxBytes = 1024U;

    // -----------------------------------------------------------------------
    // Construtor: MqttClient::MqttClient
    // Finalidade: Instancia o wrapper com um ponteiro opcional para o
    //             ``Logger`` compartilhado pelo firmware. A classe não
    //             assume posse do logger; ele deve viver pelo menos
    //             tanto quanto o próprio ``MqttClient``.
    // Parâmetros:
    //   - ``logger`` (default ``nullptr``): destino dos eventos de
    //                conexão, publicação, falhas e transições da FSM.
    //                Quando ``nullptr``, as chamadas de log viram no-op.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit MqttClient(Logger* logger = nullptr) noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttClient::configurar
    // Finalidade: Registra host/porta/credenciais/``client_id`` que
    //             serão usados pelas próximas chamadas a ``conectar()``.
    //             Tipicamente invocado uma única vez no ``setup()`` do
    //             firmware com os valores lidos de ``secrets.h``.
    //             Os ponteiros são armazenados por referência (não há
    //             cópia interna); o chamador deve garantir que as
    //             strings permaneçam válidas durante todo o ciclo de
    //             vida do ``MqttClient``.
    // Parâmetros:
    //   - ``host``:      hostname do broker (ex.: HiveMQ Cloud).
    //   - ``port``:      porta TLS (padrão 8883).
    //   - ``user``:      usuário MQTT (``MQTT_USER`` em ``secrets.h``).
    //   - ``password``:  senha MQTT (``MQTT_PASSWORD`` em ``secrets.h``).
    //   - ``client_id``: identificador único do dispositivo no broker.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void configurar(const char* host, std::uint16_t port,
                    const char* user, const char* password,
                    const char* client_id) noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttClient::conectar
    // Finalidade: Tenta estabelecer a conexão TLS 1.2+ com o broker e
    //             autenticar com usuário/senha dentro de 10 s (R8.1,
    //             R8.5). Respeita o intervalo mínimo de 5 s entre
    //             tentativas (R8.4) e dispara a transição apropriada na
    //             ``MqttReconnectFSM`` conforme o resultado
    //             (``connect_ok``, ``connect_fail`` ou
    //             ``auth_rejected``).
    //
    //             Em builds nativos (``ARDUINO`` indefinido) este método
    //             é um *stub* que sempre retorna ``false`` sem efeito
    //             colateral — a lógica de reconexão é exercitada
    //             exclusivamente pela FSM pura nos testes.
    // Parâmetros: (nenhum — usa os valores registrados em
    //             ``configurar``).
    // Retorno:    ``true`` se conectado e autenticado; ``false`` caso
    //             contrário.
    // -----------------------------------------------------------------------
    bool conectar();

    // -----------------------------------------------------------------------
    // Método:     MqttClient::loop
    // Finalidade: Processa pacotes pendentes do ``PubSubClient``
    //             (``PUBACK``, keepalive, mensagens retidas). Deve ser
    //             chamado a cada iteração do ``loop()`` principal do
    //             firmware quando a conexão estiver ativa. Em build
    //             nativo é no-op.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void loop() noexcept;

    // -----------------------------------------------------------------------
    // Método:     MqttClient::publicar
    // Finalidade: Publica ``payload`` no ``topico`` com QoS 1 (R8.3).
    //             Valida o tamanho do payload contra
    //             ``kPayloadMaxBytes`` (R8.2) antes de invocar o
    //             broker. Se o ``PUBACK`` não chegar em
    //             ``kPubAckTimeoutMs`` (R8.7) devolve ``false`` sem
    //             lançar exceção; o ``SyncScheduler`` é responsável
    //             por reencaminhar o ``Sample_Record`` ao ``EdgeBuffer``.
    //
    //             Em build nativo é um *stub* que sempre retorna
    //             ``false``.
    // Parâmetros:
    //   - ``topico``:  tópico canônico (use ``topico_telemetria`` para
    //                  amostras).
    //   - ``payload``: bytes do payload (JSON compacto em ``sinais``).
    //   - ``len``:     comprimento do payload; deve ser
    //                  ``<= kPayloadMaxBytes``.
    // Retorno:    ``true`` se ``PUBACK`` chegou dentro do timeout;
    //             ``false`` se payload excede limite, cliente não está
    //             conectado, ``publish`` falhou ou o ``PUBACK`` não
    //             chegou a tempo.
    // -----------------------------------------------------------------------
    bool publicar(const std::string& topico, const char* payload,
                  std::size_t len);

    // -----------------------------------------------------------------------
    // Método:     MqttClient::state
    // Finalidade: Proxy para ``MqttReconnectFSM::state`` — usado por
    //             outros módulos para decidir comportamento sem
    //             acoplamento direto à FSM.
    // Parâmetros: (nenhum).
    // Retorno:    estado atual da FSM de reconexão.
    // -----------------------------------------------------------------------
    MqttState state() const noexcept { return fsm_.state(); }

    // -----------------------------------------------------------------------
    // Método:     MqttClient::fsm
    // Finalidade: Referência mutável à FSM — exposta para permitir que
    //             o ``ConnectivityController`` notifique o evento
    //             ``flag_true`` (R7.5) e que o avaliador realize
    //             ``reset`` manual (R8.6) sem duplicar a máquina de
    //             estados.
    // Parâmetros: (nenhum).
    // Retorno:    referência à ``MqttReconnectFSM`` interna.
    // -----------------------------------------------------------------------
    MqttReconnectFSM& fsm() noexcept { return fsm_; }

private:
    Logger* logger_;                          // canal de log (opcional).
    MqttReconnectFSM fsm_;                    // FSM de reconexão (Property 14).
    const char* host_ = nullptr;              // hostname do broker.
    std::uint16_t port_ = kDefaultTlsPort;    // porta TLS (R8.1).
    const char* user_ = nullptr;              // usuário MQTT (R8.5).
    const char* password_ = nullptr;          // senha MQTT (R8.5).
    const char* client_id_ = nullptr;         // client_id único do device.
    std::uint32_t last_reconnect_attempt_ms_ = 0U;  // último ``conectar()``.
};

}  // namespace cardioia

#endif  // CARDIOIA_MQTT_CLIENT_H
