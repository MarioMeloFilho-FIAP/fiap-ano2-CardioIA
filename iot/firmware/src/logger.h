// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/logger.h
// Finalidade:
//   Declara o wrapper ``cardioia::Logger``, usado por todos os módulos do
//   firmware (``SensorDriver``, ``EdgeBuffer``, ``ConnectivityController``,
//   ``MqttClient``, ``SyncScheduler``, ``ConfigManager``) para emitir
//   mensagens estruturadas pelo Monitor Serial. A interface abstrata
//   ``ILoggerSink`` permite substituir ``Serial`` por um destino *fake*
//   em testes nativos Unity (env PlatformIO ``native``) sem arrastar
//   ``Arduino.h``.
//
//   Formato das linhas emitidas (compatível com R12.2):
//
//       [T+<uptime_ms>][COMPONENTE][NIVEL] descrição (extras...)
//
//   Observação pragmática: o ESP32, na ausência de RTC e de sincronização
//   NTP (caminho mais comum no Wokwi), não dispõe de relógio absoluto.
//   Portanto o prefixo é um timestamp de uptime ``[T+<ms>]`` derivado de
//   ``millis()``. Quando, em uma iteração futura, o projeto sincronizar
//   tempo via NTP/MQTT, basta trocar a função interna de *timestamp* por
//   uma que produza ``[YYYY-MM-DDThh:mm:ss.mmm]`` — o restante do formato
//   permanece idêntico.
//
//   Requisitos atendidos:
//     * R12.2 — Cada função pública carrega um bloco de comentário em
//               português brasileiro descrevendo parâmetros e retorno.
//     * R1.3, R1.4, R1.5 — Fornece canal padronizado para registrar
//               leituras inválidas do DHT22, falhas consecutivas e demais
//               eventos exigidos pelos requisitos do ``SensorDriver``.
//     * R5.4    — Expõe o mesmo canal para registrar descartes/aviso de
//               80% emitidos pelo ``EdgeBuffer``.
// =============================================================================

#ifndef CARDIOIA_LOGGER_H
#define CARDIOIA_LOGGER_H

#include <cstddef>  // std::size_t
#include <cstdint>  // std::uint32_t — sem dependência de Arduino.h

namespace cardioia {

// ---------------------------------------------------------------------------
// Enumeração de níveis aceitos pelo Logger.
// ---------------------------------------------------------------------------
// Os três níveis são suficientes para os requisitos atuais do firmware e
// mapeiam 1:1 com os métodos ``info()``, ``warn()`` e ``error()``.
enum class LogLevel {
    INFO,   // Eventos normais do ciclo de vida (boot, publicação ok, etc.).
    WARN,   // Situações anômalas, mas tratadas (ex.: buffer >= 80%).
    ERROR   // Falhas que requerem atenção (ex.: erro de I/O em SPIFFS).
};

// ---------------------------------------------------------------------------
// Interface ``ILoggerSink``
// ---------------------------------------------------------------------------
// Destino abstrato para onde o ``Logger`` envia as linhas já formatadas.
// Produz um ponto de extensão para testes nativos Unity (``native``) que
// capturam a saída em um buffer de verificação, sem depender de
// ``Serial``.
class ILoggerSink {
public:
    virtual ~ILoggerSink() = default;

    // -----------------------------------------------------------------------
    // Método:     ILoggerSink::emit
    // Finalidade: Entrega ao destino final a linha já formatada pelo
    //             ``Logger``. A quebra de linha é responsabilidade da
    //             implementação concreta (ex.: ``Serial.println`` já
    //             adiciona ``\r\n``; em testes nativos adicionamos
    //             ``\n`` manualmente).
    // Parâmetros:
    //   - ``line``: ponteiro para string C terminada em ``\0`` contendo a
    //               linha de log. Nunca ``nullptr`` na chamada feita pelo
    //               ``Logger``.
    // Retorno:    (void) — o ``Logger`` não consome retorno do sink.
    // -----------------------------------------------------------------------
    virtual void emit(const char* line) = 0;
};

// ---------------------------------------------------------------------------
// Implementação de produção: encaminha o log para ``Serial`` do ESP32.
// ---------------------------------------------------------------------------
// Compila como um *stub* em builds nativos (``#ifndef ARDUINO``), onde
// escreve em ``stdout`` apenas para facilitar diagnóstico durante a
// execução dos testes Unity.
class SerialLoggerSink : public ILoggerSink {
public:
    // -----------------------------------------------------------------------
    // Método:     SerialLoggerSink::emit
    // Finalidade: Em builds Arduino/PlatformIO (``ARDUINO`` definido),
    //             encaminha a linha para ``Serial.println``. Em builds
    //             nativos cai para ``stdout`` a fim de permitir inspeção
    //             manual durante os testes Unity — sem alterar a
    //             semântica do formato.
    // Parâmetros:
    //   - ``line``: string C com a linha já formatada (sem ``\n`` final).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void emit(const char* line) override;
};

// ---------------------------------------------------------------------------
// Classe ``Logger``
// ---------------------------------------------------------------------------
// Wrapper fino sobre um ``ILoggerSink`` que formata cada linha com
// ``snprintf`` em um buffer estático de ``kLineBufferSize`` bytes. O
// Logger NÃO usa a classe ``String`` do Arduino em nenhum ponto, para
// evitar fragmentação de heap no ESP32 — em conformidade com o briefing
// do task 19.
class Logger {
public:
    // -----------------------------------------------------------------------
    // Construtor:  Logger::Logger
    // Finalidade:  Mantém referência a um ``ILoggerSink`` previamente
    //              construído pelo chamador (padrão *dependency
    //              injection*). O ``Logger`` não possui a posse do sink.
    // Parâmetros:
    //   - ``sink``: destino das mensagens. Deve viver pelo menos tanto
    //               quanto o próprio ``Logger``.
    // Retorno:     (construtor, sem retorno).
    // -----------------------------------------------------------------------
    explicit Logger(ILoggerSink& sink);

    // -----------------------------------------------------------------------
    // Método:     Logger::info
    // Finalidade: Emite uma linha de log no nível ``INFO`` — usada para
    //             eventos normais do ciclo de vida (boot, publicação MQTT
    //             confirmada, carregamento de configuração).
    // Parâmetros:
    //   - ``componente``: identificador textual do emissor
    //                     (ex.: ``"DHT22_Sensor"``, ``"EdgeBuffer"``). Se
    //                     ``nullptr``, é substituído por ``"-"``.
    //   - ``mensagem``:   descrição curta do evento; ``nullptr`` é tratado
    //                     como string vazia para evitar crash.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void info(const char* componente, const char* mensagem);

    // -----------------------------------------------------------------------
    // Método:     Logger::warn
    // Finalidade: Emite uma linha de log no nível ``WARN`` — usada para
    //             situações anômalas mas tratadas (ex.: buffer atingindo
    //             80 % da capacidade, leitura do DHT22 fora de faixa).
    // Parâmetros:  idem ``info``.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void warn(const char* componente, const char* mensagem);

    // -----------------------------------------------------------------------
    // Método:     Logger::error
    // Finalidade: Emite uma linha de log no nível ``ERROR`` — usada para
    //             falhas que exigem atenção (erro de I/O em SPIFFS,
    //             autenticação MQTT rejeitada, descarte por limite de
    //             buffer).
    // Parâmetros:  idem ``info``.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void error(const char* componente, const char* mensagem);

    // -----------------------------------------------------------------------
    // Método:     Logger::event
    // Finalidade: Versão estendida dos métodos acima: permite informar
    //             explicitamente o ``LogLevel`` e anexar uma string de
    //             ``extras`` (ex.: valores rejeitados, códigos, métricas)
    //             entre parênteses no final da linha.
    // Parâmetros:
    //   - ``level``:      nível da mensagem (``INFO``/``WARN``/``ERROR``).
    //   - ``componente``: identificador textual do emissor.
    //   - ``mensagem``:   descrição curta do evento.
    //   - ``extras``:     string opcional adicionada entre parênteses ao
    //                     final da linha; pode ser ``nullptr`` para omitir
    //                     o bloco de extras.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void event(LogLevel level, const char* componente, const char* mensagem,
               const char* extras = nullptr);

private:
    ILoggerSink& sink_;

    // Tamanho máximo da linha formatada. O valor de 256 atende aos
    // requisitos descritos no briefing do task 19 e é igual ao limite de
    // payload JSON (``CARDIOIA_JSON_MAX``), o que simplifica a depuração
    // quando alguém loga um payload inteiro.
    static constexpr int kLineBufferSize = 256;

    // -----------------------------------------------------------------------
    // Método:     Logger::emitFormatted (privado)
    // Finalidade: Núcleo comum aos métodos públicos: monta a linha final
    //             com ``snprintf`` em buffer estático de 256 bytes e
    //             delega a entrega ao ``ILoggerSink``. Graças ao uso
    //             exclusivo de ``snprintf``, o ``Logger`` nunca aloca em
    //             heap em tempo de execução.
    // Parâmetros:
    //   - ``level``:      nível da mensagem.
    //   - ``componente``: identificador textual do emissor (pode ser
    //                     ``nullptr``; substituído por ``"-"``).
    //   - ``mensagem``:   descrição curta do evento (``nullptr`` vira
    //                     string vazia).
    //   - ``extras``:     string opcional (pode ser ``nullptr``).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void emitFormatted(LogLevel level, const char* componente,
                       const char* mensagem, const char* extras);

    // -----------------------------------------------------------------------
    // Método:     Logger::levelStr (privado, estático)
    // Finalidade: Converte um ``LogLevel`` na string C correspondente
    //             (``"INFO"``, ``"WARN"``, ``"ERROR"``). Usado apenas por
    //             ``emitFormatted``.
    // Parâmetros:
    //   - ``level``: valor do enum.
    // Retorno:    ponteiro para literal estático (``const char*``); nunca
    //             ``nullptr``.
    // -----------------------------------------------------------------------
    static const char* levelStr(LogLevel level);
};

}  // namespace cardioia

#endif  // CARDIOIA_LOGGER_H
