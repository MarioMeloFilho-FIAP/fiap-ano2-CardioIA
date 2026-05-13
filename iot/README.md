# CardioIA – Módulo de Monitoramento IoT (Fase 3)

## Visão geral

Este módulo estende o repositório `fiap-ano2-CardioIA` com um protótipo de sistema
vestível de monitoramento contínuo de pacientes cardiológicos, simulado no Wokwi com
um ESP32. O módulo é auto-contido e não altera a estrutura existente de `data/`,
`notebooks/`, `scripts/` e `cardioia-portal/`.

O firmware embarcado:

- Captura temperatura e umidade via DHT22 a cada 5 segundos.
- Calcula BPM simulado a partir de um botão (janela deslizante de 60 s).
- Serializa cada leitura como um `Sample_Record` em JSON canônico.
- Mantém um buffer FIFO local de 50 amostras para resiliência offline.
- Publica os dados em um broker MQTT na nuvem (HiveMQ Cloud) sobre TLS 1.2+.
- Sincroniza automaticamente o buffer pendente ao reconectar.

O dashboard Node-RED consome o tópico `cardioia/paciente/{paciente_id}/sinais` e
exibe gráficos de BPM e temperatura, um gauge de umidade e alertas visuais quando os
limites clínicos (BPM > 120 ou temperatura > 38,0 °C) são ultrapassados.

A estrutura do módulo é:

```
iot/
├── README.md                       (este arquivo)
├── firmware/                       (código ESP32 + diagrama Wokwi)
│   └── src/
├── dashboard/                      (fluxo Node-RED e screenshots)
│   └── screenshots/
├── docs/                           (convenções, segurança/LGPD e relatórios)
│   ├── CONVENCOES.md
│   └── SEGURANCA_LGPD.md
└── tests/                          (testes property-based em Python)
```

## Como executar o simulador Wokwi

> Este guia será detalhado após a publicação do projeto no Wokwi. Os passos abaixo
> são o roteiro planejado.

1. Abra o link do projeto Wokwi listado ao final deste README.
2. Clique em **Start the simulation** (▶) para iniciar a simulação do ESP32.
3. Abra a aba **Serial Monitor** para acompanhar o ciclo de leitura, o estado do
   `Connectivity_Flag` e as mensagens de sincronização.
4. Interaja com os botões do diagrama:
   - Botão **Pulso cardíaco** — simula batimentos (cada pressão conta como um
     batimento para o cálculo do BPM).
   - Botão **Wi-Fi ON/OFF** — alterna `Connectivity_Flag` entre `true` e `false`
     para validar os fluxos online e offline.
5. Para testar a publicação MQTT real, configure as credenciais do HiveMQ Cloud no
   arquivo `iot/firmware/src/secrets.h` (não versionado) conforme
   `secrets.example.h`.

## Comandos do Monitor Serial

O firmware aceita comandos textuais via Monitor Serial. Todos os comandos são
case-insensitive e precisam ser enviados com terminador de linha.

| Comando       | Efeito                                                                 |
|---------------|------------------------------------------------------------------------|
| `ONLINE`      | Define `Connectivity_Flag = true` e dispara a sincronização do buffer. |
| `OFFLINE`     | Define `Connectivity_Flag = false` e passa a enfileirar amostras.      |
| `STATUS`      | Imprime o estado atual da conectividade e o tamanho do buffer local.   |
| `CONFIG_SHOW` | Exibe os valores atuais de `BPM_THRESHOLD`, `TEMP_THRESHOLD`, `BUFFER_LIMIT` e `SAMPLING_INTERVAL_MS`. |

Comandos fora desse conjunto preservam o estado atual e geram uma mensagem de
rejeição no Monitor Serial.

## Entregáveis

| # | Artefato | Caminho | Status |
|---|----------|---------|--------|
| 1 | Código C++ comentado | `iot/firmware/src/` | ✅ Completo |
| 2 | Relatório Parte 1 (Edge Computing & ESP32) | `iot/docs/RELATORIO_PARTE1.pdf` | ✅ Completo |
| 3 | Relatório Parte 2 (Cloud & Dashboard) | `iot/docs/RELATORIO_PARTE2.pdf` | ✅ Completo |
| 4 | Fluxo Node-RED exportado | `iot/dashboard/cardioia_flow.json` | ✅ Completo |
| 5 | Screenshots do dashboard | `iot/dashboard/screenshots/` | ✅ Completo |
| 6 | Link permanente Wokwi | (ver abaixo) | ⏳ Pendente publicação do projeto |

**Justificativa (R14.6):** O link permanente do Wokwi será preenchido após a
publicação do projeto na plataforma. Todos os demais entregáveis estão presentes e
completos no repositório.

## Link Wokwi

Link Wokwi: <preencher ao publicar o projeto>
