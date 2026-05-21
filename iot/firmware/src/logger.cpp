// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/logger.cpp
// Finalidade:
//   Implementação do wrapper ``cardioia::Logger`` declarado em
//   ``logger.h``. A implementação utiliza exclusivamente ``<cstdio>`` e
//   ``<cstdint>`` para o caminho quente (``snprintf``), evitando por
//   completo a classe ``String`` do Arduino — requisito explícito do
//   task 19 para não fragmentar o heap do ESP32.
//
//   Estratégia de build dual (produção × testes):
//     * Quando a macro ``ARDUINO`` está definida (build PlatformIO /
//       Arduino IDE), o ``SerialLoggerSink`` encaminha para
//       ``Serial.println`` e o *timestamp* vem de ``millis()``.
//     * Em builds nativos Unity (env ``native``), ``ARDUINO`` não está
//       definida; o sink cai para ``stdout`` e o *timestamp* passa a ser
//       zero por padrão, podendo ser substituído por um ponteiro de
//       função injetado pelo teste.
//
//   Requisitos atendidos: R12.2, R1.3, R1.4, R1.5 e R5.4 — ver
//   ``logger.h`` para detalhes.
// =============================================================================

#include "logger.h"

#include <cstdio>   // std::snprintf, std::fputs, std::fputc, stdout
#include <cstdint>  // std::uint32_t

#ifdef ARDUINO
#  include <Arduino.h>  // Serial, millis()
#endif

namespace cardioia {

// ---------------------------------------------------------------------------
// Fonte de *timestamp* portátil
// ---------------------------------------------------------------------------
// No ESP32 (``ARDUINO`` definido) a origem é ``millis()`` — uptime em
// milissegundos desde o boot. Em builds nativos expomos um ponteiro de
// função substituível para que testes Unity possam injetar timestamps
// determinísticos sem depender de ``millis()``.

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Função:     now_ms
// Finalidade: Retorna o uptime em milissegundos desde o boot do ESP32,
//             usado como prefixo ``[T+<ms>]`` do formato de log.
// Parâmetros: (nenhum).
// Retorno:    valor não negativo de ``std::uint32_t`` com o uptime atual.
// -----------------------------------------------------------------------------
static std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(millis());
}

#else  // !ARDUINO — build nativo (testes Unity).

// -----------------------------------------------------------------------------
// Função:     now_ms_default
// Finalidade: Implementação padrão de ``now_ms`` para o caminho nativo.
//             Retorna zero para que os testes Unity tenham linhas
//             determinísticas até injetarem a sua própria função de
//             timestamp via ``set_now_ms``.
// Parâmetros: (nenhum).
// Retorno:    sempre ``0``.
// -----------------------------------------------------------------------------
static std::uint32_t now_ms_default() {
    return 0u;
}

// Ponteiro de função substituível — inicializado com o default acima.
// Testes Unity podem reatribuí-lo (via uma API utilitária não coberta
// por este arquivo) sem recompilar o Logger.
static std::uint32_t (*now_ms)() = &now_ms_default;

#endif  // ARDUINO

// ---------------------------------------------------------------------------
// Implementação de ``SerialLoggerSink``
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     SerialLoggerSink::emit
// Finalidade: Entrega a linha ao destino físico — ``Serial`` no ESP32 ou
//             ``stdout`` no build nativo.
// Parâmetros:
//   - ``line``: string C terminada em ``\0`` já formatada pelo
//               ``Logger`` (sem ``\n`` final).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void SerialLoggerSink::emit(const char* line) {
#ifdef ARDUINO
    // Em produção, ``Serial.println`` adiciona ``\r\n`` ao final, o que
    // facilita a leitura no Monitor Serial do Wokwi / PlatformIO.
    Serial.println(line);
#else
    // Em testes nativos caímos para ``stdout``. Adicionamos ``\n``
    // manualmente para manter o comportamento de "uma linha por emissão".
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
#endif
}

// ---------------------------------------------------------------------------
// Utilitário: tradução ``LogLevel`` → string curta
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     Logger::levelStr (privado, estático)
// Finalidade: Mapeia o enum ``LogLevel`` em um literal curto usado na
//             formatação da linha (``[...][NIVEL] ...``).
// Parâmetros:
//   - ``level``: valor do enum a ser traduzido.
// Retorno:    ponteiro para literal estático; nunca ``nullptr``. Caso o
//             valor do enum seja desconhecido (cenário defensivo),
//             retornamos ``"UNKN"`` para sinalizar o erro sem quebrar o
//             formato.
// -----------------------------------------------------------------------------
const char* Logger::levelStr(LogLevel level) {
    switch (level) {
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKN";
}

// ---------------------------------------------------------------------------
// Construtor e métodos públicos
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Construtor: Logger::Logger
// Finalidade: Amarra a instância ao ``ILoggerSink`` recebido por
//             referência; o ``Logger`` não assume posse do sink.
// Parâmetros:
//   - ``sink``: destino onde as linhas serão escritas.
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
Logger::Logger(ILoggerSink& sink) : sink_(sink) {}

// -----------------------------------------------------------------------------
// Método:     Logger::info
// Finalidade: Atalho para ``event(LogLevel::INFO, ...)`` sem extras.
// Parâmetros: descritos em ``logger.h``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void Logger::info(const char* componente, const char* mensagem) {
    emitFormatted(LogLevel::INFO, componente, mensagem, nullptr);
}

// -----------------------------------------------------------------------------
// Método:     Logger::warn
// Finalidade: Atalho para ``event(LogLevel::WARN, ...)`` sem extras.
// Parâmetros: descritos em ``logger.h``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void Logger::warn(const char* componente, const char* mensagem) {
    emitFormatted(LogLevel::WARN, componente, mensagem, nullptr);
}

// -----------------------------------------------------------------------------
// Método:     Logger::error
// Finalidade: Atalho para ``event(LogLevel::ERROR, ...)`` sem extras.
// Parâmetros: descritos em ``logger.h``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void Logger::error(const char* componente, const char* mensagem) {
    emitFormatted(LogLevel::ERROR, componente, mensagem, nullptr);
}

// -----------------------------------------------------------------------------
// Método:     Logger::event
// Finalidade: Versão estendida que permite escolher o ``LogLevel`` e
//             anexar uma string opcional de ``extras`` entre parênteses.
// Parâmetros: descritos em ``logger.h``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void Logger::event(LogLevel level, const char* componente, const char* mensagem,
                   const char* extras) {
    emitFormatted(level, componente, mensagem, extras);
}

// ---------------------------------------------------------------------------
// Núcleo de formatação
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     Logger::emitFormatted (privado)
// Finalidade: Monta, em buffer estático de 256 bytes, a linha no formato
//             ``[T+<ms>][COMPONENTE][NIVEL] mensagem`` (ou ``... (extras)``
//             quando ``extras != nullptr``) e repassa ao ``ILoggerSink``.
//             Nunca aloca memória em heap — ``snprintf`` trunca
//             silenciosamente caso a mensagem fornecida ultrapasse o
//             buffer, preservando o terminador ``\0``.
// Parâmetros:
//   - ``level``:      nível da mensagem.
//   - ``componente``: identificador textual do emissor (``nullptr`` vira
//                     ``"-"``).
//   - ``mensagem``:   descrição curta (``nullptr`` vira string vazia).
//   - ``extras``:     string opcional entre parênteses; ``nullptr``
//                     suprime o bloco.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void Logger::emitFormatted(LogLevel level, const char* componente,
                           const char* mensagem, const char* extras) {
    char buf[kLineBufferSize];
    const std::uint32_t ts = now_ms();

    // Normaliza ponteiros nulos para evitar *undefined behavior* ao
    // passá-los para ``%s`` — ``snprintf`` com ``nullptr`` e ``%s`` é UB.
    const char* componente_safe = (componente != nullptr) ? componente : "-";
    const char* mensagem_safe   = (mensagem   != nullptr) ? mensagem   : "";

    if (extras == nullptr) {
        // Caminho padrão: três colchetes + mensagem livre.
        std::snprintf(buf, static_cast<std::size_t>(kLineBufferSize),
                      "[T+%lu][%s][%s] %s",
                      static_cast<unsigned long>(ts),
                      componente_safe,
                      levelStr(level),
                      mensagem_safe);
    } else {
        // Caminho estendido: anexa ``(extras)`` ao final da linha.
        std::snprintf(buf, static_cast<std::size_t>(kLineBufferSize),
                      "[T+%lu][%s][%s] %s (%s)",
                      static_cast<unsigned long>(ts),
                      componente_safe,
                      levelStr(level),
                      mensagem_safe,
                      extras);
    }

    // ``snprintf`` garante terminação com ``\0`` desde que o tamanho
    // informado seja > 0 — condição satisfeita por ``kLineBufferSize``.
    sink_.emit(buf);
}

}  // namespace cardioia
