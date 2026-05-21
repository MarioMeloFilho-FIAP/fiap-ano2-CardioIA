"""
Testes de propriedade (PBT) da **Property 1** — round-trip do ``Sample_Record``.

Feature: cardioia-monitoramento-iot, Property 1.

Cobertura:

* R3.1 — ``timestamp`` inteiro não-negativo (em ms), ``temperatura`` float com uma
  casa decimal em ``[-40,0; 80,0]``, ``umidade`` inteiro em ``[0; 100]``, ``bpm``
  inteiro em ``[0; 250]``, ``paciente_id`` string não vazia com no máximo 32
  caracteres.
* R3.2 — JSON com **exatamente** o conjunto canônico de chaves
  ``{timestamp, temperatura, umidade, bpm, paciente_id}``, sem campos a mais ou
  a menos, na ordem canônica.
* R3.3 — Tamanho total do JSON serializado ≤ 256 caracteres.
* R3.5 — Campos indisponíveis codificados como JSON ``null``.

Oráculo: reference_model.py (espelho puro da lógica do firmware embarcado).

Perfil hypothesis: ``cardioia-thorough`` (``max_examples=500``), obrigatório para
P1 conforme a seção "Testing Strategy" do ``design.md``.
"""

from __future__ import annotations

import json

import pytest
from hypothesis import example, given, settings
from hypothesis import strategies as st

from reference_model import (
    CANONICAL_KEYS,
    HUM_MAX,
    HUM_MIN,
    MAX_BPM,
    MAX_JSON_LEN,
    MAX_PACIENTE_ID_LEN,
    SampleRecord,
    TEMP_MAX,
    TEMP_MIN,
    deserialize_sample,
    serialize_sample,
)

# =============================================================================
# Estratégias hypothesis (campos do SampleRecord)
# =============================================================================
# Cada estratégia abaixo espelha diretamente a faixa declarada no design.md /
# requirements.md (R3.1, R3.5). A intenção é gerar somente SampleRecord
# *semanticamente válidos* — exemplos fora da faixa são exercitados por
# outras propriedades (P5, P16, P19).

#: ``timestamp`` ∈ [0, 2**31 - 1]. O teto 2**31-1 espelha o overflow natural
#: de ``millis()`` em ESP32 (unsigned 32-bit), suficiente para ~49 dias.
timestamps = st.integers(min_value=0, max_value=2**31 - 1)

#: ``temperatura`` ∈ [-40,0; 80,0] ∪ {None}, com exatamente uma casa decimal
#: (R1.2, R3.1). O ``.map(lambda x: round(x, 1))`` espelha a normalização
#: aplicada por :func:`serialize_sample`; assim o round-trip fica válido sem
#: precisar de tolerância float no asserte.
temperaturas = st.one_of(
    st.none(),
    st.floats(
        min_value=TEMP_MIN,
        max_value=TEMP_MAX,
        allow_nan=False,
        allow_infinity=False,
    ).map(lambda x: round(x, 1)),
)

#: ``umidade`` ∈ [0, 100] ∪ {None} (R3.1, R3.5).
umidades = st.one_of(
    st.none(),
    st.integers(min_value=HUM_MIN, max_value=HUM_MAX),
)

#: ``bpm`` ∈ [0, 250] ∪ {None}, já saturado em MAX_BPM conforme R2.5.
bpms = st.one_of(
    st.none(),
    st.integers(min_value=0, max_value=MAX_BPM),
)

#: ``paciente_id`` com 1..32 caracteres em ``[A-Za-z0-9-]`` (R3.1). O alfabeto
#: é propositalmente mais largo do que o padrão P18 (``^PAC-\d{1,27}$``) —
#: P1 testa apenas as regras estruturais (comprimento e round-trip); a
#: anonimização (P18) é validada em ``test_lgpd_pbt.py``.
paciente_ids = st.text(
    alphabet=(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789-"
    ),
    min_size=1,
    max_size=MAX_PACIENTE_ID_LEN,
)

#: Estratégia composta: SampleRecord válido.
sample_records = st.builds(
    SampleRecord,
    timestamp=timestamps,
    temperatura=temperaturas,
    umidade=umidades,
    bpm=bpms,
    paciente_id=paciente_ids,
)


# =============================================================================
# Helpers
# =============================================================================

def _normalize_for_roundtrip(record: SampleRecord) -> SampleRecord:
    """Normaliza ``record`` para comparação pós round-trip.

    :func:`serialize_sample` arredonda ``temperatura`` para uma casa decimal
    (R1.2, R3.1). Para que a igualdade estrutural ``deserialize(serialize(r))
    == r`` valha também para os ``@example`` que não passaram pelo ``.map``
    da estratégia, aplicamos aqui a mesma normalização.
    """
    temp = (
        None
        if record.temperatura is None
        else round(float(record.temperatura), 1)
    )
    return SampleRecord(
        timestamp=record.timestamp,
        temperatura=temp,
        umidade=record.umidade,
        bpm=record.bpm,
        paciente_id=record.paciente_id,
    )


# =============================================================================
# Property 1 — Round-trip estrutural
# =============================================================================
# Estes quatro ``@example`` cobrem as fronteiras exigidas pela Tarefa 6:
#
#   * ``paciente_id`` com 1 e 32 caracteres (fronteiras do comprimento R3.1).
#   * ``SampleRecord`` com todos os campos opcionais em ``null`` (R3.5).
#   * ``SampleRecord`` com valores máximos dentro das faixas canônicas — este
#     é o JSON mais longo construível dentro da Property 1 (≈116 chars),
#     garantindo que permanece dentro do limite de 256 chars (R3.3). O
#     design.md limita os campos numéricos (timestamp ≤ 2^31-1, bpm ≤ 250,
#     temperatura com uma casa decimal), portanto o caso "exatamente 256
#     chars" não é atingível com entradas válidas: a verificação R3.3 é
#     exercitada como *cota superior* neste exemplo de máximo.

@settings(settings.get_profile("cardioia-thorough"))
@given(sample_records)
@example(SampleRecord(0, None, None, None, "P"))
@example(SampleRecord(0, None, None, None, "P" * MAX_PACIENTE_ID_LEN))
@example(SampleRecord(0, None, None, None, "PAC-0001"))
@example(
    SampleRecord(
        timestamp=2**31 - 1,
        temperatura=TEMP_MAX,
        umidade=HUM_MAX,
        bpm=MAX_BPM,
        paciente_id="P" * MAX_PACIENTE_ID_LEN,
    )
)
def test_serialize_then_deserialize_is_identity(record: SampleRecord) -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: round-trip do Sample_Record.

    Para todo ``SampleRecord`` válido, ``serialize_sample`` seguido de
    ``deserialize_sample`` produz um registro estruturalmente igual ao
    original (R3.1, R3.2, R3.3, R3.5).
    """
    payload = serialize_sample(record)
    # Dentro das faixas canônicas da Property 1, o JSON sempre cabe em 256
    # chars; um ``None`` aqui indicaria violação de R3.3 dentro do domínio
    # válido — portanto asserção forte.
    assert payload is not None, (
        f"serialize_sample retornou None para registro válido: {record}"
    )
    assert len(payload) <= MAX_JSON_LEN
    reconstructed = deserialize_sample(payload)
    assert reconstructed == _normalize_for_roundtrip(record)


# =============================================================================
# Property 1 — Chaves canônicas exatas
# =============================================================================

@settings(settings.get_profile("cardioia-thorough"))
@given(sample_records)
@example(SampleRecord(0, None, None, None, "P"))
@example(SampleRecord(0, None, None, None, "P" * MAX_PACIENTE_ID_LEN))
@example(SampleRecord(0, None, None, None, "PAC-0001"))
def test_serialized_has_canonical_keys_only(record: SampleRecord) -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: chaves canônicas exatas.

    Para todo ``SampleRecord`` válido, o JSON serializado contém
    **exatamente** o conjunto canônico de chaves (R3.2) e na ordem
    canônica definida em ``CANONICAL_KEYS``.
    """
    payload = serialize_sample(record)
    assert payload is not None
    parsed = json.loads(payload)
    # (a) conjunto exato — nem a mais, nem a menos.
    assert set(parsed.keys()) == set(CANONICAL_KEYS)
    # (b) ordem canônica (estabiliza o tamanho e facilita inspeção visual).
    assert tuple(parsed.keys()) == CANONICAL_KEYS


# =============================================================================
# Property 1 — Tamanho dentro do limite R3.3
# =============================================================================

@settings(settings.get_profile("cardioia-thorough"))
@given(sample_records)
@example(
    SampleRecord(
        timestamp=2**31 - 1,
        temperatura=TEMP_MAX,
        umidade=HUM_MAX,
        bpm=MAX_BPM,
        paciente_id="P" * MAX_PACIENTE_ID_LEN,
    )
)
def test_serialized_size_within_limit(record: SampleRecord) -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: JSON ≤ 256 chars.

    Para todo ``SampleRecord`` dentro das faixas canônicas, o JSON
    produzido respeita o limite absoluto de 256 caracteres (R3.3).
    """
    payload = serialize_sample(record)
    assert payload is not None
    assert len(payload) <= MAX_JSON_LEN


# =============================================================================
# Property 1 — Campos indisponíveis → JSON null (R3.5)
# =============================================================================

@given(timestamps, paciente_ids)
def test_none_fields_become_json_null(ts: int, pid: str) -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: None → null no JSON.

    Quando ``temperatura``, ``umidade`` ou ``bpm`` são ``None``, o JSON
    correspondente carrega o literal ``null`` (R3.5). A chave não é
    omitida — o conjunto canônico é preservado mesmo em amostras
    parcialmente indisponíveis.
    """
    record = SampleRecord(ts, None, None, None, pid)
    payload = serialize_sample(record)
    assert payload is not None
    parsed = json.loads(payload)
    # Checagem semântica (pós-parse).
    assert parsed["temperatura"] is None
    assert parsed["umidade"] is None
    assert parsed["bpm"] is None
    # Checagem textual (pré-parse) — garante que a palavra ``null`` aparece
    # literalmente no payload, descartando a hipótese de chave ausente.
    assert '"temperatura":null' in payload
    assert '"umidade":null' in payload
    assert '"bpm":null' in payload


# =============================================================================
# Property 1 — Fronteiras do paciente_id (example-based)
# =============================================================================

def test_paciente_id_com_33_caracteres_eh_rejeitado() -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: limite superior de paciente_id.

    ``paciente_id`` com mais de 32 caracteres viola R3.1 e deve ser
    rejeitado pela serialização com ``ValueError`` (falha explícita, e
    não ``None`` silencioso — ``None`` é reservado para R3.6, payload
    maior que 256 chars).
    """
    record = SampleRecord(
        timestamp=0,
        temperatura=None,
        umidade=None,
        bpm=None,
        paciente_id="P" * (MAX_PACIENTE_ID_LEN + 1),
    )
    with pytest.raises(ValueError):
        serialize_sample(record)


def test_paciente_id_vazio_eh_rejeitado() -> None:
    """Feature: cardioia-monitoramento-iot, Property 1: paciente_id não vazio.

    ``paciente_id`` vazio viola R3.1 e deve ser rejeitado pela
    serialização com ``ValueError``.
    """
    record = SampleRecord(
        timestamp=0,
        temperatura=None,
        umidade=None,
        bpm=None,
        paciente_id="",
    )
    with pytest.raises(ValueError):
        serialize_sample(record)
