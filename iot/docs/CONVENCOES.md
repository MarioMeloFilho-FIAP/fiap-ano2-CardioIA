# Convenções de Código – CardioIA IoT (Fase 3)

Este documento descreve as convenções obrigatórias de código adotadas no firmware
C++ do projeto **CardioIA – Monitoramento IoT (Fase 3)**, conforme os Requisitos
12.1, 12.2, 12.3 e 12.4. As convenções garantem legibilidade, rastreabilidade de
autoria e compreensão da lógica clínica por avaliadores e futuros mantenedores.

---

## Cabeçalho obrigatório do arquivo principal do firmware (R12.1)

O arquivo `iot/firmware/src/main.cpp` deve conter, **nas primeiras 30 linhas**,
um bloco de comentário com quatro elementos obrigatórios:

| # | Elemento                          | Formato / Exemplo                        |
|---|-----------------------------------|------------------------------------------|
| a | Nome do projeto                   | `CardioIA`                               |
| b | Identificação da fase             | `Fase 3` (texto literal)                 |
| c | Integrantes com RM                | Nome completo + `RM` seguido de 6 dígitos |
| d | Data de criação                   | `AAAA-MM-DD` (ISO 8601)                  |

### Template utilizado

```cpp
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
```

**Regras:**

- O bloco deve iniciar na **linha 1** do arquivo.
- Todos os integrantes do grupo devem estar listados com nome completo e RM.
- A data segue o formato ISO `AAAA-MM-DD` (ex.: `2025-01-15`).
- A seção "Descrição" é opcional, mas recomendada para contextualizar o módulo.

---

## Bloco único de constantes clínicas (R12.3)

O arquivo `iot/firmware/src/config.h` centraliza **todas** as constantes clínicas
e estruturais do firmware em um **único bloco contíguo** (gap máximo de 10 linhas
entre declarações consecutivas). Cada constante é declarada como `constexpr` e
precedida por um comentário em português brasileiro indicando:

1. A **finalidade funcional** da constante.
2. A **unidade de medida** aplicável.

### Convenção de nomenclatura

- Nomes em `UPPER_SNAKE_CASE`.
- Tipo explícito (`int`, `float`) — sem `#define`.
- Namespace `cardioia::` para evitar colisões.

### Bloco de constantes (extraído de `config.h`)

```cpp
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
```

### Constantes estruturais (mesmo arquivo, logo abaixo)

```cpp
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
// entre duas bordas aceitas pela ISR do botão de ``Connectivity_Flag`` (R6.2).
constexpr int DEBOUNCE_CONECTIVIDADE_MS = 50;
```

**Regras:**

- Cada `constexpr` deve ter **pelo menos uma linha de comentário `//`** acima
  contendo a unidade de medida (`bpm`, `°C`/`Celsius`, `amostras`, `ms`/`milissegundos`).
- As quatro constantes clínicas (`BPM_THRESHOLD`, `TEMP_THRESHOLD`,
  `BUFFER_LIMIT`, `SAMPLING_INTERVAL_MS`) devem estar no mesmo bloco, com no
  máximo 10 linhas de separação entre declarações consecutivas.
- Nenhuma dependência de `Arduino.h` — apenas tipos padrão (`<cstdint>`).

---

## Comentário acima de cada função (R12.2)

Toda função definida no firmware (exceto `setup()` e `loop()` quando vazias)
deve ser precedida por um **bloco de comentário em português brasileiro**
contendo:

1. **Nome da função** (identificador).
2. **Finalidade** — descrição concisa do que a função faz.
3. **Parâmetros** — lista com nome e descrição de cada parâmetro de entrada.
4. **Retorno** — descrição do valor devolvido.

### Formato padrão

```cpp
// -----------------------------------------------------------------------------
// Função:     nome_da_funcao
// Finalidade: Descrição concisa do propósito da função.
// Parâmetros:
//   - ``param1``: descrição do primeiro parâmetro.
//   - ``param2``: descrição do segundo parâmetro.
// Retorno:    Descrição do valor retornado.
// -----------------------------------------------------------------------------
```

### Exemplo real (extraído de `sensor_driver.h`)

```cpp
// -----------------------------------------------------------------------------
// Função:     format_reading
// Finalidade: Formata a linha do Monitor Serial para uma leitura
//             **válida**. A string retornada DEVE conter, como
//             substrings: ``"DHT22_Sensor"``, a representação decimal
//             de ``ts_ms``, ``temperatura`` com uma casa decimal seguida
//             de ``"°C"`` e ``umidade`` inteira seguida de ``"%"``
//             (R1.2, R1.4 / P7).
// Parâmetros:
//   - ``ts_ms``:       timestamp da leitura em milissegundos (``>= 0``).
//   - ``temperatura``: temperatura válida em °C.
//   - ``umidade``:     umidade válida em %, inteiro ``[0; 100]``.
// Retorno:    :class:`std::string` no formato
//             ``"[DHT22_Sensor] ts=<ts> temperatura=<T>°C umidade=<H>%"``.
// -----------------------------------------------------------------------------
std::string format_reading(std::uint32_t ts_ms, float temperatura, int umidade);
```

### Exemplo real (extraído de `logger.cpp`)

```cpp
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
```

**Regras:**

- O comentário deve estar em **português brasileiro**.
- Deve aparecer **imediatamente acima** da declaração/definição da função.
- Para métodos de classe, usar `Método:` em vez de `Função:`.
- Construtores usam `Construtor:` e indicam `(construtor)` no campo Retorno.

---

## Comentários em estruturas de controle de fluxo sensíveis (R12.4)

Toda estrutura de controle (`if`, `while`, `for`, `switch`) que implemente
**lógica de decisão sobre sinais clínicos** ou **processamento do buffer de
amostras** deve ser precedida por um comentário em português brasileiro
descrevendo:

1. A **condição avaliada**.
2. O **comportamento resultante esperado**.

### Exemplo 1 — Roteamento por Connectivity_Flag (extraído de `main.cpp`)

```cpp
// Roteamento via SyncScheduler (R7.3, Property P8):
// Se Connectivity_Flag é true E o EdgeBuffer está vazio, a amostra
// é publicada diretamente no MQTT. Caso contrário, é enfileirada
// no EdgeBuffer para sincronização posterior.
g_sync_scheduler.enviarOuEnfileirar(record, g_connectivity.flag());
```

### Exemplo 2 — FIFO global do EdgeBuffer (extraído de `edge_buffer.cpp`)

```cpp
// Decisão: enquanto o tamanho total já alcançou a capacidade
// global, aplicamos FIFO para liberar espaço para o novo registro
// (R5.2/R5.3). ``while`` em vez de ``if`` por robustez: se a
// capacidade mudar em runtime, ainda convergimos para o estado
// correto antes de inserir.
while (size() >= capacity_) {
    drop_oldest_for_fifo();
    evicted = true;
}
```

### Exemplo 3 — Fallback para RAM em erro de I/O (extraído de `edge_buffer.cpp`)

```cpp
// Decisão: tentamos o primário primeiro; em erro de I/O caímos
// para o fallback RAM (R4.4 / Property 9).
if (!primary_.append(record)) {
    ++primary_io_errors_;
    // ...
}
```

### Exemplo 4 — Drenagem de pulsos pendentes (extraído de `main.cpp`)

```cpp
// Drena pulsos pendentes da ISR do Pulse_Simulator (R2.1, R2.4).
// A ISR apenas incrementa o contador volatile; o loop principal
// registra cada pulso na BpmWindow com o timestamp atual.
while (cardioia::g_pulse_pending_count > 0) {
    cardioia::g_pulse_pending_count--;
    g_sensor_driver.registrar_pulso(now);
}
```

**Regras:**

- O comentário deve estar **imediatamente acima** da estrutura de controle.
- Deve ser em **português brasileiro**.
- Aplica-se a decisões sobre: limites clínicos (BPM, temperatura), estado do
  buffer (cheio, vazio, 80%), conectividade (online/offline), sincronização
  (replay, aborto) e validação de leituras de sensores.
- Estruturas triviais (ex.: `if (ptr == nullptr) return;`) não exigem
  comentário, a menos que envolvam lógica clínica ou de buffer.

---

## Padrão de mensagens no Monitor Serial

Todas as mensagens emitidas pelo firmware seguem o formato estruturado do
`Logger`:

```
[T+<uptime_ms>][COMPONENTE][NIVEL] mensagem (extras)
```

| Campo         | Descrição                                                    |
|---------------|--------------------------------------------------------------|
| `T+<ms>`      | Uptime em milissegundos desde o boot (`millis()`)            |
| `COMPONENTE`  | Identificador do módulo emissor (ex.: `EdgeBuffer`, `main`)  |
| `NIVEL`       | `INFO`, `WARN` ou `ERROR`                                    |
| `mensagem`    | Descrição curta do evento                                    |
| `(extras)`    | Dados adicionais opcionais (ex.: `size=48/50,evictions=3`)   |

### Exemplos de saída

```
[T+5023][SensorDriver][INFO] [DHT22_Sensor] ts=5023 temperatura=36.7°C umidade=58%
[T+5023][EdgeBuffer][INFO] descarte por limite de buffer (FIFO) (size=49/50,evictions=1)
[T+5023][EdgeBuffer][WARN] aviso_80_pct (RAM fallback cruzou 80% da capacidade) (size=40/50)
[T+5023][Connectivity][INFO] Reconexão — buffer pendente: 12 amostras
[T+5023][MqttClient][ERROR] erro de autenticação — suspendendo reconexão automática
```

**Regras:**

- O buffer interno do Logger é de 256 bytes — mensagens mais longas são
  truncadas silenciosamente.
- Não usar a classe `String` do Arduino (evitar fragmentação de heap).
- O prefixo `[T+<ms>]` usa uptime relativo ao boot; em iteração futura com
  NTP, será substituído por `[YYYY-MM-DDThh:mm:ss.mmm]`.

---

## Validação automática via `test_convencoes.py`

O arquivo `iot/tests/test_convencoes.py` valida automaticamente as convenções
R12.1 e R12.3 por meio de inspeção estática dos arquivos-fonte. As convenções
R12.2 e R12.4 requerem **revisão humana** por serem contextuais.

### O que é validado automaticamente

| Requisito | Verificação                                                         | Método                          |
|-----------|---------------------------------------------------------------------|---------------------------------|
| R12.1(a)  | `"CardioIA"` presente nas 30 primeiras linhas de `main.cpp`        | Busca textual                   |
| R12.1(b)  | `"Fase 3"` presente nas 30 primeiras linhas de `main.cpp`          | Busca textual                   |
| R12.1(c)  | Pelo menos um `RM` + 6 dígitos nas 30 primeiras linhas             | Regex `RM\d{6}`                 |
| R12.1(d)  | Data no formato `AAAA-MM-DD` nas 30 primeiras linhas               | Regex `\b\d{4}-\d{2}-\d{2}\b`  |
| R12.3(i)  | As 4 constantes declaradas como `constexpr ... NOME = ...;`        | Regex por constante             |
| R12.3(ii) | Comentário `//` adjacente (até 2 linhas acima) com unidade         | Busca case-insensitive          |
| R12.3(iii)| Gap máximo de 10 linhas entre declarações consecutivas             | Cálculo de posições             |

### O que requer revisão humana

| Requisito | Verificação                                                         | Motivo                          |
|-----------|---------------------------------------------------------------------|---------------------------------|
| R12.2     | Comentário pt-BR acima de cada função com parâmetros e retorno     | Semântica contextual            |
| R12.4     | Comentário pt-BR acima de `if`/`while` sobre sinais ou buffer      | Depende do domínio da decisão   |

### Como executar

```bash
# Ativa o venv e roda apenas os testes de convenções
source fiap_ano2_fase2_cap1_venv/bin/activate
python -m pytest iot/tests/test_convencoes.py -v

# Ou via Makefile (roda toda a suíte incluindo convenções)
make iot-test
```

Os testes usam `pytest.mark.skipif` para pular graciosamente quando os
arquivos-fonte ainda não existem (ex.: antes das Tarefas 18 e 28), permitindo
execução parcial da suíte sem falhas por ausência de artefatos.
