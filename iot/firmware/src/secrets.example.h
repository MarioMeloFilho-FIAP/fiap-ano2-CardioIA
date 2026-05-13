// =============================================================================
// CardioIA – Fase 3 – Monitoramento IoT
// Arquivo: secrets.example.h
// -----------------------------------------------------------------------------
// IMPORTANTE (LGPD e boas práticas de segurança):
//   * Este é um ARQUIVO DE EXEMPLO. Copie-o localmente como `secrets.h` na
//     mesma pasta (`iot/firmware/src/secrets.h`) e preencha os valores reais
//     APENAS na sua máquina.
//   * O arquivo `secrets.h` NUNCA deve ser versionado (já listado no
//     `.gitignore` da raiz do repositório). Ele contém credenciais de Wi-Fi e
//     do broker MQTT que não podem vazar para o histórico do Git.
//   * Em conformidade com a LGPD (Lei 13.709/2018) e com os requisitos R15.2
//     e R15.4 do CardioIA, o campo `PACIENTE_ID` deve conter SEMPRE um
//     identificador anonimizado no formato `PAC-<dígitos>` (ex.: "PAC-0001").
//     É PROIBIDO utilizar CPF, RG, nome, e-mail, telefone ou qualquer outro
//     dado pessoal identificável como `paciente_id`.
//   * Em ambiente produtivo, prefira variáveis de ambiente ou um secrets
//     manager (ex.: AWS Secrets Manager, HashiCorp Vault) no lugar deste
//     cabeçalho de credenciais.
// =============================================================================

#ifndef CARDIOIA_SECRETS_H
#define CARDIOIA_SECRETS_H

// -----------------------------------------------------------------------------
// Credenciais da rede Wi-Fi utilizada pelo ESP32 para acessar a Internet.
// -----------------------------------------------------------------------------
#define WIFI_SSID       "SUA_REDE_WIFI"
#define WIFI_PASSWORD   "***"

// -----------------------------------------------------------------------------
// Credenciais do broker MQTT (HiveMQ Cloud ou equivalente com suporte a TLS).
// A porta 8883 é a porta padrão para MQTT sobre TLS 1.2+ (R15.1).
// -----------------------------------------------------------------------------
#define MQTT_HOST       "SEU_BROKER.hivemq.cloud"
#define MQTT_PORT       8883
#define MQTT_USER       "SEU_USUARIO_MQTT"
#define MQTT_PASSWORD   "***"

// -----------------------------------------------------------------------------
// Identificador anonimizado do paciente monitorado (R15.2).
// Padrão aceito: expressão regular  ^PAC-\d{1,27}$
// Exemplo válido:   "PAC-0001"
// Exemplos INVÁLIDOS (NÃO usar): CPF, e-mail, nome, data de nascimento, RG.
// -----------------------------------------------------------------------------
#define PACIENTE_ID     "PAC-0001"

#endif  // CARDIOIA_SECRETS_H
