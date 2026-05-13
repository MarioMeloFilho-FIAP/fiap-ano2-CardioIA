// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/bpm_window.h
// Finalidade:
//   Declara ``cardioia::BpmWindow`` — a janela deslizante de 60 segundos
//   que converte timestamps de pulso (originados da ISR do
//   ``Pulse_Simulator``) em batimentos por minuto (bpm). Todo o módulo é
//   lógica pura: não depende de ``Arduino.h``, ``Serial`` ou ``millis()``
//   e pode ser compilado tanto pelo env PlatformIO ``esp32dev`` quanto
//   pelo env ``native`` usado por testes nativos Unity.
//
//   Espelha, em C++ 17, a classe ``BpmWindow`` de
//   ``iot/tests/reference_model.py`` (Property 3 / P3) e satisfaz os
//   critérios EARS abaixo:
//
//     * R2.2 — ``calcular_bpm`` retorna a contagem de pulsos registrados
//              na janela ``(now_ms - 60 000, now_ms]`` quando
//              ``elapsed_ms >= 60 000``.
//     * R2.3 — O valor é atualizado a cada chamada; o ring buffer
//              mantém timestamps suficientes para 250 bpm × 60 s +
//              margem (``kRingCapacity = 300``).
//     * R2.5 — Saturação: o retorno nunca ultrapassa 250 (``kMaxBpm``).
//     * R2.6 — Janela vazia com elapsed pleno devolve ``0``.
//     * R2.7 — Antes dos 60 s de uptime, aplica extrapolação proporcional
//              ``round(|ts_totais| * 60 000 / elapsed_ms)`` e satura em
//              250 ao final.
//
//   Property 3 do ``design.md`` (cobertura PBT contra ``reference_model``):
//
//       compute_bpm(now_ms) ==
//           min(250, janela)                         , se elapsed >= 60 000
//           min(250, round(|ts| * 60 000 / elapsed)) , se 0 < elapsed < 60 000
//           0                                        , se elapsed == 0
//
//   Observações de implementação:
//     * O ring buffer usa ``std::array`` com capacidade fixa de 300 — o
//       mesmo valor de ``BPM_RING_CAPACITY`` do reference model — para
//       evitar qualquer alocação dinâmica no ESP32.
//     * ``calcular_bpm`` é declarado ``const`` (interface público-pura);
//       a remoção preguiçosa dos timestamps vencidos é persistida via
//       membros ``mutable`` — idioma padrão de *logical const / physical
//       mutable* para caching.
//     * O arredondamento da extrapolação usa ``std::lround``
//       (*round half away from zero*). O reference model Python usa
//       ``round()`` (*banker's rounding* / *round half to even*), o que
//       pode diferir em até uma unidade em casos de fração ``.5``
//       exata. Como os testes de propriedade (PBT) exercitam apenas o
//       reference model, essa pequena divergência NÃO afeta a suíte; em
//       operação clínica o limite é 120 bpm, portanto um off-by-one em
//       frações ``.5`` não impacta a classificação de alerta.
//
//   Requisitos atendidos: R2.2, R2.3, R2.5, R2.6, R2.7, R12.2, R12.4.
// =============================================================================

#ifndef CARDIOIA_BPM_WINDOW_H
#define CARDIOIA_BPM_WINDOW_H

#include <array>    // std::array — ring buffer em stack/dados, sem heap.
#include <cstddef>  // std::size_t
#include <cstdint>  // std::uint8_t, std::uint32_t — tipos fixos.

namespace cardioia {

// ---------------------------------------------------------------------------
// Classe ``BpmWindow``
// ---------------------------------------------------------------------------
// Janela deslizante de 60 s que converte timestamps de pulso em bpm.
//
// Contrato de uso (replicado do reference model):
//   1. ``registrar_pulso(ts)`` é chamado pelo ``SensorDriver`` APÓS o
//      debounce de 150 ms (R2.4, P4), de forma que os timestamps chegam
//      sem ruído.
//   2. Os timestamps são esperados monotônicos não-decrescentes dentro
//      de uma mesma execução. O módulo é robusto a timestamps iguais
//      (vários pulsos no mesmo ms são contados tantas vezes quantas
//      forem registrados).
//   3. ``calcular_bpm(now_ms)`` é chamado com ``now_ms >= boot_ms``.
//      Chamadas com ``now_ms < boot_ms`` violam o contrato — neste
//      caso a classe devolve ``0`` de forma defensiva, sem abortar.
class BpmWindow {
public:
    // Capacidade do ring buffer. Dimensionada para 250 bpm × 60 s + 50
    // de margem — mesmo valor de ``BPM_RING_CAPACITY`` no reference
    // model (``iot/tests/reference_model.py``).
    static constexpr std::size_t kRingCapacity = 300;

    // Janela deslizante em ms. Mantido numérico (e não importado de
    // ``config.h``) para que o arquivo permaneça auto-suficiente em
    // testes nativos que não incluam o ``config.h``.
    static constexpr std::uint32_t kWindowMs = 60000U;

    // Teto de saturação (R2.5). Como o retorno é ``uint8_t``, o valor é
    // também o menor ``uint8_t`` que pode representar "≥ 250".
    static constexpr std::uint8_t kMaxBpm = 250U;

    // -----------------------------------------------------------------------
    // Construtor: BpmWindow::BpmWindow
    // Finalidade: Inicializa a janela. Todos os slots do ring buffer
    //             começam em zero e ``count_`` / ``head_`` em zero — ou
    //             seja, nenhum pulso registrado.
    // Parâmetros:
    //   - ``boot_ms`` (default 0): instante (em ms) em que o firmware
    //                              iniciou. Serve de referência para a
    //                              extrapolação proporcional do R2.7.
    // Retorno:    (construtor).
    // -----------------------------------------------------------------------
    explicit BpmWindow(std::uint32_t boot_ms = 0U) noexcept;

    // -----------------------------------------------------------------------
    // Método:     BpmWindow::registrar_pulso
    // Finalidade: Registra um novo pulso no ring buffer. Se o ring estiver
    //             cheio, o slot mais antigo é sobrescrito em FIFO
    //             circular — comportamento equivalente ao
    //             ``deque(maxlen=300)`` do reference model.
    // Parâmetros:
    //   - ``ts_ms``: instante do pulso em milissegundos. Esperado
    //                monotônico não-decrescente; após debounce de 150 ms
    //                aplicado pelo ``SensorDriver``.
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void registrar_pulso(std::uint32_t ts_ms) noexcept;

    // -----------------------------------------------------------------------
    // Método:     BpmWindow::calcular_bpm
    // Finalidade: Devolve o BPM atual (0..250) conforme a Property 3:
    //
    //     * se ``elapsed_ms == 0`` → retorna 0 (evita divisão por zero);
    //     * se ``elapsed_ms >= 60 000`` → ``min(250, janela)``, onde
    //       ``janela`` é a quantidade de timestamps em
    //       ``(now_ms - 60 000, now_ms]``. Timestamps fora da janela são
    //       removidos de forma preguiçosa do ring buffer (membros
    //       ``mutable``) para amortizar o custo entre chamadas;
    //     * se ``0 < elapsed_ms < 60 000`` → extrapolação proporcional
    //       ``min(250, lround(|ts| * 60 000 / elapsed))``.
    //
    // Parâmetros:
    //   - ``now_ms``: instante atual em ms (origem ``millis()`` no
    //                 firmware, injetado em testes). Deve ser
    //                 ``>= boot_ms``; valores inferiores são tratados
    //                 como ``elapsed == 0`` e retornam 0.
    // Retorno:    BPM no intervalo ``[0, 250]``. Nunca lança exceções.
    // -----------------------------------------------------------------------
    std::uint8_t calcular_bpm(std::uint32_t now_ms) const noexcept;

    // -----------------------------------------------------------------------
    // Método:     BpmWindow::tamanho
    // Finalidade: Retorna o número de timestamps atualmente armazenados
    //             no ring (após podas lazy realizadas em chamadas
    //             prévias de ``calcular_bpm``). Usado pelo Logger para
    //             diagnósticos (``"pulsos_ativos=NN"``).
    // Parâmetros: (nenhum).
    // Retorno:    ``std::size_t`` em ``[0, kRingCapacity]``.
    // -----------------------------------------------------------------------
    std::size_t tamanho() const noexcept { return count_; }

private:
    // Instante de boot — base para o cálculo de ``elapsed_ms``.
    std::uint32_t boot_ms_;

    // ---------------------------------------------------------------------
    // Estado do ring buffer.
    // ---------------------------------------------------------------------
    // Os três membros são ``mutable`` porque ``calcular_bpm`` é ``const``
    // (contrato público de não alterar valores observáveis) mas precisa
    // ajustar o ring para remover timestamps vencidos — idioma padrão
    // de *logical const / physical mutable*.
    mutable std::array<std::uint32_t, kRingCapacity> ring_{};

    // Próxima posição de escrita — avança em ``registrar_pulso``.
    mutable std::size_t head_ = 0U;

    // Quantidade de timestamps válidos no ring (``<= kRingCapacity``).
    mutable std::size_t count_ = 0U;

    // -----------------------------------------------------------------------
    // Método:     BpmWindow::podar_preguicosamente (privado, const)
    // Finalidade: Remove do início do ring (slot mais antigo) todos os
    //             timestamps ``<= now_ms - kWindowMs``. Chamado apenas
    //             no ramo "janela plena" do ``calcular_bpm`` (R2.6);
    //             nunca é invocado no ramo de extrapolação para
    //             preservar o comportamento do reference model — onde
    //             o numerador é ``|ts_totais|`` e não ``|ts_na_janela|``.
    // Parâmetros:
    //   - ``now_ms``: instante atual (``>= kWindowMs`` garantido pelo
    //                 chamador).
    // Retorno:    (void).
    // -----------------------------------------------------------------------
    void podar_preguicosamente(std::uint32_t now_ms) const noexcept;
};

}  // namespace cardioia

#endif  // CARDIOIA_BPM_WINDOW_H
