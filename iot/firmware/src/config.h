// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/config.h
// Finalidade:
//   Centraliza em um único cabeçalho os parâmetros clínicos e estruturais
//   do firmware do CardioIA. Todas as constantes são declaradas como
//   ``constexpr`` dentro do namespace ``cardioia`` para permitir checagem
//   em tempo de compilação e inclusão segura em testes nativos (env
//   PlatformIO ``native`` / Unity) sem arrastar ``Arduino.h`` — ou seja,
//   sem dependência do runtime do ESP32.
//
//   Requisitos atendidos:
//     * R12.3 — Bloco único de constantes clínicas, com comentário em
//               português brasileiro imediatamente acima de cada
//               declaração indicando a unidade de medida.
//     * R16.1 — Declaração dos quatro parâmetros clínicos ajustáveis em
//               runtime via MQTT (``BPM_THRESHOLD``, ``TEMP_THRESHOLD``,
//               ``BUFFER_LIMIT`` e ``SAMPLING_INTERVAL_MS``), com
//               valores iniciais alinhados ao reference model Python
//               (``iot/tests/reference_model.py``).
// =============================================================================

#ifndef CARDIOIA_CONFIG_H
#define CARDIOIA_CONFIG_H

#include <cstdint>  // std::uint32_t / std::size_t — tipos fixos sem Arduino.h

namespace cardioia {

// ---------------------------------------------------------------------------
// Parâmetros clínicos ajustáveis (R16.1)
// ---------------------------------------------------------------------------
// As quatro constantes abaixo formam um bloco contíguo, conforme exige o
// requisito R12.3 (bloco único), e são substituídas em runtime pelo
// ``ConfigManager`` quando chega um payload válido de configuração. Os
// valores iniciais refletem os defaults clínicos descritos no design.

// BPM_THRESHOLD: limite máximo de batimentos por minuto (bpm) antes de
// disparar o alerta ``BPM_ALTO`` no dashboard Node-RED.
constexpr int BPM_THRESHOLD = 120;

// TEMP_THRESHOLD: limite máximo de temperatura corporal em Celsius (°C)
// antes de disparar o alerta ``TEMP_ALTA`` no dashboard Node-RED.
constexpr float TEMP_THRESHOLD = 38.0f;

// BUFFER_LIMIT: capacidade máxima do ``EdgeBuffer`` offline, em número de
// amostras ``Sample_Record`` (≈ 4 min de autonomia a cada 5 s).
constexpr int BUFFER_LIMIT = 50;

// SAMPLING_INTERVAL_MS: intervalo entre amostras consecutivas, em
// milissegundos (ms). Controla o timer não bloqueante do ``loop()``.
constexpr int SAMPLING_INTERVAL_MS = 5000;

// ---------------------------------------------------------------------------
// Limites estruturais (espelho de ``iot/tests/reference_model.py``)
// ---------------------------------------------------------------------------
// Estas constantes NÃO são ajustáveis em runtime; traduzem em C++ os
// limites já validados pela suíte de PBT contra o reference model Python.

// CARDIOIA_JSON_MAX: tamanho máximo, em bytes, do JSON compacto produzido
// por ``serializar(Sample_Record, ...)`` — propriedade P1 / R3.3.
constexpr int CARDIOIA_JSON_MAX = 256;

// MQTT_PAYLOAD_MAX: tamanho máximo, em bytes, do payload publicado pelo
// ``MqttClient`` no tópico ``cardioia/paciente/{id}/sinais`` — P15 / R8.2.
constexpr int MQTT_PAYLOAD_MAX = 1024;

// DEBOUNCE_PULSO_MS: intervalo mínimo, em milissegundos (ms), entre duas
// bordas aceitas pela ISR de pulso do ``Pulse_Simulator`` — P4 / R2.4.
constexpr int DEBOUNCE_PULSO_MS = 150;

// DEBOUNCE_CONECTIVIDADE_MS: intervalo mínimo, em milissegundos (ms),
// entre duas bordas aceitas pela ISR do botão de ``Connectivity_Flag``
// (R6.2).
constexpr int DEBOUNCE_CONECTIVIDADE_MS = 50;

}  // namespace cardioia

#endif  // CARDIOIA_CONFIG_H
