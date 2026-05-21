# CardioIA – Ir Além 1
## API REST, Verificação de Risco e Disparo de E-mail

**Data:** 19 de maio de 2026

---

## 1. Objetivo

Descrever a implementação de uma API REST para recebimento de dados vitais
simulados de um ESP32, com verificação de risco em tempo real e simulação
de disparo de e-mail para alertas cardíacos.

---

## 2. Fluxo Implementado

### 2.1 Coleta de Dados

Foram geradas **30 amostras** simuladas contendo:

- **timestamp** – instante da medição
- **paciente_id** – identificador do paciente (PAC-0001)
- **temperatura** – temperatura corporal (°C)
- **umidade** – umidade ambiente (%)
- **bpm** – batimentos cardíacos por minuto
- **conectado** – status de conexão do dispositivo

Cada amostra foi enviada via **POST** para a API e armazenada em memória.

### 2.2 API REST (Flask)

| Método | Endpoint | Descrição |
|--------|----------|-----------|
| POST | `/api/sinais` | Receber dado do ESP32 |
| GET | `/api/sinais` | Listar registros |
| GET | `/api/estatisticas` | Médias, máximos e mínimos |
| GET | `/api/alertas` | Listar alertas |
| GET | `/api/health` | Health check |

### 2.3 Verificação de Risco

- **BPM > 120** → Alerta de **TAQUICARDIA**
- **BPM < 50** → Alerta de **BRADICARDIA**
- **Temperatura > 38°C** → Alerta de **FEBRE**

### 2.4 Disparo de E-mail

Quando um risco é detectado, e-mail simulado enviado para
**medico@cardioia.com** com dados do paciente e tipo de ocorrência.

### 2.5 Dashboard

Visualizações geradas ao final:
- Série temporal de BPM com limites de alerta
- Série temporal de temperatura com linha de febre
- Dispersão BPM x Temperatura
- Proporção de alertas (pizza)

---

## 3. Tecnologias Utilizadas

- **Python + Flask** – API REST
- **requests** – Cliente HTTP
- **Pandas + Matplotlib** – Análise e visualização
- **Threading** – Servidor em background no Colab

---

## 4. Resultados

- 30 amostras processadas com sucesso
- Pipeline completo: coleta → armazenamento → verificação → alerta
- Taxa de alertas variável conforme injeção de anomalias

---

## 5. Melhorias Futuras

- Integração com broker MQTT real (Mosquitto)
- E-mail real com smtplib
- Dashboard em tempo real com WebSockets
- Banco de dados persistente (SQLite/MongoDB)
