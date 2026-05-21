// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/edge_buffer.h
// Finalidade:
//   Declara o buffer de Edge Computing do firmware — o componente que
//   materializa a camada ``Local_Buffer`` do design e é responsável por
//   preservar ``Sample_Record`` enquanto ``Connectivity_Flag == false``
//   (offline) ou enquanto ainda existam amostras pendentes de
//   sincronização. O módulo é dividido em três entidades:
//
//     * ``IBufferStorage`` — interface abstrata que desacopla a lógica
//       do buffer (FIFO / limites / logs) do meio de persistência
//       (SPIFFS em produção, RAM em fallback, RAM simulada em testes
//       nativos).
//     * ``SpiffsStorage``  — implementação primária. No chip físico
//       (``ARDUINO`` definido) escreveria cada registro como uma linha
//       do arquivo ``buffer.ndjson``. Nesta iteração do firmware — e no
//       env PlatformIO ``native`` usado pelos testes — o backend é um
//       ``std::deque`` em RAM que opcionalmente **injeta erros de I/O**
//       para permitir a cosimulação de Property 9. A integração SPIFFS
//       real é um TODO marcado abaixo e será fechada quando o projeto
//       rodar em hardware.
//     * ``RamStorage``     — implementação de fallback pura em RAM,
//       também com semântica FIFO e responsável por emitir o aviso
//       único ao cruzar 80 % da capacidade (Property 10 / R4.5).
//     * ``EdgeBuffer``     — orquestrador: recebe a amostra do
//       ``SensorDriver``/``SampleBuilder``, tenta persistir no
//       primário; em caso de erro de I/O, loga e redireciona para o
//       fallback (R4.4); aplica FIFO global quando a capacidade do
//       sistema é atingida e emite log textual de descarte em até
//       100 ms (R5.4).
//
//   Propriedades cobertas (ver ``design.md``, *Correctness Properties*):
//     * P2  — FIFO limitado, `BUFFER_LIMIT` respeitado, contador em
//             ``[0, capacity]``.
//     * P8  — Roteamento direto vs. enfileiramento por
//             ``Connectivity_Flag`` (a decisão de roteamento é feita
//             pelo ``SyncScheduler``; o ``EdgeBuffer`` oferece a
//             primitiva ``push`` / ``pop_front`` sem conhecimento da
//             flag).
//     * P9  — Multiconjunto preservado sob falhas transitórias do
//             adaptador SPIFFS (redireciona ao ``RamStorage``).
//     * P10 — Aviso único ao cruzar ``ceil(0.8 * capacity)`` no
//             ``RamStorage``; rearma apenas após a ocupação cair.
//
//   Requisitos atendidos:
//     * R4.1, R4.2, R4.3, R4.4, R4.5
//     * R5.1, R5.2, R5.3, R5.4, R5.5, R5.6
//     * R12.2 — Cada função traz um bloco de comentário em pt-BR com
//               propósito, parâmetros e retorno.
//     * R12.4 — Todo ``if`` / ``while`` que toma decisão sobre o
//               estado do buffer recebe comentário em pt-BR imediato
//               acima.
//
//   Restrições:
//     * Sem dependência de ``Arduino.h`` — compila em ``esp32dev`` e
//       em ``native`` (testes Unity).
//     * Sem uso de ``String`` do Arduino.
//     * Única fonte de heap é a ``std::deque<SampleRecord>`` dos
//       storages + o ``std::string paciente_id`` do ``SampleRecord``.
//       Com ``BUFFER_LIMIT = 50`` e um ``SampleRecord`` de ~128 bytes,
//       a ocupação total fica bem abaixo dos 200 KB de SRAM do ESP32.
// =============================================================================

#ifndef CARDIOIA_EDGE_BUFFER_H
#define CARDIOIA_EDGE_BUFFER_H

#include <cstddef>     // std::size_t
#include <cstdint>     // std::uint32_t
#include <deque>       // std::deque — storage base do SPIFFS e do RAM.
#include <optional>    // std::optional<SampleRecord>

#include "config.h"          // cardioia::BUFFER_LIMIT
#include "logger.h"          // cardioia::Logger, cardioia::LogLevel
#include "sample_builder.h"  // cardioia::SampleRecord

namespace cardioia {

// ---------------------------------------------------------------------------
// Interface ``IBufferStorage``
// ---------------------------------------------------------------------------
// Abstração que desacopla o ``EdgeBuffer`` do meio físico de
// persistência. Expõe o conjunto mínimo de operações FIFO exigido pelo
// design (R4.2) e aceita que o ``append`` falhe retornando ``false`` —
// o orquestrador usa esse sinal para acionar o fallback automático
// (R4.4 / P9). Não usa exceções para manter o caminho quente livre de
// *stack unwinding* no ESP32.
class IBufferStorage {
public:
    virtual ~IBufferStorage() = default;

    // -----------------------------------------------------------------------
    // Método:     IBufferStorage::append
    // Finalidade: Anexa ``record`` ao final do storage preservando a
    //             ordem cronológica (R4.3). Em caso de erro de I/O
    //             (ex.: SPIFFS indisponível) deve retornar ``false`` sem
    //             lançar exceção; o orquestrador então redireciona a
    //             amostra ao fallback (R4.4). Implementações que
    //             aplicam FIFO internamente ao atingir a capacidade
    //             devem retornar ``true`` mesmo assim — o descarte por
    //             limite é sinalizado separadamente via ``evictions()``
    //             no orquestrador.
    // Parâmetros:
    //   - ``record``: registro a persistir (cópia).
    // Retorno:    ``true`` em sucesso; ``false`` em erro de I/O.
    // -----------------------------------------------------------------------
    virtual bool append(const SampleRecord& record) = 0;

    // -----------------------------------------------------------------------
    // Método:     IBufferStorage::pop_front
    // Finalidade: Remove e retorna o registro mais antigo. Usado pelo
    //             ``SyncScheduler`` (R7.2) — que só chama ``pop_front``
    //             após confirmação bem-sucedida do envio, para evitar
    //             duplicação em falhas transitórias.
    // Parâmetros: (nenhum).
    // Retorno:    ``std::optional<SampleRecord>`` contendo o registro
    //             removido, ou ``std::nullopt`` se o storage estava
    //             vazio.
    // -----------------------------------------------------------------------
    virtual std::optional<SampleRecord> pop_front() = 0;

    // -----------------------------------------------------------------------
    // Método:     IBufferStorage::size
    // Finalidade: Devolve o número atual de ``SampleRecord`` no storage.
    //             O valor deve sempre estar em ``[0, capacity]`` (R5.5).
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t`` com a contagem atual.
    // -----------------------------------------------------------------------
    virtual std::size_t size() const = 0;

    // -----------------------------------------------------------------------
    // Método:     IBufferStorage::is_empty
    // Finalidade: Açúcar sintático para ``size() == 0`` — expresso
    //             separadamente por clareza nos pontos de decisão do
    //             ``SyncScheduler`` (R7.3).
    // Parâmetros: (nenhum).
    // Retorno:    ``true`` se vazio; caso contrário ``false``.
    // -----------------------------------------------------------------------
    virtual bool is_empty() const = 0;
};

// ---------------------------------------------------------------------------
// Classe ``SpiffsStorage``
// ---------------------------------------------------------------------------
// Storage primário. Em produção (``ARDUINO`` definido) escreveria uma
// linha JSON por registro no arquivo ``buffer.ndjson`` do SPIFFS e
// leria de volta ao reiniciar. Nesta iteração — e nos testes nativos —
// o backend é um espelho em RAM (``std::deque<SampleRecord>``), com
// ponto de injeção para simular erros de I/O (``set_fs_available``).
// O descarte por FIFO é aplicado internamente ao atingir a capacidade.
//
// TODO(firmware): integrar SPIFFS real via ``<SPIFFS.h>`` quando o
//                 projeto for portado ao chip físico. O contrato da
//                 interface NÃO muda — apenas o corpo do ``append`` /
//                 ``pop_front`` passa a ler/escrever o arquivo.
class SpiffsStorage : public IBufferStorage {
public:
    // -----------------------------------------------------------------------
    // Construtor: SpiffsStorage::SpiffsStorage
    // Finalidade: Inicializa o storage com a capacidade solicitada. A
    //             capacidade é cacheada para evitar recomputação em
    //             cada ``append``.
    // Parâmetros:
    //   - ``capacity`` (default ``BUFFER_LIMIT``): número máximo de
    //                                              ``SampleRecord``.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit SpiffsStorage(std::size_t capacity = static_cast<std::size_t>(BUFFER_LIMIT));

    bool append(const SampleRecord& record) override;
    std::optional<SampleRecord> pop_front() override;
    std::size_t size() const override { return records_.size(); }
    bool is_empty() const override { return records_.empty(); }

    // -----------------------------------------------------------------------
    // Método:     SpiffsStorage::set_fs_available
    // Finalidade: Ponto de injeção para testes nativos Unity e para a
    //             cosimulação de Property 9 contra o reference model:
    //             quando ``available == false``, o próximo ``append``
    //             falha (retorna ``false``) como se o SPIFFS estivesse
    //             indisponível. Restabelecer ``true`` volta ao caminho
    //             normal. Em produção, este método é substituído por
    //             uma detecção real baseada no retorno de
    //             ``SPIFFS.begin()`` / ``file.print()``.
    // Parâmetros:
    //   - ``available``: estado simulado do filesystem.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void set_fs_available(bool available) noexcept { fs_available_ = available; }

    // -----------------------------------------------------------------------
    // Método:     SpiffsStorage::capacity
    // Finalidade: Expõe a capacidade configurada para que o orquestrador
    //             possa validar invariantes sem acoplar-se a
    //             ``BUFFER_LIMIT``.
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t`` com a capacidade.
    // -----------------------------------------------------------------------
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;

    // Espelho em RAM do arquivo ``buffer.ndjson`` — único armazenamento
    // enquanto o SPIFFS real não é integrado (TODO acima). Em
    // produção, esta estrutura passaria a ser apenas um *índice* com
    // offsets de arquivo, conforme o design.
    std::deque<SampleRecord> records_;

    // Flag simulada de disponibilidade. ``true`` por padrão; testes
    // alternam para ``false`` para exercitar a Property 9.
    bool fs_available_ = true;
};

// ---------------------------------------------------------------------------
// Classe ``RamStorage``
// ---------------------------------------------------------------------------
// Storage de fallback puro em RAM. Aplica FIFO ao atingir a capacidade
// e emite ``aviso_80_pct`` ao cruzar — de baixo para cima — o limiar
// ``ceil(0.8 * capacity)``. O aviso só é rearmado após a ocupação
// voltar a ``< limiar`` (semântica *edge-triggered*, espelhando a
// Property 10 do reference model).
class RamStorage : public IBufferStorage {
public:
    // -----------------------------------------------------------------------
    // Construtor: RamStorage::RamStorage
    // Finalidade: Inicializa o storage com a capacidade e um logger
    //             opcional. O logger é usado para emitir o aviso de
    //             80 % no Monitor Serial (R4.5); quando ``nullptr`` a
    //             emissão é silenciosa (caminho usado em testes puros).
    // Parâmetros:
    //   - ``capacity`` (default ``BUFFER_LIMIT``).
    //   - ``logger`` (default ``nullptr``).
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit RamStorage(std::size_t capacity = static_cast<std::size_t>(BUFFER_LIMIT),
                        Logger* logger = nullptr) noexcept;

    bool append(const SampleRecord& record) override;
    std::optional<SampleRecord> pop_front() override;
    std::size_t size() const override { return records_.size(); }
    bool is_empty() const override { return records_.empty(); }

    // -----------------------------------------------------------------------
    // Método:     RamStorage::warnings_emitted
    // Finalidade: Total acumulado de ``aviso_80_pct`` emitidos — usado
    //             pelos testes para validar P10.
    // Parâmetros: (nenhum).
    // Retorno:    contagem como ``int`` (≥ 0).
    // -----------------------------------------------------------------------
    int warnings_emitted() const noexcept { return warnings_emitted_; }

    // -----------------------------------------------------------------------
    // Método:     RamStorage::warning_threshold
    // Finalidade: Expõe o limiar cacheado ``ceil(0.8 * capacity)`` para
    //             permitir auditoria externa (testes, CLI de
    //             diagnóstico). Evita recomputação a cada consulta.
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t`` com o valor do limiar.
    // -----------------------------------------------------------------------
    std::size_t warning_threshold() const noexcept { return warning_threshold_; }

    // -----------------------------------------------------------------------
    // Método:     RamStorage::capacity
    // Finalidade: Expõe a capacidade configurada (análogo ao
    //             ``SpiffsStorage::capacity``).
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t``.
    // -----------------------------------------------------------------------
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::deque<SampleRecord> records_;
    std::size_t warning_threshold_;  // cacheado no construtor.
    Logger* logger_;                 // opcional — pode ser nullptr.
    bool above_threshold_ = false;   // estado da máquina edge-triggered.
    int warnings_emitted_ = 0;

    // -----------------------------------------------------------------------
    // Método:     RamStorage::update_threshold_state (privado)
    // Finalidade: Atualiza a máquina de estado edge-triggered e emite o
    //             evento ``aviso_80_pct`` no cruzamento ascendente.
    //             Chamado por ``append`` e ``pop_front``.
    // Parâmetros: (nenhum).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void update_threshold_state() noexcept;
};

// ---------------------------------------------------------------------------
// Classe ``EdgeBuffer``
// ---------------------------------------------------------------------------
// Orquestrador do buffer de Edge Computing. Coordena um storage
// primário (SPIFFS) e um de fallback (RAM), aplica FIFO global quando
// ambos os storages combinados alcançam a ``capacity`` alvo (``=
// BUFFER_LIMIT``) e emite logs textuais em cada descarte (R5.4).
//
// Contrato:
//   * ``push`` retorna ``true`` se não houve descarte — i.e., o
//     registro entrou sem forçar remoção do mais antigo. Retorna
//     ``false`` quando o tamanho global atingiu ``capacity`` e um
//     descarte por limite aconteceu (R5.3).
//   * ``pop_front`` devolve o mais antigo entre primário e fallback,
//     preservando a ordem cronológica global (primário-first, fallback
//     depois — espelha o *timeline* do reference model).
//   * O ``EdgeBuffer`` NÃO conhece a ``Connectivity_Flag``. A decisão
//     de roteamento (publicar direto vs. enfileirar) é do
//     ``SyncScheduler`` — propriedade P8.
class EdgeBuffer {
public:
    // -----------------------------------------------------------------------
    // Construtor: EdgeBuffer::EdgeBuffer
    // Finalidade: Monta o orquestrador com dois storages injetados e um
    //             logger opcional. ``primary`` e ``fallback`` devem
    //             viver pelo menos tanto quanto o próprio ``EdgeBuffer``
    //             — o orquestrador NÃO toma posse deles.
    // Parâmetros:
    //   - ``primary``:  storage primário (``SpiffsStorage`` em produção).
    //   - ``fallback``: storage de fallback (``RamStorage``).
    //   - ``logger``:   opcional, usado para registrar eventos (default
    //                   ``nullptr`` em testes puros).
    //   - ``capacity`` (default ``BUFFER_LIMIT``): limite global do FIFO
    //                                              combinado.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    EdgeBuffer(IBufferStorage& primary,
               IBufferStorage& fallback,
               Logger* logger = nullptr,
               std::size_t capacity = static_cast<std::size_t>(BUFFER_LIMIT)) noexcept;

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::push
    // Finalidade: Insere ``record``. Tenta o primário; em erro de I/O,
    //             loga e redireciona para o fallback (R4.4). Quando o
    //             tamanho global atinge ``capacity``, aplica FIFO
    //             (descarta o mais antigo — R5.3) e emite log em até
    //             100 ms com o ``size`` atualizado (R5.4).
    // Parâmetros:
    //   - ``record``: amostra a enfileirar (cópia).
    // Retorno:    ``true`` se inseriu sem descarte; ``false`` se aplicou
    //             FIFO e descartou um registro mais antigo.
    // -----------------------------------------------------------------------
    bool push(const SampleRecord& record);

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::pop_front
    // Finalidade: Remove e devolve o mais antigo. Drena primeiro o
    //             primário; se vazio, drena o fallback. A ordem
    //             cronológica global é preservada porque o orquestrador
    //             sempre prefere o primário no ``push`` — o fallback
    //             só recebe registros em cenários de erro de I/O.
    // Parâmetros: (nenhum).
    // Retorno:    ``std::optional<SampleRecord>`` com o registro, ou
    //             ``std::nullopt`` se ambos os storages estavam vazios.
    // -----------------------------------------------------------------------
    std::optional<SampleRecord> pop_front();

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::size
    // Finalidade: Soma os tamanhos de primário e fallback, sempre em
    //             ``[0, capacity]`` (R5.5, R5.6).
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t`` com o total.
    // -----------------------------------------------------------------------
    std::size_t size() const noexcept;

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::is_empty
    // Finalidade: ``true`` quando ambos os storages estão vazios. Usado
    //             pelo ``SyncScheduler`` para decidir entre publicar
    //             direto ou enfileirar (R7.3, Property 8).
    // Parâmetros: (nenhum).
    // Retorno:    ``bool``.
    // -----------------------------------------------------------------------
    bool is_empty() const noexcept;

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::capacity
    // Finalidade: Expõe a capacidade global configurada.
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t``.
    // -----------------------------------------------------------------------
    std::size_t capacity() const noexcept { return capacity_; }

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::evictions
    // Finalidade: Total de descartes por limite acumulados desde o
    //             boot. Usado pelos testes (P2) e pelo Monitor Serial
    //             (R5.6) para auditar a saúde do buffer.
    // Parâmetros: (nenhum).
    // Retorno:    ``int`` (≥ 0).
    // -----------------------------------------------------------------------
    int evictions() const noexcept { return evictions_; }

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::primary_io_errors
    // Finalidade: Quantas vezes o ``SpiffsStorage::append`` reportou
    //             ``false`` e a amostra foi redirecionada ao fallback.
    //             Usado em testes de P9 e em diagnósticos de campo.
    // Parâmetros: (nenhum).
    // Retorno:    ``int`` (≥ 0).
    // -----------------------------------------------------------------------
    int primary_io_errors() const noexcept { return primary_io_errors_; }

private:
    IBufferStorage& primary_;
    IBufferStorage& fallback_;
    Logger* logger_;
    std::size_t capacity_;
    int evictions_ = 0;
    int primary_io_errors_ = 0;

    // -----------------------------------------------------------------------
    // Método:     EdgeBuffer::drop_oldest_for_fifo (privado)
    // Finalidade: Aplica o descarte FIFO global: remove o mais antigo
    //             (prefere o primário) quando o tamanho total já
    //             alcançou ``capacity``. Encapsulado para manter o
    //             ``push`` legível.
    // Parâmetros: (nenhum).
    // Retorno:    (void). Atualiza ``evictions_`` e emite log no
    //             ``Logger`` quando disponível.
    // -----------------------------------------------------------------------
    void drop_oldest_for_fifo();
};

}  // namespace cardioia

#endif  // CARDIOIA_EDGE_BUFFER_H
