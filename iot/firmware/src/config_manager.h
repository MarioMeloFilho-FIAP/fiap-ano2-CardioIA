// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/config_manager.h
// Finalidade:
//   Declara ``cardioia::ConfigManager`` — módulo responsável por validar,
//   aplicar e persistir os parâmetros clínicos ajustáveis em tempo de
//   execução (``bpm_threshold``, ``temp_threshold``, ``buffer_limit``,
//   ``sampling_interval_ms``) recebidos via MQTT em
//   ``cardioia/paciente/{paciente_id}/config``. Espelha, em C++ 17, a
//   classe Python ``ConfigManager`` de
//   ``iot/tests/reference_model.py`` (Properties P19 e P20) — o oráculo
//   usado pelos testes ``test_config_manager_pbt.py``.
//
//   Requisitos EARS cobertos:
//     * R16.1 — Faixas de aceitação:
//                 bpm_threshold        ∈ [40, 220]   (int estrito)
//                 temp_threshold       ∈ [35.0, 42.0](float; int aceito)
//                 buffer_limit         ∈ [10, 500]   (int estrito)
//                 sampling_interval_ms ∈ [1000, 60000] (int estrito)
//               Payload fora das faixas é rejeitado sem alterar o
//               ``state`` atual.
//     * R16.2 — Última config válida sobrevive a reboots: ``persistir()``
//               grava em SPIFFS (caminho Arduino) ou em arquivo local
//               (caminho nativo) e ``carregar()`` é invocado em
//               ``iniciar()``.
//     * R16.3 — Payload malformado gera evento de rejeição com motivo
//               em pt-BR para ser publicado em
//               ``cardioia/paciente/{paciente_id}/rejeicao`` (publicação
//               MQTT propriamente dita fica a cargo do ``MqttClient`` —
//               este módulo apenas expõe o motivo).
//     * R16.4 — Após cada atualização válida, o chamador publica
//               mensagem *retained* em
//               ``cardioia/paciente/{paciente_id}/config/aplicado``.
//               ``ConfigManager::topicoAplicado()`` e ``topicoRejeicao()``
//               entregam as strings de tópico prontas e conformes a
//               R15.2 (só publicam se o ``paciente_id`` for anonimizado).
//     * R15.2 — ``is_anonymized_paciente_id()`` recusa PII
//               (``^PAC-\d{1,27}$`` canônico).
//     * R12.2 e R12.4 — Cabeçalho pt-BR acima de cada função e acima de
//               cada ``if``/``switch`` de validação (ver ``.cpp``).
//
//   Estratégia de build dual (ESP32 × nativo):
//     * Quando ``ARDUINO`` está definido, usamos ``ArduinoJson`` para
//       parsear o payload e ``SPIFFS`` para persistir.
//     * Em builds nativos (env PlatformIO ``native`` / Unity), usamos um
//       parser JSON minimalista embutido (apenas os quatro campos
//       esperados, chaves em ASCII) e gravação em arquivo local via
//       ``<cstdio>``. O caminho do arquivo nativo pode ser sobrescrito
//       via ``ConfigManager::setCaminhoPersistenciaNativo()`` para
//       facilitar testes determinísticos.
//
//   Observação sobre uso de ``std::string``: o design do firmware
//   proíbe a classe ``String`` do Arduino (fragmentação de heap). Toda
//   a API abaixo opera em ``std::string`` e ``const char*``.
// =============================================================================

#ifndef CARDIOIA_CONFIG_MANAGER_H
#define CARDIOIA_CONFIG_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "logger.h"

namespace cardioia {

// ---------------------------------------------------------------------------
// Estado de configuração clínica (R16.1)
// ---------------------------------------------------------------------------
// Mantém os quatro parâmetros ajustáveis em runtime. Os defaults são
// importados de ``config.h`` (constexpr), espelhando o ``DEFAULT_CONFIG``
// do reference model Python.
struct ConfigState {
    // Limite máximo de batimentos por minuto (bpm) antes do alerta.
    int bpm_threshold = BPM_THRESHOLD;

    // Limite máximo de temperatura corporal em °C antes do alerta.
    float temp_threshold = TEMP_THRESHOLD;

    // Capacidade máxima do Local_Buffer (número de Sample_Record).
    int buffer_limit = BUFFER_LIMIT;

    // Intervalo entre amostras consecutivas em milissegundos.
    int sampling_interval_ms = SAMPLING_INTERVAL_MS;
};

// Igualdade/desigualdade membro-a-membro — usada por testes nativos e
// pela verificação de "config mudou?" antes de publicar ``aplicado``.
bool operator==(const ConfigState& a, const ConfigState& b) noexcept;
bool operator!=(const ConfigState& a, const ConfigState& b) noexcept;

// ---------------------------------------------------------------------------
// Resultado da aplicação/validação de um payload
// ---------------------------------------------------------------------------
// Em sucesso, ``ok == true`` e ``state`` contém o *merge* do payload
// sobre o ``state`` recebido. Em rejeição, ``ok == false``, ``state``
// é igual ao ``state`` original (preservação, R16.1/R16.3) e ``reason``
// contém uma string pt-BR explicando o motivo.
struct ConfigApplyResult {
    bool ok = false;
    ConfigState state{};
    std::optional<std::string> reason{};
};

// ---------------------------------------------------------------------------
// Função: is_anonymized_paciente_id
// Finalidade: Valida se ``s`` casa com ``^PAC-\d{1,27}$``, espelhando a
//             Property 18 do reference model (R15.2). Nega explicitamente
//             strings que pareçam e-mail, CPF, data ``dd/mm/aaaa`` ou
//             sequências numéricas "nuas" (defesa em profundidade — a
//             regex canônica ``PAC-\d{1,27}`` já barra esses casos).
// Parâmetros:
//   - ``s``: identificador a validar. ``nullptr`` ou string vazia
//            retornam ``false``.
// Retorno:    ``true`` se anonimizado conforme R15.2; ``false`` caso
//             contrário.
// ---------------------------------------------------------------------------
bool is_anonymized_paciente_id(const std::string& s) noexcept;

// ---------------------------------------------------------------------------
// Função: apply_config_json
// Finalidade: Recebe o ``state`` atual e um payload JSON em string e
//             devolve o ``ConfigApplyResult`` correspondente. Espelha
//             a função pura ``apply_config`` do reference model (P19).
//             Esta função NÃO altera estado externo nem publica MQTT —
//             a publicação é responsabilidade do chamador
//             (``ConfigManager::aplicar``).
// Parâmetros:
//   - ``current``:     estado atual (preservado em caso de rejeição).
//   - ``payload_json``: payload JSON recebido em
//                       ``cardioia/paciente/{paciente_id}/config``.
// Retorno:   ``ConfigApplyResult`` com ``ok=true`` e state mesclado em
//            caso de sucesso; ``ok=false`` + ``state==current`` + motivo
//            em pt-BR em caso de rejeição.
// ---------------------------------------------------------------------------
ConfigApplyResult apply_config_json(const ConfigState& current,
                                    const std::string& payload_json);

// ---------------------------------------------------------------------------
// Classe: ConfigManager
// ---------------------------------------------------------------------------
// Encapsula state + last_valid + rejeições + persistência. Cada chamada
// bem-sucedida de ``aplicar()`` atualiza ambos e aciona ``persistir()``;
// rejeições acumulam o motivo em ``rejection_events_`` para publicação
// em ``.../rejeicao``. Em testes nativos, ``reboot()`` recarrega a
// última config válida a partir do storage persistente (espelhando P20).
class ConfigManager {
public:
    // Storage key usada em SPIFFS / arquivo nativo. Mesmo nome do
    // reference model para facilitar comparação direta em testes.
    static constexpr const char* kStorageKey = "cardioia.config";

    // -----------------------------------------------------------------------
    // Construtor: ConfigManager::ConfigManager
    // Finalidade: Inicializa ``state_`` e ``last_valid_`` com
    //             ``DEFAULT_CONFIG`` (ou com o ``initial`` opcional) e
    //             memoriza ``paciente_id`` para montar os tópicos MQTT.
    //             Não faz I/O — a leitura real do storage acontece em
    //             ``iniciar()`` para permitir que testes injetem um
    //             logger antes de qualquer acesso a SPIFFS/arquivo.
    // Parâmetros:
    //   - ``logger``:      ponteiro para o Logger compartilhado (pode
    //                      ser ``nullptr`` em testes que não observam
    //                      linhas de log).
    //   - ``paciente_id``: identificador anonimizado do paciente (deve
    //                      casar ``^PAC-\d{1,27}$`` para que os tópicos
    //                      publicados sejam considerados válidos —
    //                      R15.2).
    //   - ``initial``:     config inicial (opcional). Se ausente, usa os
    //                      defaults de ``config.h``.
    // Retorno:     (construtor).
    // -----------------------------------------------------------------------
    explicit ConfigManager(Logger* logger = nullptr,
                           std::string paciente_id = "PAC-0001",
                           std::optional<ConfigState> initial = std::nullopt);

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::iniciar
    // Finalidade: Tenta carregar a última config válida do storage
    //             persistente (SPIFFS no ESP32, arquivo local no build
    //             nativo). Se o storage estiver vazio, corrompido ou
    //             inacessível, mantém o ``DEFAULT_CONFIG`` e grava um
    //             snapshot para que o próximo reboot já encontre um
    //             estado consistente.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void iniciar();

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::aplicar
    // Finalidade: Aplica um payload JSON recebido em ``.../config``.
    //             Em sucesso: atualiza ``state_``/``last_valid_`` e
    //             persiste em storage (R16.2), retornando
    //             ``(true, nullopt)``. Em rejeição: preserva ``state_``
    //             e ``last_valid_`` (P20), acumula o motivo em
    //             ``rejection_events_`` (R16.3) e retorna
    //             ``(false, motivo)`` para que o chamador publique em
    //             ``.../rejeicao``.
    // Parâmetros:
    //   - ``payload_json``: string JSON recebida do MQTT. String vazia
    //                       ou JSON malformado também são rejeitados.
    // Retorno:    par ``(aplicou, motivo_opcional)``.
    // -----------------------------------------------------------------------
    std::pair<bool, std::optional<std::string>> aplicar(
        const std::string& payload_json);

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::reboot
    // Finalidade: Simula um reboot (útil em testes nativos). Reseta
    //             ``state_``/``last_valid_`` e tenta recarregar a
    //             última config válida do storage. Se o storage estiver
    //             vazio, volta a ``DEFAULT_CONFIG`` (R16.2).
    // Parâmetros: (nenhum).
    // Retorno:    ``ConfigState`` carregado (cópia).
    // -----------------------------------------------------------------------
    ConfigState reboot();

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::topicoAplicado
    // Finalidade: Retorna o tópico MQTT
    //             ``cardioia/paciente/{paciente_id}/config/aplicado``
    //             para publicação *retained* após cada atualização
    //             válida (R16.4). Retorna string vazia se
    //             ``paciente_id`` não for anonimizado — defesa contra
    //             vazamento de PII (R15.2).
    // Parâmetros: (nenhum).
    // Retorno:    tópico MQTT ou ``""``.
    // -----------------------------------------------------------------------
    std::string topicoAplicado() const;

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::topicoRejeicao
    // Finalidade: Retorna o tópico MQTT
    //             ``cardioia/paciente/{paciente_id}/rejeicao`` para
    //             publicação do motivo de rejeição (R16.3). Retorna
    //             string vazia se ``paciente_id`` não for anonimizado
    //             (R15.2).
    // Parâmetros: (nenhum).
    // Retorno:    tópico MQTT ou ``""``.
    // -----------------------------------------------------------------------
    std::string topicoRejeicao() const;

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::serializarEstado
    // Finalidade: Serializa o ``state_`` atual como JSON canônico com as
    //             quatro chaves, para publicação em
    //             ``.../config/aplicado``. Formato estável: chaves em
    //             ordem ``bpm_threshold``, ``temp_threshold``,
    //             ``buffer_limit``, ``sampling_interval_ms``.
    // Parâmetros: (nenhum).
    // Retorno:    string JSON sem espaços supérfluos.
    // -----------------------------------------------------------------------
    std::string serializarEstado() const;

    // Acessadores (leitura, cópia rasa de POD — sem alocação).
    ConfigState state() const noexcept { return state_; }
    ConfigState last_valid() const noexcept { return last_valid_; }
    const std::string& paciente_id() const noexcept { return paciente_id_; }
    const std::vector<std::string>& rejection_events() const noexcept {
        return rejection_events_;
    }

    // -----------------------------------------------------------------------
    // Método:    ConfigManager::setCaminhoPersistenciaNativo (testes)
    // Finalidade: Em build nativo, permite ao teste sobrescrever o
    //             caminho do arquivo de persistência (default:
    //             ``/tmp/cardioia_config.json``). Em build Arduino o
    //             valor é ignorado — SPIFFS define seu próprio caminho.
    // Parâmetros:
    //   - ``caminho``: caminho absoluto; ``""`` restaura o default.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void setCaminhoPersistenciaNativo(const std::string& caminho);

private:
    Logger* logger_;
    std::string paciente_id_;
    ConfigState state_{};
    ConfigState last_valid_{};
    std::vector<std::string> rejection_events_{};

    // Caminho do arquivo nativo de persistência (ignorado em Arduino).
    std::string caminho_nativo_;

    // Persiste o ``last_valid_`` atual em SPIFFS (Arduino) ou arquivo
    // (nativo). Falhas são logadas como WARN mas não abortam a
    // aplicação — o reference model não prevê rollback em caso de I/O.
    void persistir();

    // Tenta carregar a última config válida. Retorna ``true`` se
    // carregou algo válido e preencheu ``state_``/``last_valid_``;
    // ``false`` se storage vazio/corrompido (chamador mantém defaults).
    bool carregar();
};

}  // namespace cardioia

#endif  // CARDIOIA_CONFIG_MANAGER_H
