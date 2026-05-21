// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/bpm_window.cpp
// Finalidade:
//   Implementação do ``cardioia::BpmWindow`` declarado em
//   ``bpm_window.h``. Espelha, em C++ 17, a classe homônima do
//   reference model Python (``iot/tests/reference_model.py``) que é o
//   oráculo da Property 3 nos testes de propriedade.
//
//   Pontos de atenção:
//     * Nenhuma alocação dinâmica: o ring buffer é ``std::array``
//       (membro) com capacidade fixa de 300 slots.
//     * Aritmética em 64 bits no numerador da extrapolação para evitar
//       *overflow* ao multiplicar ``count_total`` por 60 000 em
//       ``std::uint32_t``.
//     * O arredondamento (*round half away from zero* via
//       ``std::lround``) pode diferir em uma unidade do ``round()``
//       Python (*round half to even*) em frações ``.5`` exatas. Essa
//       divergência é documentada no cabeçalho de ``bpm_window.h`` e
//       não afeta nem a suíte PBT (que exercita apenas o reference
//       model) nem a classificação clínica, cujo limiar é 120 bpm.
//
//   Requisitos atendidos: R2.2, R2.3, R2.5, R2.6, R2.7, R12.2, R12.4.
// =============================================================================

#include "bpm_window.h"

#include <cmath>    // std::lround
#include <cstdint>  // std::int64_t

namespace cardioia {

// ---------------------------------------------------------------------------
// Construtor
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Construtor: BpmWindow::BpmWindow
// Finalidade: Armazena o instante de boot e deixa o ring buffer vazio.
//             O ``std::array<uint32_t, 300>`` já é zerado pela inicialização
//             de membro em ``bpm_window.h`` (``ring_{}``), mantendo a classe
//             usável mesmo antes do primeiro ``registrar_pulso``.
// Parâmetros:
//   - ``boot_ms``: instante em ms considerado como origem para o cálculo
//                  de ``elapsed_ms`` na extrapolação (R2.7).
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
BpmWindow::BpmWindow(std::uint32_t boot_ms) noexcept
    : boot_ms_(boot_ms) {}

// ---------------------------------------------------------------------------
// Registro de pulsos
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     BpmWindow::registrar_pulso
// Finalidade: Insere ``ts_ms`` no fim do ring. Se o ring já estiver cheio
//             (``count_ == kRingCapacity``), o slot mais antigo é
//             sobrescrito — comportamento equivalente ao
//             ``deque(maxlen=300)`` do reference model.
// Parâmetros:
//   - ``ts_ms``: instante do pulso em ms; o chamador é responsável
//                pelo debounce de 150 ms (R2.4 / P4).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void BpmWindow::registrar_pulso(std::uint32_t ts_ms) noexcept {
    // Grava o timestamp na próxima posição livre (circular).
    ring_[head_] = ts_ms;

    // Avança ``head_`` em círculo: quando atinge ``kRingCapacity`` volta
    // a zero. Uso de divisão inteira (%) para clareza; o compilador
    // otimiza para AND em potências de 2 quando possível.
    head_ = (head_ + 1U) % kRingCapacity;

    // ``count_`` cresce até saturar em ``kRingCapacity``. A partir daí,
    // ``head_`` passa a sobrescrever os slots mais antigos — o registro
    // permanece FIFO circular.
    // Comentário pt-BR acima do ``if`` que decide entre crescer ou
    // saturar o contador de pulsos (R12.4).
    if (count_ < kRingCapacity) {
        ++count_;
    }
}

// ---------------------------------------------------------------------------
// Poda preguiçosa
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     BpmWindow::podar_preguicosamente (privado, const)
// Finalidade: Remove do ring buffer, a partir do slot mais antigo, todos
//             os timestamps ``<= (now_ms - kWindowMs)``. Imita o
//             ``while self._pulses and self._pulses[0] <= threshold:
//             self._pulses.popleft()`` do reference model. O método é
//             ``const`` mas altera membros ``mutable`` (``count_``), em
//             uso consagrado de *logical const / physical mutable*.
// Parâmetros:
//   - ``now_ms``: instante atual. O chamador garante
//                 ``now_ms >= kWindowMs``; mesmo assim, a subtração é
//                 feita em ``std::uint32_t`` e o threshold resultante é
//                 comparado com ``<=`` (mesma semântica do Python).
// Retorno:    (void). O efeito observável é ``count_`` decrescido e o
//             ``oldest_idx`` implícito avançado.
// -----------------------------------------------------------------------------
void BpmWindow::podar_preguicosamente(std::uint32_t now_ms) const noexcept {
    // ``threshold`` é o MAIOR timestamp que DEVE ser descartado: pulsos
    // com ``ts <= threshold`` estão fora da janela ``(threshold, now_ms]``.
    const std::uint32_t threshold = now_ms - kWindowMs;

    // Laço equivalente ao ``while deque and deque[0] <= threshold:
    // deque.popleft()`` do Python. Comentário pt-BR acima do ``while``
    // deixa claro o critério de parada (R12.4).
    // Enquanto houver ao menos um timestamp no ring E o mais antigo
    // estiver fora da janela, descartamo-lo decrementando ``count_``
    // (o ``head_`` não se move — a "frente" é sempre derivada de
    // ``head_`` e ``count_``).
    while (count_ > 0U) {
        // Índice do elemento mais antigo: ``head_`` aponta para a
        // próxima posição de escrita, então o mais antigo está
        // ``count_`` passos atrás — com *wrap-around* via módulo.
        const std::size_t oldest_idx =
            (head_ + kRingCapacity - count_) % kRingCapacity;

        if (ring_[oldest_idx] > threshold) {
            break;  // O mais antigo ainda está na janela → poda termina.
        }

        --count_;  // Descarta logicamente o mais antigo.
    }
}

// ---------------------------------------------------------------------------
// Cálculo do BPM
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Método:     BpmWindow::calcular_bpm
// Finalidade: Implementa a Property 3. Ramifica em três caminhos
//             (elapsed zero, janela plena e extrapolação) e satura em
//             250 no final.
// Parâmetros:
//   - ``now_ms``: instante atual em ms.
// Retorno:    BPM ``uint8_t`` em ``[0, kMaxBpm]``.
// -----------------------------------------------------------------------------
std::uint8_t BpmWindow::calcular_bpm(std::uint32_t now_ms) const noexcept {
    // Defesa contra contrato violado (``now_ms < boot_ms``): o reference
    // model lança ``ValueError``; aqui, por ser ``noexcept`` e rodar em
    // firmware, degradamos para ``0`` — nenhuma amostra indica sinal.
    if (now_ms < boot_ms_) {
        return 0U;
    }

    const std::uint32_t elapsed = now_ms - boot_ms_;

    // Caso degenerado: sem tempo decorrido desde o boot. Retornar 0
    // evita divisão por zero na extrapolação e espelha o reference
    // model (``if elapsed <= 0: return 0``).
    if (elapsed == 0U) {
        return 0U;
    }

    // Seleção entre janela plena (R2.2/R2.3/R2.6) e extrapolação
    // proporcional (R2.7). Comentário pt-BR acima do ``if`` que
    // escolhe o ramo (R12.4):
    //
    //   * ``elapsed >= kWindowMs`` (uptime cobre a janela de 60 s)
    //     → contamos os timestamps em ``(now_ms - 60 000, now_ms]``
    //       após poda preguiçosa.
    //   * caso contrário (``boot`` recente)
    //     → extrapolamos a contagem total para 60 s:
    //         bpm = round(|ts_totais| * 60 000 / elapsed).
    if (elapsed >= kWindowMs) {
        // Ramo janela plena: removemos os timestamps vencidos e
        // contamos os remanescentes. Após ``podar_preguicosamente``,
        // todos os slots válidos estão, por construção, dentro da
        // janela — logo ``count_`` é exatamente a contagem exigida.
        podar_preguicosamente(now_ms);

        if (count_ >= static_cast<std::size_t>(kMaxBpm)) {
            return kMaxBpm;  // Saturação (R2.5).
        }
        return static_cast<std::uint8_t>(count_);
    }

    // Ramo extrapolação (R2.7).
    //
    // Cuidado com *overflow*: ``count_`` cabe em 9 bits (≤ 300), mas
    // ``count_ * 60 000`` em ``std::uint32_t`` pode ultrapassar
    // 18 000 000, ainda dentro do range de 32 bits; ainda assim,
    // promovemos para 64 bits para robustez e para casar com o
    // numerador inteiro do ``round()`` do Python (que opera em
    // ``int`` de precisão arbitrária).
    const std::int64_t numerador =
        static_cast<std::int64_t>(count_) *
        static_cast<std::int64_t>(kWindowMs);

    // ``elapsed > 0`` garantido pelas guardas anteriores, então a
    // divisão é segura. ``static_cast<double>`` explícito para habilitar
    // o arredondamento em ``std::lround`` (*round half away from zero*).
    const double razao =
        static_cast<double>(numerador) / static_cast<double>(elapsed);

    const long extrapolado = std::lround(razao);

    // Saturação final (R2.5). ``extrapolado`` é ``>= 0`` por
    // construção (``count_ >= 0``, ``kWindowMs > 0``, ``elapsed > 0``).
    // Comentário pt-BR acima do ``if`` de saturação (R12.4).
    if (extrapolado >= static_cast<long>(kMaxBpm)) {
        return kMaxBpm;
    }
    return static_cast<std::uint8_t>(extrapolado);
}

}  // namespace cardioia
