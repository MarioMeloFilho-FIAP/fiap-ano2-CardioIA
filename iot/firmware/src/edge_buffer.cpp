// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/edge_buffer.cpp
// Finalidade:
//   Implementação do buffer de Edge Computing declarado em
//   ``edge_buffer.h``. O código é totalmente portável entre o env
//   PlatformIO ``esp32dev`` (produção) e o env ``native`` usado pelos
//   testes nativos Unity — nenhuma dependência de ``Arduino.h``,
//   ``SPIFFS.h`` ou ``Serial`` aparece aqui. A única fonte de heap são
//   os ``std::deque<SampleRecord>`` internos aos storages.
//
//   Estrutura do arquivo:
//     1. ``SpiffsStorage``     — append/pop com injeção de erro de I/O
//                                (espelha ``SpiffsMockStorage`` do
//                                reference model).
//     2. ``RamStorage``        — append/pop com máquina de estado
//                                edge-triggered para ``aviso_80_pct``
//                                (espelha ``RamFallbackStorage``).
//     3. ``EdgeBuffer``        — orquestrador: fallback automático em
//                                erro de I/O (R4.4), FIFO global com
//                                log textual (R5.3 / R5.4), contador
//                                exposto (R5.5 / R5.6).
//
//   Requisitos atendidos: R4.1–R4.5, R5.1–R5.6, R12.2, R12.4.
//   Propriedades suportadas: P2, P8, P9, P10.
// =============================================================================

#include "edge_buffer.h"

#include <algorithm>  // std::max, std::min
#include <cmath>      // std::ceil (usado para o limiar de 80 %)
#include <cstdio>     // std::snprintf — formatação dos logs

namespace cardioia {

// ===========================================================================
// Utilitários internos (anonymous namespace)
// ===========================================================================
namespace {

// ---------------------------------------------------------------------------
// Função:     ceil_div_4_5 (anônima)
// Finalidade: Calcula ``ceil(0.8 * capacity)`` — o limiar que dispara o
//             ``aviso_80_pct`` (R4.5, Property 10) — usando apenas
//             aritmética inteira, para evitar qualquer surpresa de
//             arredondamento com ``double`` em embarcados. A fórmula
//             equivalente é ``(4 * capacity + 4) / 5``: 4/5 = 0.8 e o
//             ``+4`` implementa o teto sem recorrer a ``std::ceil``.
// Parâmetros:
//   - ``capacity``: capacidade total do storage (``> 0``).
// Retorno:    ``std::size_t`` com o limiar ``ceil(0.8 * capacity)``.
// ---------------------------------------------------------------------------
std::size_t ceil_div_4_5(std::size_t capacity) noexcept {
    // O limiar para ``capacity == 0`` não tem sentido, mas devolvemos 0
    // de forma defensiva para não estourar o cálculo em usos acidentais.
    if (capacity == 0U) {
        return 0U;
    }
    return (capacity * 4U + 4U) / 5U;
}

}  // namespace

// ===========================================================================
// SpiffsStorage
// ===========================================================================

// ---------------------------------------------------------------------------
// Construtor: SpiffsStorage::SpiffsStorage
// Finalidade: Inicializa a capacidade e deixa o espelho em RAM vazio.
//             Em uma iteração futura (TODO marcado no cabeçalho) este
//             construtor também abriria ``buffer.ndjson`` e leria os
//             registros persistidos do boot anterior.
// Parâmetros:
//   - ``capacity``: capacidade máxima em número de ``SampleRecord``.
// Retorno:    (construtor).
// ---------------------------------------------------------------------------
SpiffsStorage::SpiffsStorage(std::size_t capacity)
    : capacity_(capacity == 0U ? 1U : capacity) {
    // ``capacity == 0`` é tratado como 1 para evitar ``deque`` de
    // tamanho zero que quebraria invariantes do FIFO interno.
}

// ---------------------------------------------------------------------------
// Método:     SpiffsStorage::append
// Finalidade: Persiste ``record`` no storage primário. Se o filesystem
//             estiver indisponível (``fs_available_ == false``), sinaliza
//             o erro retornando ``false`` sem modificar o estado — o
//             ``EdgeBuffer`` usa esse retorno para ativar o fallback
//             RAM (R4.4, P9). Em sucesso, aplica FIFO local quando o
//             tamanho atinge a capacidade individual deste storage.
// Parâmetros:
//   - ``record``: registro a persistir.
// Retorno:    ``true`` em sucesso; ``false`` em erro simulado de I/O.
// ---------------------------------------------------------------------------
bool SpiffsStorage::append(const SampleRecord& record) {
    // Decisão: quando o filesystem está marcado como indisponível,
    // sinalizamos falha de I/O para que o orquestrador roteie a
    // amostra ao fallback (R4.4).
    if (!fs_available_) {
        return false;
    }

    // Decisão: quando o ``deque`` interno alcança a capacidade
    // individual do storage, aplicamos FIFO descartando o mais antigo.
    // Esse descarte local é diferente do "descarte global" do
    // ``EdgeBuffer`` — aqui apenas mantemos a invariante de capacidade
    // por storage. O orquestrador aplica FIFO global antes de chamar
    // ``append``, o que normalmente evita que este ramo execute.
    if (records_.size() >= capacity_) {
        records_.pop_front();
    }
    records_.push_back(record);
    return true;
}

// ---------------------------------------------------------------------------
// Método:     SpiffsStorage::pop_front
// Finalidade: Remove e retorna o registro mais antigo do storage.
//             ``std::nullopt`` quando vazio — comportamento idêntico
//             ao do ``SpiffsMockStorage`` do reference model.
// Parâmetros: (nenhum).
// Retorno:    ``std::optional<SampleRecord>``.
// ---------------------------------------------------------------------------
std::optional<SampleRecord> SpiffsStorage::pop_front() {
    // Decisão: storage vazio → devolve ``nullopt`` sem alterar estado.
    if (records_.empty()) {
        return std::nullopt;
    }
    SampleRecord front = records_.front();
    records_.pop_front();
    return front;
}

// ===========================================================================
// RamStorage
// ===========================================================================

// ---------------------------------------------------------------------------
// Construtor: RamStorage::RamStorage
// Finalidade: Inicializa o fallback com capacidade alvo e caches do
//             limiar edge-triggered (``ceil(0.8 * capacity)``).
// Parâmetros:
//   - ``capacity``: capacidade máxima.
//   - ``logger``:   logger opcional (pode ser ``nullptr``).
// Retorno:    (construtor).
// ---------------------------------------------------------------------------
RamStorage::RamStorage(std::size_t capacity, Logger* logger) noexcept
    : capacity_(capacity == 0U ? 1U : capacity),
      warning_threshold_(ceil_div_4_5(capacity == 0U ? 1U : capacity)),
      logger_(logger) {}

// ---------------------------------------------------------------------------
// Método:     RamStorage::append
// Finalidade: Anexa ``record`` ao fim; aplica FIFO local ao atingir a
//             capacidade e, em seguida, atualiza a máquina de estado
//             do aviso de 80 % (R4.5, Property 10).
// Parâmetros:
//   - ``record``: registro a guardar.
// Retorno:    ``true`` — o fallback nunca reporta erro de I/O; a única
//             forma de perder uma amostra aqui é via FIFO local.
// ---------------------------------------------------------------------------
bool RamStorage::append(const SampleRecord& record) {
    // Decisão: ao atingir a capacidade, descarta o mais antigo antes
    // de inserir — mesma política FIFO do ``deque(maxlen=N)``.
    if (records_.size() >= capacity_) {
        records_.pop_front();
    }
    records_.push_back(record);
    update_threshold_state();
    return true;
}

// ---------------------------------------------------------------------------
// Método:     RamStorage::pop_front
// Finalidade: Remove e devolve o mais antigo; atualiza a máquina de
//             estado para rearmar o aviso de 80 % quando a ocupação
//             cai abaixo do limiar.
// Parâmetros: (nenhum).
// Retorno:    ``std::optional<SampleRecord>``.
// ---------------------------------------------------------------------------
std::optional<SampleRecord> RamStorage::pop_front() {
    // Decisão: vazio → ``nullopt``, sem mexer na máquina de estado.
    if (records_.empty()) {
        return std::nullopt;
    }
    SampleRecord front = records_.front();
    records_.pop_front();
    update_threshold_state();
    return front;
}

// ---------------------------------------------------------------------------
// Método:     RamStorage::update_threshold_state (privado)
// Finalidade: Máquina de estado edge-triggered para o aviso de 80 %
//             (R4.5, Property 10). Emite o evento ``aviso_80_pct``
//             apenas no cruzamento ascendente (``was_above == false``
//             e ``is_above == true``) — rearma quando a ocupação volta
//             a cair abaixo do limiar.
// Parâmetros: (nenhum).
// Retorno:    (void).
// ---------------------------------------------------------------------------
void RamStorage::update_threshold_state() noexcept {
    const bool was_above = above_threshold_;
    const bool is_above = records_.size() >= warning_threshold_;

    // Decisão: só emitimos o aviso no cruzamento ascendente do limiar,
    // para garantir "exatamente um aviso por cruzamento" como pede a
    // Property 10 do reference model.
    if (is_above && !was_above) {
        ++warnings_emitted_;
        if (logger_ != nullptr) {
            char extras[64];
            std::snprintf(extras, sizeof(extras),
                          "size=%u/%u",
                          static_cast<unsigned>(records_.size()),
                          static_cast<unsigned>(capacity_));
            logger_->event(LogLevel::WARN,
                           "EdgeBuffer",
                           "aviso_80_pct (RAM fallback cruzou 80% da capacidade)",
                           extras);
        }
    }
    above_threshold_ = is_above;
}

// ===========================================================================
// EdgeBuffer (orquestrador)
// ===========================================================================

// ---------------------------------------------------------------------------
// Construtor: EdgeBuffer::EdgeBuffer
// Finalidade: Amarra as referências aos storages, guarda o logger
//             opcional e cacheia a capacidade global. O orquestrador
//             não assume posse dos storages — o chamador é quem
//             controla o ciclo de vida.
// Parâmetros:
//   - ``primary``:  storage primário (SPIFFS).
//   - ``fallback``: storage de fallback (RAM).
//   - ``logger``:   logger opcional.
//   - ``capacity``: capacidade global alvo (``> 0``).
// Retorno:    (construtor).
// ---------------------------------------------------------------------------
EdgeBuffer::EdgeBuffer(IBufferStorage& primary,
                       IBufferStorage& fallback,
                       Logger* logger,
                       std::size_t capacity) noexcept
    : primary_(primary),
      fallback_(fallback),
      logger_(logger),
      capacity_(capacity == 0U ? 1U : capacity) {}

// ---------------------------------------------------------------------------
// Método:     EdgeBuffer::size
// Finalidade: Soma dos tamanhos dos dois storages — sempre no intervalo
//             ``[0, capacity]`` (R5.5, R5.6).
// Parâmetros: (nenhum).
// Retorno:    ``std::size_t``.
// ---------------------------------------------------------------------------
std::size_t EdgeBuffer::size() const noexcept {
    return primary_.size() + fallback_.size();
}

// ---------------------------------------------------------------------------
// Método:     EdgeBuffer::is_empty
// Finalidade: ``true`` se ambos os storages estão vazios — base para
//             a decisão de roteamento do ``SyncScheduler`` (R7.3).
// Parâmetros: (nenhum).
// Retorno:    ``bool``.
// ---------------------------------------------------------------------------
bool EdgeBuffer::is_empty() const noexcept {
    return primary_.is_empty() && fallback_.is_empty();
}

// ---------------------------------------------------------------------------
// Método:     EdgeBuffer::drop_oldest_for_fifo (privado)
// Finalidade: Aplica o descarte FIFO global (R5.3). Prefere remover do
//             primário (onde está o registro mais antigo sob operação
//             normal); recorre ao fallback só se o primário estiver
//             vazio — cenário possível quando o SPIFFS ficou
//             indisponível durante a coleta. Emite log textual
//             informativo em até 100 ms (R5.4) com o ``size``
//             atualizado pós-descarte.
// Parâmetros: (nenhum).
// Retorno:    (void). Atualiza ``evictions_``.
// ---------------------------------------------------------------------------
void EdgeBuffer::drop_oldest_for_fifo() {
    // Decisão: tentamos primeiro o primário; se ele estiver vazio,
    // caímos para o fallback. Um dos dois obrigatoriamente terá um
    // registro a descartar, porque ``drop_oldest_for_fifo`` só é
    // chamado com ``size() >= capacity`` (e ``capacity >= 1``).
    if (!primary_.is_empty()) {
        (void)primary_.pop_front();
    } else if (!fallback_.is_empty()) {
        (void)fallback_.pop_front();
    } else {
        // Guarda defensiva: chamada inesperada com ambos vazios. Não
        // atualizamos ``evictions_`` porque nada foi de fato descartado.
        return;
    }
    ++evictions_;

    if (logger_ != nullptr) {
        // Mensagem textual exigida por R5.4: precisa conter indicação
        // de "descarte por limite de buffer" e o ``size`` atualizado.
        char extras[64];
        std::snprintf(extras, sizeof(extras),
                      "size=%u/%u,evictions=%d",
                      static_cast<unsigned>(size()),
                      static_cast<unsigned>(capacity_),
                      evictions_);
        logger_->event(LogLevel::INFO,
                       "EdgeBuffer",
                       "descarte por limite de buffer (FIFO)",
                       extras);
    }
}

// ---------------------------------------------------------------------------
// Método:     EdgeBuffer::push
// Finalidade: Inserir um ``SampleRecord`` no FIFO global com fallback
//             automático e descarte FIFO global quando cheio.
//
//   Fluxo:
//     1. Se o tamanho total já alcançou ``capacity``, aplica FIFO
//        global (``drop_oldest_for_fifo``) e marca ``evicted = true``.
//     2. Tenta ``primary_.append(record)``. Se falhar (erro de I/O),
//        loga e chama ``fallback_.append(record)``.
//     3. Em ambos os caminhos, o registro entra — o fallback só retorna
//        ``false`` em cenários hipotéticos extremos (defensive).
//     4. Retorna ``!evicted``.
// Parâmetros:
//   - ``record``: amostra a enfileirar.
// Retorno:    ``true`` se inseriu sem descarte; ``false`` se o buffer
//             estava cheio e aplicou FIFO.
// ---------------------------------------------------------------------------
bool EdgeBuffer::push(const SampleRecord& record) {
    bool evicted = false;

    // Decisão: enquanto o tamanho total já alcançou a capacidade
    // global, aplicamos FIFO para liberar espaço para o novo registro
    // (R5.2/R5.3). ``while`` em vez de ``if`` por robustez: se a
    // capacidade mudar em runtime, ainda convergimos para o estado
    // correto antes de inserir. Na prática o loop executa no máximo
    // uma vez por chamada porque ``capacity_`` é constante após o
    // construtor.
    while (size() >= capacity_) {
        drop_oldest_for_fifo();
        evicted = true;
    }

    // Decisão: tentamos o primário primeiro; em erro de I/O caímos
    // para o fallback RAM (R4.4 / Property 9).
    if (!primary_.append(record)) {
        ++primary_io_errors_;
        if (logger_ != nullptr) {
            logger_->event(LogLevel::ERROR,
                           "EdgeBuffer",
                           "erro de I/O no SPIFFS — ativando fallback RAM",
                           "origem=SpiffsStorage.append");
        }
        // Defensivo: se o fallback também falhar (cenário improvável
        // neste design — o RamStorage sempre retorna true), registramos
        // o descarte silencioso da amostra.
        if (!fallback_.append(record)) {
            if (logger_ != nullptr) {
                logger_->event(LogLevel::ERROR,
                               "EdgeBuffer",
                               "falha ao gravar no fallback RAM — amostra perdida",
                               "origem=RamStorage.append");
            }
        }
    }

    return !evicted;
}

// ---------------------------------------------------------------------------
// Método:     EdgeBuffer::pop_front
// Finalidade: Remove o mais antigo entre primário e fallback,
//             preferindo o primário para preservar a ordem cronológica
//             global (o fallback só tem registros se houve erro de I/O
//             no primário em algum ponto). Usado pelo ``SyncScheduler``
//             após confirmação de envio (R7.2).
// Parâmetros: (nenhum).
// Retorno:    ``std::optional<SampleRecord>``; ``nullopt`` se vazio.
// ---------------------------------------------------------------------------
std::optional<SampleRecord> EdgeBuffer::pop_front() {
    // Decisão: drena primeiro o primário; só vamos ao fallback se
    // ele estiver vazio — política conservadora, alinhada com a
    // ordem cronológica global (o primário recebe amostras em
    // operação normal; o fallback só recebe em cenários de erro).
    if (!primary_.is_empty()) {
        return primary_.pop_front();
    }
    if (!fallback_.is_empty()) {
        return fallback_.pop_front();
    }
    return std::nullopt;
}

}  // namespace cardioia
