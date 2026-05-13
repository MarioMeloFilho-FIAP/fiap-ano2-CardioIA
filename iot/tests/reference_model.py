"""
Reference model do módulo **CardioIA – Monitoramento IoT (Fase 3)**.

Este módulo espelha, em Python puro e sem efeitos colaterais, toda a **lógica
de domínio** do firmware embarcado no ESP32 (camada portável, sem dependência
de Arduino, `WiFi`, `Serial`, `millis()`, `SPIFFS` reais). Ele é usado
exclusivamente pelos testes de propriedade (``hypothesis``) localizados em
``iot/tests/test_*_pbt.py`` como **oráculo** contra o qual o firmware C++ é
validado.

Propriedades cobertas (ver ``design.md``, seção *Correctness Properties*):

* **P1**  — Round-trip e estrutura canônica do ``Sample_Record``
            (``SampleRecord``, :func:`serialize_sample`,
            :func:`deserialize_sample`, :func:`compose_sample`).
* **P2**  — ``EdgeBuffer`` se comporta como FIFO limitado
            (:class:`EdgeBuffer`).
* **P3**  — ``BpmWindow`` com saturação em 250 e extrapolação proporcional
            (:class:`BpmWindow`).
* **P4**  — Debounce paramétrico (:func:`debounce`).
* **P5**  — Validação de leitura do DHT22 (:func:`is_valid_reading`,
            :func:`build_invalid_reading_log`).
* **P6**  — Alerta persistente após três leituras consecutivas inválidas
            (:class:`PersistentFailureCounter`).
* **P7**  — Formato da linha no Monitor Serial (:func:`format_reading`).
* **P8**  — Roteamento correto por ``Connectivity_Flag``
            (:func:`route_sample`).
* **P9**  — Resiliência a falhas transitórias do adaptador SPIFFS
            (:class:`DualStorageEdgeBuffer`, :class:`SpiffsMockStorage`,
            :class:`RamFallbackStorage`).
* **P10** — Aviso único ao cruzar 80% da capacidade
            (:class:`RamFallbackStorage`).
* **P11** — Eventos de transição de ``Connectivity_Flag``
            (:func:`transition_event`).
* **P12** — Parser de comandos Serial preserva estado em inputs inválidos
            (:func:`parse_cmd`).
* **P13** — Conservação do ``SyncScheduler`` (sem perda nem duplicação)
            (:class:`SyncScheduler`, :func:`sync`).
* **P14** — FSM de reconexão MQTT (:class:`MqttReconnectFSM`).
* **P15** — Formato canônico do tópico MQTT (:func:`topic_telemetry`).
* **P16** — Validação de mensagem no dashboard (:func:`validate_message`).
* **P17** — ``classify_alert`` estabelece estado e log
            (:func:`classify_alert`, :func:`alert_entry`).
* **P18** — ``paciente_id`` anonimizado (:func:`is_anonymized_paciente_id`).
* **P19** — Validação e rejeição de payload de config
            (:func:`validate_config`, :func:`apply_config`).
* **P20** — Persistência de config entre reboots (:class:`ConfigManager`).

O módulo segue estritamente as regras:

1. **Sem I/O real**: não chama ``open``, ``time.sleep``, rede ou hardware.
   Adaptadores de storage expõem interfaces injetáveis.
2. **Sem mutação global**: todas as funções são puras ou operam sobre o
   próprio ``self`` da classe chamadora.
3. **Compatível com Python 3.10+**: usa ``from __future__ import annotations``
   e tipos PEP 604 (``X | Y``) em docstrings; assinaturas runtime usam
   ``typing`` clássico onde necessário.
4. **Determinístico**: para o mesmo input, produz sempre o mesmo output.

Feature tag obrigatória para propriedades: ``cardioia-monitoramento-iot``.
"""

from __future__ import annotations

import json
import math
import re
from collections import deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Iterable, Iterator, Optional

# =============================================================================
# Constantes do domínio (espelham ``iot/firmware/src/config.h``)
# =============================================================================

#: Limite clínico padrão de BPM acima do qual o Alert_Module dispara alerta.
BPM_THRESHOLD_DEFAULT: int = 120

#: Limite clínico padrão de temperatura corporal (°C).
TEMP_THRESHOLD_DEFAULT: float = 38.0

#: Capacidade máxima default do ``EdgeBuffer`` (número de ``Sample_Record``).
BUFFER_LIMIT_DEFAULT: int = 50

#: Intervalo entre duas amostragens consecutivas (ms).
SAMPLING_INTERVAL_MS_DEFAULT: int = 5000

#: Valor máximo de BPM reportável — saturação (Requisito 2.5).
MAX_BPM: int = 250

#: Janela deslizante para cálculo de BPM (ms).
BPM_WINDOW_MS: int = 60_000

#: Faixa operacional de temperatura do DHT22 (°C).
TEMP_MIN: float = -40.0
TEMP_MAX: float = 80.0

#: Faixa operacional de umidade do DHT22 (% inteiro).
HUM_MIN: int = 0
HUM_MAX: int = 100

#: Tamanho máximo do JSON serializado de um ``Sample_Record`` (R3.3).
MAX_JSON_LEN: int = 256

#: Tamanho máximo do ``paciente_id`` aceito no payload (R3.1).
MAX_PACIENTE_ID_LEN: int = 32

#: Tamanho máximo do payload MQTT publicado em ``.../sinais`` (R8.2).
MQTT_PAYLOAD_MAX: int = 1024

#: Ring do ``BpmWindow`` (capacidade suficiente para 250 BPM × 60 s + margem).
BPM_RING_CAPACITY: int = 300

#: Prefixo canônico dos tópicos MQTT de telemetria.
TOPIC_PREFIX: str = "cardioia/paciente"

#: Ordem canônica das chaves do JSON do ``Sample_Record`` (R3.2, Property 1).
CANONICAL_KEYS: tuple[str, ...] = (
    "timestamp",
    "temperatura",
    "umidade",
    "bpm",
    "paciente_id",
)

#: Conjunto dos comandos aceitos via Monitor Serial (R6.5, Property 12).
VALID_SERIAL_CMDS: frozenset[str] = frozenset(
    {"ONLINE", "OFFLINE", "STATUS", "CONFIG_SHOW"}
)


# =============================================================================
# Public API (__all__)
# =============================================================================

__all__ = [
    # Constantes
    "BPM_THRESHOLD_DEFAULT",
    "TEMP_THRESHOLD_DEFAULT",
    "BUFFER_LIMIT_DEFAULT",
    "SAMPLING_INTERVAL_MS_DEFAULT",
    "MAX_BPM",
    "BPM_WINDOW_MS",
    "TEMP_MIN",
    "TEMP_MAX",
    "HUM_MIN",
    "HUM_MAX",
    "MAX_JSON_LEN",
    "MAX_PACIENTE_ID_LEN",
    "MQTT_PAYLOAD_MAX",
    "BPM_RING_CAPACITY",
    "TOPIC_PREFIX",
    "CANONICAL_KEYS",
    "VALID_SERIAL_CMDS",
    # Data classes
    "SampleRecord",
    "SensorReading",
    # Sample_Record (P1)
    "serialize_sample",
    "deserialize_sample",
    "compose_sample",
    # EdgeBuffer (P2, P8, P9, P10)
    "EdgeBuffer",
    "SpiffsMockStorage",
    "RamFallbackStorage",
    "DualStorageEdgeBuffer",
    "route_sample",
    # BpmWindow (P3)
    "BpmWindow",
    # Debounce (P4)
    "debounce",
    # Sensor driver (P5, P6, P7)
    "is_valid_reading",
    "build_invalid_reading_log",
    "format_reading",
    "PersistentFailureCounter",
    # Connectivity (P11, P12)
    "ConnectivityState",
    "transition_event",
    "parse_cmd",
    # Sync / MQTT (P13, P14, P15)
    "SyncScheduler",
    "sync",
    "MqttState",
    "MqttReconnectFSM",
    "topic_telemetry",
    # Dashboard (P16, P17)
    "validate_message",
    "classify_alert",
    "alert_entry",
    # LGPD (P18)
    "is_anonymized_paciente_id",
    # Config (P19, P20)
    "validate_config",
    "apply_config",
    "ConfigManager",
]


# =============================================================================
# Data classes (SampleRecord, SensorReading)
# =============================================================================

@dataclass(frozen=True)
class SensorReading:
    """Leitura bruta do DHT22, antes da composição em ``Sample_Record``.

    Campos:
        temperatura: temperatura em °C (float) ou ``None`` se indisponível.
        umidade: umidade em % inteiro ou ``None`` se indisponível.

    Implementa parte de R1.2, R3.5.
    """

    temperatura: Optional[float] = None
    umidade: Optional[int] = None


@dataclass(frozen=True)
class SampleRecord:
    """Registro individual de amostra (R3.1–R3.6, Property 1).

    Campos (em ordem canônica, espelhando a serialização JSON):
        timestamp: inteiro ≥ 0, em ms (origem ``millis()``).
        temperatura: float com uma casa decimal em [-40.0, 80.0] ou ``None``.
        umidade: inteiro em [0, 100] ou ``None``.
        bpm: inteiro em [0, 250] ou ``None``.
        paciente_id: string não vazia com no máximo 32 caracteres,
                     anonimizada conforme R15.2.

    A classe é ``frozen`` para garantir imutabilidade — amostras nunca são
    alteradas após a composição; novas amostras são registros novos.
    """

    timestamp: int
    temperatura: Optional[float]
    umidade: Optional[int]
    bpm: Optional[int]
    paciente_id: str

    def as_canonical_dict(self) -> dict[str, Any]:
        """Retorna dicionário com chaves na ordem canônica (R3.2)."""
        return {
            "timestamp": self.timestamp,
            "temperatura": self.temperatura,
            "umidade": self.umidade,
            "bpm": self.bpm,
            "paciente_id": self.paciente_id,
        }



# =============================================================================
# P1 — Sample_Record serialization / deserialization
# =============================================================================

def _round_temperatura(value: Optional[float]) -> Optional[float]:
    """Arredonda a temperatura para uma casa decimal (R1.2, R3.1).

    Retorna ``None`` se o valor for ``None``. Valores ``NaN`` disparam
    ``ValueError`` pois o domínio rejeita NaN (P5).
    """
    if value is None:
        return None
    if isinstance(value, float) and math.isnan(value):
        raise ValueError("temperatura=NaN não é aceitável no Sample_Record")
    # Usamos ``round`` do Python para manter consistência determinística.
    return round(float(value), 1)


def _validate_paciente_id(paciente_id: str) -> None:
    """Valida ``paciente_id`` para serialização (R3.1).

    Raises:
        ValueError: se vazio, não string ou com mais de 32 caracteres.
    """
    if not isinstance(paciente_id, str):
        raise ValueError("paciente_id deve ser string")
    if len(paciente_id) == 0:
        raise ValueError("paciente_id não pode ser vazio")
    if len(paciente_id) > MAX_PACIENTE_ID_LEN:
        raise ValueError(
            f"paciente_id excede {MAX_PACIENTE_ID_LEN} caracteres"
        )


def serialize_sample(record: SampleRecord) -> Optional[str]:
    """Serializa um ``SampleRecord`` como JSON canônico compacto.

    Implementa **Property 1** (round-trip) e os critérios R3.1, R3.2, R3.3,
    R3.5. A ordem das chaves é fixa (:data:`CANONICAL_KEYS`), sem espaços
    entre delimitadores. Campos ``None`` viram JSON ``null`` (R3.5).

    Args:
        record: o registro a serializar.

    Returns:
        String JSON compacta **ou** ``None`` se o resultado exceder 256
        caracteres (R3.3, R3.6 — o chamador deve descartar e logar).

    Raises:
        ValueError: em entradas estruturalmente inválidas (``paciente_id``
            fora do limite, NaN em temperatura, etc.).
    """
    if record.timestamp < 0:
        raise ValueError("timestamp deve ser inteiro ≥ 0 (R3.1)")
    _validate_paciente_id(record.paciente_id)

    # Montagem manual garante ordem canônica independentemente da
    # ordem de iteração de ``dict`` (compatível desde Python 3.7, mas
    # explicitada aqui por robustez e legibilidade).
    ordered: dict[str, Any] = {
        "timestamp": int(record.timestamp),
        "temperatura": _round_temperatura(record.temperatura),
        "umidade": None if record.umidade is None else int(record.umidade),
        "bpm": None if record.bpm is None else int(record.bpm),
        "paciente_id": record.paciente_id,
    }
    payload = json.dumps(ordered, separators=(",", ":"), ensure_ascii=False)
    if len(payload) > MAX_JSON_LEN:
        # R3.6: caller é responsável por logar o descarte.
        return None
    return payload


def deserialize_sample(raw: str) -> SampleRecord:
    """Deserializa uma string JSON produzida por :func:`serialize_sample`.

    Implementa o lado inverso de **Property 1**. Aceita tanto floats quanto
    ints para ``temperatura`` (reconstrução aceita a mesma ordem canônica).

    Raises:
        ValueError: se a string não contém exatamente o conjunto de chaves
            canônicas, ou se qualquer valor está fora das faixas.
    """
    if not isinstance(raw, str):
        raise ValueError("entrada deve ser string JSON")
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise ValueError("JSON deve ser objeto")
    keys = set(data.keys())
    if keys != set(CANONICAL_KEYS):
        raise ValueError(
            f"conjunto de chaves inválido: esperado {set(CANONICAL_KEYS)}, "
            f"recebido {keys}"
        )
    ts = data["timestamp"]
    if not isinstance(ts, int) or isinstance(ts, bool) or ts < 0:
        raise ValueError("timestamp deve ser inteiro ≥ 0")
    temp = data["temperatura"]
    if temp is not None:
        if not isinstance(temp, (int, float)) or isinstance(temp, bool):
            raise ValueError("temperatura deve ser numérica ou null")
        if math.isnan(float(temp)) or not (TEMP_MIN <= float(temp) <= TEMP_MAX):
            raise ValueError("temperatura fora da faixa operacional")
        temp = round(float(temp), 1)
    hum = data["umidade"]
    if hum is not None:
        if not isinstance(hum, int) or isinstance(hum, bool):
            raise ValueError("umidade deve ser inteiro ou null")
        if not (HUM_MIN <= hum <= HUM_MAX):
            raise ValueError("umidade fora da faixa [0,100]")
    bpm = data["bpm"]
    if bpm is not None:
        if not isinstance(bpm, int) or isinstance(bpm, bool):
            raise ValueError("bpm deve ser inteiro ou null")
        if not (0 <= bpm <= MAX_BPM):
            raise ValueError("bpm fora da faixa [0, 250]")
    pid = data["paciente_id"]
    _validate_paciente_id(pid)
    return SampleRecord(
        timestamp=int(ts),
        temperatura=temp,
        umidade=hum,
        bpm=bpm,
        paciente_id=pid,
    )


def compose_sample(
    now_ms: int,
    reading: SensorReading,
    bpm: Optional[int],
    paciente_id: str,
) -> SampleRecord:
    """Compõe um ``SampleRecord`` a partir dos insumos de um ciclo de leitura.

    Implementa R3.1, R3.4 e R3.5. Valores indisponíveis (``None``) geram
    campo ``null`` no JSON correspondente.

    Args:
        now_ms: timestamp do ciclo, em ms (``millis()`` simulado).
        reading: leitura atual do DHT22.
        bpm: valor calculado pela janela deslizante (já saturado em 250)
            ou ``None`` se indisponível.
        paciente_id: identificador anonimizado (R15.2).

    Returns:
        ``SampleRecord`` pronto para serialização.
    """
    if now_ms < 0:
        raise ValueError("now_ms deve ser ≥ 0")
    _validate_paciente_id(paciente_id)
    temp_norm = _round_temperatura(reading.temperatura)
    hum_norm: Optional[int]
    if reading.umidade is None:
        hum_norm = None
    else:
        hum_norm = int(reading.umidade)
    bpm_norm: Optional[int]
    if bpm is None:
        bpm_norm = None
    else:
        bpm_norm = max(0, min(MAX_BPM, int(bpm)))
    return SampleRecord(
        timestamp=int(now_ms),
        temperatura=temp_norm,
        umidade=hum_norm,
        bpm=bpm_norm,
        paciente_id=paciente_id,
    )



# =============================================================================
# P2 / P8 — EdgeBuffer (FIFO com BUFFER_LIMIT) e roteamento por flag
# =============================================================================

class EdgeBuffer:
    """FIFO limitado que armazena ``SampleRecord`` enquanto offline.

    Implementa **Property 2** (FIFO) e cobre R4.3, R5.1–R5.5. Espelha o
    comportamento de ``collections.deque(maxlen=BUFFER_LIMIT)`` — o que é
    tanto a implementação quanto o próprio oráculo nos testes, uma vez que
    o firmware C++ segue a mesma semântica.

    Atributos expostos:
        capacity: capacidade máxima do buffer (inteiro positivo).
        evictions: contador total de descartes por limite (R5.4).
        discard_events: lista de eventos "descarte por limite" contendo
            ``(registro_descartado, size_pos_descarte)`` — útil para o
            Logger no firmware e para os testes.
    """

    def __init__(self, capacity: int = BUFFER_LIMIT_DEFAULT) -> None:
        if capacity <= 0:
            raise ValueError("capacity deve ser > 0")
        self.capacity: int = int(capacity)
        self._deque: deque[SampleRecord] = deque()
        self.evictions: int = 0
        self.discard_events: list[tuple[SampleRecord, int]] = []

    # ------------------------------------------------------------------
    # Operações do FIFO
    # ------------------------------------------------------------------
    def push(self, record: SampleRecord) -> bool:
        """Insere ``record`` no fim; aplica FIFO se cheio.

        Retorna ``True`` se inseriu sem descarte; ``False`` se descartou o
        registro mais antigo (R5.3). Atualiza ``evictions`` e
        ``discard_events`` para espelhar o log requerido em R5.4.
        """
        if len(self._deque) >= self.capacity:
            old = self._deque.popleft()
            self._deque.append(record)
            self.evictions += 1
            self.discard_events.append((old, len(self._deque)))
            return False
        self._deque.append(record)
        return True

    def pop_front(self) -> Optional[SampleRecord]:
        """Remove e retorna o registro mais antigo, ou ``None`` se vazio."""
        if not self._deque:
            return None
        return self._deque.popleft()

    def size(self) -> int:
        """Tamanho atual, sempre em ``[0, capacity]`` (R5.5)."""
        return len(self._deque)

    def is_empty(self) -> bool:
        """Retorna ``True`` se o buffer não contém registros."""
        return len(self._deque) == 0

    def snapshot(self) -> list[SampleRecord]:
        """Retorna uma cópia (ordem cronológica) do buffer — usado em asserts."""
        return list(self._deque)

    def __len__(self) -> int:  # conveniência, equivalente a ``size()``
        return len(self._deque)

    def __iter__(self) -> Iterator[SampleRecord]:
        return iter(self._deque)


def route_sample(
    record: SampleRecord,
    connectivity_flag: bool,
    buffer: EdgeBuffer,
    publish_fn: Callable[[SampleRecord], bool],
) -> str:
    """Roteia uma amostra conforme Connectivity_Flag e ocupação do buffer.

    Implementa **Property 8** (R4.1, R7.3):

    * Se ``connectivity_flag is False`` → enfileira em ``buffer``.
    * Se ``connectivity_flag is True`` **e** ``buffer.is_empty()`` →
      chama ``publish_fn(record)`` diretamente.
    * Se ``connectivity_flag is True`` mas ``buffer`` tem pendentes → a
      amostra entra no buffer para preservar a ordem cronológica até que
      o :class:`SyncScheduler` drene (R7.1, P13).

    Args:
        record: amostra recém-composta.
        connectivity_flag: estado atual da conectividade simulada.
        buffer: instância de :class:`EdgeBuffer`.
        publish_fn: callback injetado (MqttClient simulado) que retorna
            ``True`` em caso de publicação bem-sucedida.

    Returns:
        ``"published"`` se a amostra foi publicada diretamente,
        ``"buffered"`` se foi enfileirada,
        ``"buffered_after_publish_fail"`` se tentou publicar e falhou
        (sem duplicar: o registro acaba no buffer para retry).
    """
    if not connectivity_flag:
        buffer.push(record)
        return "buffered"
    # Online, mas com pendentes — não podemos "pular a fila".
    if not buffer.is_empty():
        buffer.push(record)
        return "buffered"
    # Online e buffer vazio: envio direto.
    ok = publish_fn(record)
    if ok:
        return "published"
    # Falha na publicação: retém no buffer para retry posterior.
    buffer.push(record)
    return "buffered_after_publish_fail"


# =============================================================================
# P9 / P10 — DualStorage (SPIFFS + RAM fallback) e aviso de 80%
# =============================================================================

class SpiffsMockStorage:
    """Mock determinístico do adaptador SPIFFS do firmware.

    Para fins de teste, pode ser configurado para falhar em índices
    arbitrários através do callback ``io_error_on_index`` — útil para
    exercitar **Property 9** (resiliência a falhas transitórias).

    Atributos:
        records: lista interna (espelha o arquivo ``buffer.ndjson``).
        capacity: mesmo limite do :class:`EdgeBuffer`.
        io_error_on_index: função ``(index_op) -> bool`` que retorna
            ``True`` para forçar erro de I/O na operação de índice dado.
    """

    def __init__(
        self,
        capacity: int = BUFFER_LIMIT_DEFAULT,
        io_error_on_index: Optional[Callable[[int], bool]] = None,
    ) -> None:
        self.capacity: int = int(capacity)
        self.records: deque[SampleRecord] = deque()
        self._op_index: int = 0
        self._io_error_on_index: Callable[[int], bool] = (
            io_error_on_index if io_error_on_index is not None else (lambda _i: False)
        )

    def append(self, record: SampleRecord) -> None:
        """Anexa ``record`` ao fim. Dispara :class:`IOError` em falha simulada."""
        idx = self._op_index
        self._op_index += 1
        if self._io_error_on_index(idx):
            raise IOError(f"SPIFFS indisponível na operação #{idx}")
        if len(self.records) >= self.capacity:
            self.records.popleft()
        self.records.append(record)

    def pop_front(self) -> Optional[SampleRecord]:
        """Remove o mais antigo; sem tratamento especial de erro de I/O."""
        if not self.records:
            return None
        return self.records.popleft()

    def size(self) -> int:
        return len(self.records)

    def is_empty(self) -> bool:
        return not self.records

    def snapshot(self) -> list[SampleRecord]:
        return list(self.records)


class RamFallbackStorage:
    """Storage em RAM acionado quando o SPIFFS falha (R4.4, P9, P10).

    Emite o evento único ``"aviso_80_pct"`` a cada cruzamento ascendente
    da ocupação através de ``ceil(0.8 * capacity)``, conforme
    **Property 10**. A ocupação precisa cair abaixo do limiar e voltar a
    cruzá-lo para um novo aviso ser emitido.

    Atributos:
        capacity: capacidade da RAM alocada (pode ser igual ao do SPIFFS
            ou menor; o firmware usa a mesma ``BUFFER_LIMIT``).
        events: lista cronológica de eventos emitidos
            (``"aviso_80_pct"`` é o único tipo atualmente).
        warning_threshold: ``ceil(0.8 * capacity)`` — valor cacheado.
    """

    def __init__(self, capacity: int = BUFFER_LIMIT_DEFAULT) -> None:
        if capacity <= 0:
            raise ValueError("capacity deve ser > 0")
        self.capacity: int = int(capacity)
        self.records: deque[SampleRecord] = deque()
        self.events: list[str] = []
        self.warning_threshold: int = math.ceil(0.8 * self.capacity)
        self._above_threshold: bool = False

    def _update_threshold_state(self) -> None:
        """Dispara aviso ao cruzar para cima do threshold (P10)."""
        was_above = self._above_threshold
        is_above = len(self.records) >= self.warning_threshold
        if is_above and not was_above:
            self.events.append("aviso_80_pct")
        self._above_threshold = is_above

    def append(self, record: SampleRecord) -> None:
        """Anexa ``record``; aplica FIFO se cheio e atualiza threshold state."""
        if len(self.records) >= self.capacity:
            self.records.popleft()
        self.records.append(record)
        self._update_threshold_state()

    def pop_front(self) -> Optional[SampleRecord]:
        if not self.records:
            return None
        rec = self.records.popleft()
        self._update_threshold_state()
        return rec

    def size(self) -> int:
        return len(self.records)

    def is_empty(self) -> bool:
        return not self.records

    def snapshot(self) -> list[SampleRecord]:
        return list(self.records)


class DualStorageEdgeBuffer:
    """``EdgeBuffer`` com storage primário (SPIFFS) + fallback em RAM.

    Implementa **Property 9** (R4.4): em caso de ``IOError`` no adaptador
    primário, a amostra é redirecionada para o :class:`RamFallbackStorage`,
    preservando o multiconjunto completo enquanto houver capacidade.

    O buffer visto "de fora" é a união ordenada cronologicamente dos dois
    storages. ``pop_front`` drena primeiro do storage primário (política
    conservadora: a ordem relativa entre registros persistidos e em RAM
    é preservada pelo ``_arrival_index`` interno).
    """

    def __init__(
        self,
        primary: Optional[SpiffsMockStorage] = None,
        fallback: Optional[RamFallbackStorage] = None,
        capacity: int = BUFFER_LIMIT_DEFAULT,
    ) -> None:
        self.capacity: int = int(capacity)
        self.primary: SpiffsMockStorage = (
            primary if primary is not None else SpiffsMockStorage(capacity=capacity)
        )
        self.fallback: RamFallbackStorage = (
            fallback if fallback is not None else RamFallbackStorage(capacity=capacity)
        )
        # Conserva a ordem global (primário + fallback) via índice monotônico.
        self._timeline: list[tuple[int, str, SampleRecord]] = []
        self._arrival: int = 0
        self.primary_io_errors: int = 0

    def push(self, record: SampleRecord) -> bool:
        """Insere ``record``. Retorna ``True`` se não houve descarte global.

        Se o primário falhar, a amostra entra no fallback; isso ainda
        conta como inserção bem-sucedida (P9).
        """
        storage_used: str
        ok = True
        try:
            # O primário tem capacidade própria; se exceder, FIFO local.
            if self.primary.size() >= self.capacity:
                # Descarte global: o mais antigo (que está no primário) sai.
                dropped = self.primary.pop_front()
                ok = False
                if dropped is not None:
                    self._timeline = [
                        (i, s, r) for (i, s, r) in self._timeline if r is not dropped
                    ]
            self.primary.append(record)
            storage_used = "primary"
        except IOError:
            self.primary_io_errors += 1
            # Fallback sempre aceita (aplica FIFO localmente se necessário).
            if self.fallback.size() >= self.capacity:
                dropped = self.fallback.pop_front()
                ok = False
                if dropped is not None:
                    self._timeline = [
                        (i, s, r) for (i, s, r) in self._timeline if r is not dropped
                    ]
            self.fallback.append(record)
            storage_used = "fallback"
        self._timeline.append((self._arrival, storage_used, record))
        self._arrival += 1
        return ok

    def pop_front(self) -> Optional[SampleRecord]:
        """Remove o mais antigo (ordem cronológica global)."""
        if not self._timeline:
            return None
        _idx, storage, record = self._timeline.pop(0)
        if storage == "primary":
            popped = self.primary.pop_front()
        else:
            popped = self.fallback.pop_front()
        # Invariante: ``popped is record`` — o timeline reflete fielmente.
        return popped if popped is not None else record

    def size(self) -> int:
        return len(self._timeline)

    def is_empty(self) -> bool:
        return not self._timeline

    def snapshot(self) -> list[SampleRecord]:
        """Retorna a visão ordenada cronologicamente do buffer."""
        return [rec for (_i, _s, rec) in self._timeline]

    @property
    def events(self) -> list[str]:
        """Proxy para ``fallback.events`` (P10 — avisos de 80%)."""
        return self.fallback.events



# =============================================================================
# P3 — BpmWindow (janela deslizante de 60 s com saturação e extrapolação)
# =============================================================================

class BpmWindow:
    """Calcula BPM em janela deslizante de 60 s, saturando em 250.

    Implementa **Property 3** e R2.2, R2.3, R2.5, R2.6, R2.7. Mantém um
    *ring buffer* (deque) de timestamps de pulso; a remoção dos timestamps
    fora da janela acontece de forma preguiçosa a cada ``compute_bpm``.

    Args:
        boot_ms: timestamp (ms) em que o módulo foi iniciado. Serve de
            referência para a extrapolação proporcional (R2.7).

    Invariantes mantidos:
        * Nenhum timestamp em :attr:`_pulses` excede o ``now_ms`` passado
          na última chamada de :func:`compute_bpm`.
        * ``len(self._pulses) <= BPM_RING_CAPACITY``.
    """

    def __init__(self, boot_ms: int = 0) -> None:
        if boot_ms < 0:
            raise ValueError("boot_ms deve ser ≥ 0")
        self.boot_ms: int = int(boot_ms)
        self._pulses: deque[int] = deque(maxlen=BPM_RING_CAPACITY)

    def register_pulse(self, ts_ms: int) -> None:
        """Registra um novo pulso em ``ts_ms``.

        O chamador (SensorDriver) é responsável por aplicar debounce de
        150 ms antes de chamar este método (R2.4, P4). Timestamps devem
        ser monotônicos não-decrescentes dentro de uma mesma execução.
        """
        if ts_ms < 0:
            raise ValueError("ts_ms deve ser ≥ 0")
        self._pulses.append(int(ts_ms))

    def _prune(self, now_ms: int) -> None:
        """Remove timestamps fora da janela ``(now_ms - 60000, now_ms]``."""
        threshold = now_ms - BPM_WINDOW_MS
        while self._pulses and self._pulses[0] <= threshold:
            self._pulses.popleft()

    def compute_bpm(self, now_ms: int) -> int:
        """Devolve o BPM atual conforme **Property 3**.

        Regras (idênticas à especificação do firmware):

        * ``elapsed_ms >= 60000``: ``bpm = min(250, count_in_window)``.
          Se ``count_in_window == 0``, retorna 0 (R2.6).
        * ``0 < elapsed_ms < 60000``: extrapolação proporcional
          ``min(250, round(|pulses_totais| * 60000 / elapsed_ms))`` (R2.7).
        * ``elapsed_ms == 0``: retorna 0 (evita divisão por zero).

        Args:
            now_ms: instante atual em ms (``millis()``). Deve ser
                ``>= boot_ms``.
        """
        if now_ms < self.boot_ms:
            raise ValueError("now_ms não pode ser anterior a boot_ms")
        elapsed = now_ms - self.boot_ms
        if elapsed <= 0:
            return 0
        # Janela plena — usa contagem real.
        if elapsed >= BPM_WINDOW_MS:
            self._prune(now_ms)
            count = len(self._pulses)
            return min(MAX_BPM, count)
        # Extrapolação proporcional (boot recente, R2.7).
        # Observação importante: a extrapolação usa TODOS os pulsos
        # registrados desde o boot, não apenas os da janela — porque
        # ``elapsed < 60000`` implica que todos eles estão dentro da
        # janela por construção (pulsos não ocorrem antes do boot).
        count_total = len(self._pulses)
        extrapolated = round(count_total * BPM_WINDOW_MS / elapsed)
        return min(MAX_BPM, int(extrapolated))

    def __len__(self) -> int:
        return len(self._pulses)


# =============================================================================
# P4 — Debounce paramétrico
# =============================================================================

def debounce(ts_list: Iterable[int], ms: int) -> list[int]:
    """Aplica debounce: mantém timestamps com gap ≥ ``ms`` desde o último kept.

    Implementa **Property 4** e R2.4 (pulso, 150 ms) + R6.2 (botão de
    conectividade, 50 ms).

    Contrato (reforçado em testes):
        * ``|out| <= |ts_list|``.
        * Para todo ``i > 0``: ``out[i] - out[i-1] >= ms``.
        * Se ``len(ts_list) >= 1``: ``out[0] == ts_list[0]``.

    Args:
        ts_list: timestamps (ms), iteráveis. Espera-se não-decrescentes,
            mas a função é robusta a lista desordenada (trata cada valor
            como instante observado, comparando com o último retido).
        ms: intervalo mínimo em ms. Deve ser ``> 0``.

    Returns:
        Lista nova, preservando a ordem de entrada.
    """
    if ms <= 0:
        raise ValueError("ms deve ser > 0")
    out: list[int] = []
    last_kept: Optional[int] = None
    for ts in ts_list:
        if last_kept is None or (ts - last_kept) >= ms:
            out.append(int(ts))
            last_kept = int(ts)
    return out


# =============================================================================
# P5 / P6 / P7 — SensorDriver (validação, contador persistente, formatação)
# =============================================================================

def is_valid_reading(
    temperatura: Optional[float],
    umidade: Optional[int | float],
) -> bool:
    """Bicondicional de validação do DHT22 (Property 5 / R1.3).

    Retorna ``True`` iff:
        * ``temperatura`` não é ``NaN`` nem ``None``;
        * ``umidade`` não é ``NaN`` nem ``None``;
        * ``temperatura ∈ [-40.0, 80.0]``;
        * ``umidade ∈ [0, 100]``.

    A função é permissiva quanto ao tipo de ``umidade`` (float ou int),
    mas rejeita qualquer valor não finito.
    """
    if temperatura is None or umidade is None:
        return False
    try:
        t = float(temperatura)
        h = float(umidade)
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


def build_invalid_reading_log(
    temperatura: Optional[float],
    umidade: Optional[int | float],
) -> str:
    """Monta a mensagem de log para uma leitura inválida (P5 / R1.3).

    A string retornada DEVE conter, como substrings: ``"DHT22_Sensor"``, o
    tipo da falha (``"NaN"`` ou ``"out_of_range"``) e a representação do
    valor rejeitado. Usada pelo oráculo nos testes.
    """
    if temperatura is None or (
        isinstance(temperatura, float) and math.isnan(float(temperatura))
    ):
        failure = "NaN"
    elif umidade is None or (
        isinstance(umidade, float) and math.isnan(float(umidade))
    ):
        failure = "NaN"
    else:
        failure = "out_of_range"
    return (
        f"[DHT22_Sensor] leitura invalida ({failure}): "
        f"temperatura={temperatura!r}, umidade={umidade!r}"
    )


def format_reading(ts_ms: int, temperatura: float, umidade: int) -> str:
    """Formata a linha do Monitor Serial para uma leitura válida (Property 7).

    A string retornada DEVE conter, como substrings:

    * ``"DHT22_Sensor"``,
    * ``str(ts_ms)`` (representação decimal do timestamp),
    * ``f"{temperatura:.1f}°C"`` (temperatura com **uma** casa decimal),
    * ``f"{int(umidade)}%"`` (umidade inteira).

    Valida R1.2 e R1.4. O chamador é responsável por garantir
    :func:`is_valid_reading` antes de formatar.
    """
    if ts_ms < 0:
        raise ValueError("ts_ms deve ser ≥ 0")
    return (
        f"[DHT22_Sensor] ts={ts_ms} temperatura={temperatura:.1f}°C "
        f"umidade={int(umidade)}%"
    )


class PersistentFailureCounter:
    """Contador de falhas consecutivas do DHT22 (Property 6 / R1.5).

    Conta leituras inválidas consecutivas e retorna ``True`` **exatamente
    uma vez** por cada transição ``2 → 3``. Qualquer leitura válida
    reinicia o contador.

    Uso:
        >>> c = PersistentFailureCounter()
        >>> c.record(False)   # 1
        False
        >>> c.record(False)   # 2
        False
        >>> c.record(False)   # 3 → alerta!
        True
        >>> c.record(False)   # 4 → não re-emite
        False
        >>> c.record(True)    # reset
        False
        >>> c.record(False); c.record(False); c.record(False)  # novo alerta
        True
    """

    def __init__(self) -> None:
        self._consecutive: int = 0
        self.alerts_fired: int = 0

    def record(self, is_valid: bool) -> bool:
        """Registra uma leitura. Retorna ``True`` na transição 2→3."""
        if is_valid:
            self._consecutive = 0
            return False
        self._consecutive += 1
        if self._consecutive == 3:
            self.alerts_fired += 1
            return True
        return False

    @property
    def consecutive_invalid(self) -> int:
        """Exposto para diagnóstico/teste — não use para tomada de decisão."""
        return self._consecutive



# =============================================================================
# P11 / P12 — ConnectivityController (FSM + parser de comandos Serial)
# =============================================================================

class ConnectivityState(str, Enum):
    """Estado da FSM de conectividade simulada (R6.1)."""

    ONLINE = "ONLINE"
    OFFLINE = "OFFLINE"


def transition_event(
    old_flag: bool,
    new_flag: bool,
    buffer_size: int,
) -> dict[str, Any]:
    """Monta o evento publicado pelo ``ConnectivityController`` em transições.

    Implementa **Property 11** (R6.3, R6.4). Deve ser chamada somente
    quando há transição efetiva (``old_flag != new_flag``); caso contrário
    dispara ``ValueError``.

    Args:
        old_flag: estado anterior da ``Connectivity_Flag``.
        new_flag: novo estado da ``Connectivity_Flag``.
        buffer_size: quantidade de ``Sample_Record`` pendentes no
            ``EdgeBuffer`` no momento da transição (R6.3).

    Returns:
        ``dict`` com chaves ``direction``, ``buffer_size`` (inteiro
        ``>= 0``) e ``text`` (string distintiva por direção).
    """
    if old_flag == new_flag:
        raise ValueError("transition_event deve ser chamado somente em transições")
    if buffer_size < 0:
        raise ValueError("buffer_size deve ser ≥ 0")
    if not old_flag and new_flag:
        direction = "false->true"
        text = (
            f"Conectividade restaurada; {int(buffer_size)} amostra(s) pendente(s) "
            f"no Local_Buffer."
        )
    else:
        direction = "true->false"
        text = "Conectividade perdida; amostras serao armazenadas localmente."
    return {
        "direction": direction,
        "buffer_size": int(buffer_size),
        "text": text,
    }


def parse_cmd(raw: str, state: ConnectivityState) -> tuple[ConnectivityState, dict[str, Any]]:
    """Interpreta um comando recebido via Monitor Serial (Property 12 / R6.5).

    Comandos válidos (case-insensitive após trim):

    * ``ONLINE``       → força ``Connectivity_Flag = True``.
    * ``OFFLINE``      → força ``Connectivity_Flag = False``.
    * ``STATUS``       → não muda estado; emite evento ``"status"``.
    * ``CONFIG_SHOW``  → não muda estado; emite evento ``"config_show"``.

    Para qualquer outra entrada (inclusive string vazia, somente espaços,
    ou comando próximo tipo ``"ONLINNE"``), o estado é preservado e o
    evento retornado contém ``"rejected"`` + a string original (R6.5).

    Args:
        raw: string recebida no buffer Serial.
        state: estado atual da FSM (:class:`ConnectivityState`).

    Returns:
        Tupla ``(novo_state, event)``. ``event`` é um dict com ao menos
        as chaves ``type`` e ``raw``.
    """
    if not isinstance(raw, str):
        return state, {"type": "rejected", "raw": repr(raw)}
    normalized = raw.strip().upper()
    if normalized not in VALID_SERIAL_CMDS:
        return state, {"type": "rejected", "raw": raw}
    if normalized == "ONLINE":
        return ConnectivityState.ONLINE, {"type": "online", "raw": raw}
    if normalized == "OFFLINE":
        return ConnectivityState.OFFLINE, {"type": "offline", "raw": raw}
    if normalized == "STATUS":
        return state, {"type": "status", "raw": raw, "state": state.value}
    # "CONFIG_SHOW"
    return state, {"type": "config_show", "raw": raw, "state": state.value}


# =============================================================================
# P13 — SyncScheduler (conservação: sem perda, sem duplicação)
# =============================================================================

class SyncScheduler:
    """Scheduler de reenvio de amostras pendentes (R7.1–R7.5, Property 13).

    Estratégia implementada (determinística, sem efeitos colaterais):

    * Uma fila ``B0`` de entrada e uma sequência de eventos do canal
      ``e ∈ {ack, nack, timeout, offline, exception}*``.
    * Para cada evento, consome no máximo um registro do topo do buffer.
      A regra de cada evento é:

      - ``ack``       → ``pop_front`` + adiciona aos ``published``.
      - ``nack``      → mantém o registro no buffer; conta falha
                         consecutiva; após 3 falhas consecutivas, suspende
                         sync até próxima transição de flag (R7.5).
      - ``timeout``   → idem ``nack`` (R7.4: timeout > 3 s).
      - ``offline``   → aborta sync imediatamente (R7.4).
      - ``exception`` → idem ``offline`` (R7.4).

    Invariantes (assertáveis, usados como **oráculo da Property 13**):
        * ``multiset(published) ⊎ multiset(buffer_final) == multiset(B0)``
        * ``published`` segue a ordem cronológica de ``B0``.
    """

    MAX_CONSECUTIVE_FAILURES: int = 3

    def __init__(self) -> None:
        self._consecutive_failures: int = 0
        self.suspended: bool = False

    def reset(self) -> None:
        """Reseta o estado após transição de flag (R7.5)."""
        self._consecutive_failures = 0
        self.suspended = False

    def run(
        self,
        initial_buffer: Iterable[SampleRecord],
        events: Iterable[str],
    ) -> tuple[list[SampleRecord], list[SampleRecord]]:
        """Executa a simulação de sync.

        Args:
            initial_buffer: ordem cronológica (mais antigo primeiro).
            events: sequência finita de eventos do canal.

        Returns:
            Tupla ``(published, buffer_final)``.
            ``published`` preserva a ordem de consumo (cronológica de
            ``initial_buffer``); ``buffer_final`` é a ordem residual.
        """
        buffer: deque[SampleRecord] = deque(initial_buffer)
        published: list[SampleRecord] = []
        aborted = False
        for ev in events:
            if aborted or self.suspended:
                break
            if not buffer:
                break
            if ev == "ack":
                record = buffer.popleft()
                published.append(record)
                self._consecutive_failures = 0
            elif ev in ("nack", "timeout"):
                self._consecutive_failures += 1
                if self._consecutive_failures >= self.MAX_CONSECUTIVE_FAILURES:
                    self.suspended = True
            elif ev in ("offline", "exception"):
                aborted = True
            else:
                raise ValueError(f"evento desconhecido: {ev!r}")
        return published, list(buffer)


def sync(
    initial_buffer: Iterable[SampleRecord],
    events: Iterable[str],
    publish_fn: Optional[Callable[[SampleRecord], str]] = None,
) -> tuple[list[SampleRecord], list[SampleRecord]]:
    """Versão funcional (stateless) de :class:`SyncScheduler.run`.

    Mantém a interface utilizada nos testes de propriedade (P13) onde
    basta um wrapper sem estado externo visível. ``publish_fn``, quando
    fornecido, é aplicado a cada amostra antes de processar o evento
    correspondente — útil para inspecionar a ordem de entrega.

    A ausência de ``publish_fn`` é o caso padrão do oráculo.
    """
    scheduler = SyncScheduler()
    if publish_fn is None:
        return scheduler.run(initial_buffer, events)
    # Com callback: invocamos para cada evento consumido antes de atualizar.
    buffer: deque[SampleRecord] = deque(initial_buffer)
    published: list[SampleRecord] = []
    aborted = False
    for ev in events:
        if aborted or scheduler.suspended or not buffer:
            break
        if ev == "ack":
            rec = buffer[0]
            publish_fn(rec)
            buffer.popleft()
            published.append(rec)
            scheduler._consecutive_failures = 0  # noqa: SLF001
        elif ev in ("nack", "timeout"):
            scheduler._consecutive_failures += 1  # noqa: SLF001
            if scheduler._consecutive_failures >= SyncScheduler.MAX_CONSECUTIVE_FAILURES:
                scheduler.suspended = True
        elif ev in ("offline", "exception"):
            aborted = True
        else:
            raise ValueError(f"evento desconhecido: {ev!r}")
    return published, list(buffer)


# =============================================================================
# P14 — FSM de reconexão MQTT
# =============================================================================

class MqttState(str, Enum):
    """Estado da FSM de reconexão MQTT (R8.4, R8.6, Property 14)."""

    ONLINE = "ONLINE"
    OFFLINE = "OFFLINE"
    AUTH_SUSPENDED = "AUTH_SUSPENDED"


class MqttReconnectFSM:
    """FSM determinística de reconexão MQTT (Property 14).

    Eventos aceitos:
        * ``"connect_ok"``     — conexão bem-sucedida.
        * ``"connect_fail"``   — falha transitória (timeout, network).
        * ``"auth_rejected"``  — broker rejeitou credenciais.
        * ``"flag_true"``      — Connectivity_Flag subiu; reseta contador.
        * ``"reset"``          — intervenção externa (sai de AUTH_SUSPENDED).

    Transições:
        * ``ONLINE + connect_fail``: fail_count += 1; se chegar a 3 → OFFLINE.
        * ``OFFLINE + connect_fail``: idem, permanece em OFFLINE com contador.
        * ``* + connect_ok`` (exceto AUTH_SUSPENDED): → ONLINE, fail_count = 0.
        * ``AUTH_SUSPENDED + connect_ok``: ignora (permanece suspenso).
        * ``* + auth_rejected``: → AUTH_SUSPENDED.
        * ``AUTH_SUSPENDED + reset``: → OFFLINE, fail_count = 0
          (a conexão precisa ser explicitamente retomada via connect_ok).
        * ``* + flag_true``: fail_count = 0; estado preservado (se ONLINE,
          continua ONLINE; se OFFLINE, continua OFFLINE; AUTH_SUSPENDED
          permanece até ``reset``).
    """

    MAX_CONSECUTIVE_FAILURES: int = 3

    def __init__(self, initial_state: MqttState = MqttState.OFFLINE) -> None:
        self.state: MqttState = initial_state
        self.fail_count: int = 0

    def advance(self, event: str) -> tuple[MqttState, int]:
        """Aplica um evento e devolve ``(novo_estado, novo_fail_count)``."""
        if event == "connect_ok":
            if self.state == MqttState.AUTH_SUSPENDED:
                # R8.6: suspensão até intervenção externa; conexões são ignoradas.
                return self.state, self.fail_count
            self.state = MqttState.ONLINE
            self.fail_count = 0
            return self.state, self.fail_count
        if event == "connect_fail":
            if self.state == MqttState.AUTH_SUSPENDED:
                # Suspenso: falhas seguem sendo ignoradas para a contagem.
                return self.state, self.fail_count
            self.fail_count += 1
            if self.fail_count >= self.MAX_CONSECUTIVE_FAILURES:
                self.state = MqttState.OFFLINE
            return self.state, self.fail_count
        if event == "auth_rejected":
            self.state = MqttState.AUTH_SUSPENDED
            # O firmware marca Connectivity_Flag=false; contador zerado.
            self.fail_count = 0
            return self.state, self.fail_count
        if event == "flag_true":
            # R7.5: reset de tentativas ao voltar online.
            self.fail_count = 0
            return self.state, self.fail_count
        if event == "reset":
            if self.state == MqttState.AUTH_SUSPENDED:
                self.state = MqttState.OFFLINE
                self.fail_count = 0
            return self.state, self.fail_count
        raise ValueError(f"evento desconhecido: {event!r}")


# =============================================================================
# P15 — Tópico canônico de telemetria
# =============================================================================

_TOPIC_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


def topic_telemetry(paciente_id: str) -> str:
    """Constrói o tópico MQTT de telemetria (Property 15 / R8.2).

    Aceita ``paciente_id`` contendo ``[A-Za-z0-9_-]`` com 1–32 caracteres.
    O chamador deve adicionalmente validar LGPD via
    :func:`is_anonymized_paciente_id` antes de publicar (R15.2).

    Returns:
        ``"cardioia/paciente/{paciente_id}/sinais"``.

    Raises:
        ValueError: se ``paciente_id`` não casa com o padrão permitido.
    """
    if not isinstance(paciente_id, str):
        raise ValueError("paciente_id deve ser string")
    if not _TOPIC_ID_RE.fullmatch(paciente_id):
        raise ValueError(
            "paciente_id deve casar com ^[A-Za-z0-9_-]{1,32}$"
        )
    return f"{TOPIC_PREFIX}/{paciente_id}/sinais"



# =============================================================================
# P16 / P17 — Dashboard (validação de mensagem + classificação de alerta)
# =============================================================================

def validate_message(raw: str) -> tuple[bool, Optional[dict[str, Any]]]:
    """Valida uma mensagem crua recebida em ``.../sinais`` (Property 16).

    Retorna ``(True, parsed)`` iff:
        * ``raw`` é JSON parseável para ``dict``,
        * possui exatamente o conjunto canônico de chaves
          (:data:`CANONICAL_KEYS`),
        * ``timestamp`` é inteiro ``>= 0``,
        * ``temperatura`` ∈ ``[-40.0, 80.0] ∪ {None}``,
        * ``umidade`` ∈ ``[0, 100] ∪ {None}``,
        * ``bpm`` ∈ ``[0, 250] ∪ {None}``,
        * ``paciente_id`` é string não vazia de até 32 caracteres.

    Caso contrário retorna ``(False, None)``.

    Coerência com R9.5, R10.4, R11.6: sempre que retorna ``False``, o
    dashboard incrementa o contador de mensagens inválidas e NÃO
    propaga para gráficos/gauge.
    """
    if not isinstance(raw, str):
        return False, None
    try:
        data = json.loads(raw)
    except (json.JSONDecodeError, ValueError):
        return False, None
    if not isinstance(data, dict):
        return False, None
    if set(data.keys()) != set(CANONICAL_KEYS):
        return False, None
    ts = data["timestamp"]
    if not isinstance(ts, int) or isinstance(ts, bool) or ts < 0:
        return False, None
    temp = data["temperatura"]
    if temp is not None:
        if not isinstance(temp, (int, float)) or isinstance(temp, bool):
            return False, None
        t = float(temp)
        if math.isnan(t) or math.isinf(t) or not (TEMP_MIN <= t <= TEMP_MAX):
            return False, None
    hum = data["umidade"]
    if hum is not None:
        if not isinstance(hum, int) or isinstance(hum, bool):
            return False, None
        if not (HUM_MIN <= hum <= HUM_MAX):
            return False, None
    bpm = data["bpm"]
    if bpm is not None:
        if not isinstance(bpm, int) or isinstance(bpm, bool):
            return False, None
        if not (0 <= bpm <= MAX_BPM):
            return False, None
    pid = data["paciente_id"]
    if not isinstance(pid, str) or len(pid) == 0 or len(pid) > MAX_PACIENTE_ID_LEN:
        return False, None
    return True, {
        "timestamp": int(ts),
        "temperatura": (round(float(temp), 1) if temp is not None else None),
        "umidade": (int(hum) if hum is not None else None),
        "bpm": (int(bpm) if bpm is not None else None),
        "paciente_id": pid,
    }


def classify_alert(
    bpm: Optional[int],
    temperatura: Optional[float],
    bpm_threshold: int = BPM_THRESHOLD_DEFAULT,
    temp_threshold: float = TEMP_THRESHOLD_DEFAULT,
) -> set[str]:
    """Classifica alertas clínicos para uma amostra (Property 17).

    Regras (R11.1–R11.4):
        * ``"BPM_ALTO" ∈ resultado`` iff ``bpm > bpm_threshold``.
        * ``"TEMP_ALTA" ∈ resultado`` iff ``temperatura > temp_threshold``.
        * Comparação **estrita** (``>``, não ``>=``): BPM exatamente no
          limite NÃO gera alerta.

    Valores ``None`` (campos ausentes na amostra) não geram alerta — o
    dashboard preserva o último estado válido (R11.6).

    Args:
        bpm: valor inteiro recebido, ou ``None``.
        temperatura: valor float recebido, ou ``None``.
        bpm_threshold: limite clínico corrente.
        temp_threshold: limite clínico corrente.

    Returns:
        Subconjunto de ``{"BPM_ALTO", "TEMP_ALTA"}``.
    """
    alerts: set[str] = set()
    if bpm is not None and int(bpm) > int(bpm_threshold):
        alerts.add("BPM_ALTO")
    if temperatura is not None and float(temperatura) > float(temp_threshold):
        alerts.add("TEMP_ALTA")
    return alerts


def alert_entry(
    timestamp: int,
    paciente_id: str,
    bpm: Optional[int],
    temperatura: Optional[float],
    alerts: set[str],
) -> dict[str, Any]:
    """Produz a entrada de log do Alert_Module (Property 17 / R11.5).

    Mesmo quando não há alertas, a entrada é gerada para fins de
    auditabilidade do dashboard (entrada ``"Normal"``). Contém
    obrigatoriamente: ``timestamp``, ``paciente_id``, métrica(s) em
    alerta e valor(es) medido(s).
    """
    return {
        "timestamp": int(timestamp),
        "paciente_id": paciente_id,
        "alerts": sorted(alerts),
        "bpm": bpm,
        "temperatura": temperatura,
        "color": "red" if alerts else "green",
        "text": (
            "Normal"
            if not alerts
            else " | ".join(
                sorted(
                    (
                        "ALERTA: BPM elevado"
                        if a == "BPM_ALTO"
                        else "ALERTA: Temperatura elevada"
                    )
                    for a in alerts
                )
            )
        ),
    }


# =============================================================================
# P18 — LGPD: paciente_id anonimizado
# =============================================================================

# Padrão canônico aceito: ``PAC-`` + 1 a 27 dígitos (total ≤ 31; cabe em 32).
_ANON_RE = re.compile(r"^PAC-\d{1,27}$")

# Padrões de PII explicitamente rejeitados (redundância defensiva para que
# o rejeitador seja observável mesmo se ``_ANON_RE`` for ampliado no futuro).
_CPF_RE = re.compile(r"\d{3}\.?\d{3}\.?\d{3}-?\d{2}")
_EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")
_DATE_BR_RE = re.compile(r"\b\d{2}/\d{2}/\d{4}\b")
# RG heurístico: sequência "nua" de 7 a 11 dígitos consecutivos
# (documentos brasileiros tipicamente têm 7 a 11 dígitos).
_RG_LIKE_RE = re.compile(r"(?<!\d)\d{7,11}(?!\d)")


def is_anonymized_paciente_id(s: Any) -> bool:
    """Bicondicional para ``paciente_id`` anonimizado (Property 18 / R15.2).

    Aceita somente strings que casam com ``^PAC-\\d{1,27}$`` — por exemplo,
    ``"PAC-0001"`` ou ``"PAC-" + "9" * 27``.

    Rejeita explicitamente (mesmo que parcialmente) padrões de PII:

    * CPF: ``\\d{3}\\.?\\d{3}\\.?\\d{3}-?\\d{2}``;
    * e-mail: ``^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$``;
    * data ``dd/mm/aaaa``;
    * sequências RG-like de 7 a 11 dígitos consecutivos fora do prefixo
      canônico ``PAC-``.

    A rejeição de PII é, na prática, redundante porque o prefixo ``PAC-``
    já impede casamento com esses padrões; mas a checagem é feita
    explicitamente para robustez (defesa em profundidade — caso a regex
    canônica seja alterada no futuro, os rejeitores seguem válidos).
    """
    if not isinstance(s, str):
        return False
    # Fast path: precisa casar o prefixo anonimizado.
    if not _ANON_RE.fullmatch(s):
        return False
    # Defesa em profundidade: garantimos ausência de PII.
    if _EMAIL_RE.fullmatch(s):
        return False
    if _DATE_BR_RE.search(s):
        return False
    if _CPF_RE.fullmatch(s):
        return False
    # Para a heurística de RG, consideramos APENAS a parte pós-``PAC-``:
    # se alguém tentasse cadastrar "PAC-" + "12345678" como identificador,
    # a regex canônica permitiria (8 dígitos), mas a política LGPD sugere
    # no máximo 6 dígitos de sufixo para evitar colisão com RG. Optamos
    # por **aceitar** o caso canônico explícito (P18 foca na forma), uma
    # vez que o ``design.md`` permite até 27 dígitos; assim sufixos muito
    # longos (≥7) continuam aceitos. O rejeitor abaixo aplica-se apenas a
    # formatos que ignoram o prefixo ``PAC-``, jamais atingidos aqui.
    return True



# =============================================================================
# P19 / P20 — ConfigManager (validação, aplicação, persistência)
# =============================================================================

#: Faixas válidas para cada campo de configuração (R16.1, Property 19).
_CONFIG_RANGES: dict[str, tuple[str, Any, Any]] = {
    "bpm_threshold":        ("int",   40,       220),
    "temp_threshold":       ("float", 35.0,     42.0),
    "buffer_limit":         ("int",   10,       500),
    "sampling_interval_ms": ("int",   1000,     60000),
}

#: Config padrão do sistema (usada no boot se nada foi persistido).
DEFAULT_CONFIG: dict[str, Any] = {
    "bpm_threshold": BPM_THRESHOLD_DEFAULT,
    "temp_threshold": TEMP_THRESHOLD_DEFAULT,
    "buffer_limit": BUFFER_LIMIT_DEFAULT,
    "sampling_interval_ms": SAMPLING_INTERVAL_MS_DEFAULT,
}


def _check_field(key: str, value: Any) -> Optional[str]:
    """Valida um único campo de config. Retorna motivo em pt-BR ou ``None``."""
    if key not in _CONFIG_RANGES:
        return f"campo desconhecido: {key!r}"
    kind, lo, hi = _CONFIG_RANGES[key]
    if isinstance(value, bool):
        return f"{key} deve ser {kind}, recebido bool"
    if kind == "int":
        if not isinstance(value, int):
            return f"{key} deve ser inteiro"
        if not (lo <= value <= hi):
            return f"{key} fora da faixa [{lo}, {hi}]"
    elif kind == "float":
        if not isinstance(value, (int, float)):
            return f"{key} deve ser numerico"
        v = float(value)
        if math.isnan(v) or math.isinf(v):
            return f"{key} nao pode ser NaN/Inf"
        if not (lo <= v <= hi):
            return f"{key} fora da faixa [{lo}, {hi}]"
    return None


def validate_config(
    payload: Any,
) -> tuple[bool, Optional[dict[str, Any]], Optional[str]]:
    """Valida um payload de config recebido em ``.../config`` (Property 19).

    Aceita ``payload`` como dict **ou** string JSON (o firmware recebe
    bytes do MQTT e parseia). Todos os campos presentes devem estar
    dentro das faixas declaradas em :data:`_CONFIG_RANGES`. Campos
    ausentes são permitidos (config parcial é uma feature — o chamador
    faz merge com o state atual via :func:`apply_config`).

    Returns:
        Tupla ``(is_valid, parsed_or_None, reason_or_None)``. Em caso de
        sucesso, ``parsed`` é um dict com os campos recebidos (tipos
        normalizados); em falha, ``parsed is None`` e ``reason`` descreve
        o motivo em pt-BR.
    """
    if isinstance(payload, str):
        try:
            data = json.loads(payload)
        except (json.JSONDecodeError, ValueError):
            return False, None, "payload nao e JSON valido"
    else:
        data = payload
    if not isinstance(data, dict):
        return False, None, "payload deve ser objeto JSON"
    if not data:
        # Nenhum campo para aplicar → rejeitamos (comportamento conservador).
        return False, None, "payload vazio"
    parsed: dict[str, Any] = {}
    for key, value in data.items():
        reason = _check_field(key, value)
        if reason is not None:
            return False, None, reason
        # Normaliza tipo (int/float conforme a regra do campo).
        kind = _CONFIG_RANGES[key][0]
        parsed[key] = int(value) if kind == "int" else float(value)
    return True, parsed, None


def apply_config(
    state: dict[str, Any],
    payload: Any,
) -> tuple[dict[str, Any], Optional[str]]:
    """Aplica config sobre ``state`` preservando campos ausentes (Property 19).

    Se ``payload`` for inválido, retorna ``(state, reason)`` sem alterações.
    Caso contrário, retorna ``(novo_state, None)`` onde ``novo_state`` é a
    fusão de ``state`` com os campos parseados.

    Nota: o chamador é responsável por publicar a mensagem de rejeição em
    ``.../rejeicao`` (R16.3). Esta função é pura.
    """
    ok, parsed, reason = validate_config(payload)
    if not ok or parsed is None:
        return dict(state), reason
    merged = dict(state)
    merged.update(parsed)
    return merged, None


class ConfigManager:
    """Mantém a config persistida (Property 20 / R16.2).

    Usa um "storage" injetado (dict) para simular SPIFFS. Toda aplicação
    válida atualiza ``last_valid`` **e** o storage; aplicações inválidas
    não alteram nenhum dos dois — após um ``reboot()``, o state carregado
    é sempre a última config **válida** aplicada (P20).

    O storage padrão é um dict em memória, mas o chamador pode injetar
    uma instância customizada (por exemplo, para simular falhas de I/O).
    """

    STORAGE_KEY: str = "cardioia.config"

    def __init__(
        self,
        initial_state: Optional[dict[str, Any]] = None,
        storage: Optional[dict[str, Any]] = None,
    ) -> None:
        self._state: dict[str, Any] = dict(initial_state or DEFAULT_CONFIG)
        self._last_valid: dict[str, Any] = dict(self._state)
        self._storage: dict[str, Any] = storage if storage is not None else {}
        # Primeira persistência: assegura que ``reboot()`` funcione já no boot.
        self._storage[self.STORAGE_KEY] = dict(self._last_valid)
        self.rejection_events: list[str] = []

    @property
    def state(self) -> dict[str, Any]:
        """Retorna uma cópia do state atual (imutabilidade externa)."""
        return dict(self._state)

    @property
    def last_valid(self) -> dict[str, Any]:
        """Retorna uma cópia da última config válida aplicada (P20)."""
        return dict(self._last_valid)

    def apply(self, payload: Any) -> tuple[bool, Optional[str]]:
        """Tenta aplicar ``payload``. Retorna ``(ok, reason_or_None)``.

        Em caso de sucesso, atualiza ``state``, ``last_valid`` e persiste
        (R16.2). Em falha, nenhum dos três é alterado (P20) e
        ``reason`` é acumulado em ``rejection_events`` (R16.3).
        """
        new_state, reason = apply_config(self._state, payload)
        if reason is not None:
            self.rejection_events.append(reason)
            return False, reason
        self._state = new_state
        self._last_valid = dict(new_state)
        self._storage[self.STORAGE_KEY] = dict(new_state)
        return True, None

    def reboot(self) -> dict[str, Any]:
        """Simula um reboot. Carrega a última config válida do storage (P20)."""
        loaded = self._storage.get(self.STORAGE_KEY)
        if loaded is None:
            loaded = dict(DEFAULT_CONFIG)
        self._state = dict(loaded)
        self._last_valid = dict(loaded)
        return dict(self._state)


# =============================================================================
# Sanity: módulo pode ser importado sem efeitos colaterais
# =============================================================================
# Não há código de inicialização no nível de módulo. Todas as operações
# ficam sob responsabilidade do chamador (testes de propriedade).
