// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/sample_builder.h
// Finalidade:
//   Declara a lógica pura de composição e serialização do
//   ``Sample_Record`` publicado pelo firmware em
//   ``cardioia/paciente/{paciente_id}/sinais``. Espelha, em C++17, as
//   funções ``compose_sample`` e ``serialize_sample`` do reference model
//   Python (``iot/tests/reference_model.py``) usado como oráculo pela
//   suíte de PBT (Property 1).
//
//   Contrato estrutural (R3.1–R3.6 e Property 1):
//     * Ordem canônica das chaves: ``timestamp, temperatura, umidade,
//       bpm, paciente_id``.
//     * JSON compacto — sem espaços entre delimitadores.
//     * Campos indisponíveis (``std::nullopt``) serializam como
//       ``null`` JSON.
//     * Tamanho total do payload ≤ ``CARDIOIA_JSON_MAX`` (256 bytes).
//       Caso contrário, ``serializar`` retorna ``false`` SEM gravar
//       conteúdo parcial em ``out``.
//
//   Estratégia de build dual (produção × testes):
//     * O cabeçalho é 100% portátil — não inclui ``Arduino.h`` nem
//       ``ArduinoJson``. Em builds PlatformIO (``env:esp32dev``) pode-se
//       opcionalmente habilitar o caminho ArduinoJson via macro
//       ``CARDIOIA_USE_ARDUINOJSON`` na unidade de tradução; em builds
//       nativos (env ``native`` / Unity), o ``sample_builder.cpp`` cai
//       para um formatador ``snprintf`` cuidadosamente afinado para
//       produzir, byte-a-byte, o mesmo JSON do reference model Python.
//
//   Requisitos atendidos:
//     * R3.1 — Domínio dos campos.
//     * R3.2 — Chaves canônicas exatas, sem extras.
//     * R3.3 — Limite de 256 caracteres.
//     * R3.5 — Campos indisponíveis ⇒ ``null``.
//     * R3.6 — Descarte sem escrita parcial em violação do limite.
//     * R12.2 — Comentário em pt-BR acima de cada função pública.
//     * R12.4 — Comentário acima do ``if`` que avalia o tamanho.
// =============================================================================

#ifndef CARDIOIA_SAMPLE_BUILDER_H
#define CARDIOIA_SAMPLE_BUILDER_H

#include <cstdint>   // std::uint32_t
#include <cstddef>   // std::size_t
#include <optional>  // std::optional, std::nullopt
#include <string>    // std::string (alocado em heap; paciente_id curto, ≤32)

#include "config.h"  // cardioia::CARDIOIA_JSON_MAX (256), demais limites

namespace cardioia {

// ---------------------------------------------------------------------------
// struct SensorReading
// ---------------------------------------------------------------------------
// Leitura bruta do ``DHT22_Sensor`` antes da composição em
// ``Sample_Record``. Ambos os campos são ``std::optional`` para refletir
// fielmente o reference model Python: quando a leitura é inválida (NaN
// ou fora da faixa operacional — R1.3 / Property 5), o ``SensorDriver``
// propaga ``std::nullopt`` em vez de um valor sentinela, e o campo
// correspondente do JSON torna-se ``null`` (R3.5).
struct SensorReading {
    // ``temperatura``: °C com uma casa decimal útil, faixa operacional
    // ``[-40,0; 80,0]`` (R1.2, R1.3). ``std::nullopt`` ⇒ leitura
    // indisponível.
    std::optional<float> temperatura;

    // ``umidade``: percentual inteiro ``[0; 100]`` (R1.2). Mantido como
    // ``int`` em vez de ``uint8_t`` para preservar compatibilidade com
    // as rotinas ``snprintf("%d", ...)`` e evitar promoção implícita
    // para ``char`` em argumentos variádicos.
    std::optional<int> umidade;
};

// ---------------------------------------------------------------------------
// struct SampleRecord
// ---------------------------------------------------------------------------
// Registro individual da amostra (R3.1, Property 1). Os campos são
// declarados na **mesma ordem** em que serão serializados (ordem
// canônica, R3.2), o que estabiliza o layout em memória e facilita
// revisão visual dos testes que comparam byte-a-byte com o reference
// model Python.
struct SampleRecord {
    // Timestamp da amostra em ms (origem ``millis()``), sempre ≥ 0. Usa
    // ``uint32_t`` para acomodar até ~49 dias de uptime sem overflow,
    // alinhado ao que o reference model Python permite em seus testes
    // (faixa ``[0, 2**31-1]``).
    std::uint32_t timestamp_ms = 0;

    // Temperatura em °C. Quando ``std::nullopt``, o JSON carrega
    // ``"temperatura":null`` (R3.5).
    std::optional<float> temperatura;

    // Umidade em %. Quando ``std::nullopt``, o JSON carrega
    // ``"umidade":null`` (R3.5).
    std::optional<int> umidade;

    // BPM inteiro saturado em 250 (R2.5). Quando ``std::nullopt``, o
    // JSON carrega ``"bpm":null`` (R3.5).
    std::optional<int> bpm;

    // Identificador anonimizado do paciente (R3.1, R15.2). Obrigatório,
    // não vazio e com no máximo 32 caracteres. Validado em
    // :func:`serializar` — ver contrato abaixo.
    std::string paciente_id;
};

// ---------------------------------------------------------------------------
// Função:     compor
// Finalidade: Compõe um ``SampleRecord`` a partir dos insumos de um
//             ciclo de leitura do firmware (R3.1, R3.4, R3.5). Espelho
//             direto de ``compose_sample`` no reference model Python.
//
//             Valores indisponíveis devem chegar como ``std::nullopt``
//             e produzirão ``null`` no JSON. A saturação de ``bpm`` em
//             ``[0; 250]`` (R2.5) é aplicada aqui para garantir que
//             todo ``SampleRecord`` emitido pelo pipeline esteja dentro
//             da faixa aceita pela serialização.
// Parâmetros:
//   - ``now_ms``:      instante do ciclo em ms (``millis()`` simulado).
//   - ``leitura``:     leitura atual do DHT22 (temperatura + umidade).
//   - ``bpm``:         valor calculado pela ``BpmWindow`` (já saturado
//                      pelo chamador) ou ``std::nullopt`` se
//                      indisponível.
//   - ``paciente_id``: identificador anonimizado (R15.2). Deve ser não
//                      vazio e com até 32 caracteres — caso contrário,
//                      :func:`serializar` rejeitará o registro.
// Retorno:    ``SampleRecord`` pronto para serialização via
//             :func:`serializar`.
// ---------------------------------------------------------------------------
SampleRecord compor(std::uint32_t now_ms,
                    const SensorReading& leitura,
                    std::optional<int> bpm,
                    const std::string& paciente_id);

// ---------------------------------------------------------------------------
// Função:     serializar
// Finalidade: Serializa ``record`` em JSON compacto, na ordem canônica
//             definida por R3.2, e grava o resultado em ``out`` como
//             string C terminada em ``\0``. Espelho direto de
//             ``serialize_sample`` no reference model Python.
//
//             Regras de falha (retorna ``false`` sem tocar em ``out``):
//               (a) ``paciente_id`` vazio ou com mais de 32 caracteres
//                   (violação de R3.1).
//               (b) JSON resultante excede ``CARDIOIA_JSON_MAX`` (256)
//                   caracteres (violação de R3.3 / R3.6).
//               (c) Buffer ``out`` menor do que o necessário para
//                   acomodar o JSON + terminador ``\0``.
//
//             A estratégia de implementação evita gravação parcial:
//             o JSON é montado primeiro em um buffer local temporário
//             e só depois copiado integralmente para ``out`` quando
//             todas as condições são satisfeitas.
// Parâmetros:
//   - ``record``:   registro a serializar.
//   - ``out``:      buffer destino (não pode ser ``nullptr``).
//   - ``out_size``: capacidade do buffer ``out``, em bytes, incluindo o
//                   terminador ``\0``.
// Retorno:    ``true`` em caso de sucesso (``out`` contém JSON válido
//             terminado em ``\0``); ``false`` caso contrário, sem
//             alterar o conteúdo de ``out``.
// ---------------------------------------------------------------------------
bool serializar(const SampleRecord& record, char* out, std::size_t out_size);

}  // namespace cardioia

#endif  // CARDIOIA_SAMPLE_BUILDER_H
