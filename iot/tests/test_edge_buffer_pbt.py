"""
Testes de propriedade para o ``EdgeBuffer`` e adjacentes.

Feature: ``cardioia-monitoramento-iot``.

Este módulo valida as seguintes propriedades do design (``design.md``
seção *Correctness Properties*) contra o ``reference_model.py`` — que por
contrato espelha a lógica do firmware C++ e serve como oráculo:

* **Property 2**  — FIFO limitado do :class:`EdgeBuffer` coincide com
  ``collections.deque(maxlen=BUFFER_LIMIT)`` em qualquer sequência de
  ``push``/``pop_front`` de até 1000 operações (R4.3, R5.1–R5.5).
  Executado sob o perfil ``cardioia-thorough`` (500 exemplos).

* **Property 8**  — :func:`route_sample` roteia corretamente por
  ``Connectivity_Flag`` e ocupação do ``EdgeBuffer`` (R4.1, R7.3):
    - ``flag=False`` → amostra é enfileirada;
    - ``flag=True`` + buffer vazio → amostra vai direto ao ``publish_fn``;
    - ``flag=True`` + buffer não vazio → amostra é enfileirada para
      preservar a ordem cronológica.

* **Property 9**  — :class:`DualStorageEdgeBuffer` preserva o multiconjunto
  completo de registros sob falhas transitórias do :class:`SpiffsMockStorage`
  (redirecionamento ao :class:`RamFallbackStorage` ─ R4.4), desde que a
  ocupação total não exceda a capacidade.

* **Property 10** — :class:`RamFallbackStorage` emite o evento
  ``"aviso_80_pct"`` **exatamente uma vez** por cruzamento ascendente da
  ocupação através de ``ceil(0.8 * capacity)`` (R4.5). O aviso é
  "rearmado" apenas após a ocupação cair abaixo do limiar.

Convenção de tag: todo docstring de propriedade inicia com
``"Feature: cardioia-monitoramento-iot, Property N: …"``.
"""

from __future__ import annotations

from collections import deque
from typing import List, Tuple, Union

from hypothesis import HealthCheck, example, given, settings
from hypothesis import strategies as st

from reference_model import (
    BUFFER_LIMIT_DEFAULT,
    DualStorageEdgeBuffer,
    EdgeBuffer,
    RamFallbackStorage,
    SampleRecord,
    SpiffsMockStorage,
    route_sample,
)


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def _sample(i: int) -> SampleRecord:
    """Constrói um :class:`SampleRecord` mínimo e determinístico.

    Usa valores clinicamente plausíveis para evitar surpresas caso algum
    caminho futuro do reference model passe a validar faixas no ``push``.
    O ``timestamp`` (``i``) é também usado como identidade lógica nos
    testes — permite comparar ordens multiconjunto sem depender de
    ``id()`` / referência.
    """
    return SampleRecord(
        timestamp=int(i),
        temperatura=36.5,
        umidade=50,
        bpm=70,
        paciente_id="PAC-0001",
    )


# Operação sintética usada por P2 e P10: ``"pop"`` ou ``("push", timestamp)``.
_Op = Union[str, Tuple[str, int]]


def _op_strategy(
    min_size: int = 0,
    max_size: int = 1000,
) -> st.SearchStrategy[List[_Op]]:
    """Gera sequências de operações ``push``/``pop_front`` (mistas)."""
    push = st.builds(lambda v: ("push", v), st.integers(min_value=0, max_value=10_000))
    pop = st.just("pop")
    return st.lists(st.one_of(push, pop), min_size=min_size, max_size=max_size)


# =============================================================================
# Property 2 — FIFO limitado idêntico a ``collections.deque(maxlen=BUFFER_LIMIT)``
# =============================================================================

@settings(
    parent=settings.get_profile("cardioia-thorough"),
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.data_too_large],
)
@given(ops=_op_strategy(min_size=0, max_size=1000))
def test_edge_buffer_matches_deque_oracle(ops: List[_Op]) -> None:
    """Feature: cardioia-monitoramento-iot, Property 2: EdgeBuffer se comporta como FIFO limitado.

    Compara estado a estado o :class:`EdgeBuffer` (capacidade 50) contra o
    oráculo ``collections.deque(maxlen=50)`` para sequências de até 1000
    operações. Em cada passo verifica:

    * ``buffer.size() == len(oracle)`` (R5.5);
    * ``buffer.snapshot() == list(oracle)`` (ordem FIFO ─ R5.1, R5.2);
    * Ao final, ``buffer.evictions`` coincide com o contador de descartes
      induzidos pela capacidade no oráculo (R5.3, R5.4).
    """
    capacity = BUFFER_LIMIT_DEFAULT  # 50
    buffer = EdgeBuffer(capacity=capacity)
    oracle: deque[SampleRecord] = deque(maxlen=capacity)
    oracle_evictions = 0

    for op in ops:
        if op == "pop":
            popped_buf = buffer.pop_front()
            popped_oracle = oracle.popleft() if oracle else None
            assert popped_buf == popped_oracle, (
                "pop_front deve retornar o mesmo registro do oráculo, "
                f"buffer={popped_buf!r}, oracle={popped_oracle!r}"
            )
        else:
            # ``op`` é tupla ``("push", value)``.
            _, value = op
            record = _sample(value)
            # ``deque(maxlen)`` descarta silenciosamente ao atingir o
            # limite — detectamos o descarte comparando o tamanho antes.
            was_full = len(oracle) == oracle.maxlen
            oracle.append(record)
            if was_full:
                oracle_evictions += 1
            buffer.push(record)

        # Invariantes após cada operação (R5.5, FIFO):
        assert buffer.size() == len(oracle), (
            f"size divergiu: buffer={buffer.size()}, oracle={len(oracle)}"
        )
        assert buffer.snapshot() == list(oracle), (
            "ordem FIFO divergiu entre EdgeBuffer e deque(maxlen=BUFFER_LIMIT)"
        )
        assert 0 <= buffer.size() <= capacity

    assert buffer.evictions == oracle_evictions, (
        f"contador de descartes por limite divergiu: "
        f"EdgeBuffer={buffer.evictions}, oracle={oracle_evictions}"
    )
    # ``discard_events`` deve ter exatamente um registro por descarte.
    assert len(buffer.discard_events) == oracle_evictions


# =============================================================================
# Property 8 — Roteamento por Connectivity_Flag e ocupação do buffer
# =============================================================================

@given(
    steps=st.lists(
        st.tuples(
            st.booleans(),                                   # flag
            st.booleans(),                                   # prefill buffer?
            st.integers(min_value=0, max_value=10_000),      # record id
        ),
        min_size=0,
        max_size=50,
    )
)
def test_route_sample_honors_connectivity_and_buffer_state(
    steps: List[Tuple[bool, bool, int]],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 8: roteamento por Connectivity_Flag.

    Para cada passo ``(flag, prefill, record_id)``:

    * Se ``flag is False`` → ``route_sample`` retorna ``"buffered"``,
      o buffer cresce em 1 e ``publish_fn`` **não** é chamada.
    * Se ``flag is True`` e o buffer está vazio → retorna ``"published"``,
      o buffer permanece em tamanho 0 e ``publish_fn`` é chamada com o
      próprio ``record``.
    * Se ``flag is True`` e o buffer tem pendentes → retorna ``"buffered"``
      (preservação de ordem, R7.3), o buffer cresce em 1 e ``publish_fn``
      **não** é chamada.

    Cada passo usa um ``EdgeBuffer`` independente para isolar o efeito de
    uma decisão de roteamento — essa é justamente a unidade de
    comportamento descrita pela Property 8 no ``design.md``.
    """
    for flag, prefill, record_id in steps:
        buffer = EdgeBuffer(capacity=BUFFER_LIMIT_DEFAULT)
        if prefill:
            # Pré-enchemos com um registro qualquer (timestamp = sentinela)
            # para simular um buffer não vazio.
            buffer.push(_sample(999_999))

        publish_calls: List[SampleRecord] = []

        def publish_fn(rec: SampleRecord) -> bool:
            publish_calls.append(rec)
            return True

        record = _sample(record_id)
        size_before = buffer.size()

        result = route_sample(record, flag, buffer, publish_fn)

        if not flag:
            # Caminho offline: sempre enfileira (R4.1).
            assert result == "buffered"
            assert buffer.size() == size_before + 1
            assert publish_calls == [], (
                "publish_fn nao deve ser chamada quando flag=False"
            )
            # O registro recém-empurrado deve ser o último no snapshot.
            assert buffer.snapshot()[-1] == record
        elif size_before == 0:
            # Online + buffer vazio → envio direto (R7.3).
            assert result == "published"
            assert buffer.size() == 0
            assert publish_calls == [record], (
                "publish_fn deve ser chamada exatamente uma vez com o record"
            )
        else:
            # Online + buffer não vazio → preserva ordem cronológica.
            assert result == "buffered"
            assert buffer.size() == size_before + 1
            assert publish_calls == [], (
                "publish_fn nao deve ser chamada quando ha pendentes"
            )
            assert buffer.snapshot()[-1] == record


# =============================================================================
# Property 9 — DualStorageEdgeBuffer resistente a falhas transitórias do SPIFFS
# =============================================================================

@given(
    records=st.lists(
        st.integers(min_value=0, max_value=10_000),
        min_size=0,
        max_size=30,
    ),
    failures=st.lists(
        st.booleans(),
        min_size=0,
        max_size=60,
    ),
)
def test_dual_storage_preserves_multiset_under_transient_io_errors(
    records: List[int],
    failures: List[bool],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 9: resiliência a falhas transitórias do SPIFFS.

    Empurra ``records`` para o :class:`DualStorageEdgeBuffer` enquanto o
    :class:`SpiffsMockStorage` pode disparar :class:`IOError` em
    operações cujos índices estão sinalizados em ``failures``. Enquanto a
    ocupação total ≤ capacidade, o ``snapshot()`` final deve preservar
    exatamente o mesmo multiconjunto ─ e, como executamos apenas
    ``push`` (sem ``pop_front``), também a mesma ordem cronológica ─ que
    se teria no cenário "SPIFFS sempre OK".

    Capacidade escolhida (100) é estritamente maior que o limite superior
    de ``max_size`` das listas (30), portanto nenhum descarte por FIFO
    ocorre em qualquer storage — isolamos o efeito das falhas de I/O.
    """
    capacity = 100  # > max_size dos records → sem FIFO global.

    def io_error_on_index(i: int) -> bool:
        # Índices além de ``len(failures)`` não falham ─ comportamento
        # conservador, compatível com o contrato de ``SpiffsMockStorage``.
        return failures[i] if 0 <= i < len(failures) else False

    primary = SpiffsMockStorage(
        capacity=capacity, io_error_on_index=io_error_on_index
    )
    fallback = RamFallbackStorage(capacity=capacity)
    dual = DualStorageEdgeBuffer(
        primary=primary, fallback=fallback, capacity=capacity
    )

    expected: List[SampleRecord] = []
    for value in records:
        rec = _sample(value)
        pushed = dual.push(rec)
        expected.append(rec)
        # Com capacidade folgada nenhuma inserção deve acionar descarte.
        assert pushed is True

    # Cenário "tudo ok" (referência): mesma sequência sem falhas.
    ref_primary = SpiffsMockStorage(capacity=capacity)
    ref_fallback = RamFallbackStorage(capacity=capacity)
    ref_dual = DualStorageEdgeBuffer(
        primary=ref_primary, fallback=ref_fallback, capacity=capacity
    )
    for value in records:
        ref_dual.push(_sample(value))

    snap = dual.snapshot()
    ref_snap = ref_dual.snapshot()

    # Multiconjunto idêntico ─ núcleo da Property 9.
    assert sorted(r.timestamp for r in snap) == sorted(
        r.timestamp for r in ref_snap
    ), "multiconjunto de registros divergiu entre cenarios com/sem falha"
    # Como só fizemos push, a ordem cronológica também coincide.
    assert snap == expected
    assert snap == ref_snap
    # Contagem de I/O errors deve refletir a soma das falhas efetivamente
    # acionadas dentro da faixa de operações executadas.
    effective_failures = sum(
        1 for i in range(len(records)) if io_error_on_index(i)
    )
    assert dual.primary_io_errors == effective_failures
    # Registros roteados ao fallback == número de IOErrors efetivos.
    assert fallback.size() == effective_failures
    assert primary.size() == len(records) - effective_failures


# =============================================================================
# Property 10 — aviso_80_pct emitido em cada cruzamento ascendente do threshold
# =============================================================================

@example(
    ops=[
        ("push", 0),
        ("push", 1),
        ("push", 2),
        ("push", 3),
        ("push", 4),
        ("push", 5),
        ("push", 6),
        ("push", 7),   # size = 8 → primeiro aviso (cruza ceil(0.8*10)=8)
        "pop",         # size = 7 → desarma
        ("push", 100), # size = 8 → segundo aviso (re-arm)
    ]
)
@given(ops=_op_strategy(min_size=0, max_size=100))
def test_ram_fallback_emits_warning_on_upward_threshold_crossings(
    ops: List[_Op],
) -> None:
    """Feature: cardioia-monitoramento-iot, Property 10: aviso unico ao cruzar 80% da capacidade.

    Para ``RamFallbackStorage(capacity=10)`` (threshold = ⌈0.8·10⌉ = 8),
    o número de eventos ``"aviso_80_pct"`` emitidos deve ser **exatamente**
    igual ao número de transições ascendentes ``size < 8 → size ≥ 8``
    observadas ao longo da sequência de operações ─ ou seja, o aviso é
    rearmado somente após a ocupação voltar a ``< 8``.

    A simulação manual abaixo é o oráculo: mantém o mesmo contrato de
    FIFO (``append`` quando cheio descarta o mais antigo) e o mesmo
    limiar que o :class:`RamFallbackStorage`.
    """
    capacity = 10
    storage = RamFallbackStorage(capacity=capacity)
    threshold = storage.warning_threshold  # 8
    assert threshold == 8  # sanidade: ceil(0.8 * 10) == 8

    # Oráculo manual.
    oracle_size = 0
    oracle_above = False
    expected_events = 0

    for op in ops:
        if op == "pop":
            # Só reduz se houver algo para remover ─ idêntico ao reference.
            if oracle_size > 0:
                oracle_size -= 1
            storage.pop_front()
        else:
            _, value = op
            # ``append`` aplica FIFO em cheio: ocupação satura em ``capacity``.
            if oracle_size < capacity:
                oracle_size += 1
            storage.append(_sample(value))

        is_above = oracle_size >= threshold
        # Cruzamento ascendente = transição False → True.
        if is_above and not oracle_above:
            expected_events += 1
        oracle_above = is_above

    # Sanidade de invariantes do storage.
    assert storage.size() == oracle_size
    # Núcleo da Property 10: exatamente um evento por cruzamento ascendente.
    assert storage.events.count("aviso_80_pct") == expected_events, (
        f"numero de avisos divergiu: esperado={expected_events}, "
        f"recebido={storage.events.count('aviso_80_pct')}, ops={ops!r}"
    )
    # O único tipo de evento emitido atualmente é ``"aviso_80_pct"``.
    assert all(ev == "aviso_80_pct" for ev in storage.events)
