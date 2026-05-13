# Relatório Parte 1 — Edge Computing & Firmware Embarcado

**Projeto:** CardioIA — Monitoramento IoT (Fase 3)  
**Grupo:**  
- Mário Melo Filho — RM563769  
- Stephanie Dias dos Santos — RM564315  

**Data:** 2025-01-15  
**Disciplina:** FIAP — 2º Ano — Fase 3 (Edge Computing & IoT)

---

## 1. Visão Geral do Firmware

O firmware do CardioIA executa em um ESP32 simulado no Wokwi e é responsável pela captura contínua de sinais vitais, processamento local (edge computing) e publicação em nuvem via MQTT. A arquitetura segue o princípio **edge-first, cloud-ready**: toda a lógica crítica — validação de sensores, cálculo de BPM, buffer offline e serialização — é executada localmente no microcontrolador, sem depender de conectividade para funcionar corretamente.

O ponto de entrada do firmware é o arquivo `main.cpp`, que orquestra dois métodos fundamentais do framework Arduino:

### 1.1 Função `setup()`

Executada uma única vez no boot do ESP32, a função `setup()` inicializa todos os módulos na seguinte ordem de dependência:

1. **Serial** — comunicação a 115200 baud para o Monitor Serial.
2. **ConfigManager** — carrega do SPIFFS a última configuração clínica válida (limites de BPM, temperatura, tamanho do buffer e intervalo de amostragem).
3. **SensorDriver** — configura o pino GPIO 15 para o DHT22 e o GPIO 4 para a ISR do botão de pulso cardíaco.
4. **ConnectivityController** — configura o GPIO 2 para o botão de simulação Wi-Fi e inicializa `Connectivity_Flag = true`.
5. **MqttClient** — configura host, porta (8883/TLS), credenciais e client-id para o HiveMQ Cloud.
6. **SyncScheduler hook** — registra o callback que dispara a sincronização automática quando a conectividade é restabelecida.

### 1.2 Função `loop()`

O ciclo principal é executado continuamente e utiliza um **timer não bloqueante** baseado em `millis()` para respeitar o intervalo de amostragem de 5 segundos (`SAMPLING_INTERVAL_MS = 5000`). A cada iteração do loop, o firmware:

1. Processa pendências da ISR do botão de conectividade (debounce de 50 ms).
2. Verifica e trata comandos recebidos pelo Monitor Serial (`ONLINE`, `OFFLINE`, `STATUS`, `CONFIG_SHOW`).
3. Quando o intervalo de amostragem é atingido:
   - Lê temperatura e umidade do DHT22 via `SensorDriver`.
   - Calcula o BPM atual na janela deslizante de 60 s via `BpmWindow`.
   - Compõe o `Sample_Record` em JSON canônico via `SampleBuilder`.
   - Roteia a amostra: se online e buffer vazio, publica diretamente no MQTT; caso contrário, enfileira no `EdgeBuffer`.
4. Executa o tick do `SyncScheduler` para replay de amostras pendentes.
5. Processa keepalive e PUBACKs do MQTT.
6. Drena pulsos pendentes da ISR do botão de pulso cardíaco para a `BpmWindow`.

---

## 2. Captura de Sensores

### 2.1 DHT22 — Temperatura e Umidade

O sensor DHT22 é lido a cada ciclo de amostragem (5 s). O `SensorDriver` implementa as seguintes validações:

- **Faixa operacional:** temperatura entre −40,0 °C e 80,0 °C; umidade entre 0% e 100%.
- **Detecção de NaN:** leituras que retornam `NaN` são descartadas imediatamente.
- **Log de erro:** toda leitura inválida gera uma mensagem no Monitor Serial contendo o identificador `"DHT22_Sensor"`, o tipo de falha (`NaN` ou `out_of_range`) e o valor rejeitado.
- **Alerta persistente:** após 3 leituras consecutivas inválidas, o firmware emite um alerta de falha persistente do sensor e continua tentando no próximo ciclo.

### 2.2 Pulse_Simulator — Batimentos Cardíacos

O simulador de batimentos utiliza um push-button no Wokwi. Cada pressão (borda de subida) é detectada por uma ISR (Interrupt Service Routine) com **debounce de 150 ms** para eliminar ruído mecânico. Os timestamps dos pulsos válidos alimentam a classe `BpmWindow`.

A `BpmWindow` implementa uma **janela deslizante de 60 segundos** usando um ring buffer de 300 posições. O cálculo do BPM segue estas regras:

- **Janela plena** (≥ 60 s desde o boot): BPM = número de pulsos nos últimos 60 s, saturado em 250.
- **Extrapolação no boot** (< 60 s): BPM = `round(pulsos × 60000 / elapsed_ms)`, saturado em 250.
- **Sem pulsos** nos últimos 60 s: BPM = 0.

---

## 3. Composição do Sample_Record

A cada ciclo de amostragem, o `SampleBuilder` compõe um registro estruturado em JSON canônico com exatamente cinco campos, na seguinte ordem fixa:

```json
{
  "timestamp": 1737212812345,
  "temperatura": 36.7,
  "umidade": 58,
  "bpm": 74,
  "paciente_id": "PAC-0001"
}
```

**Regras de composição:**

| Campo         | Tipo              | Faixa                    | Observação                          |
|---------------|-------------------|--------------------------|-------------------------------------|
| `timestamp`   | inteiro ≥ 0       | ms desde boot (`millis()`) | Identificador temporal da amostra |
| `temperatura` | float \| `null`   | −40,0 a 80,0 °C         | `null` se leitura indisponível      |
| `umidade`     | inteiro \| `null` | 0 a 100 %               | `null` se leitura indisponível      |
| `bpm`         | inteiro \| `null` | 0 a 250                  | Saturado em 250                     |
| `paciente_id` | string            | ≤ 32 caracteres          | Anonimizado (ex: `PAC-0001`)        |

O JSON compacto (sem espaços) é limitado a **256 caracteres**. Caso a serialização ultrapasse esse limite, o registro é descartado e o descarte é sinalizado no Monitor Serial. Campos indisponíveis (sensor com falha) são representados como `null` no JSON.

---

## 4. Estratégia de Resiliência Offline (Edge Computing)

### 4.1 EdgeBuffer — FIFO Limitado

O `EdgeBuffer` é o componente central da estratégia de edge computing do CardioIA. Ele garante que **nenhuma leitura seja perdida** durante falhas de conectividade, armazenando localmente as amostras até que a conexão seja restabelecida.

**Características do EdgeBuffer:**

- **Política FIFO (First In, First Out):** quando o buffer atinge a capacidade máxima, a amostra mais antiga é descartada para dar lugar à nova, garantindo que os dados mais recentes sejam sempre preservados.
- **Capacidade:** `BUFFER_LIMIT = 50` amostras.
- **Armazenamento dual:**
  - **Primário:** arquivo `buffer.ndjson` no sistema de arquivos SPIFFS (uma linha JSON por registro, em ordem cronológica crescente).
  - **Fallback:** `std::deque<SampleRecord>` em memória RAM, ativado automaticamente quando o SPIFFS apresenta erro de I/O.
- **Aviso de capacidade:** quando o storage RAM de fallback atinge 80% da capacidade alocada (40 amostras), um aviso é emitido no Monitor Serial antes do próximo descarte.
- **Contador exposto:** o tamanho atual do buffer é sempre acessível via Monitor Serial (comando `STATUS`), permitindo verificação externa do estado.

### 4.2 Justificativa do Buffer_Limit = 50

A escolha de `BUFFER_LIMIT = 50` foi dimensionada considerando o cenário de monitoramento clínico contínuo:

```
Autonomia offline = BUFFER_LIMIT × SAMPLING_INTERVAL
                  = 50 amostras × 5 segundos
                  = 250 segundos
                  ≈ 4 minutos e 10 segundos
```

**Justificativa clínica e técnica:**

1. **Cobertura de interrupções breves:** em ambientes hospitalares e domiciliares, as falhas de conectividade Wi-Fi são tipicamente transitórias (reinício de roteador, handoff entre access points, interferência momentânea). Um buffer de ~4 minutos cobre a grande maioria desses cenários sem perda de dados.

2. **Footprint de memória controlado:** cada `Sample_Record` serializado ocupa no máximo 256 bytes. O buffer completo consome:
   ```
   50 amostras × ~128 bytes (média) = ~6,4 KB
   ```
   Esse valor é insignificante frente aos 320 KB de SRAM disponíveis no ESP32, deixando ampla margem para a pilha TCP/IP, o runtime do Arduino e as demais estruturas do firmware.

3. **Dados recentes priorizados:** a política FIFO garante que, em desconexões prolongadas (> 4 min), os dados mais antigos são descartados em favor dos mais recentes — comportamento clinicamente preferível, pois o estado atual do paciente é mais relevante que leituras históricas já defasadas.

4. **Sincronização rápida:** ao reconectar, o `SyncScheduler` envia as 50 amostras pendentes com intervalo de 100 ms entre cada uma, completando a sincronização em ~5 segundos — tempo aceitável para não sobrecarregar o broker MQTT.

### 4.3 Simulação de Conectividade

Para validar os fluxos online e offline sem depender de hardware real, o firmware expõe a variável `Connectivity_Flag` controlável por:

- **Botão dedicado** no Wokwi (GPIO 2) com debounce de 50 ms.
- **Comandos Serial:** `ONLINE` e `OFFLINE` (case-insensitive).

Quando `Connectivity_Flag` transita de `false` para `true`, o firmware registra no Monitor Serial a mensagem de reconexão com o número de amostras pendentes e dispara automaticamente a sincronização.

### 4.4 Sincronização ao Reconectar

O `SyncScheduler` orquestra o reenvio das amostras pendentes seguindo estas regras:

- **Início:** até 5 segundos após a transição `false → true`.
- **Intervalo entre envios:** mínimo de 100 ms para não sobrecarregar o broker.
- **Confirmação antes de remover:** cada amostra só é removida do buffer após confirmação de envio bem-sucedido (sem exceção e com retorno da chamada).
- **Aborto seguro:** se ocorrer exceção, timeout > 3 s ou nova transição para offline, a sincronização é interrompida imediatamente e as amostras não confirmadas permanecem no buffer na ordem cronológica original.
- **Retry com limite:** após falha, nova tentativa em 5 s, até 3 tentativas consecutivas. Após isso, a sincronização é suspensa até a próxima transição de conectividade.

---

## 5. Evidências do Monitor Serial

### 5.1 Modo Online — Ciclo Normal de Leitura

Abaixo, exemplo representativo da saída do Monitor Serial durante operação normal com `Connectivity_Flag = true`:

```
[2025-01-15T10:00:00.000][MAIN][INFO] CardioIA Fase 3 — inicializando...
[2025-01-15T10:00:00.100][MAIN][INFO] CardioIA Fase 3 inicializado com sucesso
[2025-01-15T10:00:05.001][DHT22_Sensor][INFO] Leitura válida: timestamp=5001, temperatura=36.5°C, umidade=62%
[2025-01-15T10:00:05.002][BPM_WINDOW][INFO] BPM calculado: 72
[2025-01-15T10:00:05.003][MQTT][INFO] Publicado em cardioia/paciente/PAC-0001/sinais (128 bytes, QoS 1)
[2025-01-15T10:00:10.001][DHT22_Sensor][INFO] Leitura válida: timestamp=10001, temperatura=36.6°C, umidade=61%
[2025-01-15T10:00:10.002][BPM_WINDOW][INFO] BPM calculado: 74
[2025-01-15T10:00:10.003][MQTT][INFO] Publicado em cardioia/paciente/PAC-0001/sinais (128 bytes, QoS 1)
[2025-01-15T10:00:15.001][DHT22_Sensor][INFO] Leitura válida: timestamp=15001, temperatura=36.7°C, umidade=60%
[2025-01-15T10:00:15.002][BPM_WINDOW][INFO] BPM calculado: 75
[2025-01-15T10:00:15.003][MQTT][INFO] Publicado em cardioia/paciente/PAC-0001/sinais (128 bytes, QoS 1)
```

*Figura 1 — Monitor Serial em modo online: cada ciclo de 5 s lê o DHT22, calcula o BPM e publica diretamente no broker MQTT.*

### 5.2 Modo Offline — Enfileiramento no EdgeBuffer

Abaixo, exemplo representativo da saída do Monitor Serial após o comando `OFFLINE` ser enviado:

```
[2025-01-15T10:01:00.000][CONNECTIVITY][WARN] Comando recebido: OFFLINE
[2025-01-15T10:01:00.001][CONNECTIVITY][WARN] Connectivity_Flag: true → false (perda de conexão)
[2025-01-15T10:01:05.001][DHT22_Sensor][INFO] Leitura válida: timestamp=65001, temperatura=36.8°C, umidade=59%
[2025-01-15T10:01:05.002][BPM_WINDOW][INFO] BPM calculado: 78
[2025-01-15T10:01:05.003][EDGE_BUFFER][INFO] Amostra enfileirada no buffer local (tamanho: 1/50)
[2025-01-15T10:01:10.001][DHT22_Sensor][INFO] Leitura válida: timestamp=70001, temperatura=36.9°C, umidade=58%
[2025-01-15T10:01:10.002][BPM_WINDOW][INFO] BPM calculado: 80
[2025-01-15T10:01:10.003][EDGE_BUFFER][INFO] Amostra enfileirada no buffer local (tamanho: 2/50)
[2025-01-15T10:01:15.001][DHT22_Sensor][INFO] Leitura válida: timestamp=75001, temperatura=37.0°C, umidade=57%
[2025-01-15T10:01:15.002][BPM_WINDOW][INFO] BPM calculado: 82
[2025-01-15T10:01:15.003][EDGE_BUFFER][INFO] Amostra enfileirada no buffer local (tamanho: 3/50)
[2025-01-15T10:01:30.000][CONNECTIVITY][INFO] Comando recebido: ONLINE
[2025-01-15T10:01:30.001][CONNECTIVITY][INFO] Connectivity_Flag: false → true (reconexão). Amostras pendentes: 6
[2025-01-15T10:01:35.001][SYNC][INFO] Iniciando sincronização de 6 amostras pendentes...
[2025-01-15T10:01:35.101][SYNC][INFO] Amostra 1/6 sincronizada com sucesso
[2025-01-15T10:01:35.201][SYNC][INFO] Amostra 2/6 sincronizada com sucesso
[2025-01-15T10:01:35.301][SYNC][INFO] Amostra 3/6 sincronizada com sucesso
[2025-01-15T10:01:35.401][SYNC][INFO] Amostra 4/6 sincronizada com sucesso
[2025-01-15T10:01:35.501][SYNC][INFO] Amostra 5/6 sincronizada com sucesso
[2025-01-15T10:01:35.601][SYNC][INFO] Amostra 6/6 sincronizada com sucesso
[2025-01-15T10:01:35.602][SYNC][INFO] Sincronização concluída. Buffer vazio.
```

*Figura 2 — Monitor Serial em modo offline: amostras são enfileiradas no EdgeBuffer e, ao reconectar, sincronizadas automaticamente em ordem cronológica.*

---

## 6. Parâmetros Configuráveis

Os parâmetros clínicos e estruturais do firmware são declarados em um bloco único no arquivo `config.h`:

| Constante              | Valor Padrão | Unidade        | Finalidade                                      |
|------------------------|:------------:|:--------------:|-------------------------------------------------|
| `BPM_THRESHOLD`        | 120          | bpm            | Limite para alerta de BPM elevado               |
| `TEMP_THRESHOLD`       | 38,0         | °C             | Limite para alerta de temperatura elevada       |
| `BUFFER_LIMIT`         | 50           | amostras       | Capacidade máxima do EdgeBuffer offline          |
| `SAMPLING_INTERVAL_MS` | 5000         | milissegundos  | Intervalo entre leituras consecutivas           |

Esses valores podem ser ajustados em tempo de execução via mensagem MQTT no tópico `cardioia/paciente/{paciente_id}/config`, com validação de faixas e persistência entre reinicializações.

---

## 7. Referências

- **Link Wokwi:** \<preencher ao publicar o projeto\>
- **Documentação completa do módulo IoT:** [`iot/README.md`](../README.md)
- **Convenções de código:** [`iot/docs/CONVENCOES.md`](CONVENCOES.md)
- **Segurança e LGPD:** [`iot/docs/SEGURANCA_LGPD.md`](SEGURANCA_LGPD.md)
- **Código-fonte do firmware:** `iot/firmware/src/`
- **Testes property-based (reference model):** `iot/tests/`

---

*Documento gerado como parte dos entregáveis da Parte 1 (Edge Computing) do projeto CardioIA — Fase 3.*
