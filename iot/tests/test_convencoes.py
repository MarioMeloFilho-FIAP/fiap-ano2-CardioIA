# -*- coding: utf-8 -*-
"""Testes example-based de convenções de código do firmware (R12.1, R12.3).

Este módulo valida, via inspeção estática dos arquivos-fonte, duas convenções
obrigatórias do firmware C++ do CardioIA – Monitoramento IoT (Fase 3):

* **R12.1 — Cabeçalho do ``main.cpp``**: as 30 primeiras linhas do ponto de
  entrada do firmware devem conter, obrigatoriamente, o nome do projeto
  ``"CardioIA"``, a identificação ``"Fase 3"``, pelo menos um RM no formato
  ``RM`` seguido de seis dígitos e uma data de criação no formato ``AAAA-MM-DD``.

* **R12.3 — Bloco único de constantes clínicas em ``config.h``**: as constantes
  ``BPM_THRESHOLD``, ``TEMP_THRESHOLD``, ``BUFFER_LIMIT`` e
  ``SAMPLING_INTERVAL_MS`` devem ser declaradas num **único bloco** do arquivo
  (reforçado aqui por um gap máximo de 10 linhas entre declarações
  consecutivas), cada qual com um comentário adjacente em português brasileiro
  imediatamente acima indicando a unidade de medida (``bpm``, ``Celsius`` ou
  ``°C``, ``amostras``, ``ms`` ou ``milissegundos``).

Diferente dos demais testes da suíte, estes são *example-based* (não PBT),
porque R12.1 e R12.3 são propriedades estruturais de arquivos específicos e
não propriedades universais sobre um espaço de entradas.

Os arquivos inspecionados são criados apenas nas Tarefas 18 (``config.h``) e
28 (``main.cpp``) do plano de implementação. Enquanto isso, os testes deste
módulo usam ``pytest.mark.skipif`` para *pular* graciosamente, permitindo que
a suíte completa rode nos *checkpoints* anteriores (Tarefa 17) sem falhar por
ausência dos artefatos de firmware.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

# -----------------------------------------------------------------------------
# Localização dos arquivos-fonte do firmware
# -----------------------------------------------------------------------------
# ``__file__`` está em ``.../fiap-ano2-CardioIA/iot/tests/test_convencoes.py``.
# ``parents[2]`` sobe dois níveis e aterrissa em ``.../fiap-ano2-CardioIA/``,
# que é a raiz do repositório do módulo IoT.
REPO_ROOT: Path = Path(__file__).resolve().parents[2]
MAIN_CPP: Path = REPO_ROOT / "iot" / "firmware" / "src" / "main.cpp"
CONFIG_H: Path = REPO_ROOT / "iot" / "firmware" / "src" / "config.h"

# Mensagens de skip em pt-BR para facilitar diagnóstico no output do pytest.
SKIP_REASON_MAIN = (
    "main.cpp ainda não foi criado (tarefa 28 pendente) — cabeçalho R12.1 "
    "será validado quando o arquivo existir."
)
SKIP_REASON_CONFIG = (
    "config.h ainda não foi criado (tarefa 18 pendente) — bloco de constantes "
    "R12.3 será validado quando o arquivo existir."
)

# Palavras-chave esperadas por constante para validar o comentário de unidade
# adjacente (match case-insensitive sobre o comentário ``//`` acima da
# declaração). Para ``TEMP_THRESHOLD`` aceitamos tanto ``Celsius`` quanto o
# símbolo ``°C`` (ambos previstos no design).
UNITS_BY_CONST: dict[str, tuple[str, ...]] = {
    "BPM_THRESHOLD": ("bpm",),
    "TEMP_THRESHOLD": ("celsius", "°c"),
    "BUFFER_LIMIT": ("amostras",),
    "SAMPLING_INTERVAL_MS": ("ms", "milissegundos"),
}

# Gap máximo, em linhas, entre declarações consecutivas das quatro constantes
# clínicas para que elas sejam consideradas parte de "um único bloco" (R12.3).
# Permitimos 10 linhas para acomodar comentário pt-BR (1–3 linhas) entre cada
# ``constexpr`` sem autorizar que constantes fiquem espalhadas pelo arquivo.
MAX_GAP_LINHAS_BLOCO: int = 10


# -----------------------------------------------------------------------------
# Utilitários de leitura
# -----------------------------------------------------------------------------
def _ler_primeiras_linhas(caminho: Path, n: int) -> str:
    """Lê as ``n`` primeiras linhas de ``caminho`` e retorna como string única."""
    with caminho.open("r", encoding="utf-8") as fh:
        linhas = [next(fh, "") for _ in range(n)]
    return "".join(linhas)


def _ler_linhas(caminho: Path) -> list[str]:
    """Lê o arquivo inteiro e retorna a lista de linhas (sem ``\\n`` final)."""
    return caminho.read_text(encoding="utf-8").splitlines()


# =============================================================================
# R12.1 — Cabeçalho do ``main.cpp``
# =============================================================================
# As 30 primeiras linhas do ``main.cpp`` devem conter, obrigatoriamente:
#   (a) "CardioIA"            → nome do projeto
#   (b) "Fase 3"              → identificação da fase
#   (c) RM\d{6}               → pelo menos um RM de integrante
#   (d) \b\d{4}-\d{2}-\d{2}\b → data de criação em formato ISO (AAAA-MM-DD)


@pytest.mark.skipif(not MAIN_CPP.exists(), reason=SKIP_REASON_MAIN)
def test_main_cpp_header_contains_cardioia() -> None:
    """R12.1: o cabeçalho deve citar o nome do projeto ``CardioIA``."""
    cabecalho = _ler_primeiras_linhas(MAIN_CPP, 30)
    assert "CardioIA" in cabecalho, (
        "Cabeçalho do main.cpp (30 primeiras linhas) não contém o nome do "
        "projeto 'CardioIA' — requisito R12.1(a) não atendido."
    )


@pytest.mark.skipif(not MAIN_CPP.exists(), reason=SKIP_REASON_MAIN)
def test_main_cpp_header_contains_fase_3() -> None:
    """R12.1: o cabeçalho deve identificar a fase como ``Fase 3``."""
    cabecalho = _ler_primeiras_linhas(MAIN_CPP, 30)
    assert "Fase 3" in cabecalho, (
        "Cabeçalho do main.cpp (30 primeiras linhas) não contém a "
        "identificação 'Fase 3' — requisito R12.1(b) não atendido."
    )


@pytest.mark.skipif(not MAIN_CPP.exists(), reason=SKIP_REASON_MAIN)
def test_main_cpp_header_contains_rm() -> None:
    """R12.1: o cabeçalho deve citar pelo menos um RM (``RM`` + 6 dígitos)."""
    cabecalho = _ler_primeiras_linhas(MAIN_CPP, 30)
    match = re.search(r"RM\d{6}", cabecalho)
    assert match is not None, (
        "Cabeçalho do main.cpp (30 primeiras linhas) não contém nenhum RM no "
        "formato 'RM' seguido de exatamente 6 dígitos — requisito R12.1(c) "
        "não atendido. Inclua o RM de cada integrante."
    )


@pytest.mark.skipif(not MAIN_CPP.exists(), reason=SKIP_REASON_MAIN)
def test_main_cpp_header_contains_iso_date() -> None:
    """R12.1: o cabeçalho deve conter uma data no formato ``AAAA-MM-DD``."""
    cabecalho = _ler_primeiras_linhas(MAIN_CPP, 30)
    match = re.search(r"\b\d{4}-\d{2}-\d{2}\b", cabecalho)
    assert match is not None, (
        "Cabeçalho do main.cpp (30 primeiras linhas) não contém uma data de "
        "criação no formato ISO 'AAAA-MM-DD' — requisito R12.1(d) não "
        "atendido."
    )


# =============================================================================
# R12.3 — Bloco único de constantes clínicas em ``config.h``
# =============================================================================
# As quatro constantes devem:
#   (i)   ser declaradas via ``constexpr ... = ...;`` no mesmo arquivo;
#   (ii)  ter comentário pt-BR imediatamente acima indicando a unidade; e
#   (iii) estar agrupadas num único bloco (gap ≤ 10 linhas entre declarações).

# Regex que identifica uma declaração ``constexpr`` da constante ``NOME``:
# captura linhas como ``constexpr int BPM_THRESHOLD = 120;`` e
# ``constexpr float TEMP_THRESHOLD = 38.0f;``.
_CONSTEXPR_PATTERN = r"^\s*constexpr\s+[A-Za-z_][A-Za-z0-9_:\s\*&]*\s+{name}\s*=[^;]+;"


def _encontrar_linha_declaracao(linhas: list[str], nome: str) -> int | None:
    """Retorna o índice (0-based) da linha que declara ``constexpr ... NOME = ...;``.

    Retorna ``None`` se nenhuma linha casar.
    """
    padrao = re.compile(_CONSTEXPR_PATTERN.format(name=re.escape(nome)))
    for idx, linha in enumerate(linhas):
        if padrao.match(linha):
            return idx
    return None


@pytest.mark.skipif(not CONFIG_H.exists(), reason=SKIP_REASON_CONFIG)
def test_config_h_declares_all_thresholds() -> None:
    """R12.3: ``config.h`` deve declarar as 4 constantes como ``constexpr``.

    Verifica que cada uma de ``BPM_THRESHOLD``, ``TEMP_THRESHOLD``,
    ``BUFFER_LIMIT`` e ``SAMPLING_INTERVAL_MS`` aparece como declaração
    ``constexpr <tipo> NOME = <valor>;`` no arquivo.
    """
    linhas = _ler_linhas(CONFIG_H)
    faltantes: list[str] = []
    for nome in UNITS_BY_CONST:
        if _encontrar_linha_declaracao(linhas, nome) is None:
            faltantes.append(nome)
    assert not faltantes, (
        "Constantes clínicas ausentes (ou sem declaração ``constexpr ... "
        f"NOME = ...;``) em config.h: {faltantes}. R12.3 exige que as quatro "
        "constantes sejam declaradas como ``constexpr`` num único bloco."
    )


@pytest.mark.skipif(not CONFIG_H.exists(), reason=SKIP_REASON_CONFIG)
def test_config_h_constants_have_pt_br_comments_with_units() -> None:
    """R12.3: cada constante deve ter comentário de unidade logo acima.

    Para cada constante, inspeciona até 2 linhas não-brancas imediatamente
    acima da declaração e verifica que pelo menos uma delas é um comentário
    ``//`` contendo a palavra-chave de unidade esperada (case-insensitive):

    * ``BPM_THRESHOLD``        → ``bpm``
    * ``TEMP_THRESHOLD``       → ``Celsius`` ou ``°C``
    * ``BUFFER_LIMIT``         → ``amostras``
    * ``SAMPLING_INTERVAL_MS`` → ``ms`` ou ``milissegundos``
    """
    linhas = _ler_linhas(CONFIG_H)
    falhas: list[str] = []

    for nome, unidades in UNITS_BY_CONST.items():
        idx = _encontrar_linha_declaracao(linhas, nome)
        assert idx is not None, (
            f"Declaração de {nome} não foi encontrada em config.h. Execute "
            "primeiro o teste test_config_h_declares_all_thresholds para "
            "diagnóstico mais preciso."
        )

        # Coleta até 2 linhas não-brancas imediatamente acima da declaração.
        vizinhos: list[str] = []
        j = idx - 1
        while j >= 0 and len(vizinhos) < 2:
            if linhas[j].strip() == "":
                j -= 1
                continue
            vizinhos.append(linhas[j])
            j -= 1

        # Uma das linhas vizinhas DEVE ser um comentário ``//`` contendo a
        # palavra-chave de unidade (match case-insensitive).
        tem_comentario_com_unidade = any(
            "//" in linha
            and any(unidade.lower() in linha.lower() for unidade in unidades)
            for linha in vizinhos
        )
        if not tem_comentario_com_unidade:
            falhas.append(
                f"{nome} (linha {idx + 1}): esperava-se comentário pt-BR "
                f"adjacente (até 2 linhas acima) mencionando unidade "
                f"{'/'.join(unidades)!r}; linhas inspecionadas: {vizinhos!r}"
            )

    assert not falhas, (
        "Comentário de unidade ausente ou incorreto para constantes em "
        "config.h (R12.3):\n  - " + "\n  - ".join(falhas)
    )


@pytest.mark.skipif(not CONFIG_H.exists(), reason=SKIP_REASON_CONFIG)
def test_config_h_constants_in_single_block() -> None:
    """R12.3: as 4 constantes devem ficar agrupadas num único bloco.

    Calcula o gap (em linhas) entre declarações consecutivas — ordenadas pela
    linha em que aparecem — e exige que o maior gap não exceda
    ``MAX_GAP_LINHAS_BLOCO``. Isso impede que as constantes fiquem espalhadas
    pelo arquivo enquanto ainda permite comentários pt-BR intercalados.
    """
    linhas = _ler_linhas(CONFIG_H)
    posicoes: dict[str, int] = {}
    for nome in UNITS_BY_CONST:
        idx = _encontrar_linha_declaracao(linhas, nome)
        assert idx is not None, (
            f"Declaração de {nome} não foi encontrada em config.h. Execute "
            "primeiro o teste test_config_h_declares_all_thresholds para "
            "diagnóstico mais preciso."
        )
        posicoes[nome] = idx

    ordenadas = sorted(posicoes.items(), key=lambda kv: kv[1])
    gaps = [
        (ordenadas[i + 1][0], ordenadas[i + 1][1] - ordenadas[i][1])
        for i in range(len(ordenadas) - 1)
    ]
    gap_maximo = max((g for _, g in gaps), default=0)

    assert gap_maximo <= MAX_GAP_LINHAS_BLOCO, (
        "Constantes clínicas não estão agrupadas em um único bloco (R12.3): "
        f"gap máximo observado = {gap_maximo} linhas "
        f"(limite permitido = {MAX_GAP_LINHAS_BLOCO}).\n"
        f"Posições (linha 1-based): "
        f"{ {n: p + 1 for n, p in posicoes.items()} }\n"
        f"Gaps consecutivos: {gaps}"
    )
