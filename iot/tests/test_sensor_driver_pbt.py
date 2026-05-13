"""
Testes de propriedade do SensorDriver — Feature: cardioia-monitoramento-iot.

Este módulo valida três propriedades do driver do DHT22 e dos utilitários
derivados, todas definidas no ``design.md`` (seção *Correctness Properties*)
e rastreadas aos respectivos critérios EARS do ``requirements.md``:

* **Property 5** — Validação de leitura do DHT22 (bicondicional da faixa
  operacional, mais formato do log de descarte). _Requirements: 1.3._
* **Property 6** — Alerta persistente após três leituras consecutivas
  inválidas (transição estrita ``2 → 3`` no contador, reset em toda
  leitura válida). _Requirements: 1.5._
* **Property 7** — Formato da linha de leitura no Monitor Serial (deve
  conter ``"DHT22_Sensor"``, timestamp decimal, temperatura com uma casa
  decimal seguida de ``"°C"`` e umidade inteira seguida de ``"%"``).
  _Requirements: 1.2, 1.4._

Os testes utilizam como oráculo as funções puras de ``reference_model.py``
(:func:`is_valid_reading`, :func:`build_invalid_reading_log`,
:func:`format_reading`, :class:`PersistentFailureCounter`), que espelham a
lógica pura do firmware C++ embarcado no ESP32.

Convenções obrigatórias:

* O docstring de cada propriedade inicia com a tag
  ``"Feature: cardioia-monitoramento-iot, Property N: ..."``.
* Perfil ``hypothesis`` aplicado: ``cardioia-fast`` (P5/P6/P7 não estão
  entre as propriedades obrigatoriamente ``thorough``, conforme design).
"""

from __future__ import annotations

import math

from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import (
    HUM_MAX,
    HUM_MIN,
    TEMP_MAX,
    TEMP_MIN,
    PersistentFailureCounter,
    build_invalid_reading_log,
    format_reading,
    is_valid_reading,
)

# Perfil padrão do módulo IoT (registrado em ``conftest.py``).
_FAST = settings(settings.get_profile("cardioia-fast"))


# -----------------------------------------------------------------------------
# Estratégias compartilhadas (P5)
# -----------------------------------------------------------------------------
# A bicondicional de P5 precisa ser exercitada tanto com valores válidos
# quanto com os dois sinais canônicos de falha do DHT22: ``None`` (leitura
# indisponível) e ``NaN`` (retorno típico da lib em falha de CRC). Por isso
# cada estratégia combina três fontes com ``one_of``.

_TEMP_STRATEGY = st.one_of(
    st.none(),
    st.just(float("nan")),
    st.floats(
        min_value=-1000.0,
        max_value=1000.0,
        allow_nan=False,
        allow_infinity=False,
    ),
)

_HUM_STRATEGY = st.one_of(
    st.none(),
    st.just(float("nan")),
    st.floats(
        min_value=-100.0,
        max_value=200.0,
        allow_nan=False,
        allow_infinity=False,
    ),
    st.integers(min_value=-100, max_value=200),
)


def _oracle_is_valid(temp, hum) -> bool:
    """Oráculo explícito da bicondicional de P5 (R1.3).

    Reproduz a regra descrita em ``design.md`` sem reutilizar o próprio
    ``is_valid_reading``, para que o teste falhe se houver desvio entre a
    implementação e a especificação.
    """
    if temp is None or hum is None:
        return False
    try:
        t = float(temp)
        h = float(hum)
    except (TypeError, ValueError):
        return False
    if math.isnan(t) or math.isnan(h):
        return False
    if math.isinf(t) or math.isinf(h):
        return False
    if not (TEMP_MIN <= t <= TEMP_MAX):
        return False
    if not (HUM_MIN <= h <= HUM_MAX):
        return False
    return True


# -----------------------------------------------------------------------------
# Property 5 — Validação de leitura do DHT22
# -----------------------------------------------------------------------------

@_FAST
@given(temp=_TEMP_STRATEGY, hum=_HUM_STRATEGY)
def test_is_valid_reading_bicondicional(temp, hum):
    """Feature: cardioia-monitoramento-iot, Property 5: bicondicional da validação DHT22.

    Para todo ``(temperatura, umidade)`` em ``floats ∪ {NaN, None}``,
    :func:`is_valid_reading` deve retornar ``True`` se e somente se os
    dois valores são finitos, não nulos e estão dentro das faixas
    operacionais ``[-40.0, 80.0]`` °C e ``[0, 100]`` %. Valida R1.3.
    """
    expected = _oracle_is_valid(temp, hum)
    actual = is_valid_reading(temp, hum)
    assert actual is expected, (
        f"is_valid_reading({temp!r}, {hum!r}) = {actual!r}, esperado {expected!r}"
    )


@_FAST
@given(temp=_TEMP_STRATEGY, hum=_HUM_STRATEGY)
def test_invalid_reading_log_contains_required_substrings(temp, hum):
    """Feature: cardioia-monitoramento-iot, Property 5: log de leitura inválida do DHT22.

    Para toda leitura rejeitada por :func:`is_valid_reading`, a linha
    produzida por :func:`build_invalid_reading_log` deve conter, como
    substrings: ``"DHT22_Sensor"`` (identificador do sensor), o tipo
    canônico de falha (``"NaN"`` ou ``"out_of_range"``) e a
    representação dos valores rejeitados. Valida R1.3.
    """
    # Descarta entradas válidas: P5 só diz respeito ao log das inválidas.
    if is_valid_reading(temp, hum):
        return

    log = build_invalid_reading_log(temp, hum)

    assert "DHT22_Sensor" in log, (
        f"log sem identificador do sensor: {log!r}"
    )
    assert ("NaN" in log) or ("out_of_range" in log), (
        f"log sem tipo de falha reconhecido: {log!r}"
    )
    # ``repr`` cobre ``None``, ``float('nan')`` e números finitos — é o
    # formato mais forte para afirmar que o valor rejeitado aparece na
    # mensagem (usa ``!r`` também no oráculo Python).
    assert repr(temp) in log, (
        f"log sem representação da temperatura rejeitada: {log!r}"
    )
    assert repr(hum) in log, (
        f"log sem representação da umidade rejeitada: {log!r}"
    )


# -----------------------------------------------------------------------------
# Property 6 — Alerta persistente após 3 leituras consecutivas inválidas
# -----------------------------------------------------------------------------

def _oracle_persistent_alerts(seq) -> int:
    """Oráculo da contagem de alertas persistentes (Property 6 / R1.5).

    Para a sequência booleana ``seq`` (``True`` = leitura válida,
    ``False`` = inválida), conta transições estritas ``2 → 3`` da
    contagem consecutiva de inválidos e reinicia em toda válida.
    """
    consecutive = 0
    alerts = 0
    for is_valid in seq:
        if is_valid:
            consecutive = 0
            continue
        consecutive += 1
        if consecutive == 3:
            alerts += 1
    return alerts


@_FAST
@example(seq=[False, False, False])  # um único alerta na transição 2→3
@example(seq=[False, False, False, False])  # 3→4 não re-emite (apenas 1 alerta)
@example(
    seq=[False, False, False, True, False, False, False]
)  # reset por válida + nova transição 2→3 ⇒ 2 alertas
@example(seq=[])  # borda inferior: nenhuma leitura, nenhum alerta
@example(seq=[True, True, True])  # nenhuma inválida ⇒ nenhum alerta
@given(seq=st.lists(st.booleans(), min_size=0, max_size=200))
def test_persistent_failure_counter_fires_on_2_to_3_transition(seq):
    """Feature: cardioia-monitoramento-iot, Property 6: alerta persistente do DHT22.

    Para toda sequência ``seq`` de resultados ``{válido, inválido}*``
    processada por :class:`PersistentFailureCounter`, o número de
    retornos ``True`` de ``record`` SHALL ser igual ao número de
    transições estritas ``2 → 3`` da contagem consecutiva de inválidos,
    com reset em toda entrada ``True`` (leitura válida). Valida R1.5.
    """
    counter = PersistentFailureCounter()

    # Cada ``record`` retorna ``True`` exatamente na transição 2→3.
    actual_alerts_from_returns = sum(1 for is_valid in seq if counter.record(is_valid))

    expected_alerts = _oracle_persistent_alerts(seq)

    assert counter.alerts_fired == actual_alerts_from_returns, (
        "alerts_fired deve contar exatamente os retornos True de record(): "
        f"alerts_fired={counter.alerts_fired}, retornos_True={actual_alerts_from_returns}"
    )
    assert counter.alerts_fired == expected_alerts, (
        f"alerts_fired={counter.alerts_fired} diverge do oráculo ({expected_alerts}) "
        f"para seq={seq!r}"
    )


# -----------------------------------------------------------------------------
# Property 7 — Formato da linha de leitura no Monitor Serial
# -----------------------------------------------------------------------------

@_FAST
@given(
    ts=st.integers(min_value=0, max_value=2**31 - 1),
    temp=st.floats(
        min_value=-40.0,
        max_value=80.0,
        allow_nan=False,
        allow_infinity=False,
    ),
    hum=st.integers(min_value=0, max_value=100),
)
def test_format_reading_contains_required_substrings(ts, temp, hum):
    """Feature: cardioia-monitoramento-iot, Property 7: formatação do Monitor Serial.

    Para toda leitura válida ``(ts, temperatura, umidade)``, a string
    retornada por :func:`format_reading` deve conter, como substrings:
    ``"DHT22_Sensor"``, ``str(ts)`` (timestamp decimal),
    ``f"{temp:.1f}°C"`` (temperatura com uma casa decimal e unidade)
    e ``f"{int(hum)}%"`` (umidade inteira com símbolo de percentual).
    Valida R1.2 e R1.4.
    """
    line = format_reading(ts, temp, hum)

    assert "DHT22_Sensor" in line, (
        f"linha sem identificador do sensor: {line!r}"
    )
    assert str(ts) in line, (
        f"linha sem timestamp decimal str({ts})={str(ts)!r}: {line!r}"
    )
    assert f"{temp:.1f}°C" in line, (
        f"linha sem temperatura formatada {temp:.1f}°C: {line!r}"
    )
    assert f"{int(hum)}%" in line, (
        f"linha sem umidade formatada {int(hum)}%: {line!r}"
    )
