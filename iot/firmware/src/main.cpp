/******************************************************************
 * Projeto:   CardioIA
 * Fase:      Fase 3 – Monitoramento IoT (Edge + Cloud + Dashboard)
 * Grupo:
 *   - Mário Melo Filho              RM563769
 *   - Stephanie Dias dos Santos     RM564315
 * Data de criação: 2025-01-15
 *
 * Descrição:
 *   Ponto de entrada do firmware embarcado no ESP32 (Wokwi). Integra
 *   todos os módulos do CardioIA: captura de sensores (DHT22 + pulso),
 *   cálculo de BPM em janela deslizante, composição de Sample_Record,
 *   buffer offline (EdgeBuffer), simulação de conectividade Wi-Fi,
 *   publicação MQTT com TLS 1.2+ e gerenciamento de configuração
 *   clínica em tempo de execução.
 *
 *   Nenhum dado PII real é lido ou publicado — o campo paciente_id
 *   provém de secrets.h e segue o padrão anonimizado PAC-XXXX (R15.2).
 ******************************************************************/

#include <cstdint>
#include <string>

#ifdef ARDUINO
#include <Arduino.h>
#include <SPIFFS.h>
#endif

#include "config.h"
#include "logger.h"
#include "sample_builder.h"
#include "bpm_window.h"
#include "edge_buffer.h"
#include "sensor_driver.h"
#include "connectivity.h"
#include "mqtt_client.h"
#include "sync_scheduler.h"
#include "config_manager.h"

// secrets.h contém credenciais Wi-Fi, MQTT e o PACIENTE_ID anonimizado.
// Este arquivo é .gitignored e NUNCA deve ser versionado (R15.2, R15.4).
#include "secrets.h"

// ===========================================================================
// Instâncias globais dos módulos do firmware
// ===========================================================================

// Logger — wrapper de Serial para emissão estruturada de mensagens
cardioia::SerialLoggerSink g_serial_sink;
cardioia::Logger g_logger(g_serial_sink);

// BpmWindow — janela deslizante de 60 s para cálculo de BPM (R2.2)
cardioia::BpmWindow g_bpm_window(0);

// EdgeBuffer — buffer FIFO offline com storage primário (SPIFFS) e fallback (RAM)
cardioia::SpiffsStorage g_spiffs_storage;
cardioia::RamStorage g_ram_storage(cardioia::BUFFER_LIMIT, &g_logger);
cardioia::EdgeBuffer g_edge_buffer(g_spiffs_storage, g_ram_storage, &g_logger);

// SensorDriver — leitura do DHT22 e registro de pulsos via ISR
cardioia::SensorDriver g_sensor_driver(&g_logger, &g_bpm_window);

// ConnectivityController — FSM da Connectivity_Flag (R6.1)
cardioia::ConnectivityController g_connectivity(&g_logger);

// MqttClient — publicação TLS 1.2+ com QoS 1 no HiveMQ Cloud (R8)
cardioia::MqttClient g_mqtt_client(&g_logger);

// SyncScheduler — orquestra replay de amostras pendentes ao reconectar (R7)
cardioia::SyncScheduler g_sync_scheduler(g_edge_buffer, g_mqtt_client, &g_logger);

// ConfigManager — parâmetros clínicos ajustáveis em runtime (R16)
cardioia::ConfigManager g_config_manager(&g_logger, PACIENTE_ID);

// ===========================================================================
// Pinos do hardware (conforme diagram.json do Wokwi)
// ===========================================================================
constexpr int PIN_DHT22 = 15;          // GPIO do sensor DHT22
constexpr int PIN_PULSE_BUTTON = 4;    // GPIO do botão Pulse_Simulator
constexpr int PIN_WIFI_BUTTON = 2;     // GPIO do botão Connectivity_Flag

// ===========================================================================
// Timer não bloqueante para amostragem periódica (R1.1)
// ===========================================================================
std::uint32_t g_last_sample_ms = 0;

// ===========================================================================
// Função: setup
// Finalidade: Inicializa todos os módulos do firmware na ordem correta
//             de dependência. Executada uma única vez no boot do ESP32.
// ===========================================================================
void setup() {
#ifdef ARDUINO
    Serial.begin(115200);
    delay(100);  // Aguarda estabilização do Serial
#endif

    g_logger.info("main", "CardioIA Fase 3 — inicializando...");

    // Inicializa o ConfigManager (carrega última config válida do storage)
    g_config_manager.iniciar();

    // Inicializa o SensorDriver com pinos do DHT22 e do Pulse_Simulator
    g_sensor_driver.iniciar(PIN_DHT22, PIN_PULSE_BUTTON);

    // Inicializa o ConnectivityController com o pino do botão Wi-Fi
    g_connectivity.iniciar(PIN_WIFI_BUTTON);

    // Configura o MqttClient com credenciais de secrets.h (R8.5)
    g_mqtt_client.configurar(MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD,
                             "cardioia-esp32");

    // Registra o hook do SyncScheduler no ConnectivityController (R6.3).
    // Quando Connectivity_Flag transita de false para true, o scheduler
    // inicia o replay das amostras pendentes no EdgeBuffer.
    cardioia::set_sync_scheduler_hook([](std::size_t /* buffer_size */) {
        g_sync_scheduler.onReconectar();
    });

    g_logger.info("main", "CardioIA Fase 3 inicializado com sucesso");
}

// ===========================================================================
// Função: loop
// Finalidade: Ciclo principal do firmware. Executa periodicamente a
//             leitura de sensores, composição de amostras, roteamento
//             via SyncScheduler e tratamento de comandos Serial.
//             Usa timer não bloqueante para respeitar o SAMPLING_INTERVAL_MS.
// ===========================================================================
void loop() {
#ifdef ARDUINO
    std::uint32_t now = millis();
#else
    // Em builds nativos, simula millis() como 0 (testes controlam o tempo)
    std::uint32_t now = 0;
#endif

    // Processa pendências da ISR do botão de conectividade (R6.2)
    g_connectivity.processar_pendencias(g_edge_buffer.size());

    // Processa comandos recebidos pelo Monitor Serial (R6.5)
#ifdef ARDUINO
    if (Serial.available()) {
        // Lê a linha completa do Serial — único uso de Arduino String,
        // convertido imediatamente para std::string (R12.4)
        String raw_arduino = Serial.readStringUntil('\n');
        std::string cmd(raw_arduino.c_str());
        g_connectivity.handle_serial_command(cmd, g_edge_buffer.size());
    }
#endif

    // Timer não bloqueante para amostragem periódica (R1.1).
    // Se o intervalo desde a última amostra atingiu SAMPLING_INTERVAL_MS,
    // executa um novo ciclo de leitura e composição de Sample_Record.
    if ((now - g_last_sample_ms) >=
        static_cast<std::uint32_t>(cardioia::SAMPLING_INTERVAL_MS)) {
        g_last_sample_ms = now;

        // Lê sensores (DHT22 — temperatura e umidade)
        cardioia::SensorReading reading = g_sensor_driver.ler(now);

        // Calcula BPM na janela deslizante de 60 s (R2.2)
        std::uint8_t bpm = g_bpm_window.calcular_bpm(now);

        // Compõe o Sample_Record com todos os campos (R3.1, R3.4).
        // O paciente_id provém de secrets.h e é anonimizado (R15.2).
        cardioia::SampleRecord record = cardioia::compor(
            now,
            reading,
            static_cast<int>(bpm),
            std::string(PACIENTE_ID));

        // Roteamento via SyncScheduler (R7.3, Property P8):
        // Se Connectivity_Flag é true E o EdgeBuffer está vazio, a amostra
        // é publicada diretamente no MQTT. Caso contrário, é enfileirada
        // no EdgeBuffer para sincronização posterior.
        g_sync_scheduler.enviarOuEnfileirar(record, g_connectivity.flag());
    }

    // Tick do SyncScheduler — processa replay pendente do EdgeBuffer (R7.1)
    g_sync_scheduler.tick(now, g_connectivity.flag());

    // Loop do MQTT — processa keepalive e PUBACKs pendentes (R8.1)
    if (g_connectivity.flag()) {
        g_mqtt_client.loop();
    }

    // Drena pulsos pendentes da ISR do Pulse_Simulator (R2.1, R2.4).
    // A ISR apenas incrementa o contador volatile; o loop principal
    // registra cada pulso na BpmWindow com o timestamp atual.
    while (cardioia::g_pulse_pending_count > 0) {
        cardioia::g_pulse_pending_count--;
        g_sensor_driver.registrar_pulso(now);
    }
}
