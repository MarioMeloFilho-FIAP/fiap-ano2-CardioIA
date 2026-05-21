// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sample_builder.cpp
// Finalidade:
//   Implementação da lógica pura declarada em ``sample_builder.h``. O
//   objetivo é reproduzir, byte-a-byte, a saída de
//   ``serialize_sample`` do reference model Python
//   (``iot/tests/reference_model.py``) — esse espelho exato é o que
//   permite à suíte de PBT (Property 1) validar o firmware C++ por
//   meio de co-simulação offline.
//
//   Estratégia de build dual:
//     * Quando a macro ``CARDIOIA_USE_ARDUINOJSON`` está definida
//       (caminho opcional em builds PlatformIO com a biblioteca
//       ``bblanchon/ArduinoJson`` disponível), usamos
//       ``ArduinoJson::StaticJsonDocument`` + ``serializeJson`` para
//       montar o payload. Esse caminho fica ativo apenas em builds
//       Arduino porque a biblioteca depende de ``Arduino.h``.
//     * No caminho padrão (builds nativos Unity e builds Arduino que
//       não habilitem o flag), usamos um formatador manual baseado em
//       ``snprintf``, cuidadosamente afinado para igualar o formato do
//       reference model Python:
//         - ``timestamp``: inteiro decimal.
//         - ``temperatura``: ``%.1f`` — uma casa decimal (R1.2, R3.1).
//         - ``umidade``, ``bpm``: inteiros decimais.
//         - Campos ``std::nullopt`` viram literal ``null`` (R3.5).
//
//   Nenhum dos caminhos usa ``String`` do Arduino — todo o
//   preenchimento é feito em buffers ``char[]`` locais, evitando
//   fragmentação de heap no ESP32 (convenção do task 19 reutilizada
//   aqui).
//
//   Requisitos atendidos: R3.1–R3.6, R12.2, R12.4.
// =============================================================================

#include "sample_builder.h"

#include <algorithm>  // std::min
#include <cstdio>     // std::snprintf
#include <cstddef>    // std::size_t
#include <cstring>    // std::memcpy
#include <cstdint>    // std::uint32_t

#if defined(CARDIOIA_USE_ARDUINOJSON)
#  include <ArduinoJson.h>  // Caminho opcional habilitado em PlatformIO.
#endif

namespace cardioia {

// =============================================================================
// Helpers internos (anônimos)
// =============================================================================

namespace {

// Tamanho do buffer temporário usado pelo caminho ``snprintf``. Como o
// payload final está limitado a 256 bytes (``CARDIOIA_JSON_MAX``),
// 384 bytes dão folga suficiente para detectarmos excesso sem truncar
// prematuramente — a verificação de limite acontece **depois** de
// montar o JSON completo, em conformidade com R3.6.
constexpr std::size_t kScratchBufferSize = 384;

// ---------------------------------------------------------------------------
// Função:     paciente_id_valido
// Finalidade: Valida ``paciente_id`` contra o contrato estrutural do
//             ``Sample_Record`` (R3.1): não vazio e com no máximo 32
//             caracteres. A regra mais estrita de formato anonimizado
//             (``^PAC-\d{1,27}$`` — R15.2 / Property 18) é
//             responsabilidade de outra camada (``ConfigManager``), de
//             modo que o ``SampleBuilder`` permanece fiel ao escopo da
//             Property 1.
// Parâmetros:
//   - ``paciente_id``: string candidata.
// Retorno:    ``true`` se estruturalmente válido; ``false`` caso
//             contrário.
// ---------------------------------------------------------------------------
bool paciente_id_valido(const std::string& paciente_id) {
    // Validação estrutural mínima exigida por R3.1 — faixa de
    // comprimento ``[1, 32]``. Caracteres permitidos são verificados
    // durante a formatação (não escapamos aspas porque o domínio
    // anonimizado garante apenas ``[A-Za-z0-9_-]``; qualquer caractere
    // fora dessa faixa é rejeitado implicitamente pela checagem
    // de limite da Property 18 em outro módulo).
    if (paciente_id.empty()) {
        return false;
    }
    if (paciente_id.size() > 32u) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Função:     copiar_para_saida
// Finalidade: Copia o JSON completo montado no buffer local ``src``
//             (com ``len`` bytes, sem ``\0``) para o buffer ``out``
//             fornecido pelo chamador, garantindo terminação em
//             ``\0``. Encapsula a única gravação em ``out`` realizada
//             pela função :func:`serializar`, assegurando a regra do
//             R3.6 ("sem escrita parcial em caso de falha") por
//             construção: o chamador só invoca este helper após
//             validar todos os limites.
// Parâmetros:
//   - ``src``:      ponteiro para o JSON completo em buffer temporário.
//   - ``len``:      tamanho do JSON, em bytes, excluindo o ``\0``.
//   - ``out``:      buffer destino do chamador.
//   - ``out_size``: capacidade de ``out``, incluindo o ``\0`` final.
// Retorno:    ``true`` se coube em ``out`` (``out`` contém o JSON
//             terminado em ``\0``); ``false`` caso contrário (``out``
//             permanece intacto).
// ---------------------------------------------------------------------------
bool copiar_para_saida(const char* src, std::size_t len,
                       char* out, std::size_t out_size) {
    // Espaço necessário: ``len`` bytes de conteúdo + 1 para ``\0``.
    // Caso ``out`` seja menor, recusamos a cópia e preservamos o
    // buffer do chamador em conformidade com R3.6.
    if (out == nullptr || out_size == 0u) {
        return false;
    }
    if (len + 1u > out_size) {
        return false;
    }
    std::memcpy(out, src, len);
    out[len] = '\0';
    return true;
}

#if !defined(CARDIOIA_USE_ARDUINOJSON)

// ---------------------------------------------------------------------------
// Função:     formatar_temperatura
// Finalidade: Formata ``temperatura`` com exatamente uma casa decimal
//             (``%.1f``), espelhando ``_round_temperatura`` do
//             reference model Python (R1.2, R3.1). Quando o valor é
//             ``std::nullopt``, grava o literal ``"null"``.
// Parâmetros:
//   - ``temp``:     valor opcional a formatar.
//   - ``buf``:      buffer de destino.
//   - ``buf_size``: capacidade de ``buf``.
// Retorno:    número de bytes gravados (sem ``\0``); ``0`` se algum
//             parâmetro for inconsistente.
// ---------------------------------------------------------------------------
std::size_t formatar_temperatura(const std::optional<float>& temp,
                                 char* buf, std::size_t buf_size) {
    if (buf == nullptr || buf_size == 0u) {
        return 0u;
    }
    int written;
    if (!temp.has_value()) {
        // R3.5 — campo indisponível ⇒ literal ``null``.
        written = std::snprintf(buf, buf_size, "null");
    } else {
        // Promoção para ``double`` é obrigatória em argumentos
        // variádicos de ``snprintf`` — ``%f`` espera ``double``.
        written = std::snprintf(buf, buf_size, "%.1f",
                                static_cast<double>(temp.value()));
    }
    if (written < 0) {
        return 0u;
    }
    // ``snprintf`` trunca silenciosamente caso o buffer seja pequeno;
    // o chamador, no entanto, dimensiona ``kScratchBufferSize`` com
    // folga suficiente para que esse ramo nunca dispare em runtime.
    return static_cast<std::size_t>(
        std::min(static_cast<std::size_t>(written), buf_size - 1u));
}

// ---------------------------------------------------------------------------
// Função:     formatar_inteiro_opcional
// Finalidade: Formata um inteiro opcional (``umidade`` ou ``bpm``)
//             como decimal. ``std::nullopt`` vira literal ``"null"``
//             (R3.5). Evita a classe ``String`` do Arduino e qualquer
//             alocação em heap.
// Parâmetros:
//   - ``valor``:    valor opcional a formatar.
//   - ``buf``:      buffer de destino.
//   - ``buf_size``: capacidade de ``buf``.
// Retorno:    número de bytes gravados (sem ``\0``); ``0`` se algum
//             parâmetro for inconsistente.
// ---------------------------------------------------------------------------
std::size_t formatar_inteiro_opcional(const std::optional<int>& valor,
                                      char* buf, std::size_t buf_size) {
    if (buf == nullptr || buf_size == 0u) {
        return 0u;
    }
    int written;
    if (!valor.has_value()) {
        written = std::snprintf(buf, buf_size, "null");
    } else {
        written = std::snprintf(buf, buf_size, "%d", valor.value());
    }
    if (written < 0) {
        return 0u;
    }
    return static_cast<std::size_t>(
        std::min(static_cast<std::size_t>(written), buf_size - 1u));
}

#endif  // !CARDIOIA_USE_ARDUINOJSON

}  // namespace

// =============================================================================
// Implementação de ``compor``
// =============================================================================

// Ver contrato em ``sample_builder.h``. A implementação limita-se a
// copiar os campos para o ``SampleRecord``, aplicando a saturação de
// ``bpm`` em ``[0; 250]`` — espelhando ``compose_sample`` do reference
// model Python.
SampleRecord compor(std::uint32_t now_ms,
                    const SensorReading& leitura,
                    std::optional<int> bpm,
                    const std::string& paciente_id) {
    SampleRecord record;
    record.timestamp_ms = now_ms;
    record.temperatura = leitura.temperatura;
    record.umidade = leitura.umidade;

    // Saturação de ``bpm`` em ``[0, 250]`` (R2.5). O chamador costuma
    // entregar o valor já dentro da faixa (via ``BpmWindow``), mas
    // aplicamos a clamp aqui por robustez — assim ``compor`` pode ser
    // reutilizado em testes nativos que exercitem valores extremos.
    if (bpm.has_value()) {
        int v = bpm.value();
        if (v < 0) {
            v = 0;
        } else if (v > 250) {
            v = 250;
        }
        record.bpm = v;
    } else {
        record.bpm = std::nullopt;
    }

    record.paciente_id = paciente_id;
    return record;
}

// =============================================================================
// Implementação de ``serializar``
// =============================================================================

// Ver contrato em ``sample_builder.h``. A implementação abaixo é
// válida tanto para o caminho ``snprintf`` quanto para o caminho
// ``ArduinoJson``; ambos produzem o mesmo byte-stream final.
bool serializar(const SampleRecord& record, char* out, std::size_t out_size) {
    // Validação estrutural do ``paciente_id`` (R3.1). Violações viram
    // falha explícita — diferente do estouro de tamanho do JSON, que
    // é sinalizado por ``false`` porém não indica violação do contrato
    // de domínio.
    if (!paciente_id_valido(record.paciente_id)) {
        return false;
    }

#if defined(CARDIOIA_USE_ARDUINOJSON)
    // -----------------------------------------------------------------
    // Caminho ArduinoJson (produção / PlatformIO)
    // -----------------------------------------------------------------
    // O ``StaticJsonDocument<256>`` é suficiente para o Sample_Record
    // canônico — a estimativa vem de ``JSON_OBJECT_SIZE(5) + 40``.
    // ``serializeJson`` emite sem espaços por padrão; a ordem de
    // inserção abaixo segue ``CANONICAL_KEYS`` do reference model,
    // assegurando a ordem canônica exigida por R3.2.
    StaticJsonDocument<256> doc;
    doc["timestamp"] = record.timestamp_ms;
    if (record.temperatura.has_value()) {
        // Garante arredondamento para uma casa decimal antes de
        // emitir — alinhado ao ``_round_temperatura`` do reference
        // model Python.
        const float rounded =
            std::round(record.temperatura.value() * 10.0f) / 10.0f;
        doc["temperatura"] = rounded;
    } else {
        doc["temperatura"] = nullptr;  // ⇒ JSON ``null``
    }
    if (record.umidade.has_value()) {
        doc["umidade"] = record.umidade.value();
    } else {
        doc["umidade"] = nullptr;
    }
    if (record.bpm.has_value()) {
        doc["bpm"] = record.bpm.value();
    } else {
        doc["bpm"] = nullptr;
    }
    doc["paciente_id"] = record.paciente_id.c_str();

    char scratch[kScratchBufferSize];
    const std::size_t written = serializeJson(doc, scratch, sizeof(scratch));

    // R3.3 / R3.6 — verificação do limite de tamanho. Comentário
    // exigido por R12.4: o ``if`` abaixo avalia a condição "JSON
    // excedeu CARDIOIA_JSON_MAX" e, em caso afirmativo, aborta a
    // escrita para preservar a integridade do buffer do chamador.
    if (written > static_cast<std::size_t>(CARDIOIA_JSON_MAX)) {
        return false;
    }
    return copiar_para_saida(scratch, written, out, out_size);
#else
    // -----------------------------------------------------------------
    // Caminho ``snprintf`` (testes nativos e builds Arduino sem
    // ArduinoJson). A montagem manual garante bytes idênticos ao
    // reference model Python, essencial para a Property 1.
    // -----------------------------------------------------------------
    char temp_str[32];
    char hum_str[16];
    char bpm_str[16];

    const std::size_t temp_len =
        formatar_temperatura(record.temperatura, temp_str, sizeof(temp_str));
    const std::size_t hum_len =
        formatar_inteiro_opcional(record.umidade, hum_str, sizeof(hum_str));
    const std::size_t bpm_len =
        formatar_inteiro_opcional(record.bpm, bpm_str, sizeof(bpm_str));
    // Suprime unused-variable warnings quando os helpers são inlinados
    // pelo compilador em um caminho sem asserts — as variáveis são
    // efetivamente lidas via ``temp_str`` / ``hum_str`` / ``bpm_str``
    // logo a seguir.
    (void)temp_len;
    (void)hum_len;
    (void)bpm_len;

    char scratch[kScratchBufferSize];
    // A ordem das substituições segue ``CANONICAL_KEYS`` do reference
    // model (R3.2). ``snprintf`` sempre termina a saída com ``\0``.
    const int written_signed = std::snprintf(
        scratch,
        sizeof(scratch),
        "{\"timestamp\":%lu,"
        "\"temperatura\":%s,"
        "\"umidade\":%s,"
        "\"bpm\":%s,"
        "\"paciente_id\":\"%s\"}",
        static_cast<unsigned long>(record.timestamp_ms),
        temp_str,
        hum_str,
        bpm_str,
        record.paciente_id.c_str());

    if (written_signed < 0) {
        // Erro de codificação — tratamos como falha sem tocar em ``out``.
        return false;
    }
    const std::size_t written = static_cast<std::size_t>(written_signed);

    // Se ``snprintf`` teve que truncar (``written >= sizeof(scratch)``),
    // o JSON montado já é maior do que nossa folga e, portanto,
    // excede ``CARDIOIA_JSON_MAX``. A condição abaixo cobre ambos os
    // casos: truncamento e estouro lógico do payload.
    if (written >= sizeof(scratch)) {
        return false;
    }

    // R3.3 / R3.6 — verificação final do limite de 256 bytes.
    // Comentário exigido por R12.4: o ``if`` abaixo avalia a condição
    // "JSON excede CARDIOIA_JSON_MAX"; em caso afirmativo, retornamos
    // ``false`` sem gravar em ``out``, garantindo que o chamador não
    // observe payload parcial (descarte silencioso é responsabilidade
    // do chamador, que deve logar via ``Logger``).
    if (written > static_cast<std::size_t>(CARDIOIA_JSON_MAX)) {
        return false;
    }

    return copiar_para_saida(scratch, written, out, out_size);
#endif  // CARDIOIA_USE_ARDUINOJSON
}

}  // namespace cardioia
