"""
Testes de propriedade do módulo **CardioIA – Monitoramento IoT (Fase 3)**
cobrindo o ``ConnectivityController`` (transições da ``Connectivity_Flag`` e
parser de comandos Serial).

Feature tag obrigatória: ``cardioia-monitoramento-iot``.

Propriedades validadas:

* **Property 11** — Eventos de transição de ``Connectivity_Flag``.
  Para toda transição efetiva (``old_flag != new_flag``) e todo tamanho de
  buffer ``n >= 0``, :func:`reference_model.transition_event` SHALL emitir
  um evento contendo (a) a direção (``"false->true"`` ou ``"true->false"``),
  (b) ``buffer_size`` inteiro ``>= 0`` igual ao número de Sample_Record
  pendentes e (c) um texto distintivo para cada direção.
  Valida Requirements 6.3 e 6.4.

* **Property 12** — O parser de comandos Serial preserva estado em inputs
  inválidos. Para toda string ``raw``, :func:`reference_model.parse_cmd`
  SHALL aceitar exclusivamente os comandos canônicos
  ``{"ONLINE", "OFFLINE", "STATUS", "CONFIG_SHOW"}`` após ``strip().upper()``
  e, em qualquer outra entrada, preservar o estado da FSM e registrar a
  string original no evento de rejeição. Valida Requirement 6.5.

Exemplos fixos (``@example``) cobrem: string vazia, string só de espaços,
combinações de maiúsculas/minúsculas (aceitas), espaços em torno do comando
(aceitos após trim), caracteres de espaço estendidos como ``\\n``/``\\t``
(aceitos após trim), e comandos próximos porém inválidos (``"ONLINNE"``).
"""

from __future__ import annotations

from hypothesis import example, given, settings, strategies as st

from reference_model import (
    VALID_SERIAL_CMDS,
    ConnectivityState,
    parse_cmd,
    transition_event,
)


# ---------------------------------------------------------------------------
# Perfil de execução — aplicamos ``cardioia-fast`` (P11/P12 não estão no
# grupo ``cardioia-thorough``, que cobre P1, P2 e P13 conforme o design).
# ---------------------------------------------------------------------------
_FAST = settings.get_profile("cardioia-fast")


# ===========================================================================
# Property 11 — Eventos de transição de Connectivity_Flag
# ===========================================================================


@settings(_FAST)
@given(
    old=st.booleans(),
    new=st.booleans(),
    buf=st.integers(min_value=0, max_value=10**6),
)
@example(old=False, new=True, buf=0)
@example(old=True, new=False, buf=0)
@example(old=False, new=True, buf=50)
@example(old=True, new=False, buf=10**6)
def test_transition_event_contains_required_fields(
    old: bool, new: bool, buf: int
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 11: evento de transição
    da Connectivity_Flag contém tipo, tamanho de buffer ``n >= 0`` e texto
    distintivo por direção.

    Validates: Requirements 6.3, 6.4
    """
    # O evento só é emitido em transições efetivas da Connectivity_Flag; a
    # assume descarta amostras em que ``old == new`` (não há transição).
    from hypothesis import assume

    assume(old != new)

    event = transition_event(old, new, buf)

    # (a) Campos obrigatórios presentes.
    assert set(event.keys()) >= {"direction", "buffer_size", "text"}, (
        f"evento deve conter ao menos 'direction', 'buffer_size' e 'text'; "
        f"recebido: {set(event.keys())}"
    )

    # (b) Direction canônica e consistente com (old, new).
    assert event["direction"] in ("false->true", "true->false"), (
        f"direction inválida: {event['direction']!r}"
    )
    expected_direction = "false->true" if (not old and new) else "true->false"
    assert event["direction"] == expected_direction, (
        f"direction inconsistente: esperado {expected_direction!r}, "
        f"recebido {event['direction']!r}"
    )

    # (c) buffer_size é inteiro ``>= 0`` e preserva o valor informado.
    assert isinstance(event["buffer_size"], int), (
        f"buffer_size deve ser int, recebido {type(event['buffer_size']).__name__}"
    )
    # ``bool`` é subclasse de ``int`` em Python, então rejeitamos explicitamente.
    assert not isinstance(event["buffer_size"], bool), (
        "buffer_size não pode ser booleano"
    )
    assert event["buffer_size"] >= 0, "buffer_size deve ser ≥ 0 (R6.3)"
    assert event["buffer_size"] == buf, (
        f"buffer_size deve espelhar o valor de entrada ({buf}), "
        f"recebido {event['buffer_size']}"
    )

    # (d) Texto é string não vazia e distintivo por direção.
    assert isinstance(event["text"], str), "text deve ser string"
    assert len(event["text"]) > 0, "text não pode ser vazio"

    # Verifica que o texto é distintivo por direção: reconstruímos o evento
    # da direção oposta (com o mesmo buffer_size) e comparamos o texto.
    opposite_event = transition_event(new, old, buf)
    assert event["text"] != opposite_event["text"], (
        f"o texto deve ser distintivo por direção; mesmo texto encontrado para "
        f"{event['direction']!r} e {opposite_event['direction']!r}: "
        f"{event['text']!r}"
    )


def test_transition_event_rejects_non_transition() -> None:
    """Example-based: ``transition_event`` rejeita chamadas sem transição.

    Feature: cardioia-monitoramento-iot, Property 11 (caso degenerado).
    Chamar com ``old == new`` viola a pré-condição do evento de transição e
    SHALL disparar ``ValueError``.

    Validates: Requirements 6.3, 6.4
    """
    import pytest

    with pytest.raises(ValueError):
        transition_event(True, True, 0)
    with pytest.raises(ValueError):
        transition_event(False, False, 0)
    with pytest.raises(ValueError):
        transition_event(True, True, 25)


def test_transition_event_rejects_negative_buffer() -> None:
    """Example-based: ``transition_event`` rejeita ``buffer_size`` negativo.

    Feature: cardioia-monitoramento-iot, Property 11 (pré-condição R6.3 de
    que ``n >= 0``).

    Validates: Requirement 6.3
    """
    import pytest

    with pytest.raises(ValueError):
        transition_event(False, True, -1)
    with pytest.raises(ValueError):
        transition_event(True, False, -42)


# ===========================================================================
# Property 12 — Parser de comandos Serial preserva estado em inputs inválidos
# ===========================================================================


def _expected_parse(raw: str, state: ConnectivityState) -> tuple[ConnectivityState, str]:
    """Oracle determinístico de :func:`parse_cmd` (espelha o reference model).

    Retorna ``(novo_state, tipo_do_evento)``. Não mimetiza os campos extras
    (``raw``, ``state``) — estes são conferidos diretamente pelo teste.
    """
    normalized = raw.strip().upper() if isinstance(raw, str) else ""
    if normalized == "ONLINE":
        return ConnectivityState.ONLINE, "online"
    if normalized == "OFFLINE":
        return ConnectivityState.OFFLINE, "offline"
    if normalized == "STATUS":
        return state, "status"
    if normalized == "CONFIG_SHOW":
        return state, "config_show"
    return state, "rejected"


@settings(_FAST)
@given(
    raw=st.text(min_size=0, max_size=30),
    state=st.sampled_from(list(ConnectivityState)),
)
# Aceitos após normalização (strip + upper).
@example(raw="ONLINE", state=ConnectivityState.OFFLINE)
@example(raw="offline", state=ConnectivityState.ONLINE)
@example(raw="onLINE", state=ConnectivityState.OFFLINE)
@example(raw="  status  ", state=ConnectivityState.ONLINE)
@example(raw="\nSTATUS\t", state=ConnectivityState.ONLINE)
@example(raw="OFFLINE ", state=ConnectivityState.ONLINE)
@example(raw="CONFIG_SHOW", state=ConnectivityState.OFFLINE)
# Rejeitados.
@example(raw="", state=ConnectivityState.ONLINE)
@example(raw="   ", state=ConnectivityState.OFFLINE)
@example(raw="ONLINNE", state=ConnectivityState.OFFLINE)
@example(raw="CONFIG SHOW", state=ConnectivityState.ONLINE)
def test_parse_cmd_accepts_canonical_and_rejects_others(
    raw: str, state: ConnectivityState
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 12: ``parse_cmd`` só
    aceita ``{"ONLINE","OFFLINE","STATUS","CONFIG_SHOW"}`` (case-insensitive
    após trim); preserva estado em inputs inválidos e registra a string
    original no evento de rejeição.

    Validates: Requirement 6.5
    """
    new_state, event = parse_cmd(raw, state)

    expected_state, expected_type = _expected_parse(raw, state)

    # (a) Estado resultante bate com o oráculo.
    assert new_state == expected_state, (
        f"estado resultante inconsistente; raw={raw!r}, state={state}, "
        f"esperado={expected_state}, recebido={new_state}"
    )

    # (b) Tipo do evento bate com o oráculo.
    assert event.get("type") == expected_type, (
        f"tipo de evento inconsistente; raw={raw!r}, "
        f"esperado={expected_type!r}, recebido={event.get('type')!r}"
    )

    # (c) Em qualquer direção, o evento preserva a string ``raw`` original
    # (critério explícito de rastreabilidade do log; R6.5 exige isso no caso
    # de rejeição e o reference model estende para todos os eventos).
    assert event.get("raw") == raw, (
        f"o evento deve preservar a string original; esperado {raw!r}, "
        f"recebido {event.get('raw')!r}"
    )

    # (d) Para STATUS e CONFIG_SHOW, o evento também reporta o estado atual.
    if expected_type in ("status", "config_show"):
        assert event.get("state") == state.value, (
            f"evento {expected_type!r} deve reportar state={state.value!r}, "
            f"recebido {event.get('state')!r}"
        )


@settings(_FAST)
@given(
    raw=st.text(min_size=0, max_size=30).filter(
        lambda s: s.strip().upper() not in VALID_SERIAL_CMDS
    ),
    state=st.sampled_from(list(ConnectivityState)),
)
@example(raw="", state=ConnectivityState.ONLINE)
@example(raw="   ", state=ConnectivityState.OFFLINE)
@example(raw="ONLINNE", state=ConnectivityState.ONLINE)
@example(raw="OF FLINE", state=ConnectivityState.OFFLINE)
@example(raw="config show", state=ConnectivityState.ONLINE)
@example(raw="?", state=ConnectivityState.OFFLINE)
def test_parse_cmd_preserves_state_on_rejection(
    raw: str, state: ConnectivityState
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 12: para toda string
    ``raw`` fora do conjunto canônico (após ``strip().upper()``),
    ``parse_cmd(raw, state) == (state, {..., "type": "rejected", "raw": raw})``.

    Validates: Requirement 6.5
    """
    new_state, event = parse_cmd(raw, state)

    # Estado preservado integralmente.
    assert new_state == state, (
        f"estado deveria permanecer {state} em rejeição; "
        f"raw={raw!r}, recebido={new_state}"
    )

    # Evento marcado como rejeitado.
    assert event.get("type") == "rejected", (
        f"evento de comando inválido deve ter type='rejected'; "
        f"raw={raw!r}, recebido={event.get('type')!r}"
    )

    # Log de rejeição contém a string original (R6.5).
    assert event.get("raw") == raw, (
        f"log de rejeição deve conter a string original; esperado {raw!r}, "
        f"recebido {event.get('raw')!r}"
    )
