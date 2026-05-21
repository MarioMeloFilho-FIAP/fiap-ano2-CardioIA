# Segurança e LGPD – CardioIA IoT (Fase 3)

## Introdução

O módulo **CardioIA – Monitoramento IoT** opera no domínio de dispositivos médicos vestíveis,
coletando sinais vitais (temperatura, umidade e batimentos cardíacos simulados) de pacientes
cardiológicos e transmitindo-os para a nuvem via protocolo MQTT. Esse cenário exige atenção
especial a dois marcos regulatórios:

- **LGPD (Lei 13.709/2018)** — Lei Geral de Proteção de Dados Pessoais, que estabelece
  princípios de finalidade, adequação, necessidade, segurança e prevenção no tratamento de
  dados pessoais, incluindo dados sensíveis de saúde (art. 5, II; art. 11).
- **ANVISA RDC 657/2022** — Resolução que dispõe sobre requisitos de segurança cibernética
  para dispositivos médicos, exigindo integridade, confidencialidade e disponibilidade dos
  dados clínicos em trânsito e em repouso.

Este documento detalha as boas práticas de segurança aplicadas ao protótipo, as recomendações
para ambiente produtivo e as limitações conhecidas do ambiente de simulação (Wokwi), em
conformidade com os Requisitos 15.1 a 15.5 da especificação do projeto.

---

## Boas práticas aplicadas ao protótipo (R15.3)

### 1. Autenticação forte no broker MQTT

**Descrição:**
Toda conexão do ESP32 ao broker MQTT (HiveMQ Cloud) exige autenticação por usuário e senha
exclusivos por dispositivo. Sem credenciais válidas, o broker recusa a conexão e nenhum dado
é publicado ou consumido.

**Justificativa clínica/regulatória:**
- **LGPD art. 46** — O agente de tratamento deve adotar medidas de segurança, técnicas e
  administrativas aptas a proteger os dados pessoais de acessos não autorizados.
- **ANVISA RDC 657/2022** — Controle de acesso é requisito essencial para dispositivos
  médicos conectados, impedindo que atores não autorizados injetem ou interceptem dados
  clínicos.

**Aplicação no protótipo:**
- As credenciais (`MQTT_USER`, `MQTT_PASSWORD`) são definidas no arquivo `secrets.h`, que
  **não é versionado** (listado em `.gitignore`).
- O arquivo `secrets.example.h` fornece placeholders para orientar a configuração local.
- Cada dispositivo deve possuir credenciais individuais; em produção, recomenda-se rotação
  periódica de senhas (mínimo a cada 90 dias) e uso de certificados X.509 (mTLS) em vez de
  usuário/senha.
- Caso o broker rejeite a autenticação, o firmware entra no estado `AUTH_SUSPENDED` e
  suspende tentativas automáticas de reconexão até intervenção externa (R8.6).

---

### 2. Criptografia em trânsito (TLS 1.2+) — R15.1

**Descrição:**
Toda comunicação entre o ESP32 e o broker MQTT HiveMQ Cloud é realizada exclusivamente pela
porta **8883** com TLS (Transport Layer Security) versão **1.2 ou superior**. Isso garante
que os dados clínicos em trânsito sejam cifrados ponta a ponta, impedindo interceptação
(eavesdropping) e adulteração (tampering) por terceiros.

**Justificativa clínica/regulatória:**
- **LGPD art. 6, VII — Segurança:** Utilização de medidas técnicas e administrativas aptas
  a proteger os dados pessoais de acessos não autorizados e de situações acidentais ou
  ilícitas de destruição, perda, alteração, comunicação ou difusão.
- **LGPD art. 6, VIII — Prevenção:** Adoção de medidas para prevenir a ocorrência de danos
  em virtude do tratamento de dados pessoais.
- **ANVISA RDC 657/2022 — Integridade e Confidencialidade:** Dispositivos médicos
  conectados devem garantir que dados clínicos não sejam alterados ou expostos durante a
  transmissão, utilizando protocolos criptográficos reconhecidos.

**Aplicação no protótipo:**
- O firmware utiliza a classe `WiFiClientSecure` do ESP32 para estabelecer conexão TLS.
- Quando disponível, o certificado CA do HiveMQ Cloud é carregado via `setCACert()` para
  validação completa da cadeia de certificados.
- Em ambiente de simulação (Wokwi), onde a validação de certificados pode não ser suportada,
  utiliza-se `setInsecure()` como fallback documentado (ver seção "Limitações conhecidas").
- A porta 8883 é a porta padrão MQTT sobre TLS, conforme especificação OASIS MQTT v3.1.1 e
  v5.0.

---

### 3. Anonimização e minimização de dados do paciente — R15.2

**Descrição:**
O sistema utiliza exclusivamente um identificador anonimizado no campo `paciente_id`,
seguindo o formato `^PAC-\d{1,27}$` (exemplo: `PAC-0001`). Nenhum dado pessoalmente
identificável (PII) real do paciente é coletado, armazenado ou transmitido pelo protótipo.
São explicitamente rejeitados:

- CPF (formato `\d{3}\.?\d{3}\.?\d{3}-?\d{2}`)
- E-mail (formato `[^@\s]+@[^@\s]+\.[^@\s]+`)
- Data de nascimento (formato `dd/mm/aaaa`)
- RG e sequências numéricas que possam identificar o paciente

**Justificativa clínica/regulatória:**
- **LGPD art. 6, III — Necessidade:** Limitação do tratamento ao mínimo necessário para a
  realização de suas finalidades, com abrangência dos dados pertinentes, proporcionais e
  não excessivos.
- **LGPD art. 12 — Anonimização:** Dados anonimizados não são considerados dados pessoais
  para os fins da Lei, desde que o processo de anonimização não possa ser revertido com
  esforços razoáveis.
- **Property P18 (design.md):** Define formalmente a bicondicional de aceitação do
  `paciente_id`, garantindo que apenas identificadores no formato canônico `PAC-\d{1,27}`
  sejam aceitos pelo sistema.

**Aplicação no protótipo:**
- A função `is_anonymized_paciente_id(s)` valida o formato antes de qualquer publicação
  MQTT, rejeitando strings que não casem com o padrão canônico.
- Defesa em profundidade: mesmo que a regex principal seja satisfeita, verificações
  adicionais contra padrões de CPF, e-mail e data são executadas para robustez futura.
- O valor padrão `PAC-0001` é definido em `secrets.h` e pode ser alterado por dispositivo
  sem expor dados reais do paciente.
- O payload JSON publicado contém apenas: `timestamp`, `temperatura`, `umidade`, `bpm` e
  `paciente_id` — nenhum campo adicional com informações pessoais é incluído (minimização
  de dados).

---

## Recomendações para ambiente produtivo (R15.4)

### Uso de variáveis de ambiente e secrets manager

O protótipo armazena credenciais de acesso ao broker MQTT (usuário, senha, host) no arquivo
`secrets.h`, que é excluído do versionamento via `.gitignore`. Essa abordagem é aceitável
para fins de simulação e desenvolvimento local, mas **não é adequada para produção**.

**Recomendações para ambiente produtivo:**

| Abordagem | Descrição | Exemplo |
|-----------|-----------|---------|
| **Variáveis de ambiente** | Credenciais injetadas no runtime via variáveis de ambiente do sistema operacional ou do container | `export MQTT_PASSWORD=$(vault read secret/cardioia/mqtt)` |
| **AWS Secrets Manager** | Serviço gerenciado para armazenamento, rotação automática e auditoria de segredos | Integração via SDK no firmware de produção (ESP-IDF + AWS IoT Core) |
| **HashiCorp Vault** | Solução open-source para gerenciamento centralizado de segredos com controle de acesso granular | Políticas por dispositivo, rotação automática, audit log |
| **Azure Key Vault / GCP Secret Manager** | Alternativas em nuvem com integração nativa aos respectivos ecossistemas IoT | Azure IoT Hub + Key Vault, GCP IoT Core + Secret Manager |

**Justificativa (R15.4):** Credenciais hardcoded em código-fonte, mesmo que não versionadas,
representam risco de exposição acidental (backup, compartilhamento de máquina, engenharia
reversa do binário). Em produção, o ciclo de vida dos segredos deve ser gerenciado por
infraestrutura dedicada com:

- Rotação automática periódica (mínimo 90 dias)
- Auditoria de acesso (quem acessou, quando)
- Princípio do menor privilégio (cada dispositivo acessa apenas seus próprios segredos)
- Revogação imediata em caso de comprometimento

### Segregação de rede e controle de acesso

Em ambiente produtivo de IoT médico, recomenda-se:

- **VLANs dedicadas:** Isolar o tráfego dos dispositivos IoT médicos em segmentos de rede
  separados dos demais sistemas hospitalares, reduzindo a superfície de ataque.
- **Firewall e ACLs:** Permitir apenas tráfego MQTT (porta 8883/TLS) de saída dos
  dispositivos para o broker, bloqueando qualquer outra comunicação não autorizada.
- **mTLS (Mutual TLS):** Em produção, substituir autenticação por usuário/senha por
  certificados X.509 bidirecionais, onde tanto o cliente (ESP32) quanto o servidor (broker)
  apresentam certificados válidos, eliminando o risco de credenciais estáticas.
- **Zero Trust Architecture:** Cada requisição deve ser autenticada e autorizada
  independentemente, sem confiar na rede como perímetro de segurança.
- **Monitoramento e SIEM:** Integrar logs do broker MQTT a um sistema de detecção de
  intrusão (IDS) e correlação de eventos de segurança para identificar anomalias em tempo
  real.

---

## Limitações conhecidas do ambiente de simulação (R15.5)

O ambiente de simulação **Wokwi** apresenta limitações técnicas que impactam a implementação
completa de TLS no protótipo:

| Limitação | Impacto | Mitigação no protótipo |
|-----------|---------|------------------------|
| Validação de certificados CA pode não ser suportada pelo simulador | Impossibilidade de usar `setCACert()` com verificação completa da cadeia | Uso de `setInsecure()` como fallback, desabilitando a verificação de certificado do servidor |
| Ausência de hardware criptográfico real | Operações TLS são emuladas, sem garantia de entropia adequada para geração de chaves | Aceitável para fins pedagógicos; em produção, o ESP32 possui acelerador criptográfico nativo |
| Rede simulada sem interceptação real | Não é possível demonstrar ataques man-in-the-middle no simulador | Documentação teórica do risco e da proteção oferecida por TLS |

**Justificativa pedagógica:**
O uso de `setInsecure()` no ambiente Wokwi é uma concessão exclusivamente didática que permite
ao protótipo demonstrar o fluxo completo de comunicação MQTT com TLS (handshake, cifração do
canal) sem bloquear a execução por falha na validação de certificados em ambiente simulado.
O código-fonte contém comentário explícito indicando essa limitação:

```cpp
// [LIMITAÇÃO SIMULAÇÃO] Wokwi não suporta validação completa de CA.
// Em produção, substituir por: wifiClient.setCACert(hivemq_ca_cert);
wifiClient.setInsecure();
```

**Recomendação produtiva equivalente:**
Em ambiente de produção com ESP32 físico conectado ao HiveMQ Cloud:

1. Obter o certificado CA raiz do HiveMQ Cloud (ISRG Root X1 ou equivalente).
2. Embutir o certificado no firmware via `setCACert(ca_cert)`.
3. Validar que a conexão TLS falha caso o certificado do servidor não seja assinado pela CA
   esperada (proteção contra man-in-the-middle).
4. Considerar certificate pinning para ambientes de alta segurança.
5. Monitorar a expiração dos certificados e implementar mecanismo de atualização OTA
   (Over-The-Air) para renovação.

---

## Referências

- **LGPD — Lei 13.709/2018:** [planalto.gov.br/ccivil_03/_ato2015-2018/2018/lei/l13709.htm](http://www.planalto.gov.br/ccivil_03/_ato2015-2018/2018/lei/l13709.htm)
- **ANVISA RDC 657/2022:** Resolução sobre requisitos de segurança cibernética para
  dispositivos médicos.
- **OASIS MQTT v3.1.1:** Especificação do protocolo MQTT, seção sobre transporte seguro
  (porta 8883).
- **HiveMQ Cloud — TLS Configuration:** Documentação oficial sobre conexão TLS com
  certificados CA.
- **NIST SP 800-183:** Networks of 'Things' — referência para segurança em IoT.
- **Property P18 (design.md):** Definição formal da bicondicional de anonimização do
  `paciente_id`.
