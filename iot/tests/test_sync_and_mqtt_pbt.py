"""
Testes de propriedade (PBT) para o módulo **CardioIA – Monitoramento IoT (Fase 3)**.

Feature: ``cardioia-monitoramento-iot``

Este arquivo valida três propriedades centrais da camada de transporte do
firmware embarcado (lógica pura espelhada em ``reference_model.py``):

* **Property 13 — Conservação do ``SyncScheduler`` (sem perda nem duplicação).**
  Para todo buffer inicial ``B0`` ordenado por timestamp e toda sequência de
  eventos ``e ∈ {ack, nack, timeout, offline, exception}*``, vale a identidade
  ``multiset(published) ⊎ multiset(buffer_final) == multiset(B0)`` e a lista
  ``published`` preserva a ordem cronológica de ``B0``.

* **Property 14 — FSM de reconexão MQTT.**
  Para toda sequência de eventos
  ``ev ∈ {connect_ok, connect_fail, auth_rejected, flag_true}*``, o estado e
  o contador de falhas consecutivas da FSM real são idênticos aos produzidos
  por um oráculo Python que reimplementa a especificação (R8.4, R8.6, R7.5).

* **Property 15 — Formato canônico do tópico MQTT de telemetria.**
  Para todo ``paciente_id`` casando com ``^[A-Za-z0-9_-]{1,32}$``,
  ``topic_telemetry(id) == "cardioia/paciente/" + id + "/sinais"`` e o
  tamanho do payload publicado respeita o limite superior
  ``MQTT_PAYLOAD_MAX = 1024`` bytes (R8.2).

Perfis de `hypothesis` utilizados:

* ``cardioia-thorough`` (``max_examples=500``) — aplicado à Property 13,
  conforme ``design.md`` (seção "Testing Strategy"); a conservação do
  SyncScheduler exercita sequências combinatoriais grandes de buffer ×
  eventos.
* ``cardioia-fast`` (``max_examples=200``, padrão) — aplicado às
  Properties 14 e 15, cujas cardinalidades efetivas de input são menores.

_Requirements: 7.1, 7.2, 7.4, 7.5, 8.2, 8.4, 8.6, 8.7_
_Properties: P13, P14, P15_
"""

from __future__ import annotations

from collections import Counter

import pytest
from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import (
    MQTT_PAYLOAD_MAX,
    MqttReconnectFSM,
    MqttState,
    SampleRecord,
    serialize_sample,
    sync,
    topic_telemetry,
)


# =============================================================================
# Estratégias auxiliares compartilhadas
# =============================================================================

#: Estratégia de timestamps ordenados (cronológicos) para o ``initial_buffer``
#: da Property 13. A função :func:`sorted` garante a ordem crescente exigida
#: pelo invariante de P13; permitimos duplicatas (timestamps iguais) para
#: exercitar o comportamento correto de ``Counter`` sobre ``SampleRecord``
#: frozen (dataclass hashable).
_timestamps_sorted = st.lists(
    st.integers(min_value=0, max_value=10**6),
    min_size=0,
    max_size=50,
).map(sorted)

#: Estratégia dos eventos do canal de sync (R7.2, R7.4).
_sync_events = st.lists(
    st.sampled_from(["ack", "nack", "timeout", "offline", "exception"]),
    min_size=0,
    max_size=100,
)

#: Estratégia dos eventos da FSM de reconexão MQTT (P14).
_mqtt_events = st.lists(
    st.sampled_from(["connect_ok", "connect_fail", "auth_rejected", "flag_true"]),
    min_size=0,
    max_size=50,
)

#: Regex do ``paciente_id`` aceito pelo tópico canônico (Property 15 / R8.2).
_PACIENTE_ID_TOPIC_REGEX = r"[A-Za-z0-9_-]{1,32}"


def _build_sample(timestamp: int) -> SampleRecord:
    """Monta um ``SampleRecord`` determinístico a partir de ``timestamp``.

    Os demais campos são fixos; isso é intencional — o que importa para a
    Property 13 é a **identidade** dos registros (via igualdade estrutural
    do dataclass frozen), não a variação de valores clínicos. Concentrar a
    variação em ``timestamp`` mantém os exemplos falhos minimizados pelo
    ``hypothesis`` legíveis.
    """
    return SampleRecord(
        timestamp=int(timestamp),
        temperatura=36.5,
        umidade=50,
        bpm=70,
        paciente_id="PAC-0001",
    )


# =============================================================================
# Property 13 — Conservação do SyncScheduler
# =============================================================================


@settings(settings.get_profile("cardioia-thorough"))
@given(timestamps=_timestamps_sorted, events=_sync_events)
def test_sync_preserves_multiset_and_order(timestamps, events):
    """Feature: cardioia-monitoramento-iot, Property 13: conservação do SyncScheduler.

    *Para toda* configuração inicial do ``EdgeBuffer`` ``B0`` (ordenada por
    ``timestamp`` crescente) e toda sequência de eventos
    ``e ∈ {ack, nack, timeout, offline, exception}*``, ao final do
    processamento vale:

    1. ``Counter(published) + Counter(buffer_final) == Counter(B0)``
       (nenhum registro é perdido, nenhum é duplicado).
    2. ``[r.timestamp for r in published]`` é não-decrescente (os registros
       publicados saem na ordem cronológica original de ``B0``).

    **Validates: Requirements 7.1, 7.2, 7.4, 8.7**
    """
    initial_buffer = [_build_sample(ts) for ts in timestamps]
    published, buffer_final = sync(initial_buffer, events)

    # Conservação (multiset): nenhum registro surge do nada nem desaparece.
    assert Counter(published) + Counter(buffer_final) == Counter(initial_buffer), (
        "SyncScheduler violou conservação: "
        f"|B0|={len(initial_buffer)}, |publicados|={len(published)}, "
        f"|buffer_final|={len(buffer_final)}, eventos={events}"
    )

    # Ordem cronológica crescente dos registros publicados (R7.2).
    published_ts = [r.timestamp for r in published]
    assert published_ts == sorted(published_ts), (
        "publicados violaram a ordem cronológica crescente: "
        f"{published_ts}"
    )

    # Sanidade adicional: |publicados| <= número de 'ack' em events (cada
    # record só sai do buffer via um evento 'ack'; 'nack'/'timeout' não
    # consomem, 'offline'/'exception' interrompem).
    acks = events.count("ack")
    assert len(published) <= acks, (
        f"|publicados|={len(published)} excedeu número de acks ({acks})"
    )


# =============================================================================
# Property 14 — FSM de reconexão MQTT
# =============================================================================


def _mqtt_oracle(events: list[str]) -> tuple[MqttState, int]:
    """Oráculo Python da :class:`MqttReconnectFSM` para a Property 14.

    Replica passo-a-passo a especificação da FSM (R8.4, R8.6, R7.5), de
    forma independente da implementação sob teste, para servir de
    referência ao teste property-based.

    Invariantes da FSM (espelhando a documentação do ``reference_model``):

    * Estado inicial: ``OFFLINE`` com ``fail_count = 0``.
    * ``connect_ok`` quando ``state != AUTH_SUSPENDED``:
      ``state = ONLINE, fail_count = 0``. Em ``AUTH_SUSPENDED`` é ignorado.
    * ``connect_fail`` quando ``state != AUTH_SUSPENDED``:
      ``fail_count += 1``; se ``fail_count >= 3`` então ``state = OFFLINE``.
      Em ``AUTH_SUSPENDED`` é ignorado (contador não avança).
    * ``auth_rejected``: ``state = AUTH_SUSPENDED, fail_count = 0``.
    * ``flag_true``: ``fail_count = 0``, estado preservado.
    """
    state = MqttState.OFFLINE
    fail_count = 0
    for ev in events:
        if ev == "connect_ok":
            if state != MqttState.AUTH_SUSPENDED:
                state = MqttState.ONLINE
                fail_count = 0
        elif ev == "connect_fail":
            if state != MqttState.AUTH_SUSPENDED:
                fail_count += 1
                if fail_count >= 3:
                    state = MqttState.OFFLINE
        elif ev == "auth_rejected":
            state = MqttState.AUTH_SUSPENDED
            fail_count = 0
        elif ev == "flag_true":
            fail_count = 0
        else:  # pragma: no cover — estratégia não emite outros eventos.
            raise ValueError(f"evento desconhecido no oráculo: {ev!r}")
    return state, fail_count


@given(events=_mqtt_events)
def test_mqtt_reconnect_fsm_rules(events):
    """Feature: cardioia-monitoramento-iot, Property 14: FSM de reconexão MQTT.

    *Para toda* sequência de eventos
    ``ev ∈ {connect_ok, connect_fail, auth_rejected, flag_true}*``, o
    ``(state, fail_count)`` final produzido pela
    :class:`MqttReconnectFSM` é idêntico ao valor calculado pelo oráculo
    :func:`_mqtt_oracle` (reimplementação independente da especificação).

    **Validates: Requirements 7.5, 8.4, 8.6**
    """
    fsm = MqttReconnectFSM()
    for ev in events:
        fsm.advance(ev)
    expected_state, expected_fail_count = _mqtt_oracle(events)
    assert (fsm.state, fsm.fail_count) == (expected_state, expected_fail_count), (
        "FSM divergiu do oráculo:\n"
        f"  eventos={events}\n"
        f"  esperado=({expected_state}, {expected_fail_count})\n"
        f"  obtido=({fsm.state}, {fsm.fail_count})"
    )


@given(events=_mqtt_events)
def test_mqtt_three_consecutive_connect_fail_go_offline(events):
    """Feature: cardioia-monitoramento-iot, Property 14: 3 falhas consecutivas → OFFLINE.

    Sub-regra explícita (R8.4): depois de processar ``events`` seguidos de
    3 eventos ``"connect_fail"`` consecutivos **sem** que a FSM esteja em
    ``AUTH_SUSPENDED`` ao iniciar a sequência final, o estado resultante
    SHALL ser ``OFFLINE``.
    """
    fsm = MqttReconnectFSM()
    for ev in events:
        fsm.advance(ev)
    # Se a FSM está suspensa por auth_rejected, a regra dos 3 fails não se
    # aplica — ela fica em AUTH_SUSPENDED até um ``reset`` externo (R8.6).
    if fsm.state == MqttState.AUTH_SUSPENDED:
        return
    for _ in range(3):
        fsm.advance("connect_fail")
    assert fsm.state == MqttState.OFFLINE, (
        f"3 'connect_fail' consecutivos não levaram a OFFLINE (state={fsm.state})"
    )


def test_auth_suspended_ignores_connect_ok_until_reset():
    """Feature: cardioia-monitoramento-iot, Property 14: AUTH_SUSPENDED exige reset.

    Cenário baseado em exemplo (R8.6): após ``auth_rejected``, eventos
    ``connect_ok`` e ``flag_true`` SHALL ser ignorados (estado permanece
    ``AUTH_SUSPENDED``). Apenas um evento externo ``reset`` libera a FSM
    de volta a ``OFFLINE``; a partir daí, um ``connect_ok`` SHALL
    restaurar o estado ``ONLINE``.
    """
    fsm = MqttReconnectFSM()

    # auth_rejected → AUTH_SUSPENDED
    fsm.advance("auth_rejected")
    assert fsm.state == MqttState.AUTH_SUSPENDED
    assert fsm.fail_count == 0

    # connect_ok e flag_true não devem sair de AUTH_SUSPENDED
    fsm.advance("connect_ok")
    assert fsm.state == MqttState.AUTH_SUSPENDED, (
        "connect_ok não pode transicionar a FSM para fora de AUTH_SUSPENDED"
    )
    fsm.advance("connect_ok")
    assert fsm.state == MqttState.AUTH_SUSPENDED
    fsm.advance("flag_true")
    assert fsm.state == MqttState.AUTH_SUSPENDED
    assert fsm.fail_count == 0

    # reset externo → OFFLINE, fail_count zerado
    fsm.advance("reset")
    assert fsm.state == MqttState.OFFLINE
    assert fsm.fail_count == 0

    # Após reset, connect_ok volta a transicionar normalmente
    fsm.advance("connect_ok")
    assert fsm.state == MqttState.ONLINE
    assert fsm.fail_count == 0


# =============================================================================
# Property 15 — Tópico canônico e limite de payload MQTT
# =============================================================================


@given(pid=st.from_regex(_PACIENTE_ID_TOPIC_REGEX, fullmatch=True))
def test_topic_telemetry_format(pid):
    """Feature: cardioia-monitoramento-iot, Property 15: formato canônico do tópico.

    *Para todo* ``paciente_id`` casando com ``^[A-Za-z0-9_-]{1,32}$``,
    ``topic_telemetry(paciente_id) == "cardioia/paciente/" + paciente_id + "/sinais"``.

    **Validates: Requirements 8.2**
    """
    expected = f"cardioia/paciente/{pid}/sinais"
    assert topic_telemetry(pid) == expected


@pytest.mark.parametrize(
    "invalid_id",
    [
        "",                  # vazio — viola ``{1,32}`` mínimo
        "a" * 33,            # 33 caracteres — excede ``{1,32}`` máximo
        "foo bar",           # espaço — fora do alfabeto ``[A-Za-z0-9_-]``
        "PAC/1",             # barra — fora do alfabeto
        "PAC.1",             # ponto — fora do alfabeto
        "paciente@x",        # @ — fora do alfabeto
    ],
    ids=["empty", "too_long", "space", "slash", "dot", "at_sign"],
)
def test_topic_telemetry_rejects_invalid_ids(invalid_id):
    """Feature: cardioia-monitoramento-iot, Property 15: rejeição de paciente_id inválido.

    Exemplos explícitos (R8.2, R15.2): strings vazias, com mais de 32
    caracteres ou contendo caracteres fora de ``[A-Za-z0-9_-]`` SHALL
    disparar ``ValueError`` em :func:`topic_telemetry`.
    """
    with pytest.raises(ValueError):
        topic_telemetry(invalid_id)


@st.composite
def _valid_sample_records_for_mqtt(draw):
    """Estratégia de ``SampleRecord`` válido para testes de payload MQTT.

    Constrange ``paciente_id`` ao padrão anonimizado
    ``PAC-\\d{1,27}`` (Property 18) e os campos clínicos às faixas
    normativas (R3.1). Temperatura e umidade podem ser ``None`` para
    exercitar o caminho de serialização com ``null``.
    """
    timestamp = draw(st.integers(min_value=0, max_value=2**31 - 1))
    temperatura = draw(
        st.one_of(
            st.none(),
            st.floats(
                min_value=-40.0,
                max_value=80.0,
                allow_nan=False,
                allow_infinity=False,
            ),
        )
    )
    umidade = draw(st.one_of(st.none(), st.integers(min_value=0, max_value=100)))
    bpm = draw(st.one_of(st.none(), st.integers(min_value=0, max_value=250)))
    pid = draw(st.from_regex(r"PAC-\d{1,27}", fullmatch=True))
    return SampleRecord(
        timestamp=timestamp,
        temperatura=temperatura,
        umidade=umidade,
        bpm=bpm,
        paciente_id=pid,
    )


@given(record=_valid_sample_records_for_mqtt())
@example(  # caso canônico do protótipo
    record=SampleRecord(
        timestamp=1_737_212_812_345,
        temperatura=36.7,
        umidade=58,
        bpm=74,
        paciente_id="PAC-0001",
    )
)
def test_sample_record_payload_within_mqtt_limit(record):
    """Feature: cardioia-monitoramento-iot, Property 15: payload ≤ MQTT_PAYLOAD_MAX.

    *Para todo* ``SampleRecord`` válido cujo JSON cabe em ``MAX_JSON_LEN``
    (256 caracteres, R3.3), o payload publicado em
    ``cardioia/paciente/{paciente_id}/sinais`` SHALL ter no máximo
    ``MQTT_PAYLOAD_MAX = 1024`` bytes (R8.2). O bound é trivialmente
    satisfeito — já que 256 caracteres ASCII/Latin produzem ≤ 256 bytes
    UTF-8 para o alfabeto canônico do ``SampleRecord`` —, mas a
    propriedade formaliza a garantia ponta-a-ponta entre R3.3 e R8.2.

    **Validates: Requirements 3.3, 8.2**
    """
    payload = serialize_sample(record)
    # R3.6: serialização pode devolver None se o JSON exceder 256 chars;
    # nesse caso, o firmware descarta e nada é publicado — a propriedade
    # do payload MQTT não se aplica.
    if payload is None:
        return
    payload_bytes = payload.encode("utf-8")
    assert len(payload_bytes) <= MQTT_PAYLOAD_MAX, (
        f"payload excede MQTT_PAYLOAD_MAX={MQTT_PAYLOAD_MAX}: "
        f"|payload|={len(payload_bytes)} bytes, record={record!r}"
    )
