# FIAP - Faculdade de Informática e Administração Paulista

<p align="center">
<a href= "https://www.fiap.com.br/"><img src="https://github.com/agodoi/templateFiapVfinal/blob/50e1e2720637b222357a7ebd1919c38a44af7cd2/assets/logo-fiap.png" alt="FIAP - Faculdade de Informática e Admnistração Paulista" border="0" width=40% height=40%></a>
</p>

<br>


## 👥 Integrantes

| Nome | RM |
|------|----|
| Mário Melo Filho | RM563769 |
| Stephanie Dias dos Santos | RM564315 |

# CardioIA — Fase 2: Diagnóstico Automatizado

> **IA no Estetoscópio Digital** — Projeto acadêmico FIAP (PBL)

Sistema de diagnóstico automatizado que aplica NLP e Machine Learning para análise de dados clínicos cardiológicos.

## 🎥 Vídeo de Demonstração

> Link do vídeo no YouTube : https://youtu.be/KSBVl9UmIYo

---

## 📋 Estrutura do Projeto

```
fiap-ano2-fase2-cap1/
├── data/                          # Datasets e imagens geradas
│   ├── sintomas_pacientes.txt     # 10 relatos de pacientes (Parte 1)
│   ├── mapa_conhecimento_expandido.csv  # Ontologia sintoma→doença (~500 linhas)
│   ├── dataset_risco_combinado.csv      # Dataset de risco (10.000 linhas)
│   └── *.png                      # Gráficos gerados pelos notebooks
│
├── notebooks/                     # Jupyter Notebooks
│   ├── parte1_extracao_informacoes.ipynb   # Parte 1: Extração de sintomas
│   ├── parte2_classificador_risco.ipynb    # Parte 2: Classificador TF-IDF + ML
│   └── ir_alem2_rede_neural_ecg.ipynb      # Ir Além 2: MLP para ECG
│
├── scripts/                       # Scripts utilitários
│   ├── download_medquad.py        # Download do dataset MedQuAD (NIH)
│   └── create_combined_dataset.py # Criação do dataset combinado
│
├── cardioia-portal/               # Interface React/Vite (Ir Além 1)
│   ├── src/
│   │   ├── contexts/              # AuthContext (autenticação simulada)
│   │   ├── components/            # Navbar, ProtectedRoute
│   │   ├── services/              # pacientesService, agendamentosService
│   │   └── pages/                 # Login, Dashboard, Pacientes, Agendamentos
│   ├── package.json
│   └── vite.config.js
│
├── requirements.txt               # Dependências Python
└── README.md
```

---

## 🚀 Instalação e Execução

### Pré-requisitos
- Python 3.10+ (recomendado 3.11)
- Node.js 18+
- npm 9+

### Python — Notebooks e Scripts

#### Opção 1 — via Make (recomendado)

```bash
# Cria o venv, instala dependências e abre shell com venv ativo
make

# Ou separadamente:
make prep-venv          # só cria o venv e instala deps
make jupyter            # abre Jupyter já dentro do venv
make datasets           # roda o script de geração de datasets
make clean              # remove o venv
```

Após `make`, você estará dentro de um shell com o venv ativo. Para sair: `exit`.

#### Opção 2 — manualmente

```bash
# 1. Criar ambiente virtual
python3 -m venv fiap_ano2_fase2_cap1_venv

# 2. Ativar
source fiap_ano2_fase2_cap1_venv/bin/activate   # macOS/Linux
# fiap_ano2_fase2_cap1_venv\Scripts\activate    # Windows

# 3. Instalar dependências
pip install -r requirements.txt

# 4. (Opcional) Recriar datasets
python scripts/download_medquad.py
python scripts/create_combined_dataset.py

# 5. Executar notebooks
jupyter notebook notebooks/
```

### React — Interface CardioIA

```bash
cd cardioia-portal
npm install
npm run dev
```

Acesse: http://localhost:5173

**Credenciais de demonstração:**
- `carlos@cardioia.com` / `123456`
- `ana@cardioia.com` / `123456`
- `admin@cardioia.com` / `admin`

---

## 📓 Notebooks

### Parte 1 — Extração de Informações (`parte1_extracao_informacoes.ipynb`)
- Carrega 10 relatos de pacientes (`sintomas_pacientes.txt`)
- Usa mapa de conhecimento expandido para identificar sintomas
- Sugere diagnósticos baseados na ontologia
- Exibe resumo estatístico

### Parte 2 — Classificador de Risco (`parte2_classificador_risco.ipynb`)
- Carrega dataset de 10.000 frases cardiológicas
- Vetorização com **TF-IDF** (unigramas + bigramas)
- Treina e compara 3 modelos: Logistic Regression, Decision Tree, Naive Bayes
- Avalia com acurácia, precisão, recall, F1 e matriz de confusão
- Analisa padrões e vieses do modelo
- Classifica novas frases

### Ir Além 2 — Rede Neural ECG (`ir_alem2_rede_neural_ecg.ipynb`)
- MLP (256→128→64→32) com sklearn
- Classifica sinais de ECG como Normal vs. Anormal
- Suporta dataset real MIT-BIH (Kaggle) ou dados sintéticos
- Curva ROC, matriz de confusão, distribuição de probabilidades

---

## 🌐 Interface React (Ir Além 1)

Aplicação web responsiva com:
- **Autenticação simulada** via Context API + JWT fake no localStorage
- **Proteção de rotas** com AuthContext
- **Dashboard** com métricas de pacientes e agendamentos
- **Listagem de pacientes** com filtros por risco (JSONPlaceholder API)
- **Formulário de agendamento** com `useState` + `useReducer`
- **Estilização** com CSS Modules

---

## 📊 Datasets

| Arquivo | Linhas | Descrição |
|---------|--------|-----------|
| `sintomas_pacientes.txt` | 10 | Relatos de pacientes (obrigatório) |
| `mapa_conhecimento_expandido.csv` | ~500 | Ontologia sintoma→doença |
| `dataset_risco_combinado.csv` | 10.000 | Frases médicas com risco |

### Fontes dos Dados
- **MedQuAD** (NIH) — 47.457 pares Q&A médicos, licença CC BY 4.0
  - Referência: Ben Abacha & Demner-Fushman, BMC Bioinformatics, 2019
  - URL: https://github.com/abachaa/MedQuAD
- **Dados sintéticos CardioIA** — gerados com base em terminologia médica cardiológica

### Dataset ECG (Ir Além 2 — opcional)
Para usar o dataset real de ECG:
1. Baixe de: https://www.kaggle.com/datasets/shayanfazeli/heartbeat
2. Coloque `mitbih_train.csv` e `mitbih_test.csv` em `data/`

---

## ⚖️ Considerações Éticas e Governança de Dados

- Todos os dados são **simulados** — não representam pacientes reais
- O sistema é uma **demonstração acadêmica** e não deve ser usado para diagnóstico clínico real
- Dados reais de saúde estão sujeitos à **LGPD** (Lei Geral de Proteção de Dados) e exigem aprovação de comitê de ética
- O modelo de classificação apresenta **vieses documentados** (ver Seção 7 do notebook Parte 2):
  - Viés de vocabulário técnico
  - Viés de dados sintéticos com templates fixos
  - Ausência de contexto semântico (TF-IDF)

---


---

## 🛠️ Tecnologias

**Python:** pandas, numpy, scikit-learn, matplotlib, seaborn, jupyter  
**React:** Vite, React Router, Context API, CSS Modules  
**Dados:** MedQuAD (NIH), MIT-BIH Arrhythmia Database (Kaggle)

---

## 📚 Fontes e Referências

### Datasets

**[1] MedQuAD — Medical Question Answering Dataset**  
Ben Abacha, A., & Demner-Fushman, D. (2019). A Question-Entailment Approach to Question Answering. *BMC Bioinformatics*, 20(1), 511.  
Fonte: NIH (National Institutes of Health) — 12 websites oficiais  
Licença: Creative Commons Attribution 4.0 International (CC BY 4.0)  
GitHub: https://github.com/abachaa/MedQuAD  
Hugging Face: https://huggingface.co/datasets/keivalya/MedQuad-MedicalQnADataset

**[2] MIT-BIH Arrhythmia Database — Heartbeat Dataset** *(Ir Além 2)*  
Kachuee, M., Fazeli, S., & Sarrafzadeh, M. (2018). ECG Heartbeat Classification: A Deep Transferable Representation. *IEEE ICHI 2018*.  
Licença: Open Database License (ODbL)  
Kaggle: https://www.kaggle.com/datasets/shayanfazeli/heartbeat

**[3] Dados Sintéticos CardioIA**  
Gerados com templates baseados em terminologia médica cardiológica padrão.  
Criados para fins educacionais — não representam pacientes reais.  
Fonte: FIAP — Projeto CardioIA, Fase 2 (2026)

**[4] Frases de Sintomas de Pacientes** *(sintomas_pacientes.txt)*  
10 frases criadas manualmente seguindo o padrão do enunciado da atividade FIAP.  
Baseadas nos exemplos fornecidos no enunciado e em sintomas cardiológicos comuns da literatura médica.  
Não são dados reais de pacientes.

### Bibliotecas e Ferramentas

| Biblioteca | Uso | Referência |
|------------|-----|------------|
| scikit-learn | TF-IDF, classificadores, MLP | https://scikit-learn.org |
| pandas / numpy | Manipulação de dados | https://pandas.pydata.org |
| matplotlib / seaborn | Visualizações | https://matplotlib.org |
| React + Vite | Interface web | https://react.dev / https://vite.dev |
| React Router | Navegação SPA | https://reactrouter.com |
| JSONPlaceholder | API fake para pacientes | https://jsonplaceholder.typicode.com |

---

*Projeto acadêmico — FIAP Ano 2, Fase 2 | CardioIA: A Nova Era da Cardiologia Inteligente*

---

## Fase 3 – Monitoramento IoT

> **CardioIA: Monitoramento Contínuo de Pacientes Cardiológicos via IoT**

O módulo IoT estende o projeto CardioIA com um protótipo de sistema vestível simulado no Wokwi (ESP32 + DHT22 + botões), integrado a um broker MQTT em nuvem (HiveMQ Cloud) e a um dashboard Node-RED com alertas automáticos.

### Documentação e Entregáveis

| Artefato | Caminho |
|----------|---------|
| Código-fonte do firmware (C++) | [`iot/firmware/src/`](iot/firmware/src/) |
| README do módulo IoT | [`iot/README.md`](iot/README.md) |
| Relatório Parte 1 (Edge Computing) | [`iot/docs/RELATORIO_PARTE1.pdf`](iot/docs/RELATORIO_PARTE1.pdf) |
| Relatório Parte 2 (Cloud + Dashboard) | [`iot/docs/RELATORIO_PARTE2.pdf`](iot/docs/RELATORIO_PARTE2.pdf) |
| Fluxo Node-RED exportado | [`iot/dashboard/cardioia_flow.json`](iot/dashboard/cardioia_flow.json) |
| Screenshots do dashboard | [`iot/dashboard/screenshots/`](iot/dashboard/screenshots/) |
| Convenções de código | [`iot/docs/CONVENCOES.md`](iot/docs/CONVENCOES.md) |
| Segurança e LGPD | [`iot/docs/SEGURANCA_LGPD.md`](iot/docs/SEGURANCA_LGPD.md) |

### Como executar

```bash
# Testes property-based (Python + hypothesis)
make iot-test

# Validar entregáveis
make iot-export
```

Para detalhes completos, consulte [`iot/README.md`](iot/README.md).
