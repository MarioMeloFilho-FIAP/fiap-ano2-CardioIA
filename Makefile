SHELL := /bin/bash

VENV_NAME := fiap_ano2_fase2_cap1_venv
VENV_BIN  := $(VENV_NAME)/bin
PYTHON    := $(VENV_BIN)/python3
PIP       := $(VENV_BIN)/pip

.DEFAULT_GOAL := all

.PHONY: all prep-venv shell jupyter datasets clean iot-test iot-export help

all: prep-venv
	source $(VENV_BIN)/activate && /bin/bash

prep-venv:
	python3 -m venv $(VENV_NAME)
	$(PIP) install --upgrade pip --quiet
	$(PIP) install -r requirements.txt --quiet

jupyter:
	source $(VENV_BIN)/activate && jupyter notebook notebooks/

datasets:
	source $(VENV_BIN)/activate && $(PYTHON) scripts/create_combined_dataset.py

clean:
	-rm -rf $(VENV_NAME)

# -----------------------------------------------------------------------------
# Targets do módulo IoT (Fase 3 — CardioIA Monitoramento IoT)
# -----------------------------------------------------------------------------

iot-test: prep-venv
	@echo ">> Executando suíte de testes IoT em iot/tests"
	source $(VENV_BIN)/activate && $(PYTHON) -m pytest iot/tests -q

iot-export:
	@echo ">> Verificando entregáveis da Fase 3 (módulo IoT)"
	@fail=0; \
	for f in iot/docs/RELATORIO_PARTE1.pdf iot/docs/RELATORIO_PARTE2.pdf iot/dashboard/cardioia_flow.json; do \
		if [ -f "$$f" ]; then \
			echo "  [OK]   $$f"; \
		else \
			echo "  [FALTA] $$f"; \
			fail=1; \
		fi; \
	done; \
	shots_dir="iot/dashboard/screenshots"; \
	if [ -d "$$shots_dir" ]; then \
		n=$$(find "$$shots_dir" -maxdepth 1 -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' \) | wc -l | tr -d ' '); \
	else \
		n=0; \
	fi; \
	if [ "$$n" -ge 3 ]; then \
		echo "  [OK]   $$shots_dir contém $$n screenshot(s) (mínimo 3)"; \
	else \
		echo "  [FALTA] $$shots_dir precisa de pelo menos 3 screenshots PNG/JPG (encontrado(s): $$n)"; \
		fail=1; \
	fi; \
	if [ "$$fail" -ne 0 ]; then \
		echo ">> Entregáveis incompletos — consulte as mensagens acima."; \
		exit 1; \
	fi; \
	echo ">> Entregáveis da Fase 3 presentes."

help:
	@echo "Targets disponíveis (execute com 'make <target>'):"
	@echo "  all         Prepara o venv e abre um shell com ele ativado (padrão)"
	@echo "  prep-venv   Cria/atualiza o venv '$(VENV_NAME)' e instala requirements"
	@echo "  jupyter     Abre o Jupyter Notebook apontando para notebooks/"
	@echo "  datasets    Gera o dataset combinado via scripts/create_combined_dataset.py"
	@echo "  clean       Remove o venv '$(VENV_NAME)'"
	@echo "  iot-test    Roda a suíte de testes do módulo IoT (iot/tests) dentro do venv"
	@echo "  iot-export  Valida entregáveis da Fase 3 (relatórios PDF, fluxo Node-RED e screenshots)"
	@echo "  help        Exibe esta mensagem de ajuda"
