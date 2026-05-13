"""
Infraestrutura de testes do módulo CardioIA – Monitoramento IoT (Fase 3).

Este ``conftest.py`` concentra:

* O ajuste de ``sys.path`` para que os testes de propriedade consigam importar
  ``reference_model`` diretamente a partir de ``iot/tests/`` (o *reference
  model* Python é criado na Tarefa 5 do plano de implementação e espelha a
  lógica pura do firmware embarcado).
* O registro dos dois perfis de execução do `hypothesis` definidos no design:

  - ``cardioia-fast``     → perfil padrão, com ``max_examples=200`` e
    ``deadline=500 ms``. Aplicável à maioria das propriedades (P3–P12, P14–P20).
  - ``cardioia-thorough`` → perfil intensivo, com ``max_examples=500`` e
    ``deadline=1500 ms``. Obrigatório para as propriedades P1, P2 e P13
    conforme a seção "Testing Strategy" do ``design.md``.

* Duas ``fixtures`` (``settings_fast`` e ``settings_thorough``) que retornam o
  respectivo objeto ``hypothesis.settings`` para os raros testes que preferem
  consumir a configuração via parâmetro em vez do decorador
  ``@settings(...)``. Tanto a via decorador quanto a via fixture são
  suportadas; o decorador é a forma idiomática e recomendada.

Convenção obrigatória para TODOS os testes de propriedade deste módulo:

    O docstring de cada propriedade DEVE iniciar com a tag de feature
    ``"Feature: cardioia-monitoramento-iot, Property N: <texto da propriedade>"``
    (substituindo ``N`` pelo número canônico da propriedade, de 1 a 20, e
    preservando o texto resumido conforme o ``design.md``). Essa tag é
    obrigatória para rastreabilidade entre propriedades, requisitos EARS
    (``requirements.md``) e documentação de arquitetura (``design.md``).

Exemplo mínimo de uso em um teste::

    # iot/tests/test_sample_json_pbt.py
    from hypothesis import given, settings, strategies as st
    from reference_model import serialize_sample, deserialize_sample

    @settings(settings.get_profile("cardioia-thorough"))
    @given(...)
    def test_sample_roundtrip(...):
        \"\"\"Feature: cardioia-monitoramento-iot, Property 1: round-trip do Sample_Record.\"\"\"
        ...
"""

from __future__ import annotations

import sys
from datetime import timedelta
from pathlib import Path

import pytest
from hypothesis import HealthCheck, settings

# -----------------------------------------------------------------------------
# Ajuste de ``sys.path``
# -----------------------------------------------------------------------------
# Os testes importam ``reference_model`` diretamente (ex.: ``from
# reference_model import EdgeBuffer``). Inserimos o diretório ``iot/tests/``
# no início de ``sys.path`` para que a importação funcione independentemente
# do diretório a partir do qual o ``pytest`` é invocado (raiz do repositório,
# diretório ``iot/`` ou o próprio ``iot/tests/``).
_TESTS_DIR = Path(__file__).resolve().parent
if str(_TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(_TESTS_DIR))


# -----------------------------------------------------------------------------
# Perfis do ``hypothesis``
# -----------------------------------------------------------------------------
# Perfil padrão — aplicado à maioria das propriedades do módulo IoT.
# ``max_examples=200`` garante cobertura robusta mantendo o tempo total da
# suíte em segundos; ``deadline=500 ms`` protege contra propriedades que
# regridem em desempenho.
settings.register_profile(
    "cardioia-fast",
    max_examples=200,
    deadline=timedelta(milliseconds=500),
    # ``data_too_large`` é seguro desativar aqui porque nossos geradores já
    # limitam explicitamente tamanhos (ex.: listas de até 1000 operações).
    suppress_health_check=[HealthCheck.too_slow],
)

# Perfil intensivo — obrigatório para P1 (round-trip do Sample_Record),
# P2 (FIFO do EdgeBuffer) e P13 (conservação do SyncScheduler), que exercitam
# sequências longas de operações e exigem maior cobertura para detectar
# contraexemplos sutis de ordenação e de fronteira do buffer.
settings.register_profile(
    "cardioia-thorough",
    max_examples=500,
    deadline=timedelta(milliseconds=1500),
    suppress_health_check=[HealthCheck.too_slow],
)

# Carrega o perfil padrão para toda a suíte. Testes específicos (P1, P2, P13)
# podem aplicar ``@settings(settings.get_profile("cardioia-thorough"))`` ou
# consumir a fixture ``settings_thorough``.
settings.load_profile("cardioia-fast")


# -----------------------------------------------------------------------------
# Fixtures de conveniência
# -----------------------------------------------------------------------------
@pytest.fixture(scope="session")
def settings_fast() -> settings:
    """Retorna o objeto ``hypothesis.settings`` do perfil ``cardioia-fast``.

    Uso opcional: a forma idiomática é o decorador
    ``@settings(settings.get_profile("cardioia-fast"))`` diretamente sobre o
    teste. Esta fixture existe para cenários em que a configuração precisa
    ser composta dinamicamente.
    """
    return settings.get_profile("cardioia-fast")


@pytest.fixture(scope="session")
def settings_thorough() -> settings:
    """Retorna o objeto ``hypothesis.settings`` do perfil ``cardioia-thorough``.

    Deve ser utilizado (via fixture ou via ``@settings(...)``) pelos testes
    que validam as propriedades P1, P2 e P13, conforme a seção
    "Testing Strategy" do ``design.md``.
    """
    return settings.get_profile("cardioia-thorough")
