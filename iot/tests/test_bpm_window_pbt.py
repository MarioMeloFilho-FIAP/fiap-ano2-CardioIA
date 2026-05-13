"""Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

Testa a classe :class:`BpmWindow` do *reference model* (espelho Python da
lógica pura do firmware embarcado) contra a especificação da Property 3 do
``design.md``, que cobre os critérios R2.2, R2.3, R2.5, R2.6 e R2.7:

* Janela plena (``elapsed_ms >= 60_000``)::

      compute_bpm(now_ms) == min(250, |{t ∈ ts : now_ms - 60000 < t <= now_ms}|)

* Extrapolação proporcional (``0 < elapsed_ms < 60_000``, R2.7)::

      compute_bpm(now_ms) == min(250, round(|ts| * 60_000 / elapsed_ms))

* Janela vazia com ``elapsed_ms >= 60_000`` → ``0`` (R2.6).
* Elapsed zero (``now_ms == boot_ms``) → ``0`` (evita divisão por zero).
* Saturação em 250 (R2.5) independentemente da quantidade de pulsos.

Os ``@example`` cobrem os cenários clínicos relevantes:

* ``bpm = 250`` — saturação pura (300 pulsos no mesmo instante).
* ``bpm = 0`` — janela vazia de 60 s sem pulsos.
* *Boot recente* — extrapolação com poucos pulsos e ``elapsed`` curto.
"""

from __future__ import annotations

from hypothesis import HealthCheck, example, given, settings
from hypothesis import strategies as st

from reference_model import BPM_WINDOW_MS, BpmWindow, MAX_BPM


# -----------------------------------------------------------------------------
# Estratégias
# -----------------------------------------------------------------------------
# As estratégias são escritas de forma *dependente* para evitar uso massivo
# de ``assume`` — garantem, por construção, que todos os pulsos gerados
# caem em ``[0, now_ms]`` e que ``now_ms`` está no ramo apropriado da
# Property 3 (janela plena vs. extrapolação).
#
# ``max_size=300`` respeita ``BPM_RING_CAPACITY`` do reference model — com
# mais de 300 pulsos, a ``deque`` interna descartaria os mais antigos e a
# igualdade contra o oráculo deixaria de valer por construção.

@st.composite
def full_window_inputs(draw):
    """Gera ``(ts, now_ms)`` com ``now_ms >= 60_000`` e ``ts[i] in [0, now_ms]``.

    Corresponde ao ramo "janela plena" da Property 3 (R2.2, R2.3, R2.5, R2.6).
    """
    now_ms = draw(st.integers(min_value=BPM_WINDOW_MS, max_value=3_600_000))
    ts = draw(
        st.lists(
            st.integers(min_value=0, max_value=now_ms),
            min_size=0,
            max_size=300,
        ).map(sorted)
    )
    return ts, now_ms


@st.composite
def boot_window_inputs(draw):
    """Gera ``(ts, now_ms)`` com ``0 < now_ms < 60_000`` e ``ts[i] in [0, now_ms]``.

    Corresponde ao ramo "extrapolação proporcional" da Property 3 (R2.5, R2.7).
    """
    now_ms = draw(st.integers(min_value=1, max_value=BPM_WINDOW_MS - 1))
    ts = draw(
        st.lists(
            st.integers(min_value=0, max_value=now_ms),
            min_size=0,
            max_size=300,
        ).map(sorted)
    )
    return ts, now_ms


# -----------------------------------------------------------------------------
# Test A — janela plena (elapsed >= 60_000)
# -----------------------------------------------------------------------------
@settings(
    settings.get_profile("cardioia-fast"),
    suppress_health_check=[HealthCheck.too_slow],
)
@given(data=full_window_inputs())
# Saturação pura (R2.5): 250 pulsos no limite direito da janela → 250.
@example(data=([60_000] * 250, 60_000))
# Janela vazia (R2.6): sem pulsos, elapsed plena → 0.
@example(data=([], 60_000))
# Contagem simples dentro da janela (>0 e <250): 42 pulsos no instante atual.
@example(data=([90_000] * 42, 90_000))
def test_bpm_full_window(data: tuple[list[int], int]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

    Com ``elapsed_ms = now_ms - boot_ms >= 60_000`` e todos os pulsos em
    ``[0, now_ms]``, ``compute_bpm(now_ms)`` deve ser igual a
    ``min(250, contagem_na_janela)`` onde a janela é ``(now_ms - 60_000, now_ms]``
    (R2.2, R2.3, R2.5, R2.6).
    """
    ts, now_ms = data

    window = BpmWindow(boot_ms=0)
    for t in ts:
        window.register_pulse(t)

    expected = min(
        MAX_BPM,
        sum(1 for t in ts if now_ms - BPM_WINDOW_MS < t <= now_ms),
    )
    actual = window.compute_bpm(now_ms)
    assert actual == expected, (
        f"BPM em janela plena divergiu: actual={actual} expected={expected} "
        f"ts={ts} now_ms={now_ms}"
    )


# -----------------------------------------------------------------------------
# Test B — extrapolação proporcional (0 < elapsed < 60_000)
# -----------------------------------------------------------------------------
@settings(
    settings.get_profile("cardioia-fast"),
    suppress_health_check=[HealthCheck.too_slow],
)
@given(data=boot_window_inputs())
# Boot recente sem pulsos ainda → extrapolação de 0 * ... = 0.
@example(data=([], 1_000))
# Boot recente com saturação pela extrapolação: 5 pulsos em 100 ms
# → round(5 * 60000 / 100) = 3000 → min(250, 3000) = 250 (R2.5 via R2.7).
@example(data=([0, 10, 20, 30, 40], 100))
# Boot recente com BPM intermediário: 1 pulso em 30_000 ms
# → round(1 * 60000 / 30000) = 2.
@example(data=([15_000], 30_000))
def test_bpm_extrapolation_during_boot_window(
    data: tuple[list[int], int],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

    Com ``0 < elapsed_ms < 60_000`` e todos os pulsos em ``[0, now_ms]``,
    ``compute_bpm(now_ms)`` deve ser igual a
    ``min(250, round(|ts| * 60_000 / elapsed_ms))`` — extrapolação proporcional
    durante o período de boot recente (R2.5, R2.7).
    """
    ts, now_ms = data

    window = BpmWindow(boot_ms=0)
    for t in ts:
        window.register_pulse(t)

    expected = min(MAX_BPM, round(len(ts) * BPM_WINDOW_MS / now_ms))
    actual = window.compute_bpm(now_ms)
    assert actual == expected, (
        f"BPM por extrapolação divergiu: actual={actual} expected={expected} "
        f"ts={ts} now_ms={now_ms}"
    )


# -----------------------------------------------------------------------------
# Test C — janela vazia com elapsed pleno retorna 0 (R2.6)
# -----------------------------------------------------------------------------
@settings(settings.get_profile("cardioia-fast"))
@given(now_ms=st.integers(min_value=BPM_WINDOW_MS, max_value=3_600_000))
@example(now_ms=BPM_WINDOW_MS)
@example(now_ms=3_600_000)
def test_bpm_zero_when_no_pulses_and_full_window(now_ms: int) -> None:
    """Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

    Para todo ``now_ms >= 60_000``, um :class:`BpmWindow` recém-criado (sem
    pulsos registrados) deve retornar ``0`` (R2.6).
    """
    window = BpmWindow(boot_ms=0)
    assert window.compute_bpm(now_ms) == 0, (
        f"janela vazia com elapsed pleno deveria retornar 0; now_ms={now_ms}"
    )


# -----------------------------------------------------------------------------
# Test D — saturação em 250 (R2.5), example-based
# -----------------------------------------------------------------------------
def test_bpm_saturates_at_250() -> None:
    """Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

    Saturação em 250 (R2.5): independentemente da quantidade de pulsos
    registrados na janela (até o limite do ring buffer), ``compute_bpm``
    nunca retorna valor maior que ``MAX_BPM`` (250).

    Cenários cobertos:

    * 300 pulsos exatamente em ``now_ms=60_000`` — todos no limite direito
      da janela ``(0, 60000]`` → contagem bruta = 300, saturação → 250.
    * 250 pulsos em ``now_ms=60_000`` — exatamente no limiar de saturação
      → 250.
    * Extrapolação que geraria valor > 250 (muitos pulsos em elapsed curto)
      → saturação em 250 (R2.5 via R2.7).
    """
    # 300 pulsos no limite direito da janela: a contagem crua é 300
    # (todos em (0, 60000]) e deve saturar em 250.
    window = BpmWindow(boot_ms=0)
    for _ in range(300):
        window.register_pulse(60_000)
    assert window.compute_bpm(60_000) == MAX_BPM, (
        "300 pulsos na janela plena deveriam saturar em 250"
    )

    # 250 pulsos no limiar de saturação — valor exato.
    window2 = BpmWindow(boot_ms=0)
    for _ in range(250):
        window2.register_pulse(60_000)
    assert window2.compute_bpm(60_000) == MAX_BPM, (
        "250 pulsos na janela plena deveriam retornar 250 (limiar)"
    )

    # Saturação via extrapolação: 5 pulsos em 100 ms → round(5 * 600) = 3000
    # → min(250, 3000) == 250.
    window3 = BpmWindow(boot_ms=0)
    for t in (0, 10, 20, 30, 40):
        window3.register_pulse(t)
    assert window3.compute_bpm(100) == MAX_BPM, (
        "extrapolação de 5 pulsos em 100 ms deveria saturar em 250"
    )


# -----------------------------------------------------------------------------
# Test E — elapsed zero retorna 0
# -----------------------------------------------------------------------------
@settings(settings.get_profile("cardioia-fast"))
@given(
    ts=st.lists(
        st.integers(min_value=0, max_value=1_000),
        min_size=0,
        max_size=50,
    ).map(sorted)
)
@example(ts=[])
@example(ts=[0, 500, 1_000])
def test_bpm_elapsed_zero_returns_zero(ts: list[int]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 3: BpmWindow com saturação e extrapolação.

    Quando ``now_ms == boot_ms`` (``elapsed_ms == 0``), ``compute_bpm``
    deve retornar ``0`` para evitar divisão por zero na extrapolação,
    independentemente dos pulsos já registrados.
    """
    window = BpmWindow(boot_ms=1_000)
    for t in ts:
        window.register_pulse(t)
    assert window.compute_bpm(1_000) == 0, (
        f"elapsed=0 deveria retornar 0; ts={ts}"
    )
