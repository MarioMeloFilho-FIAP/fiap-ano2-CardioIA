"""Feature: cardioia-monitoramento-iot, Properties 19 & 20: ConfigManager.

Testa ``validate_config``, ``apply_config`` e :class:`ConfigManager` do
*reference model* (espelho Python de ``iot/firmware/src/config_manager.*``).

* **Property 19** — bicondicional entre :func:`validate_config` e o oráculo
  de faixas declarado em ``design.md`` / ``requirements.md`` (R16.1):

  - ``bpm_threshold ∈ [40, 220]`` (int estrito — ``bool`` é rejeitado);
  - ``temp_threshold ∈ [35.0, 42.0]`` (``float`` ou ``int`` no intervalo;
    ``NaN``/``Inf`` rejeitados);
  - ``buffer_limit ∈ [10, 500]`` (int estrito);
  - ``sampling_interval_ms ∈ [1000, 60000]`` (int estrito).

  Também verifica que :func:`apply_config` preserva ``state`` quando o
  payload é inválido e devolve um motivo de rejeição pt-BR (R16.3), e que
  executa *merge* determinístico quando o payload é válido.

* **Property 20** — :class:`ConfigManager` sobrevive a reboots: para toda
  sequência ``[c1..cn]`` com mistura de válidos e inválidos seguida de
  :meth:`ConfigManager.reboot`, o estado carregado é **exatamente** a
  última config válida aplicada (R16.2).

Os ``@example`` cobrem os limites inferior e superior exatos de cada faixa
e casos típicos de rejeição (``{}``, bool, NaN, chave desconhecida).
"""

from __future__ import annotations

import math
from typing import Any, Callable, Dict, List, Optional

import pytest
from hypothesis import HealthCheck, example, given, settings
from hypothesis import strategies as st

from reference_model import (
    BPM_THRESHOLD_DEFAULT,
    BUFFER_LIMIT_DEFAULT,
    DEFAULT_CONFIG,
    SAMPLING_INTERVAL_MS_DEFAULT,
    TEMP_THRESHOLD_DEFAULT,
    ConfigManager,
    apply_config,
    validate_config,
)

# -----------------------------------------------------------------------------
# Oráculo local das faixas (espelha ``_CONFIG_RANGES`` do reference model).
#
# O teste replica intencionalmente as faixas aqui para que a bicondicional
# seja verificada contra uma descrição **independente** da implementação
# sob teste — se alguma das duas divergir, o teste detecta o desvio.
# -----------------------------------------------------------------------------
_RANGES: Dict[str, tuple[str, Any, Any]] = {
    "bpm_threshold":        ("int",   40,     220),
    "temp_threshold":       ("float", 35.0,   42.0),
    "buffer_limit":         ("int",   10,     500),
    "sampling_interval_ms": ("int",   1000,   60000),
}


def _oracle_valid(payload: Any) -> bool:
    """Oráculo independente para Property 19: bicondicional de validade.

    Retorna ``True`` iff ``payload`` é dict não vazio, todas as chaves são
    canônicas (em :data:`_RANGES`) e cada valor casa tipo+faixa sem ser
    ``bool``, ``NaN`` ou ``Inf``.
    """
    if not isinstance(payload, dict) or not payload:
        return False
    for key, value in payload.items():
        if key not in _RANGES:
            return False
        # Rejeitamos ``bool`` explicitamente: ``True``/``False`` são ``int``
        # em Python, porém semanticamente inválidos como limiar clínico.
        if isinstance(value, bool):
            return False
        kind, lo, hi = _RANGES[key]
        if kind == "int":
            if not isinstance(value, int):
                return False
            if not (lo <= value <= hi):
                return False
        else:  # "float"
            if not isinstance(value, (int, float)):
                return False
            v = float(value)
            if math.isnan(v) or math.isinf(v):
                return False
            if not (lo <= v <= hi):
                return False
    return True


def _expected_parsed(payload: Dict[str, Any]) -> Dict[str, Any]:
    """Normaliza um payload válido como :func:`apply_config` o faria.

    Campos ``int`` são coagidos para ``int``, ``float`` para ``float``.
    Assume que ``_oracle_valid(payload) is True``.
    """
    parsed: Dict[str, Any] = {}
    for key, value in payload.items():
        kind = _RANGES[key][0]
        parsed[key] = int(value) if kind == "int" else float(value)
    return parsed


# -----------------------------------------------------------------------------
# Estratégias
# -----------------------------------------------------------------------------
# Valores "ruidosos" — mistura proposital de válidos e inválidos para que
# ``validate_config`` e o oráculo sejam exercitados simetricamente.
_noisy_value_st: st.SearchStrategy[Any] = st.one_of(
    # Inteiros cobrindo limites das três faixas int (40..220, 10..500, 1000..60000).
    st.integers(min_value=-100, max_value=1000),
    st.integers(min_value=900, max_value=70_000),
    # Floats cobrindo faixa de ``temp_threshold`` (35.0..42.0) e além.
    st.floats(
        min_value=30.0, max_value=50.0,
        allow_nan=False, allow_infinity=False,
    ),
    # Patológicos — esperamos rejeição determinística em todos eles.
    st.floats(allow_nan=True, allow_infinity=True),
    st.booleans(),
    st.text(max_size=3),
    st.none(),
)

# Chaves — mistura de canônicas e desconhecidas para exercitar R16.1
# (rejeição por campo desconhecido).
_key_st: st.SearchStrategy[str] = st.sampled_from(
    [
        "bpm_threshold",
        "temp_threshold",
        "buffer_limit",
        "sampling_interval_ms",
        "unknown_field",
        "ghost",
        "",
    ]
)

# Payload "ruidoso" — dicts de até 6 pares (cobre o espaço completo de
# subconjuntos dos 4 campos canônicos mais chaves desconhecidas).
_payload_st: st.SearchStrategy[Dict[str, Any]] = st.dictionaries(
    keys=_key_st,
    values=_noisy_value_st,
    max_size=6,
)


# Estratégia de payloads garantidamente válidos — um subconjunto não vazio
# dos 4 campos canônicos, cada um com valor em sua faixa exata.
def _valid_value_for(key: str) -> st.SearchStrategy[Any]:
    kind, lo, hi = _RANGES[key]
    if kind == "int":
        return st.integers(min_value=lo, max_value=hi)
    # float: inclui ``int`` dentro da faixa como caso legítimo (R16.1
    # aceita qualquer numérico finito no intervalo).
    return st.one_of(
        st.floats(
            min_value=float(lo), max_value=float(hi),
            allow_nan=False, allow_infinity=False,
        ),
        st.integers(min_value=int(lo), max_value=int(hi)),
    )


_valid_payload_st: st.SearchStrategy[Dict[str, Any]] = (
    st.lists(
        st.sampled_from(list(_RANGES.keys())),
        min_size=1, max_size=4, unique=True,
    ).flatmap(
        lambda keys: st.fixed_dictionaries(
            {k: _valid_value_for(k) for k in keys}
        )
    )
)


# =============================================================================
# Property 19 — validate_config bicondicional contra o oráculo de faixas
# =============================================================================

@settings(
    parent=settings.get_profile("cardioia-fast"),
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.filter_too_much],
)
@given(payload=_payload_st)
# Limites exatos de cada faixa — oráculo diz "válido".
@example(payload={"bpm_threshold": 40})
@example(payload={"bpm_threshold": 220})
@example(payload={"buffer_limit": 10})
@example(payload={"buffer_limit": 500})
@example(payload={"sampling_interval_ms": 1000})
@example(payload={"sampling_interval_ms": 60000})
@example(payload={"temp_threshold": 35.0})
@example(payload={"temp_threshold": 42.0})
# Logo fora dos limites — oráculo diz "inválido".
@example(payload={"bpm_threshold": 39})
@example(payload={"bpm_threshold": 221})
@example(payload={"temp_threshold": 34.9})
@example(payload={"temp_threshold": 42.1})
@example(payload={"buffer_limit": 9})
@example(payload={"buffer_limit": 501})
@example(payload={"sampling_interval_ms": 999})
@example(payload={"sampling_interval_ms": 60001})
# Patológicos — rejeição independente do valor.
@example(payload={})
@example(payload={"unknown_field": 10})
@example(payload={"bpm_threshold": True})       # bool rejeitado como int
@example(payload={"bpm_threshold": False})      # idem
@example(payload={"temp_threshold": float("nan")})
@example(payload={"temp_threshold": float("inf")})
def test_validate_config_bicondicional(payload: Dict[str, Any]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 19: validate_config bicondicional.

    Para todo ``payload`` dict (possivelmente com chaves/valores
    adversariais), ``validate_config(payload)[0]`` SHALL ser igual ao
    oráculo independente de faixas ``_oracle_valid(payload)``.
    """
    ok, parsed, reason = validate_config(payload)
    expected = _oracle_valid(payload)
    assert ok is expected, (
        f"bicondicional violada: validate_config={ok}, oráculo={expected} "
        f"para payload={payload!r} (reason={reason!r})"
    )
    if ok:
        # Sucesso: ``parsed`` deve conter tipos normalizados e sem motivo.
        assert reason is None
        assert parsed == _expected_parsed(payload), (
            f"parsed divergiu do esperado: got={parsed!r}, "
            f"expected={_expected_parsed(payload)!r}"
        )
    else:
        # Rejeição: parsed ausente e motivo em pt-BR obrigatório.
        assert parsed is None
        assert isinstance(reason, str) and len(reason) > 0, (
            "motivo de rejeição deve ser string não vazia em pt-BR"
        )


# =============================================================================
# Property 19 — apply_config preserva state quando payload é inválido
# =============================================================================

@settings(
    parent=settings.get_profile("cardioia-fast"),
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.filter_too_much],
)
@given(payload=_payload_st)
@example(payload={})
@example(payload={"unknown_field": 123})
@example(payload={"bpm_threshold": 39})     # fora da faixa
@example(payload={"temp_threshold": 42.5})  # fora da faixa
@example(payload={"buffer_limit": True})    # bool
@example(payload={"sampling_interval_ms": "5000"})
def test_apply_config_preserves_state_on_invalid(
    payload: Dict[str, Any],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 19: state inalterado em rejeição.

    Para todo ``payload`` **inválido**, ``apply_config(state, payload)``
    SHALL retornar um dict igual a ``state`` e um ``reason`` não nulo em
    pt-BR (R16.3). Também verifica que :class:`ConfigManager` acumula o
    motivo em ``rejection_events`` e não altera ``last_valid``.
    """
    # Pré-condição: queremos exercitar apenas os casos de rejeição.
    if _oracle_valid(payload):
        return  # caso coberto pelo teste de merge abaixo

    state = dict(DEFAULT_CONFIG)
    new_state, reason = apply_config(state, payload)

    assert new_state == state, (
        f"apply_config alterou state em rejeição: "
        f"antes={state!r}, depois={new_state!r}, payload={payload!r}"
    )
    # ``dict(state)`` — apply_config devolve cópia, não a referência original.
    assert new_state is not state or new_state == state
    assert reason is not None, "motivo de rejeição obrigatório (R16.3)"
    assert isinstance(reason, str) and len(reason) > 0

    # Nível superior: ConfigManager registra o motivo para publicação em
    # ``.../rejeicao`` (R16.3) e preserva last_valid (P20 em ação — sem
    # sobrescrita após rejeição).
    cm = ConfigManager(initial_state=DEFAULT_CONFIG)
    previous_last_valid = cm.last_valid
    ok, cm_reason = cm.apply(payload)
    assert ok is False
    assert cm_reason == reason
    assert cm.rejection_events[-1] == reason
    assert cm.state == state, "ConfigManager.state divergiu de DEFAULT_CONFIG"
    assert cm.last_valid == previous_last_valid, (
        "last_valid não pode ser sobrescrito por payload inválido (P20)"
    )


# =============================================================================
# Property 19 — apply_config faz merge determinístico quando válido
# =============================================================================

@settings(parent=settings.get_profile("cardioia-fast"))
@given(payload=_valid_payload_st)
@example(payload={"bpm_threshold": 40})
@example(payload={"bpm_threshold": 220})
@example(payload={"temp_threshold": 35.0})
@example(payload={"temp_threshold": 42.0})
@example(payload={"buffer_limit": 10})
@example(payload={"buffer_limit": 500})
@example(payload={"sampling_interval_ms": 1000})
@example(payload={"sampling_interval_ms": 60000})
@example(
    payload={
        "bpm_threshold": 120,
        "temp_threshold": 38.0,
        "buffer_limit": 50,
        "sampling_interval_ms": 5000,
    }
)
def test_apply_config_merges_on_valid(payload: Dict[str, Any]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 19: merge de payload válido.

    Para todo ``payload`` válido, ``apply_config(DEFAULT_CONFIG, payload)``
    SHALL retornar ``(novo_state, None)`` onde ``novo_state`` é
    ``DEFAULT_CONFIG`` com os campos parseados do payload sobrescritos
    (R16.2, parcial) e demais campos preservados.
    """
    state = dict(DEFAULT_CONFIG)
    new_state, reason = apply_config(state, payload)

    assert reason is None, (
        f"payload válido rejeitado inesperadamente: payload={payload!r}"
    )

    expected = dict(DEFAULT_CONFIG)
    expected.update(_expected_parsed(payload))
    assert new_state == expected, (
        f"merge incorreto: got={new_state!r}, expected={expected!r}, "
        f"payload={payload!r}"
    )

    # Campos não mencionados no payload permanecem idênticos ao state base.
    for key in DEFAULT_CONFIG:
        if key not in payload:
            assert new_state[key] == DEFAULT_CONFIG[key], (
                f"campo {key!r} preservado incorretamente em merge: "
                f"got={new_state[key]!r}, expected={DEFAULT_CONFIG[key]!r}"
            )


# =============================================================================
# Property 20 — ConfigManager.reboot() carrega a última config válida
# =============================================================================

# Sequência de payloads: alterna mecanicamente válidos e ruidosos para
# maximizar a chance de exercitar o caminho "última válida é N passos atrás".
_sequence_st: st.SearchStrategy[List[Dict[str, Any]]] = st.lists(
    st.one_of(_valid_payload_st, _payload_st),
    min_size=0, max_size=20,
)


@settings(
    parent=settings.get_profile("cardioia-fast"),
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.filter_too_much],
)
@given(sequence=_sequence_st)
# Caso 1: nenhuma config aplicada — reboot carrega DEFAULT_CONFIG.
@example(sequence=[])
# Caso 2: última aplicação válida — state pós-reboot == resultado da última.
@example(sequence=[{"bpm_threshold": 100}])
# Caso 3: válida seguida de inválida — reboot carrega a válida.
@example(sequence=[{"bpm_threshold": 100}, {"bpm_threshold": 999}])
# Caso 4: inválida no meio — ignorada; merge cumulativo das válidas.
@example(
    sequence=[
        {"bpm_threshold": 100},
        {"unknown_x": 1},           # rejeitada
        {"temp_threshold": 37.5},
    ]
)
# Caso 5: limites exatos — reboot preserva as coerções de tipo.
@example(
    sequence=[
        {"bpm_threshold": 40, "temp_threshold": 35.0},
        {"buffer_limit": 500, "sampling_interval_ms": 60000},
    ]
)
def test_last_valid_survives_reboot(sequence: List[Dict[str, Any]]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 20: última config válida persistida.

    Para toda sequência ``[c1..cn]`` de payloads (válidos e inválidos),
    após aplicar todos em um :class:`ConfigManager` e chamar
    :meth:`ConfigManager.reboot`, o estado carregado SHALL ser **exatamente**
    o resultado cumulativo das aplicações **válidas** (R16.2).
    """
    cm = ConfigManager(initial_state=DEFAULT_CONFIG)

    # Oráculo: aplica manualmente cada payload e mantém a "última config válida"
    # como a fusão cumulativa das aplicações válidas sobre ``DEFAULT_CONFIG``.
    oracle_state: Dict[str, Any] = dict(DEFAULT_CONFIG)
    applied_count = 0
    for payload in sequence:
        ok_cm, _reason_cm = cm.apply(payload)
        new_oracle, reason_oracle = apply_config(oracle_state, payload)
        # Sanidade: ConfigManager.apply e apply_config concordam sobre
        # validade do mesmo payload.
        assert (reason_oracle is None) is ok_cm, (
            f"divergência entre ConfigManager.apply e apply_config: "
            f"payload={payload!r}, cm_ok={ok_cm}, oracle_reason={reason_oracle!r}"
        )
        if reason_oracle is None:
            oracle_state = new_oracle
            applied_count += 1

    # Entre o último apply e o reboot, ``state`` já deve refletir a última
    # config válida (P20 é estrita: rejeições não alteram nada).
    assert cm.state == oracle_state, (
        f"state pré-reboot divergiu do oráculo: "
        f"cm.state={cm.state!r}, oracle={oracle_state!r}"
    )
    assert cm.last_valid == oracle_state

    loaded = cm.reboot()
    assert loaded == oracle_state, (
        f"reboot não restaurou a última config válida: "
        f"loaded={loaded!r}, oracle={oracle_state!r}, "
        f"aplicações válidas={applied_count}"
    )
    assert cm.state == oracle_state
    assert cm.last_valid == oracle_state


def test_reboot_default_when_no_config_applied() -> None:
    """Feature: cardioia-monitoramento-iot, Property 20: boot limpo carrega DEFAULT_CONFIG.

    Um :class:`ConfigManager` recém-instanciado sem aplicações anteriores
    SHALL, após ``reboot()``, devolver exatamente :data:`DEFAULT_CONFIG`
    (R16.2 — config padrão é persistida como ``last_valid`` no boot).
    """
    cm = ConfigManager()  # ``initial_state=None`` → DEFAULT_CONFIG
    loaded = cm.reboot()
    assert loaded == dict(DEFAULT_CONFIG)
    # Sanidade: defaults do reference model coerentes com config.h do firmware.
    assert loaded["bpm_threshold"] == BPM_THRESHOLD_DEFAULT
    assert loaded["temp_threshold"] == TEMP_THRESHOLD_DEFAULT
    assert loaded["buffer_limit"] == BUFFER_LIMIT_DEFAULT
    assert loaded["sampling_interval_ms"] == SAMPLING_INTERVAL_MS_DEFAULT
    assert cm.state == dict(DEFAULT_CONFIG)
    assert cm.last_valid == dict(DEFAULT_CONFIG)
    assert cm.rejection_events == []


# =============================================================================
# Cobertura extra — @example para faixas exatas (R16.1)
# =============================================================================
# Um teste example-based dedicado para cada limite (inferior/superior) facilita
# a leitura dos relatórios de CI e mantém o contrato R16.1 explícito, mesmo
# se alguma das propriedades acima for reescrita no futuro.


@pytest.mark.parametrize(
    "payload,expected_valid",
    [
        # bpm_threshold ∈ [40, 220]
        ({"bpm_threshold": 40},  True),
        ({"bpm_threshold": 220}, True),
        ({"bpm_threshold": 39},  False),
        ({"bpm_threshold": 221}, False),
        # temp_threshold ∈ [35.0, 42.0]
        ({"temp_threshold": 35.0}, True),
        ({"temp_threshold": 42.0}, True),
        ({"temp_threshold": 34.9}, False),
        ({"temp_threshold": 42.1}, False),
        # buffer_limit ∈ [10, 500]
        ({"buffer_limit": 10},  True),
        ({"buffer_limit": 500}, True),
        ({"buffer_limit": 9},   False),
        ({"buffer_limit": 501}, False),
        # sampling_interval_ms ∈ [1000, 60000]
        ({"sampling_interval_ms": 1000},  True),
        ({"sampling_interval_ms": 60000}, True),
        ({"sampling_interval_ms": 999},   False),
        ({"sampling_interval_ms": 60001}, False),
        # Patológicos
        ({}, False),
        ({"unknown_field": 10}, False),
        ({"bpm_threshold": True}, False),        # bool rejeitado como int
        ({"temp_threshold": float("nan")}, False),
    ],
)
def test_validate_config_range_boundaries_examples(
    payload: Dict[str, Any], expected_valid: bool
) -> None:
    """Limites exatos e casos patológicos — R16.1 (faixas inferior/superior).

    Cobertura example-based das fronteiras declaradas em
    ``requirements.md`` (Requisito 16.1). Complementa
    :func:`test_validate_config_bicondicional` com casos legíveis em CI.
    """
    ok, parsed, reason = validate_config(payload)
    assert ok is expected_valid, (
        f"bicondicional de fronteira violada: payload={payload!r}, "
        f"esperado_valido={expected_valid}, got={ok}, reason={reason!r}"
    )
    if expected_valid:
        assert parsed == _expected_parsed(payload)
        assert reason is None
    else:
        assert parsed is None
        assert isinstance(reason, str) and len(reason) > 0
