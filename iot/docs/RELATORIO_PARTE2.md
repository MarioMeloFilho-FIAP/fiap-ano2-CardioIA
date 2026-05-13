# Relatório Parte 2 — Cloud Computing e Dashboard (CardioIA Fase 3)

**Projeto:** CardioIA — Monitoramento IoT de Pacientes Cardiológicos  
**Fase:** 3 — Integração em Nuvem e Dashboard  
**Disciplina:** FIAP — Inteligência Artificial / IoT  
**Data:** 2025-01-20

---

## 1. Fluxo de Comunicação MQTT

### 1.1 Arquitetura Geral

O sistema CardioIA utiliza o protocolo MQTT (Message Queuing Telemetry Transport) como camada de transporte entre o dispositivo embarcado (ESP32) e o dashboard de visualização (Node-RED). A arquitetura segue o padrão publish/subscribe com um broker centralizado em nuvem:

```
[ESP32 (Wokwi)] ──TLS 8883──▶ [HiveMQ Cloud Broker] ──TLS 8883──▶ [Node-RED Dashboard]
```

O fluxo completo de uma amostra é:

1. O ESP32 captura temperatura (DHT22), umidade (DHT22) e BPM (botão simulador de pulso).
2. O `SampleBuilder` compõe um `Sample_Record` em formato JSON compacto.
3. Se `Connectivity_Flag = true` e o buffer está vazio, o `MqttClient` publica diretamente no tópico `cardioia/paciente/{paciente_id}/sinais` via TLS na porta 8883.
4. Se `Connectivity_Flag = false`, o `EdgeBuffer` armazena localmente (FIFO, máximo 50 registros).
5. Ao reconectar, o `SyncScheduler` reenvia os registros pendentes em ordem cronológica.
6. O broker HiveMQ Cloud roteia a mensagem para todos os assinantes (Node-RED Dashboard).
7. O Node-RED valida, processa e exibe os dados em gráficos, gauges e alertas.

### 1.2 Tópicos MQTT

O sistema utiliza quatro tópicos organizados hierarquicamente por paciente:

| Tópico | Direção | QoS | Retained | Descrição |
|--------|---------|-----|----------|-----------|
| `cardioia/paciente/{id}/sinais` | ESP32 → Broker | 1 | Não | Telemetria de sinais vitais |
| `cardioia/paciente/{id}/config` | Broker → ESP32 | 1 | Sim | Reconfiguração de limites clínicos |
| `cardioia/paciente/{id}/config/aplicado` | ESP32 → Broker | 1 | Sim | Eco da configuração vigente |
| `cardioia/paciente/{id}/rejeicao` | ESP32 → Broker | 1 | Não | Motivo de rejeição de config inválida |

O `{id}` é um identificador anonimizado no formato `PAC-XXXX` (ex.: `PAC-0001`), sem qualquer dado pessoalmente identificável do paciente, em conformidade com a LGPD.

### 1.3 Autenticação e Qualidade de Serviço (QoS)

**Autenticação:**
- O ESP32 autentica-se no broker HiveMQ Cloud com credenciais exclusivas (usuário e senha) definidas no arquivo `secrets.h` (não versionado).
- Caso o broker rejeite as credenciais, o firmware entra no estado `AUTH_SUSPENDED` e suspende tentativas automáticas de reconexão até intervenção externa (reset ou nova configuração).
- O timeout de autenticação é de 10 segundos; caso excedido, o firmware trata como falha de conexão.

**QoS 1 (At Least Once):**
- Todas as publicações utilizam QoS 1, garantindo que cada mensagem seja entregue ao broker pelo menos uma vez.
- O ESP32 aguarda o `PUBACK` do broker por até 5 segundos. Se não recebido, o `Sample_Record` é reencaminhado ao `EdgeBuffer` para reenvio posterior.
- O keepalive MQTT é configurado em 60 segundos, com timeout de conexão de 10 segundos.

**Política de reconexão:**
- Em caso de perda de conexão, o firmware tenta reconectar a cada 5 segundos.
- Após 3 tentativas consecutivas sem sucesso, `Connectivity_Flag` é marcado como `false` e o sistema opera em modo offline.
- A reconexão é retomada automaticamente quando `Connectivity_Flag` transita para `true`.

### 1.4 Criptografia TLS 1.2+

Toda comunicação entre o ESP32 e o broker HiveMQ Cloud é cifrada com TLS (Transport Layer Security) versão 1.2 ou superior:

- **Porta:** 8883 (porta padrão MQTT sobre TLS, conforme especificação OASIS).
- **Implementação:** Classe `WiFiClientSecure` do ESP-IDF/Arduino, que utiliza o acelerador criptográfico nativo do ESP32.
- **Validação de certificado:** Em produção, o certificado CA raiz do HiveMQ Cloud é carregado via `setCACert()` para validação completa da cadeia.
- **Proteção oferecida:** Confidencialidade (cifração do canal), integridade (detecção de adulteração) e autenticidade do servidor (verificação do certificado).

---

## 2. Configuração do Broker HiveMQ Cloud

### 2.1 Criação do Cluster

O HiveMQ Cloud é um broker MQTT gerenciado que oferece plano gratuito (Serverless) adequado para prototipagem:

1. **Criar conta:** Acessar [hivemq.com/cloud](https://www.hivemq.com/cloud/) e registrar-se.
2. **Criar cluster:** Selecionar plano "Serverless (Free)" → região mais próxima (ex.: `eu-west-1`).
3. **Anotar o host:** O cluster recebe um hostname único no formato `XXXXXXXX.s1.eu.hivemq.cloud`.
4. **Porta TLS:** 8883 (única porta disponível no plano gratuito — TLS é obrigatório).

### 2.2 Criação de Credenciais

1. No painel do cluster, acessar **Access Management**.
2. Criar credenciais com usuário e senha fortes (mínimo 12 caracteres, alfanuméricos + especiais).
3. Definir permissões: publicação e assinatura nos tópicos `cardioia/#`.
4. Copiar as credenciais para o arquivo local `secrets.h` (nunca versionar).

### 2.3 Configuração TLS

O HiveMQ Cloud exige TLS em todas as conexões:

- O certificado CA raiz utilizado é o **ISRG Root X1** (Let's Encrypt) ou equivalente fornecido pelo HiveMQ.
- No firmware, o certificado é embutido como string PEM e carregado via `wifiClient.setCACert(ca_cert)`.
- Em ambiente de simulação (Wokwi), utiliza-se `wifiClient.setInsecure()` como fallback documentado (ver Seção 7).

---

## 3. Configuração do Dashboard Node-RED

### 3.1 Visão Geral dos Nós Utilizados

O fluxo Node-RED (`iot/dashboard/cardioia_flow.json`) é composto pelos seguintes nós principais:

| Nó | Tipo | Função |
|----|------|--------|
| `mqtt in` (sinais) | `mqtt in` | Assina `cardioia/paciente/+/sinais` via TLS 8883 |
| `mqtt in` (config) | `mqtt in` | Assina `cardioia/paciente/+/config/aplicado` (retained) |
| `json` | `json` | Converte payload string em objeto JavaScript |
| `Validar Sample_Record` | `function` | Valida campos obrigatórios e faixas de valores |
| `Gráfico BPM` | `ui_chart` | Gráfico de linha: BPM ao longo do tempo |
| `Gráfico Temperatura` | `ui_chart` | Gráfico de linha: temperatura corporal |
| `Gauge Umidade` | `ui_gauge` | Medidor circular com três faixas de cor |
| `Alert_Module` | `function` | Classifica alertas por limites clínicos |
| `Alerta BPM` | `ui_text` | Indicador visual de alerta de BPM |
| `Alerta Temperatura` | `ui_text` | Indicador visual de alerta de temperatura |
| `Status Conexão` | `ui_text` | Estado da conexão MQTT (conectado/desconectado) |
| `Limites Atuais` | `ui_text` | Exibe os limiares clínicos vigentes |
| `Mensagens Inválidas` | `ui_text` | Contador de mensagens descartadas |
| `Destaque Anômalo` | `function` | Marca pontos que excedem limiares por 5 s |


### 3.2 Validação de Mensagens (R9.5)

O nó `function` "Validar Sample_Record" implementa a seguinte lógica de validação antes de propagar dados para os gráficos e alertas:

```javascript
// Verificação de campos obrigatórios e faixas
var msg_obj = msg.payload;
if (typeof msg_obj !== 'object' || msg_obj === null) { /* rejeita */ }
if (typeof msg_obj.bpm !== 'number' && msg_obj.bpm !== null) { /* rejeita */ }
if (typeof msg_obj.temperatura !== 'number' && msg_obj.temperatura !== null) { /* rejeita */ }
if (typeof msg_obj.umidade !== 'number' && msg_obj.umidade !== null) { /* rejeita */ }
if (typeof msg_obj.paciente_id !== 'string' || msg_obj.paciente_id === '') { /* rejeita */ }
// Faixas: bpm [0,250], temperatura [-40,80], umidade [0,100]
```

Mensagens inválidas (JSON malformado, campos ausentes ou valores fora das faixas) são descartadas sem atualizar os gráficos, e o contador de mensagens inválidas é incrementado em 1. Isso garante que dados corrompidos ou adversariais não contaminem a visualização clínica.

### 3.3 Gráficos de Sinais Vitais (R9.2, R9.3)

**Gráfico de BPM:**
- Tipo: `ui_chart` (linha)
- Escala vertical: 30 a 220 BPM
- Janela deslizante: configurável entre 60 e 300 segundos
- Rótulo: "BPM" com escala temporal no formato `hh:mm:ss`
- Atualização: em até 2 segundos após recepção de mensagem válida
- Destaque visual: pontos que excedem `BPM_Threshold` (120) são marcados em vermelho por 5 segundos

**Gráfico de Temperatura:**
- Tipo: `ui_chart` (linha)
- Escala vertical: 30,0 a 45,0 °C
- Janela deslizante: configurável entre 60 e 300 segundos
- Rótulo: "°C" com escala temporal no formato `hh:mm:ss`
- Atualização: em até 2 segundos após recepção de mensagem válida
- Destaque visual: pontos que excedem `Temperature_Threshold` (38,0 °C) são marcados em vermelho por 5 segundos

### 3.4 Gauge de Umidade (R10)

O medidor de umidade é implementado como `ui_gauge` com as seguintes características:

- Escala: 0% a 100%
- Três faixas de cor contíguas e sem sobreposição:
  - **Verde** (0,0% – 30,0%): umidade baixa
  - **Amarelo** (>30,0% – 70,0%): umidade ideal
  - **Vermelho** (>70,0% – 100,0%): umidade alta
- Valor numérico exibido ao lado do gauge com uma casa decimal e símbolo "%"
- Preserva o último valor válido caso uma mensagem não contenha o campo de umidade ou apresente valor inválido, com indicação visual de leitura inválida

### 3.5 Alertas Automáticos — Alert_Module (R11)

O nó `function` "Alert_Module" implementa a lógica de classificação de alertas:

- **BPM > 120:** Ativa indicador visual vermelho com texto "ALERTA: BPM elevado" em até 1 segundo.
- **Temperatura > 38,0 °C:** Ativa indicador visual vermelho com texto "ALERTA: Temperatura elevada" em até 1 segundo.
- **Ambos ultrapassados simultaneamente:** Exibe os dois indicadores de alerta simultaneamente, sem sobrescrever um ao outro.
- **Valores normais (BPM ≤ 120 e temperatura ≤ 38,0):** Mantém indicador verde com texto "Normal".

A comparação utiliza operador estritamente maior (`>`), ou seja, valores exatamente iguais aos limiares (BPM = 120, temperatura = 38,0) são considerados normais.

Cada ativação de alerta gera uma entrada de log no Node-RED contendo: timestamp, `paciente_id`, métrica em alerta e valor medido.

### 3.6 Limites Atuais (R16.4)

O dashboard assina o tópico `cardioia/paciente/+/config/aplicado` (mensagem retained) e exibe em uma área dedicada os valores atuais dos limiares clínicos utilizados pelo `Alert_Module`:

- BPM Threshold: valor atual (default: 120 BPM)
- Temperature Threshold: valor atual (default: 38,0 °C)

A atualização ocorre em até 10 segundos após a publicação de uma nova configuração pelo ESP32.

---

## 4. Evidências do Dashboard em Funcionamento

### 4.1 Gráfico de Sinais Vitais

![Gráfico de BPM e Temperatura em tempo real](../dashboard/screenshots/01_grafico_sinais.png)

*Figura 1: Gráficos de linha exibindo BPM (escala 30–220) e temperatura corporal (escala 30,0–45,0 °C) com janela deslizante de 60 segundos. Os dados são atualizados a cada 5 segundos conforme o `Sampling_Interval` do ESP32.*

### 4.2 Gauge de Umidade

![Gauge de Umidade com três faixas de cor](../dashboard/screenshots/02_gauge_umidade.png)

*Figura 2: Medidor circular de umidade ambiente com três faixas de cor (verde: 0–30%, amarelo: 30–70%, vermelho: 70–100%). O valor numérico atual é exibido ao centro com uma casa decimal.*

### 4.3 Alerta Ativo

![Alerta ativo quando limite clínico é ultrapassado](../dashboard/screenshots/03_alerta_ativo.png)

*Figura 3: Dashboard com alerta ativo (indicador vermelho) disparado quando BPM > 120 e/ou temperatura > 38,0 °C. Os textos "ALERTA: BPM elevado" e "ALERTA: Temperatura elevada" são exibidos simultaneamente quando ambos os limiares são ultrapassados.*

---

## 5. Boas Práticas de Segurança em IoT Médico (R15.3)

### 5.1 Autenticação Forte

**Descrição:** Toda conexão do ESP32 ao broker MQTT exige autenticação por credenciais exclusivas (usuário e senha). Sem credenciais válidas, o broker recusa a conexão e nenhum dado clínico é publicado ou consumido por terceiros não autorizados.

**Justificativa clínica/regulatória:**
- **LGPD art. 46:** O agente de tratamento deve adotar medidas de segurança, técnicas e administrativas aptas a proteger os dados pessoais de acessos não autorizados e de situações acidentais ou ilícitas.
- **ANVISA RDC 657/2022:** Controle de acesso é requisito essencial para dispositivos médicos conectados, impedindo que atores não autorizados injetem dados falsos (que poderiam levar a decisões clínicas incorretas) ou interceptem dados sensíveis de saúde.

**Aplicação no protótipo:**
- Credenciais definidas em `secrets.h` (não versionado, listado em `.gitignore`).
- Arquivo `secrets.example.h` com placeholders para orientar configuração local.
- Estado `AUTH_SUSPENDED` no firmware quando credenciais são rejeitadas, suspendendo reconexões automáticas.
- Em produção: recomenda-se rotação periódica de senhas (mínimo 90 dias) e migração para certificados X.509 (mTLS).

### 5.2 Criptografia em Trânsito (TLS 1.2+)

**Descrição:** Toda comunicação entre o ESP32 e o broker HiveMQ Cloud é realizada exclusivamente pela porta 8883 com TLS versão 1.2 ou superior, garantindo que os dados clínicos em trânsito sejam cifrados ponta a ponta.

**Justificativa clínica/regulatória:**
- **LGPD art. 6, VII (Segurança):** Utilização de medidas técnicas aptas a proteger os dados pessoais de acessos não autorizados e de situações acidentais de destruição, perda, alteração ou difusão.
- **LGPD art. 6, VIII (Prevenção):** Adoção de medidas para prevenir a ocorrência de danos em virtude do tratamento de dados pessoais.
- **ANVISA RDC 657/2022:** Dispositivos médicos conectados devem garantir integridade e confidencialidade dos dados clínicos durante a transmissão, utilizando protocolos criptográficos reconhecidos.

**Aplicação no protótipo:**
- Classe `WiFiClientSecure` do ESP32 para conexão TLS.
- Porta 8883 (padrão MQTT sobre TLS, conforme OASIS MQTT v3.1.1/v5.0).
- Certificado CA carregado via `setCACert()` em produção.
- Fallback `setInsecure()` documentado para ambiente de simulação Wokwi (ver Seção 7).

### 5.3 Anonimização e Minimização de Dados

**Descrição:** O sistema utiliza exclusivamente um identificador anonimizado no campo `paciente_id`, no formato `PAC-XXXX` (ex.: `PAC-0001`). Nenhum dado pessoalmente identificável (PII) real do paciente é coletado, armazenado ou transmitido. São explicitamente rejeitados: CPF, e-mail, data de nascimento, RG, nome, endereço, telefone e prontuário.

**Justificativa clínica/regulatória:**
- **LGPD art. 6, III (Necessidade):** Limitação do tratamento ao mínimo necessário para a realização de suas finalidades, com abrangência dos dados pertinentes, proporcionais e não excessivos.
- **LGPD art. 12 (Anonimização):** Dados anonimizados não são considerados dados pessoais para os fins da Lei, desde que o processo de anonimização não possa ser revertido com esforços razoáveis.
- **ANVISA RDC 657/2022:** Princípio de minimização de dados em dispositivos médicos — coletar apenas o estritamente necessário para a finalidade clínica.

**Aplicação no protótipo:**
- Função `is_anonymized_paciente_id(s)` valida o formato antes de qualquer publicação MQTT.
- Verificações adicionais contra padrões de CPF, e-mail e data para defesa em profundidade.
- Payload JSON contém apenas: `timestamp`, `temperatura`, `umidade`, `bpm` e `paciente_id` — nenhum campo adicional com informações pessoais.
- O valor padrão `PAC-0001` é definido em `secrets.h` e pode ser alterado por dispositivo sem expor dados reais.

---

## 6. Recomendações para Ambiente Produtivo (R15.4)

### 6.1 Gerenciamento de Credenciais

O protótipo armazena credenciais de acesso ao broker MQTT no arquivo `secrets.h`, excluído do versionamento via `.gitignore`. Essa abordagem é aceitável para simulação e desenvolvimento local, mas **não é adequada para produção**.

**Recomendações:**

| Abordagem | Descrição |
|-----------|-----------|
| **Variáveis de ambiente** | Credenciais injetadas no runtime via variáveis de ambiente do SO ou container |
| **AWS Secrets Manager** | Armazenamento gerenciado com rotação automática e auditoria |
| **HashiCorp Vault** | Gerenciamento centralizado com controle de acesso granular |
| **Azure Key Vault** | Integração nativa com Azure IoT Hub |

**Práticas recomendadas em produção:**
- Rotação automática de credenciais a cada 90 dias (mínimo).
- Auditoria de acesso (quem acessou, quando, de onde).
- Princípio do menor privilégio (cada dispositivo acessa apenas seus próprios segredos).
- Revogação imediata em caso de comprometimento.
- Migração de usuário/senha para certificados X.509 (mTLS) para eliminação de credenciais estáticas.

### 6.2 Segregação de Rede

Em ambiente hospitalar de produção:
- **VLANs dedicadas** para dispositivos IoT médicos, isolados dos demais sistemas.
- **Firewall e ACLs** permitindo apenas tráfego MQTT (porta 8883/TLS) de saída.
- **Zero Trust Architecture** com autenticação e autorização independentes por requisição.
- **Monitoramento SIEM** integrado aos logs do broker para detecção de anomalias.

---

## 7. Limitações do Ambiente de Simulação (R15.5)

### 7.1 Limitação de TLS no Wokwi

O ambiente de simulação Wokwi apresenta limitações técnicas que impactam a implementação completa de TLS:

| Limitação | Impacto | Mitigação |
|-----------|---------|-----------|
| Validação de certificados CA pode não ser suportada | Impossibilidade de usar `setCACert()` com verificação completa | Uso de `setInsecure()` como fallback |
| Ausência de hardware criptográfico real | Operações TLS emuladas sem garantia de entropia adequada | Aceitável para fins pedagógicos |
| Rede simulada sem interceptação real | Impossibilidade de demonstrar ataques MITM | Documentação teórica do risco |

### 7.2 Justificativa Pedagógica

O uso de `setInsecure()` no ambiente Wokwi é uma concessão exclusivamente didática que permite ao protótipo demonstrar o fluxo completo de comunicação MQTT com TLS (handshake, cifração do canal) sem bloquear a execução por falha na validação de certificados em ambiente simulado.

O código-fonte contém comentário explícito:

```cpp
// [LIMITAÇÃO SIMULAÇÃO] Wokwi não suporta validação completa de CA.
// Em produção, substituir por: wifiClient.setCACert(hivemq_ca_cert);
wifiClient.setInsecure();
```

### 7.3 Recomendação Produtiva Equivalente

Em ambiente de produção com ESP32 físico:

1. Obter o certificado CA raiz do HiveMQ Cloud (ISRG Root X1 ou equivalente).
2. Embutir o certificado no firmware via `setCACert(ca_cert)`.
3. Validar que a conexão TLS falha caso o certificado do servidor não seja assinado pela CA esperada.
4. Considerar certificate pinning para ambientes de alta segurança.
5. Implementar atualização OTA (Over-The-Air) para renovação de certificados.

---

## 8. Integração com Grafana Cloud (Opcional — R14.5)

Embora o dashboard principal do protótipo seja implementado em Node-RED, o sistema é compatível com integração ao Grafana Cloud para cenários de monitoramento avançado:

- **Fonte de dados:** O Node-RED pode encaminhar os dados validados para um banco de dados de séries temporais (InfluxDB, Prometheus) que serve como datasource do Grafana.
- **Painéis:** Gráficos de BPM e temperatura com alertas configuráveis, histórico de longo prazo e correlação entre métricas.
- **Vantagens:** Retenção de dados de longo prazo, alertas por e-mail/Slack, dashboards compartilháveis e controle de acesso por equipe.

Esta integração não foi implementada no protótipo atual, mas a arquitetura MQTT permite sua adição sem modificações no firmware.

---

## 9. Referências

- **LGPD — Lei 13.709/2018:** Lei Geral de Proteção de Dados Pessoais. Disponível em: [planalto.gov.br](http://www.planalto.gov.br/ccivil_03/_ato2015-2018/2018/lei/l13709.htm)
- **ANVISA RDC 657/2022:** Resolução sobre requisitos de segurança cibernética para dispositivos médicos.
- **OASIS MQTT v3.1.1 / v5.0:** Especificação do protocolo MQTT, incluindo transporte seguro (porta 8883). Disponível em: [docs.oasis-open.org/mqtt](https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
- **HiveMQ Cloud Documentation:** Configuração de TLS e gerenciamento de credenciais. Disponível em: [hivemq.com/docs](https://www.hivemq.com/docs/hivemq-cloud/introduction.html)
- **Node-RED Documentation:** Nós de dashboard e configuração MQTT. Disponível em: [nodered.org/docs](https://nodered.org/docs/)
- **NIST SP 800-183:** Networks of 'Things' — referência para segurança em IoT.
- **ESP-IDF TLS Documentation:** Configuração de WiFiClientSecure e certificados. Disponível em: [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/esp_tls.html)
