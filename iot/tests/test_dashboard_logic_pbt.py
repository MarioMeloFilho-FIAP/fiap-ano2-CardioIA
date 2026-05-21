"""
Testes de propriedade para a lógica do dashboard Node-RED (Alert_Module).

Feature: ``cardioia-monitoramento-iot``.

Este módulo valida as seguintes propriedades do design (``design.md``
seção *Correctness Properties*) contra o ``reference_model.py`` — que por
contrato espelha a lógica pura do dashboard e serve como oráculo:

* **Property 16** — :func:`validate_message` retorna ``válida`` se e
  somente se ``raw`` é JSON parseável com exatamente as chaves canônicas
  e valores dentro das faixas declaradas. Caso inválido, retorna
  ``(False, None)`` — o dashboard incrementa o contador de mensagens
  inválidas em 1 e NÃO atualiza gráficos ou gauge (R9.5, R10.4, R11.6).

* **Property 17** — :func:`classify_alert` produz o subconjunto
  ``{BPM_ALTO : bpm > bpm_threshold, TEMP_ALTA : temp > temp_threshold}``
  com comparação estrita (R11.1, R11.2, R11.3). :func:`alert_entry`
  formata a entrada com cor verde + texto ``"Normal"`` quando não há
  alertas e cor vermelha com os textos clínicos simultâneos quando há
  (R11.4). Toda entrada contém obrigatoriamente ``timestamp``,
  ``paciente_id``, métrica(s) e valor(es) medido(s) para fins de
  auditabilidade do dashboard (R11.5).

Convenção de tag: todo docstring de propriedade inicia com
``"Feature: cardioia-monitoramento-iot, Property N: …"``.
"""

from __future__ import annotations

import json
import math
from typing import Any, Optional

import pytest
from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import (
    BPM_THRESHOLD_DEFAULT,
    CANONICAL_KEYS,
    HUM_MAX,
    HUM_MIN,
    MAX_BPM,
    MAX_PACIENTE_ID_LEN,
    TEMP_MAX,
    TEMP_MIN,
    TEMP_THRESHOLD_DEFAULT,
    alert_entry,
    classify_alert,
    validate_message,
)


# =============================================================================
# Estratégias compartilhadas
# =============================================================================

# ``timestamp``: inteiro não negativo em ms. Limite em 2**31-1 para espelhar
# o comportamento de ``millis()`` do ESP32 (32 bits).
timestamp_strategy = st.integers(min_value=0, max_value=(2**31) - 1)

# ``temperatura``: float com uma casa decimal ou ``None`` (R3.5). O
# arredondamento para uma casa decimal reflete a política de serialização
# canônica (R1.2), evitando falsos contraexemplos por ruído numérico.
temperatura_strategy = st.one_of(
    st.none(),
    st.floats(
        min_value=TEMP_MIN,
        max_value=TEMP_MAX,
        allow_nan=False,
        allow_infinity=False,
    ).map(lambda x: round(x, 1)),
)

# ``umidade``: inteiro em [0, 100] ou ``None``.
umidade_strategy = st.one_of(
    st.none(),
    st.integers(min_value=HUM_MIN, max_value=HUM_MAX),
)

# ``bpm``: inteiro em [0, 250] ou ``None``.
bpm_strategy = st.one_of(
    st.none(),
    st.integers(min_value=0, max_value=MAX_BPM),
)

# ``paciente_id``: string não vazia, até 32 caracteres. Evitamos
# ``surrogate halves`` (categoria Unicode ``Cs``) porque ``json.dumps``
# não os representa de forma estável sem ``ensure_ascii=False`` + encode.
# A restrição é funcionalmente equivalente à utilizada pelo firmware.
paciente_id_strategy = st.text(
    alphabet=st.characters(blacklist_categories=("Cs",)),
    min_size=1,
    max_size=MAX_PACIENTE_ID_LEN,
)

# ``bpm_threshold`` e ``temp_threshold`` para P17 (faixas de R16.1).
bpm_threshold_strategy = st.integers(min_value=40, max_value=220)
temp_threshold_strategy = st.floats(
    min_value=35.0,
    max_value=42.0,
    allow_nan=False,
    allow_infinity=False,
)


def _build_canonical_json(
    timestamp: int,
    temperatura: Optional[float],
    umidade: Optional[int],
    bpm: Optional[int],
    paciente_id: str,
) -> str:
    """Serializa um payload canônico na mesma ordem de :data:`CANONICAL_KEYS`.

    Usado por :func:`test_validate_message_accepts_canonical_json` para
    garantir que a entrada gerada casa exatamente com o formato que o
    firmware publicaria em ``.../sinais``.
    """
    ordered = {
        "timestamp": int(timestamp),
        "temperatura": temperatura,
        "umidade": umidade,
        "bpm": bpm,
        "paciente_id": paciente_id,
    }
    return json.dumps(ordered, separators=(",", ":"), ensure_ascii=False)


# =============================================================================
# Property 16 — validate_message
# =============================================================================


@settings(settings.get_profile("cardioia-fast"))
@given(
    timestamp=timestamp_strategy,
    temperatura=temperatura_strategy,
    umidade=umidade_strategy,
    bpm=bpm_strategy,
    paciente_id=paciente_id_strategy,
)
@example(
    timestamp=0,
    temperatura=None,
    umidade=None,
    bpm=None,
    paciente_id="PAC-0001",
)
@example(
    timestamp=1,
    temperatura=36.7,
    umidade=58,
    bpm=74,
    paciente_id="PAC-0001",
)
@example(
    timestamp=1,
    temperatura=-40.0,
    umidade=0,
    bpm=0,
    paciente_id="A",
)
@example(
    timestamp=1,
    temperatura=80.0,
    umidade=100,
    bpm=MAX_BPM,
    paciente_id="X" * MAX_PACIENTE_ID_LEN,
)
def test_validate_message_accepts_canonical_json(
    timestamp: int,
    temperatura: Optional[float],
    umidade: Optional[int],
    bpm: Optional[int],
    paciente_id: str,
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 16: validate_message aceita JSON canônico.

    Para todo payload canônico gerado nas mesmas faixas do firmware
    (``timestamp >= 0``, ``temperatura ∈ [-40.0, 80.0] ∪ {None}``,
    ``umidade ∈ [0, 100] ∪ {None}``, ``bpm ∈ [0, 250] ∪ {None}``,
    ``paciente_id`` não vazio ≤ 32 chars), ``validate_message`` retorna
    ``(True, parsed)`` com ``parsed`` estruturalmente equivalente ao
    dicionário original (R3.2, R9.5).
    """
    raw = _build_canonical_json(timestamp, temperatura, umidade, bpm, paciente_id)

    ok, parsed = validate_message(raw)

    assert ok is True, (
        f"validate_message rejeitou payload canônico: raw={raw!r}"
    )
    assert parsed is not None
    # (1) Chaves exatamente canônicas — nem a mais, nem a menos (R3.2).
    assert set(parsed.keys()) == set(CANONICAL_KEYS), (
        f"chaves inválidas no parsed: {set(parsed.keys())}"
    )
    # (2) Igualdade estrutural com os valores gerados.
    assert parsed["timestamp"] == int(timestamp)
    assert parsed["umidade"] == umidade
    assert parsed["bpm"] == bpm
    assert parsed["paciente_id"] == paciente_id
    # Temperatura compara com tolerância mínima (o reference model
    # re-arredonda para 1 casa decimal em :func:`validate_message`).
    if temperatura is None:
        assert parsed["temperatura"] is None
    else:
        assert parsed["temperatura"] == round(float(temperatura), 1)


@settings(settings.get_profile("cardioia-fast"))
@given(raw=st.text(max_size=200))
def test_validate_message_rejects_arbitrary_strings(raw: str) -> None:
    """Feature: cardioia-monitoramento-iot, Property 16: validate_message sobre strings adversariais.

    Para toda string arbitrária ``raw``, ``validate_message`` retorna um
    par ``(ok, parsed)`` obedecendo os invariantes:

    * ``ok`` é ``bool``;
    * ``parsed is None`` se e somente se ``ok is False``;
    * se ``ok is True``, ``parsed`` é um ``dict`` com exatamente as
      chaves canônicas e valores dentro das faixas declaradas
      (``bpm ∈ [0, 250] ∪ {None}``, ``temperatura ∈ [-40.0, 80.0] ∪ {None}``,
      ``umidade ∈ [0, 100] ∪ {None}``, ``paciente_id`` string não vazia
      com até 32 caracteres e ``timestamp >= 0``).

    Os invariantes encapsulam o contrato de R9.5/R10.4/R11.6: em caso
    inválido o dashboard não propaga para gráficos/gauge.
    """
    ok, parsed = validate_message(raw)

    assert isinstance(ok, bool), f"ok deve ser bool, veio {type(ok)!r}"
    if not ok:
        assert parsed is None
        return

    # Caso ``ok is True``: o parsed deve satisfazer todas as faixas.
    assert isinstance(parsed, dict)
    assert set(parsed.keys()) == set(CANONICAL_KEYS)

    ts = parsed["timestamp"]
    assert isinstance(ts, int) and not isinstance(ts, bool) and ts >= 0

    temp = parsed["temperatura"]
    if temp is not None:
        assert isinstance(temp, (int, float)) and not isinstance(temp, bool)
        assert not math.isnan(float(temp)) and not math.isinf(float(temp))
        assert TEMP_MIN <= float(temp) <= TEMP_MAX

    hum = parsed["umidade"]
    if hum is not None:
        assert isinstance(hum, int) and not isinstance(hum, bool)
        assert HUM_MIN <= hum <= HUM_MAX

    bpm = parsed["bpm"]
    if bpm is not None:
        assert isinstance(bpm, int) and not isinstance(bpm, bool)
        assert 0 <= bpm <= MAX_BPM

    pid = parsed["paciente_id"]
    assert isinstance(pid, str)
    assert 1 <= len(pid) <= MAX_PACIENTE_ID_LEN


@pytest.mark.parametrize(
    "raw, descricao",
    [
        # Chave extra — não pertence ao conjunto canônico.
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                    "extra": True,
                }
            ),
            "chave extra",
        ),
        # Chave faltando — ``bpm`` ausente.
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "paciente_id": "PAC-0001",
                }
            ),
            "chave faltando (bpm)",
        ),
        # Tipo top-level errado — array em vez de objeto.
        (json.dumps([1, 2, 3]), "array no lugar de objeto"),
        # JSON válido mas não é objeto — string.
        (json.dumps("PAC-0001"), "string no lugar de objeto"),
        # JSON válido mas null.
        (json.dumps(None), "null no lugar de objeto"),
        # String vazia.
        ("", "string vazia"),
        # Texto não-JSON.
        ("não é JSON", "texto livre"),
    ],
)
def test_validate_message_rejects_extra_or_missing_keys(
    raw: str, descricao: str
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 16: rejeição por chaves incorretas.

    Reforça casos example-based em que o payload não é um objeto JSON
    com exatamente as chaves canônicas — todos devem retornar
    ``(False, None)``.
    """
    ok, parsed = validate_message(raw)
    assert ok is False, f"esperava rejeição ({descricao}): raw={raw!r}"
    assert parsed is None


@pytest.mark.parametrize(
    "raw, descricao",
    [
        (None, "entrada None (não-string)"),
        (42, "entrada int (não-string)"),
        (3.14, "entrada float (não-string)"),
        (b"{}", "entrada bytes (não-string)"),
        ({"a": 1}, "entrada dict (não-string)"),
    ],
)
def test_validate_message_rejects_non_string_inputs(
    raw: Any, descricao: str
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 16: rejeição de tipos não-string.

    Entradas não-string não podem sequer ser parseadas como JSON;
    ``validate_message`` deve devolver ``(False, None)`` (R9.5).
    """
    ok, parsed = validate_message(raw)
    assert ok is False, f"esperava rejeição ({descricao}): raw={raw!r}"
    assert parsed is None


@pytest.mark.parametrize(
    "raw, descricao",
    [
        # bpm fora da faixa [0, 250].
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": 300,
                    "paciente_id": "PAC-0001",
                }
            ),
            "bpm=300 acima do limite",
        ),
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": -1,
                    "paciente_id": "PAC-0001",
                }
            ),
            "bpm=-1 negativo",
        ),
        # temperatura fora da faixa [-40.0, 80.0].
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 100.0,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                }
            ),
            "temperatura=100.0 acima do limite",
        ),
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": -50.0,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                }
            ),
            "temperatura=-50.0 abaixo do limite",
        ),
        # umidade fora da faixa [0, 100].
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": -1,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                }
            ),
            "umidade=-1 negativa",
        ),
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 101,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                }
            ),
            "umidade=101 acima do limite",
        ),
        # timestamp negativo.
        (
            json.dumps(
                {
                    "timestamp": -1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "PAC-0001",
                }
            ),
            "timestamp negativo",
        ),
        # paciente_id vazio.
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "",
                }
            ),
            "paciente_id vazio",
        ),
        # paciente_id excede 32 caracteres.
        (
            json.dumps(
                {
                    "timestamp": 1,
                    "temperatura": 36.7,
                    "umidade": 58,
                    "bpm": 74,
                    "paciente_id": "x" * (MAX_PACIENTE_ID_LEN + 1),
                }
            ),
            "paciente_id com 33 caracteres",
        ),
    ],
)
def test_validate_message_rejects_out_of_range(
    raw: str, descricao: str
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 16: rejeição por faixas inválidas.

    Casos example-based em que as chaves estão corretas mas os valores
    excedem as faixas de R9.5 (``bpm``, ``temperatura``, ``umidade``) ou
    R3.1 (``timestamp``, ``paciente_id``). Todos retornam
    ``(False, None)``.
    """
    ok, parsed = validate_message(raw)
    assert ok is False, f"esperava rejeição ({descricao}): raw={raw!r}"
    assert parsed is None


# =============================================================================
# Property 17 — classify_alert + alert_entry
# =============================================================================


@settings(settings.get_profile("cardioia-fast"))
@given(
    bpm=bpm_strategy,
    temperatura=temperatura_strategy,
    bpm_threshold=bpm_threshold_strategy,
    temp_threshold=temp_threshold_strategy,
)
@example(bpm=120, temperatura=38.0, bpm_threshold=120, temp_threshold=38.0)
@example(bpm=121, temperatura=38.0, bpm_threshold=120, temp_threshold=38.0)
@example(bpm=120, temperatura=38.1, bpm_threshold=120, temp_threshold=38.0)
@example(bpm=150, temperatura=40.0, bpm_threshold=120, temp_threshold=38.0)
@example(bpm=None, temperatura=None, bpm_threshold=120, temp_threshold=38.0)
def test_classify_alert_strict_greater_than(
    bpm: Optional[int],
    temperatura: Optional[float],
    bpm_threshold: int,
    temp_threshold: float,
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 17: classify_alert usa ``>`` estrito.

    Para todo ``(bpm, temperatura, bpm_threshold, temp_threshold)``, o
    conjunto retornado é exatamente
    ``{BPM_ALTO: bpm > bpm_threshold, TEMP_ALTA: temp > temp_threshold}``.

    Valores ``None`` nunca disparam alerta (R11.6 — o dashboard preserva
    o último estado válido). A comparação é estritamente ``>``: valores
    exatamente iguais ao limite NÃO disparam (R11.1, R11.2).
    """
    # Oráculo: reproduz a regra canônica em forma mínima.
    esperado: set[str] = set()
    if bpm is not None and bpm > bpm_threshold:
        esperado.add("BPM_ALTO")
    if temperatura is not None and temperatura > temp_threshold:
        esperado.add("TEMP_ALTA")

    obtido = classify_alert(bpm, temperatura, bpm_threshold, temp_threshold)

    assert obtido == esperado, (
        "classify_alert divergiu do oráculo: "
        f"bpm={bpm}, temp={temperatura}, bpm_thr={bpm_threshold}, "
        f"temp_thr={temp_threshold}, esperado={esperado}, obtido={obtido}"
    )
    # O resultado é sempre subconjunto do universo canônico.
    assert obtido <= {"BPM_ALTO", "TEMP_ALTA"}


@pytest.mark.parametrize(
    "bpm, temperatura, esperado, descricao",
    [
        # R11.1 — fronteira BPM: 120 não alerta, 121 alerta.
        (120, 38.0, set(), "bpm=120 exatamente no limite (sem alerta)"),
        (121, 38.0, {"BPM_ALTO"}, "bpm=121 acima do limite"),
        # R11.2 — fronteira temperatura: 38.0 não alerta, 38.1 alerta.
        (120, 38.1, {"TEMP_ALTA"}, "temp=38.1 acima do limite"),
        (100, 38.0, set(), "temp=38.0 exatamente no limite (sem alerta)"),
        # R11.3 — ambos simultâneos.
        (150, 40.0, {"BPM_ALTO", "TEMP_ALTA"}, "ambos os alertas"),
        # R11.6 — None não dispara.
        (None, None, set(), "sem métricas (ambas None)"),
        (None, 38.1, {"TEMP_ALTA"}, "bpm None, temp acima"),
        (121, None, {"BPM_ALTO"}, "temp None, bpm acima"),
    ],
)
def test_classify_alert_boundary_examples(
    bpm: Optional[int],
    temperatura: Optional[float],
    esperado: set[str],
    descricao: str,
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 17: fronteiras clínicas.

    Exemplos example-based nas fronteiras exatas dos limites clínicos
    padrão (``BPM_THRESHOLD=120``, ``TEMP_THRESHOLD=38.0``) — incluindo o
    caso de None (métrica ausente na amostra).
    """
    obtido = classify_alert(
        bpm,
        temperatura,
        BPM_THRESHOLD_DEFAULT,
        TEMP_THRESHOLD_DEFAULT,
    )
    assert obtido == esperado, (
        f"caso '{descricao}' falhou: esperado={esperado}, obtido={obtido}"
    )


# Subconjuntos possíveis do universo de alertas — gerados como ``set``.
_ALERTS_UNIVERSE: tuple[set[str], ...] = (
    set(),
    {"BPM_ALTO"},
    {"TEMP_ALTA"},
    {"BPM_ALTO", "TEMP_ALTA"},
)

alerts_strategy = st.sampled_from(_ALERTS_UNIVERSE)


@settings(settings.get_profile("cardioia-fast"))
@given(
    timestamp=timestamp_strategy,
    paciente_id=paciente_id_strategy,
    bpm=bpm_strategy,
    temperatura=temperatura_strategy,
    alerts=alerts_strategy,
)
@example(
    timestamp=0,
    paciente_id="PAC-0001",
    bpm=None,
    temperatura=None,
    alerts=set(),
)
@example(
    timestamp=1,
    paciente_id="PAC-0001",
    bpm=121,
    temperatura=38.1,
    alerts={"BPM_ALTO", "TEMP_ALTA"},
)
def test_alert_entry_schema(
    timestamp: int,
    paciente_id: str,
    bpm: Optional[int],
    temperatura: Optional[float],
    alerts: set[str],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 17: schema e cor/texto de alert_entry.

    Para toda entrada ``alert_entry(ts, pid, bpm, temp, alerts)``:

    * As chaves obrigatórias ``{timestamp, paciente_id, alerts, bpm,
      temperatura, color, text}`` estão presentes (R11.5);
    * ``color == "red"`` se e somente se ``alerts`` é não vazio, caso
      contrário ``"green"`` (R11.4);
    * ``text == "Normal"`` se e somente se ``alerts`` é vazio;
    * se ``alerts`` é não vazio, o texto contém ``"ALERTA:"`` e cada
      alerta contribui com sua frase clínica canônica.
    """
    entry = alert_entry(timestamp, paciente_id, bpm, temperatura, alerts)

    # (1) Conjunto de chaves obrigatórias (R11.5).
    chaves_obrigatorias = {
        "timestamp",
        "paciente_id",
        "alerts",
        "bpm",
        "temperatura",
        "color",
        "text",
    }
    assert chaves_obrigatorias <= set(entry.keys()), (
        f"alert_entry não expôs todas as chaves obrigatórias: "
        f"faltando {chaves_obrigatorias - set(entry.keys())}"
    )

    # (2) Valores espelham o input — auditabilidade (R11.5).
    assert entry["timestamp"] == int(timestamp)
    assert entry["paciente_id"] == paciente_id
    assert entry["bpm"] == bpm
    assert entry["temperatura"] == temperatura
    # A lista de alertas é ordenada para determinismo dos logs.
    assert set(entry["alerts"]) == alerts
    assert entry["alerts"] == sorted(entry["alerts"])

    # (3) Cor — bicondicional com a presença de alertas (R11.4).
    if alerts:
        assert entry["color"] == "red", (
            f"esperava cor vermelha para alerts={alerts}, obtido={entry['color']}"
        )
    else:
        assert entry["color"] == "green", (
            f"esperava cor verde para alerts vazios, obtido={entry['color']}"
        )

    # (4) Texto — "Normal" iff sem alertas, senão contém "ALERTA:".
    if not alerts:
        assert entry["text"] == "Normal"
    else:
        assert entry["text"] != "Normal"
        assert "ALERTA:" in entry["text"], (
            f"esperava substring 'ALERTA:' em text={entry['text']!r}"
        )
        # R11.3 — se ambos os alertas disparam, os dois textos clínicos
        # aparecem simultaneamente (sem sobrescrever um ao outro).
        if "BPM_ALTO" in alerts:
            assert "ALERTA: BPM elevado" in entry["text"]
        if "TEMP_ALTA" in alerts:
            assert "ALERTA: Temperatura elevada" in entry["text"]
