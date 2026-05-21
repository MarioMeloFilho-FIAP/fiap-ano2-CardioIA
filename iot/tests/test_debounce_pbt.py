"""Feature: cardioia-monitoramento-iot, Property 4: debounce paramétrico.

Testa a função ``debounce`` do *reference model* (espelho Python da lógica
pura do firmware embarcado) contra a especificação da Property 4 do
``design.md``:

* ``|out| <= |ts|``
* ``out[0] == ts[0]`` quando ``|ts| >= 1``
* Para todo ``i > 0``: ``out[i] - out[i-1] >= ms``

Além da propriedade principal (Test A), exercitamos:

* Idempotência — aplicar ``debounce`` sobre o resultado de ``debounce`` com
  o mesmo ``ms`` não altera o resultado (consequência direta do contrato
  acima).
* Validação do parâmetro ``ms`` — valores ``<= 0`` devem levantar
  ``ValueError`` (R2.4 / R6.2).

Os ``@example`` cobrem os dois casos operacionais do firmware: pulso
cardíaco (``ms=150``, R2.4) e botão de conectividade (``ms=50``, R6.2).
"""

from __future__ import annotations

import pytest
from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import debounce


# -----------------------------------------------------------------------------
# Estratégias
# -----------------------------------------------------------------------------
# ``ts``: lista de timestamps em ms, não-decrescente, com até 500 entradas.
# ``max_value`` limitado a 10**9 para manter geração rápida sem perder o
# espaço interessante (1e9 ms ≈ 11 dias de operação contínua).
ts_strategy = st.lists(
    st.integers(min_value=0, max_value=10**9),
    min_size=0,
    max_size=500,
).map(sorted)

# ``ms``: intervalo de debounce. O firmware utiliza 50 ms (botão Wi-Fi)
# e 150 ms (pulso cardíaco); o limite superior de 5000 ms é generoso
# para cobrir configurações alternativas sem tornar a geração patológica.
ms_strategy = st.integers(min_value=1, max_value=5000)


# -----------------------------------------------------------------------------
# Test A — propriedade principal
# -----------------------------------------------------------------------------
@settings(settings.get_profile("cardioia-fast"))
@given(ts=ts_strategy, ms=ms_strategy)
@example(ts=[], ms=150)
@example(ts=[0], ms=150)
# Pulso cardíaco (R2.4): dois vizinhos dentro de 150 ms colapsam num só.
@example(ts=[0, 1, 2, 150, 151], ms=150)
# Botão de conectividade (R6.2): gap exatamente igual a ``ms`` é aceito.
@example(ts=[0, 50, 100, 150], ms=50)
def test_debounce_preserves_first_and_enforces_gap(
    ts: list[int], ms: int
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 4: debounce paramétrico.

    Para toda lista ``ts`` ordenada não-decrescente e todo ``ms > 0``,
    ``debounce(ts, ms)`` produz ``out`` tal que:

    * ``len(out) <= len(ts)``;
    * se ``len(ts) >= 1``, ``out[0] == ts[0]``;
    * para todo ``i > 0``, ``out[i] - out[i-1] >= ms``.
    """
    out = debounce(ts, ms)

    # (1) Não-expansão: nunca adicionamos timestamps.
    assert len(out) <= len(ts), (
        f"debounce expandiu a sequência: len(out)={len(out)} > len(ts)={len(ts)}"
    )

    # (2) Preservação do primeiro elemento quando a entrada é não-vazia.
    if len(ts) >= 1:
        assert out[0] == ts[0], (
            f"primeiro elemento perdido: out[0]={out[0]} != ts[0]={ts[0]}"
        )

    # (3) Gap mínimo entre elementos consecutivos do resultado.
    for i in range(1, len(out)):
        gap = out[i] - out[i - 1]
        assert gap >= ms, (
            f"gap insuficiente em i={i}: out[i]-out[i-1]={gap} < ms={ms}"
        )


# -----------------------------------------------------------------------------
# Test B — idempotência
# -----------------------------------------------------------------------------
@settings(settings.get_profile("cardioia-fast"))
@given(ts=ts_strategy, ms=ms_strategy)
def test_debounce_idempotent(ts: list[int], ms: int) -> None:
    """Feature: cardioia-monitoramento-iot, Property 4: debounce paramétrico.

    Aplicar ``debounce`` sobre o próprio resultado (com o mesmo ``ms``) é
    idempotente — consequência direta da Property 4: a saída já satisfaz
    ``out[i] - out[i-1] >= ms``, portanto nada é descartado na segunda
    aplicação.
    """
    once = debounce(ts, ms)
    twice = debounce(once, ms)
    assert twice == once, (
        f"debounce não é idempotente: once={once} twice={twice}"
    )


# -----------------------------------------------------------------------------
# Test C — rejeição de ``ms`` inválido (example-based)
# -----------------------------------------------------------------------------
@pytest.mark.parametrize("ms_invalido", [0, -5])
def test_debounce_invalid_ms_raises(ms_invalido: int) -> None:
    """``debounce`` rejeita ``ms <= 0`` com ``ValueError`` (R2.4 / R6.2)."""
    with pytest.raises(ValueError):
        debounce([0, 1, 2], ms_invalido)
