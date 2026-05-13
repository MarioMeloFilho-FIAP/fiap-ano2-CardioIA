"""
Testes de propriedade para o validador de ``paciente_id`` anonimizado (LGPD).

Feature: ``cardioia-monitoramento-iot``.

Este módulo valida a seguinte propriedade do design (``design.md`` seção
*Correctness Properties*) contra o ``reference_model.py`` — que espelha a
lógica do firmware C++ e serve como oráculo:

* **Property 18** — ``is_anonymized_paciente_id(s)`` é uma **bicondicional**
  da forma canônica exigida pela política LGPD do projeto (R15.2). Ou seja,
  para toda entrada ``s``::

      is_anonymized_paciente_id(s) ⇔ (
          isinstance(s, str)
          and re.fullmatch(r"PAC-\d{1,27}", s) is not None
      )

  A bicondicional garante que:
    (a) qualquer string no formato canônico ``PAC-`` seguido de 1 a 27
        dígitos é **aceita** (Test A + Test B);
    (b) qualquer padrão de PII (CPF, e-mail, data ``dd/mm/aaaa``, RG-like,
        strings vazias/brancas, maiúsculas/minúsculas trocadas, padding)
        é **rejeitado** (Test A + Test C);
    (c) entradas que não são ``str`` (``None``, ``int``, ``float``,
        ``bool``, ``list``) são **rejeitadas** (Test A + Test D).

Convenção de tag: todo docstring de propriedade inicia com
``"Feature: cardioia-monitoramento-iot, Property 18: …"``.
"""

from __future__ import annotations

import re
from typing import Any

import pytest
from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import is_anonymized_paciente_id


# -----------------------------------------------------------------------------
# Regex canônica usada como oráculo (espelha ``_ANON_RE`` do reference_model).
# -----------------------------------------------------------------------------
_CANONICAL_RE = re.compile(r"PAC-\d{1,27}")


# =============================================================================
# Test A — bicondicional sobre strings arbitrárias
# =============================================================================
@settings(settings.get_profile("cardioia-fast"))
@given(s=st.text(max_size=50))
# Exemplos-âncora exigidos pelo plano (Task 14):
@example(s="PAC-0001")                 # canônico válido
@example(s="123.456.789-00")           # CPF formatado — rejeitado
@example(s="joao@exemplo.com")         # e-mail — rejeitado
@example(s="01/01/2000")               # data dd/mm/aaaa — rejeitado
@example(s=" ")                        # só espaço — rejeitado
@example(s="")                         # vazio — rejeitado
@example(s="pac-0001")                 # case sensitivity — rejeitado
def test_bicondicional_regex_canonica(s: str) -> None:
    """Feature: cardioia-monitoramento-iot, Property 18: paciente_id anonimizado.

    Para toda string ``s``, ``is_anonymized_paciente_id(s)`` é ``True`` se,
    e somente se, ``s`` casa integralmente (``re.fullmatch``) com a regex
    canônica ``PAC-\\d{1,27}``.

    Esta é a forma bicondicional completa exigida pela Property 18 do
    ``design.md`` — qualquer divergência entre o validador do firmware e a
    regex canônica é detectada aqui.
    """
    expected = _CANONICAL_RE.fullmatch(s) is not None
    actual = is_anonymized_paciente_id(s)
    assert actual is expected, (
        f"bicondicional violada para s={s!r}: "
        f"esperado={expected}, obtido={actual}"
    )


# =============================================================================
# Test B — positivos gerados a partir da regex canônica
# =============================================================================
@settings(settings.get_profile("cardioia-fast"))
@given(s=st.from_regex(r"PAC-\d{1,27}", fullmatch=True))
@example(s="PAC-0001")
@example(s="PAC-" + "9" * 27)
def test_positive_examples_sao_aceitos(s: str) -> None:
    """Feature: cardioia-monitoramento-iot, Property 18: paciente_id anonimizado.

    Toda string gerada a partir da regex canônica ``^PAC-\\d{1,27}$`` (com
    ``fullmatch=True``) SHALL ser aceita por ``is_anonymized_paciente_id``.

    Cobre o lado "⇒" da bicondicional (R15.2): todo ``paciente_id`` no
    formato canônico é considerado anonimizado.
    """
    assert is_anonymized_paciente_id(s) is True, (
        f"forma canônica rejeitada indevidamente: {s!r}"
    )


# =============================================================================
# Test C — rejeições explícitas (example-based) de PII e variantes inválidas
# =============================================================================
# Cobre os casos pedidos pelo plano e a gama de padrões PII listados em R15.2.
_PII_REJEITADOS: list[tuple[Any, str]] = [
    # --- Casos canônicos válidos, para confirmar o lado "⇐" da bicondicional.
    ("PAC-0001", "canônico de 4 dígitos é aceito"),
    ("PAC-" + "1" * 27, "canônico com sufixo máximo (27 dígitos) é aceito"),
    # --- Variantes que quebram o formato canônico.
    ("PAC-" + "1" * 28, "sufixo com 28 dígitos excede o limite de 27"),
    ("PAC-A", "sufixo não-numérico é rejeitado"),
    ("PAC-", "sufixo vazio é rejeitado"),
    ("", "string vazia é rejeitada"),
    (" ", "string apenas com espaço é rejeitada"),
    ("  PAC-0001  ", "padding com espaços quebra ``fullmatch``"),
    ("pac-0001", "prefixo em minúsculas é rejeitado (case-sensitive)"),
    ("PAC-0001\n", "caractere newline no final é rejeitado"),
    ("PAC-0001 extra", "texto adicional após o sufixo é rejeitado"),
    # --- Padrões de PII reais (R15.2).
    ("123.456.789-00", "CPF formatado — PII rejeitado"),
    ("12345678900", "CPF sem formatação (11 dígitos) — PII rejeitado"),
    ("joao@exemplo.com", "e-mail — PII rejeitado"),
    ("01/01/2000", "data dd/mm/aaaa — PII rejeitado"),
    ("Joao Silva", "nome próprio — PII rejeitado"),
    ("RG 12.345.678-9", "RG formatado — PII rejeitado"),
    # --- Entradas não-string.
    (None, "None não é string — rejeitado"),
    (12345, "inteiro não é string — rejeitado"),
]


@pytest.mark.parametrize(
    "entrada,motivo",
    _PII_REJEITADOS,
    ids=[f"case-{i:02d}" for i in range(len(_PII_REJEITADOS))],
)
def test_pii_e_variantes_sao_rejeitados(entrada: Any, motivo: str) -> None:
    """Feature: cardioia-monitoramento-iot, Property 18: paciente_id anonimizado.

    Para cada entrada listada em ``_PII_REJEITADOS``, o retorno de
    ``is_anonymized_paciente_id`` SHALL coincidir com a regex canônica
    (``True`` apenas para entradas que casam com ``^PAC-\\d{1,27}$``).

    Cobre R15.2 — CPF, e-mail, data ``dd/mm/aaaa``, RG-like, strings
    vazias/brancas, padding, case incorreto e tipos não-string são todos
    rejeitados por serem (a) PII real ou (b) fora do formato canônico.
    """
    if isinstance(entrada, str):
        esperado = _CANONICAL_RE.fullmatch(entrada) is not None
    else:
        esperado = False
    assert is_anonymized_paciente_id(entrada) is esperado, (
        f"divergência em {entrada!r} ({motivo}): esperado={esperado}"
    )


# =============================================================================
# Test D — entradas não-string arbitrárias
# =============================================================================
@settings(settings.get_profile("cardioia-fast"))
@given(
    x=st.one_of(
        st.integers(),
        st.floats(allow_nan=True, allow_infinity=True),
        st.booleans(),
        st.none(),
        st.lists(st.integers(), max_size=5),
    )
)
@example(x=None)
@example(x=0)
@example(x=False)
@example(x=True)
@example(x=[])
def test_non_string_inputs_sao_rejeitados(x: Any) -> None:
    """Feature: cardioia-monitoramento-iot, Property 18: paciente_id anonimizado.

    Toda entrada ``x`` que **não** é ``str`` SHALL resultar em ``False``.

    Reforça o lado "⇒" da bicondicional e protege contra coerções
    implícitas — em particular, ``bool`` (subclasse de ``int``) e ``None``
    nunca podem ser interpretados como identificador anonimizado.
    """
    assert is_anonymized_paciente_id(x) is False, (
        f"entrada não-string aceita indevidamente: {x!r} (tipo={type(x).__name__})"
    )
