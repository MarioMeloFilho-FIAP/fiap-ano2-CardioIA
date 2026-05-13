// =============================================================================
// Projeto:   CardioIA – Monitoramento IoT (Fase 3)
// Arquivo:   iot/firmware/src/config_manager.cpp
// Finalidade:
//   Implementação do ``cardioia::ConfigManager`` declarado em
//   ``config_manager.h``. Cobre as Properties P19 (validação /
//   aplicação / rejeição) e P20 (persistência entre reboots) e mantém
//   paridade bit-a-bit com o reference model Python
//   (``iot/tests/reference_model.py``).
//
//   Decisões de implementação:
//     * Parser JSON dual: em builds Arduino (``#ifdef ARDUINO``) usamos
//       ``ArduinoJson``; em builds nativos (Unity/PBT) usamos um parser
//       minimalista embutido que reconhece exatamente os quatro campos
//       esperados. O parser nativo é intencionalmente simples — qualquer
//       desvio (campo desconhecido, tipo errado, fora da faixa) dispara
//       rejeição com motivo pt-BR alinhado ao reference model.
//     * Persistência dual: em ``ARDUINO`` gravamos em SPIFFS no caminho
//       ``/cardioia_config.json``; em nativo, em
//       ``/tmp/cardioia_config.json`` (substituível via
//       ``setCaminhoPersistenciaNativo`` para testes determinísticos).
//     * Comentários pt-BR acima de **cada função** (R12.2) e acima de
//       **cada ``if``/``switch`` de validação** (R12.4).
// =============================================================================

#include "config_manager.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef ARDUINO
#  include <Arduino.h>
#  include <SPIFFS.h>
#  include <ArduinoJson.h>
#endif

namespace cardioia {

// ---------------------------------------------------------------------------
// Defaults compartilhados com ``config.h`` (espelho de DEFAULT_CONFIG).
// ---------------------------------------------------------------------------
// Comentário acima da função: construímos o default uma única vez aqui
// para que os testes consigam comparar contra o valor canônico sem
// depender de constexpr — a cópia local permite usar em contextos
// ``std::optional``.

// -----------------------------------------------------------------------------
// Função:     defaultConfigState
// Finalidade: Constrói o ``ConfigState`` inicial, refletindo os defaults
//             declarados como ``constexpr`` em ``config.h``. Usado no
//             construtor quando o chamador não fornece ``initial``.
// Parâmetros: (nenhum).
// Retorno:    ``ConfigState`` com os quatro campos preenchidos pelos
//             defaults canônicos (R16.1).
// -----------------------------------------------------------------------------
static ConfigState defaultConfigState() noexcept {
    ConfigState s;
    s.bpm_threshold = BPM_THRESHOLD;
    s.temp_threshold = TEMP_THRESHOLD;
    s.buffer_limit = BUFFER_LIMIT;
    s.sampling_interval_ms = SAMPLING_INTERVAL_MS;
    return s;
}

// ---------------------------------------------------------------------------
// Operadores de igualdade do ConfigState
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     operator== (ConfigState)
// Finalidade: Compara dois ``ConfigState`` campo-a-campo para que testes
//             nativos e o caminho de publicação ``config/aplicado``
//             consigam detectar mudanças reais e evitar republicação
//             redundante. Para ``temp_threshold`` usamos comparação
//             exata (``==`` de float) porque o parser normaliza o valor
//             sempre para o exato mesmo caminho de conversão.
// Parâmetros:
//   - ``a``, ``b``: estados a comparar.
// Retorno:    ``true`` se todos os campos coincidem; ``false`` caso
//             contrário.
// -----------------------------------------------------------------------------
bool operator==(const ConfigState& a, const ConfigState& b) noexcept {
    return a.bpm_threshold == b.bpm_threshold
        && a.temp_threshold == b.temp_threshold
        && a.buffer_limit == b.buffer_limit
        && a.sampling_interval_ms == b.sampling_interval_ms;
}

// -----------------------------------------------------------------------------
// Função:     operator!= (ConfigState)
// Finalidade: Negação de ``operator==``. Existe como atalho para clareza
//             em pontos do código onde a comparação lógica é "mudou".
// Parâmetros: idem ``operator==``.
// Retorno:    ``!(a == b)``.
// -----------------------------------------------------------------------------
bool operator!=(const ConfigState& a, const ConfigState& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
// Validação de paciente_id (espelho da Property 18 / R15.2)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     is_anonymized_paciente_id
// Finalidade: Verifica se ``s`` casa com o padrão ``^PAC-\d{1,27}$``,
//             rejeitando qualquer PII. Implementação manual (sem
//             ``<regex>``) para manter a ABI compatível com a toolchain
//             Xtensa do ESP32, onde ``std::regex`` é notadamente pesada
//             em flash/RAM.
// Parâmetros:
//   - ``s``: identificador a validar.
// Retorno:    ``true`` se anonimizado; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool is_anonymized_paciente_id(const std::string& s) noexcept {
    // Comentário acima do bloco de validação (R12.4): o prefixo ``PAC-``
    // é a âncora canônica — qualquer desvio reprova imediatamente.
    if (s.size() < 5 || s.size() > 31) {
        // Formato mínimo: ``PAC-`` + 1 dígito → 5 chars. Máximo: 4 + 27 = 31.
        return false;
    }
    if (s[0] != 'P' || s[1] != 'A' || s[2] != 'C' || s[3] != '-') {
        // Prefixo ausente → reprova (R15.2).
        return false;
    }
    // Comentário acima do loop de validação (R12.4): cada caractere após
    // o prefixo precisa ser um dígito ASCII ``0..9``. Qualquer outro
    // caractere configura PII potencial ou identificador inválido.
    for (std::size_t i = 4; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parser JSON minimalista para o payload de config (build nativo)
// ---------------------------------------------------------------------------
// Não é um parser JSON completo — reconhece apenas objetos planos cujos
// valores sejam números, ``true``/``false``, ``null`` ou strings. É
// suficiente para o formato esperado (``{"bpm_threshold": 120, ...}``)
// e aceita espaços/TABs/newlines entre tokens.

namespace {  // helpers internos — não parte da API pública

// Resultado de leitura de um único par chave/valor.
struct KVToken {
    std::string key;
    std::string raw_value;  // literal inteiro: "120", "38.0", "true", ...
    // Tipo do valor para permitir ao validador distinguir bool/null/etc.:
    enum Kind { INT, FLOAT, BOOL, NUL, STRING, UNKNOWN } kind = UNKNOWN;
};

// -----------------------------------------------------------------------------
// Função:     pularBrancos (helper interno)
// Finalidade: Avança ``pos`` enquanto houver whitespace (espaço, tab,
//             newline, CR) em ``s``. Usado entre cada token do parser
//             JSON nativo.
// Parâmetros:
//   - ``s``:   string de entrada.
//   - ``pos``: índice atual (modificado in-place).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void pularBrancos(const std::string& s, std::size_t& pos) {
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++pos;
        } else {
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// Função:     lerStringJson (helper interno)
// Finalidade: Lê uma string JSON delimitada por ``"`` (já considera-se
//             que ``pos`` aponta para o ``"`` inicial). Suporta escapes
//             básicos (``\"``, ``\\``, ``\n``, ``\t``) que raramente
//             ocorrem em chaves de config; o suficiente para robustez
//             contra geradores que produzam nomes escapados.
// Parâmetros:
//   - ``s``:     string de entrada.
//   - ``pos``:   índice do primeiro ``"`` (modificado para apontar após
//                o ``"`` de fechamento em caso de sucesso).
//   - ``out``:   string decodificada.
// Retorno:    ``true`` se conseguiu ler a string até o ``"`` de
//             fechamento; ``false`` se o JSON estiver malformado.
// -----------------------------------------------------------------------------
bool lerStringJson(const std::string& s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    ++pos;  // consome o ``"`` de abertura
    out.clear();
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == '"') {
            ++pos;
            return true;
        }
        if (c == '\\' && (pos + 1) < s.size()) {
            const char next = s[pos + 1];
            switch (next) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                default:
                    // Escape desconhecido → falha (payload inválido).
                    return false;
            }
            pos += 2;
            continue;
        }
        out.push_back(c);
        ++pos;
    }
    return false;  // ``"`` de fechamento não encontrado
}

// -----------------------------------------------------------------------------
// Função:     lerValorJson (helper interno)
// Finalidade: Lê o próximo token de valor (número, bool, null ou string)
//             a partir de ``pos``. Preenche ``tok.raw_value`` e
//             ``tok.kind`` conforme o tipo detectado.
// Parâmetros:
//   - ``s``:   string de entrada.
//   - ``pos``: índice atual (modificado in-place).
//   - ``tok``: token onde ``raw_value`` e ``kind`` serão preenchidos.
// Retorno:    ``true`` se conseguiu extrair um token reconhecido;
//             ``false`` em caso de EOF ou caractere inválido.
// -----------------------------------------------------------------------------
bool lerValorJson(const std::string& s, std::size_t& pos, KVToken& tok) {
    pularBrancos(s, pos);
    if (pos >= s.size()) {
        return false;
    }
    const char c = s[pos];

    // Comentário acima do switch de decisão por tipo de valor (R12.4):
    // cada ramo reconhece um dos tipos JSON que o formato de config
    // aceita; qualquer outro caractere configura payload malformado.
    switch (c) {
        case '"': {
            std::string value;
            if (!lerStringJson(s, pos, value)) {
                return false;
            }
            tok.raw_value = value;
            tok.kind = KVToken::STRING;
            return true;
        }
        case 't': {
            if (s.compare(pos, 4, "true") == 0) {
                tok.raw_value = "true";
                tok.kind = KVToken::BOOL;
                pos += 4;
                return true;
            }
            return false;
        }
        case 'f': {
            if (s.compare(pos, 5, "false") == 0) {
                tok.raw_value = "false";
                tok.kind = KVToken::BOOL;
                pos += 5;
                return true;
            }
            return false;
        }
        case 'n': {
            if (s.compare(pos, 4, "null") == 0) {
                tok.raw_value = "null";
                tok.kind = KVToken::NUL;
                pos += 4;
                return true;
            }
            return false;
        }
        default: break;
    }

    // Caso contrário, tenta número (int ou float, com sinal).
    // Comentário acima do bloco de leitura numérica (R12.4): aceitamos
    // dígitos, um sinal inicial e um ponto decimal. Expoente (``e``/``E``)
    // é rejeitado para alinhar com o reference model, que só aceita a
    // forma canônica ``[-]?dígitos(\.dígitos)?`` nos campos clínicos.
    const std::size_t inicio = pos;
    bool tem_ponto = false;
    bool tem_digito = false;
    if (s[pos] == '+' || s[pos] == '-') {
        ++pos;
    }
    while (pos < s.size()) {
        const char d = s[pos];
        if (d >= '0' && d <= '9') {
            tem_digito = true;
            ++pos;
        } else if (d == '.' && !tem_ponto) {
            tem_ponto = true;
            ++pos;
        } else {
            break;
        }
    }
    if (!tem_digito) {
        return false;
    }
    tok.raw_value.assign(s, inicio, pos - inicio);
    tok.kind = tem_ponto ? KVToken::FLOAT : KVToken::INT;
    return true;
}

// -----------------------------------------------------------------------------
// Função:     parseObjetoPlano (helper interno)
// Finalidade: Parseia um objeto JSON plano ``{"k1": v1, "k2": v2, ...}``
//             preenchendo o vetor ``out`` com todos os pares encontrados.
//             Não valida as chaves nem as faixas — isso é trabalho do
//             ``apply_config_json`` mais abaixo.
// Parâmetros:
//   - ``s``:   string completa do payload.
//   - ``out``: vetor de tokens resultante.
// Retorno:    ``true`` se o parse terminou consumindo o ``}`` final;
//             ``false`` em qualquer caso de payload malformado.
// -----------------------------------------------------------------------------
bool parseObjetoPlano(const std::string& s, std::vector<KVToken>& out) {
    std::size_t pos = 0;
    pularBrancos(s, pos);
    if (pos >= s.size() || s[pos] != '{') {
        return false;
    }
    ++pos;
    pularBrancos(s, pos);
    if (pos < s.size() && s[pos] == '}') {
        // Objeto vazio — parse válido, mas o chamador decide rejeitar.
        ++pos;
        return true;
    }

    while (pos < s.size()) {
        pularBrancos(s, pos);
        KVToken tok;
        if (!lerStringJson(s, pos, tok.key)) {
            return false;
        }
        pularBrancos(s, pos);
        if (pos >= s.size() || s[pos] != ':') {
            return false;
        }
        ++pos;  // consome ``:``
        if (!lerValorJson(s, pos, tok)) {
            return false;
        }
        out.push_back(std::move(tok));
        pularBrancos(s, pos);
        if (pos < s.size() && s[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            // Verifica ausência de lixo após o ``}``.
            pularBrancos(s, pos);
            return pos == s.size();
        }
        return false;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Função:     parseLongCuidadoso (helper interno)
// Finalidade: Converte ``raw`` (assumido como sequência de dígitos com
//             possível sinal) em ``long long`` sem aceitar lixo após o
//             número. ``strtoll`` é suficiente aqui porque o parser
//             acima já garantiu o formato.
// Parâmetros:
//   - ``raw``: string numérica.
//   - ``out``: valor convertido.
// Retorno:    ``true`` em sucesso, ``false`` em overflow/underflow.
// -----------------------------------------------------------------------------
bool parseLongCuidadoso(const std::string& raw, long long& out) {
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(raw.c_str(), &end, 10);
    if (errno != 0 || end == raw.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

// -----------------------------------------------------------------------------
// Função:     parseDoubleCuidadoso (helper interno)
// Finalidade: Converte ``raw`` em ``double`` usando ``strtod``. Rejeita
//             NaN, Inf e strings com lixo residual. Aceita tanto inteiros
//             quanto floats — o validador mais adiante aceita ambos em
//             ``temp_threshold`` (R16.1 / reference model).
// Parâmetros:
//   - ``raw``: string numérica.
//   - ``out``: valor convertido.
// Retorno:    ``true`` em sucesso; ``false`` em conversão inválida.
// -----------------------------------------------------------------------------
bool parseDoubleCuidadoso(const std::string& raw, double& out) {
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(raw.c_str(), &end);
    if (errno != 0 || end == raw.c_str() || *end != '\0') {
        return false;
    }
    if (std::isnan(v) || std::isinf(v)) {
        return false;
    }
    out = v;
    return true;
}

// Chaves canônicas aceitas e suas faixas. A ordem é a mesma usada na
// serialização ``serializarEstado`` para determinismo.
struct FieldSpec {
    const char* key;
    enum Kind { INT, FLOAT } kind;
    double lo;
    double hi;
};

constexpr FieldSpec kFieldSpecs[] = {
    {"bpm_threshold",        FieldSpec::INT,   40.0,    220.0   },
    {"temp_threshold",       FieldSpec::FLOAT, 35.0,    42.0    },
    {"buffer_limit",         FieldSpec::INT,   10.0,    500.0   },
    {"sampling_interval_ms", FieldSpec::INT,   1000.0,  60000.0 },
};

// -----------------------------------------------------------------------------
// Função:     encontrarSpec (helper interno)
// Finalidade: Busca linearmente a especificação (faixa/tipo) para uma
//             chave recebida. A busca linear sobre 4 elementos é
//             O(1) na prática e evita a necessidade de um ``std::map``.
// Parâmetros:
//   - ``key``: nome do campo lido no payload.
// Retorno:    ponteiro para a ``FieldSpec`` correspondente ou
//             ``nullptr`` se a chave não for canônica.
// -----------------------------------------------------------------------------
const FieldSpec* encontrarSpec(const std::string& key) {
    for (const auto& spec : kFieldSpecs) {
        if (key == spec.key) {
            return &spec;
        }
    }
    return nullptr;
}

}  // namespace (anônimo)

// ---------------------------------------------------------------------------
// apply_config_json — função pura (espelho de reference_model.apply_config)
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Função:     apply_config_json
// Finalidade: Recebe ``current`` + ``payload_json``, valida todos os
//             campos contra as faixas canônicas e, em sucesso, devolve
//             um ``ConfigState`` com o merge aplicado. Em falha
//             preserva ``current`` e preenche ``reason`` com motivo
//             pt-BR (R16.1/R16.3, Property 19).
// Parâmetros: descritos em ``config_manager.h``.
// Retorno:    ``ConfigApplyResult``.
// -----------------------------------------------------------------------------
ConfigApplyResult apply_config_json(const ConfigState& current,
                                    const std::string& payload_json) {
    ConfigApplyResult result;
    result.state = current;
    result.ok = false;

    // Comentário acima do if que rejeita payload vazio (R12.4): string
    // vazia é tratada explicitamente como payload malformado, alinhada
    // com o reference model (que rejeita ``{}`` como "payload vazio").
    if (payload_json.empty()) {
        result.reason = "payload vazio";
        return result;
    }

    std::vector<KVToken> tokens;
    if (!parseObjetoPlano(payload_json, tokens)) {
        result.reason = "payload nao e JSON valido";
        return result;
    }

    // Comentário acima do if de objeto vazio (R12.4): um objeto JSON
    // sintaticamente válido mas sem campos não tem nada para aplicar,
    // portanto é rejeitado conservadoramente (P19).
    if (tokens.empty()) {
        result.reason = "payload vazio";
        return result;
    }

    ConfigState proposed = current;

    // Itera em ordem de chegada — a validação falha já no primeiro erro
    // encontrado, preservando ``current`` inteiro (P19).
    for (const auto& tok : tokens) {
        const FieldSpec* spec = encontrarSpec(tok.key);
        // Comentário acima do if de chave desconhecida (R12.4): qualquer
        // chave fora do conjunto canônico é tratada como payload
        // malformado para evitar vazamento semântico — espelho exato do
        // reference model (``campo desconhecido: '...'``).
        if (spec == nullptr) {
            result.reason = "campo desconhecido: '" + tok.key + "'";
            result.state = current;
            return result;
        }

        // Comentário acima do if que rejeita bool como numérico (R12.4):
        // o reference model distingue bool de int — ``True``/``False``
        // nunca são aceitos em campos clínicos, mesmo que o parser JSON
        // permita representá-los.
        if (tok.kind == KVToken::BOOL) {
            result.reason = std::string(spec->key) + " deve ser "
                          + (spec->kind == FieldSpec::INT ? "int" : "float")
                          + ", recebido bool";
            result.state = current;
            return result;
        }

        // Comentário acima do if que rejeita null/string (R12.4): campos
        // clínicos precisam ser numéricos; ``null`` ou strings geram
        // rejeição imediata antes mesmo da verificação de faixa.
        if (tok.kind == KVToken::NUL || tok.kind == KVToken::STRING) {
            result.reason = std::string(spec->key)
                          + (spec->kind == FieldSpec::INT ? " deve ser inteiro"
                                                          : " deve ser numerico");
            result.state = current;
            return result;
        }

        // Comentário acima do switch de tipo esperado (R12.4): bifurcamos
        // o processamento entre campos inteiros estritos e campos
        // numéricos (que aceitam int ou float dentro da faixa).
        switch (spec->kind) {
            case FieldSpec::INT: {
                // Comentário acima do if que rejeita float em campo int
                // (R12.4): campos inteiros devem ser literalmente inteiros
                // no JSON — ``38.0`` em ``bpm_threshold`` é rejeitado.
                if (tok.kind != KVToken::INT) {
                    result.reason =
                        std::string(spec->key) + " deve ser inteiro";
                    result.state = current;
                    return result;
                }
                long long v = 0;
                if (!parseLongCuidadoso(tok.raw_value, v)) {
                    result.reason =
                        std::string(spec->key) + " deve ser inteiro";
                    result.state = current;
                    return result;
                }
                // Comentário acima do if de faixa int (R12.4): cada campo
                // clínico tem seu intervalo fechado; qualquer valor fora
                // desse intervalo é rejeitado conforme R16.1.
                if (static_cast<double>(v) < spec->lo
                    || static_cast<double>(v) > spec->hi) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "%s fora da faixa [%d, %d]",
                                  spec->key,
                                  static_cast<int>(spec->lo),
                                  static_cast<int>(spec->hi));
                    result.reason = buf;
                    result.state = current;
                    return result;
                }
                // Escreve o valor normalizado no ``proposed``.
                if (std::strcmp(spec->key, "bpm_threshold") == 0) {
                    proposed.bpm_threshold = static_cast<int>(v);
                } else if (std::strcmp(spec->key, "buffer_limit") == 0) {
                    proposed.buffer_limit = static_cast<int>(v);
                } else if (std::strcmp(spec->key, "sampling_interval_ms") == 0) {
                    proposed.sampling_interval_ms = static_cast<int>(v);
                }
                break;
            }
            case FieldSpec::FLOAT: {
                double v = 0.0;
                if (!parseDoubleCuidadoso(tok.raw_value, v)) {
                    result.reason =
                        std::string(spec->key) + " deve ser numerico";
                    result.state = current;
                    return result;
                }
                // Comentário acima do if de faixa float (R12.4): mesma
                // regra de R16.1 — intervalo fechado; NaN/Inf já foram
                // filtrados por ``parseDoubleCuidadoso``.
                if (v < spec->lo || v > spec->hi) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                                  "%s fora da faixa [%.1f, %.1f]",
                                  spec->key, spec->lo, spec->hi);
                    result.reason = buf;
                    result.state = current;
                    return result;
                }
                if (std::strcmp(spec->key, "temp_threshold") == 0) {
                    proposed.temp_threshold = static_cast<float>(v);
                }
                break;
            }
        }
    }

    result.ok = true;
    result.state = proposed;
    result.reason.reset();
    return result;
}

// ---------------------------------------------------------------------------
// ConfigManager — implementação
// ---------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Construtor: ConfigManager::ConfigManager
// Finalidade: Inicializa ``state_``/``last_valid_`` com ``initial`` (se
//             fornecido) ou com os defaults. O storage só é tocado em
//             ``iniciar()`` para manter o construtor livre de I/O.
// Parâmetros: descritos em ``config_manager.h``.
// Retorno:    (construtor).
// -----------------------------------------------------------------------------
ConfigManager::ConfigManager(Logger* logger,
                             std::string paciente_id,
                             std::optional<ConfigState> initial)
    : logger_(logger),
      paciente_id_(std::move(paciente_id)),
      state_(initial.has_value() ? initial.value() : defaultConfigState()),
      last_valid_(state_),
      caminho_nativo_("/tmp/cardioia_config.json") {}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::iniciar
// Finalidade: Tenta carregar a última config do storage. Em falha
//             (storage vazio/corrompido) mantém os defaults atuais e
//             persiste um snapshot para que o próximo reboot já encontre
//             algo válido — espelhando o comportamento do reference
//             model (que persiste no construtor).
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConfigManager::iniciar() {
    // Comentário acima do if de carregamento (R12.4): se o storage tiver
    // um snapshot válido, usamo-lo como estado inicial. Caso contrário,
    // permanecemos com o default e gravamos um snapshot para consistência.
    if (!carregar()) {
        persistir();
        if (logger_ != nullptr) {
            logger_->info("ConfigManager",
                          "storage vazio ou invalido, usando defaults");
        }
    } else if (logger_ != nullptr) {
        logger_->info("ConfigManager", "config carregada do storage");
    }
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::aplicar
// Finalidade: Implementa o fluxo completo de P19/P20 — delega a
//             validação para ``apply_config_json`` e, em sucesso,
//             atualiza state + last_valid + persistência. Em rejeição
//             acumula o motivo em ``rejection_events_`` para que o
//             chamador publique em ``.../rejeicao``.
// Parâmetros: descritos em ``config_manager.h``.
// Retorno:    par ``(aplicou, motivo_opcional)``.
// -----------------------------------------------------------------------------
std::pair<bool, std::optional<std::string>>
ConfigManager::aplicar(const std::string& payload_json) {
    const ConfigApplyResult res = apply_config_json(state_, payload_json);

    // Comentário acima do if principal de decisão (R12.4): se o resultado
    // falhou a validação R16.1, registramos o motivo para publicação em
    // ``.../rejeicao`` (R16.3) e deixamos ``state_``/``last_valid_``
    // intactos (P20).
    if (!res.ok) {
        const std::string reason = res.reason.value_or("payload invalido");
        rejection_events_.push_back(reason);
        if (logger_ != nullptr) {
            logger_->warn("ConfigManager", ("rejeicao: " + reason).c_str());
        }
        return {false, reason};
    }

    state_ = res.state;
    last_valid_ = res.state;
    persistir();
    if (logger_ != nullptr) {
        logger_->info("ConfigManager", "config aplicada com sucesso");
    }
    return {true, std::nullopt};
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::reboot
// Finalidade: Simula um reboot: reseta o estado em memória e delega a
//             ``carregar()`` para recuperar a última config válida
//             (R16.2, P20). Se o storage não contiver dados, volta aos
//             defaults (comportamento idêntico ao reference model).
// Parâmetros: (nenhum).
// Retorno:    ``ConfigState`` carregado.
// -----------------------------------------------------------------------------
ConfigState ConfigManager::reboot() {
    state_ = defaultConfigState();
    last_valid_ = state_;
    if (!carregar()) {
        // Comentário acima do if de recuperação-vazia (R12.4): sem
        // snapshot, mantemos os defaults e persistimos para que o
        // próximo ``reboot()`` encontre algo — invariante do reference
        // model que facilita testes determinísticos.
        persistir();
    }
    return state_;
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::topicoAplicado
// Finalidade: Monta o tópico ``cardioia/paciente/{paciente_id}/config/aplicado``
//             se ``paciente_id_`` for anonimizado (R15.2); caso
//             contrário devolve string vazia para que o chamador
//             suprima a publicação e preserve a LGPD.
// Parâmetros: (nenhum).
// Retorno:    tópico MQTT ou ``""``.
// -----------------------------------------------------------------------------
std::string ConfigManager::topicoAplicado() const {
    // Comentário acima do if de guarda LGPD (R12.4): mesmo que o
    // chamador esqueça de validar, garantimos que nenhum paciente_id
    // com PII seja incorporado a tópicos MQTT.
    if (!is_anonymized_paciente_id(paciente_id_)) {
        return "";
    }
    return "cardioia/paciente/" + paciente_id_ + "/config/aplicado";
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::topicoRejeicao
// Finalidade: Monta o tópico ``cardioia/paciente/{paciente_id}/rejeicao``
//             seguindo a mesma guarda de LGPD. Payload do MQTT é
//             responsabilidade do chamador — este método só provê o
//             tópico seguro.
// Parâmetros: (nenhum).
// Retorno:    tópico MQTT ou ``""``.
// -----------------------------------------------------------------------------
std::string ConfigManager::topicoRejeicao() const {
    // Comentário acima do if de guarda LGPD (R12.4): idêntico ao de
    // ``topicoAplicado`` — ambos os tópicos exigem paciente_id anonimizado.
    if (!is_anonymized_paciente_id(paciente_id_)) {
        return "";
    }
    return "cardioia/paciente/" + paciente_id_ + "/rejeicao";
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::serializarEstado
// Finalidade: Serializa ``state_`` em JSON canônico compacto com as
//             quatro chaves na ordem declarada em ``kFieldSpecs``. O
//             formato estável permite ao Node-RED (R16.4) parsear sem
//             surpresas. Usamos ``snprintf`` em buffer local para evitar
//             alocação heap — consistente com ``logger.cpp``.
// Parâmetros: (nenhum).
// Retorno:    string JSON sem espaços supérfluos.
// -----------------------------------------------------------------------------
std::string ConfigManager::serializarEstado() const {
    char buf[256];
    // Formato: {"bpm_threshold":120,"temp_threshold":38.0,"buffer_limit":50,"sampling_interval_ms":5000}
    const int n = std::snprintf(
        buf, sizeof(buf),
        "{\"bpm_threshold\":%d,\"temp_threshold\":%.1f,"
        "\"buffer_limit\":%d,\"sampling_interval_ms\":%d}",
        state_.bpm_threshold,
        static_cast<double>(state_.temp_threshold),
        state_.buffer_limit,
        state_.sampling_interval_ms);
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
        return "{}";
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::setCaminhoPersistenciaNativo
// Finalidade: Permite aos testes nativos redirecionar o arquivo de
//             persistência para um caminho determinístico (evita
//             contaminação entre execuções). Em builds Arduino, o
//             caminho é ignorado — SPIFFS não suporta caminho arbitrário.
// Parâmetros: descritos em ``config_manager.h``.
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConfigManager::setCaminhoPersistenciaNativo(const std::string& caminho) {
    // Comentário acima do if de caminho vazio (R12.4): aceitamos ``""``
    // como comando para restaurar o default ``/tmp/cardioia_config.json``.
    if (caminho.empty()) {
        caminho_nativo_ = "/tmp/cardioia_config.json";
        return;
    }
    caminho_nativo_ = caminho;
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::persistir (privado)
// Finalidade: Grava o snapshot de ``last_valid_`` em SPIFFS (Arduino)
//             ou em arquivo local (nativo). Falhas são logadas como
//             WARN e não propagam — o firmware continua operando com a
//             config em memória, conforme R16.2 / P20.
// Parâmetros: (nenhum).
// Retorno:    (void).
// -----------------------------------------------------------------------------
void ConfigManager::persistir() {
    const std::string payload = serializarEstado();
#ifdef ARDUINO
    // Comentário acima do if do SPIFFS.begin (R12.4): em builds Arduino
    // precisamos inicializar o SPIFFS antes de abrir o arquivo. Se a
    // montagem falhar, logamos WARN e abortamos a persistência — o
    // estado em RAM permanece válido até o próximo boot.
    if (!SPIFFS.begin(true)) {
        if (logger_ != nullptr) {
            logger_->warn("ConfigManager",
                          "SPIFFS indisponivel, persistencia abortada");
        }
        return;
    }
    File f = SPIFFS.open("/cardioia_config.json", FILE_WRITE);
    if (!f) {
        if (logger_ != nullptr) {
            logger_->warn("ConfigManager",
                          "falha ao abrir /cardioia_config.json");
        }
        return;
    }
    f.print(payload.c_str());
    f.close();
#else
    // Caminho nativo: gravação simples em arquivo.
    std::FILE* f = std::fopen(caminho_nativo_.c_str(), "w");
    // Comentário acima do if de fopen (R12.4): em testes nativos a
    // falha de abertura costuma indicar diretório inexistente; logamos
    // e seguimos adiante sem abortar para manter a semântica de "melhor
    // esforço" do reference model.
    if (f == nullptr) {
        if (logger_ != nullptr) {
            logger_->warn("ConfigManager",
                          "nao foi possivel abrir arquivo nativo");
        }
        return;
    }
    std::fwrite(payload.data(), 1, payload.size(), f);
    std::fclose(f);
#endif
}

// -----------------------------------------------------------------------------
// Método:     ConfigManager::carregar (privado)
// Finalidade: Tenta ler o último snapshot do storage e aplicar os
//             valores lidos sobre ``DEFAULT_CONFIG`` — se o payload
//             persistido for parcial, os campos ausentes assumem os
//             defaults. Retorna ``true`` somente se o payload
//             carregado for integralmente válido (R16.1).
// Parâmetros: (nenhum).
// Retorno:    ``true`` se carregou algo válido; ``false`` caso contrário.
// -----------------------------------------------------------------------------
bool ConfigManager::carregar() {
    std::string payload;
#ifdef ARDUINO
    // Comentário acima do if do SPIFFS.begin (R12.4): mesma regra do
    // ``persistir`` — se não conseguirmos montar o SPIFFS, não há como
    // carregar; devolvemos ``false`` para que o chamador use defaults.
    if (!SPIFFS.begin(true)) {
        return false;
    }
    if (!SPIFFS.exists("/cardioia_config.json")) {
        return false;
    }
    File f = SPIFFS.open("/cardioia_config.json", FILE_READ);
    if (!f) {
        return false;
    }
    while (f.available()) {
        payload.push_back(static_cast<char>(f.read()));
    }
    f.close();
#else
    std::FILE* f = std::fopen(caminho_nativo_.c_str(), "r");
    if (f == nullptr) {
        return false;
    }
    char chunk[128];
    std::size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        payload.append(chunk, n);
    }
    std::fclose(f);
#endif

    // Comentário acima do if de payload vazio (R12.4): storage pode
    // existir mas estar vazio (ex.: primeiro boot com arquivo truncado).
    if (payload.empty()) {
        return false;
    }

    const ConfigState defaults = defaultConfigState();
    const ConfigApplyResult res = apply_config_json(defaults, payload);
    // Comentário acima do if de validade do snapshot (R12.4): storage
    // corrompido ou incompatível é ignorado — preferimos voltar ao
    // default a aceitar valores fora das faixas declaradas.
    if (!res.ok) {
        return false;
    }
    state_ = res.state;
    last_valid_ = res.state;
    return true;
}

}  // namespace cardioia
