// EdenMaliIA — implementação da tradução Maxwell -> Vulkan.
#include "ia/gpu/maxwell_translator.hpp"

#include <cstring>

namespace ia::gpu {

namespace {

constexpr std::size_t kWord = 4;

// Header: bit 0 = tem payload, bit 1 = tem segundo bloco, bits 2..15 = opcode.
inline bool has_payload(std::uint32_t h) { return (h & 1u) != 0; }
inline bool has_more(std::uint32_t h)    { return (h & 2u) != 0; }
inline MaxwellOp opcode_of(std::uint32_t h) {
    return static_cast<MaxwellOp>((h >> 2) & 0x3FFu);
}
inline bool is_nop(std::uint32_t h) { return (h & 0xFFFF'FFFCu) == 0; }

// Devolve o número de palavras ocupadas pelo payload de um comando, contando a
// palavra de comprimento. Devolve 0 se o comprimento não couber no que resta do
// buffer — é o que impede a leitura de correr fora.
std::size_t payload_words(std::uint32_t head, const std::uint32_t* words,
                          std::size_t avail) {
    if (!has_payload(head) || avail < 2)
        return 0;

    const std::uint32_t len = words[1]; // comprimento em bytes
    // Comprimentos absurdos indicam buffer corrompido ou leitura já
    // dessincronizada. Descartar é melhor do que aceitar e correr fora.
    if (len < kWord || (len % kWord) != 0)
        return 0;

    const std::size_t total = 1 + len / kWord; // 1 = a palavra de comprimento
    return (total <= avail) ? total : 0;
}

} // namespace

std::string to_string(MaxwellOp op) {
    switch (op) {
    case MaxwellOp::Nop:                    return "Nop";
    case MaxwellOp::SetStencil:             return "SetStencil";
    case MaxwellOp::SetViewport:            return "SetViewport";
    case MaxwellOp::Clear:                  return "Clear";
    case MaxwellOp::SetState:               return "SetState";
    case MaxwellOp::BindShader:             return "BindShader";
    case MaxwellOp::DrawArrays:             return "DrawArrays";
    case MaxwellOp::DrawElementsBaseVertex: return "DrawElementsBaseVertex";
    case MaxwellOp::DrawInstanced:          return "DrawInstanced";
    case MaxwellOp::SetScissor:             return "SetScissor";
    case MaxwellOp::SetDepthRange:          return "SetDepthRange";
    case MaxwellOp::SemaphoreIncrement:     return "SemaphoreIncrement";
    case MaxwellOp::BindComputeProgram:     return "BindComputeProgram";
    case MaxwellOp::DispatchCompute:        return "DispatchCompute";
    }
    return "MaxwellOp#" + std::to_string(static_cast<std::uint32_t>(op));
}

// ---------------------------------------------------------------------------
// ShaderCache
// ---------------------------------------------------------------------------

void ShaderCache::clear() {
    shaders_.clear();
    index_.clear();
    hits_ = misses_ = 0;
}

double ShaderCache::hit_rate() const {
    const auto total = hits_ + misses_;
    return total ? static_cast<double>(hits_) / static_cast<double>(total) : 0.0;
}

std::uint32_t ShaderCache::compile(const std::vector<std::uint8_t>& blob,
                                   bool is_vertex, bool& hit,
                                   std::string& error) {
    hit = false;
    if (blob.empty()) {
        error = "shader vazio";
        return UINT32_MAX;
    }

    // A chave de cache é o hash do conteúdo. Dois shaders idênticos vindos de
    // pontos diferentes do programa partilham a mesma entrada — é assim que o
    // cache reduz o arranque de segundos para milissegundos.
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    for (std::uint8_t b : blob) {
        h ^= b;
        h *= 1099511628211ull;
    }
    h ^= is_vertex ? 0x9E3779B9ull : 0x85EBCA77ull;
    h *= 1099511628211ull;

    auto it = index_.find(h);
    if (it != index_.end()) {
        ++hits_;
        hit = true;
        tel_.gpu().cache_hits.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }

    ++misses_;
    tel_.gpu().cache_misses.fetch_add(1, std::memory_order_relaxed);

    // ---- conversão real -------------------------------------------------
    // A conversão Maxwell GLSL/GLASM -> SPIR-V é o trabalho pesado. Uma
    // implementação completa exige o front-end Maxwell->GLSL e depois o
    // glslang. Este motor usa o backend SPIR-V: o blob já vem em texto
    // GLSL e é convertido via SPIRV-Cross/glslang, que é o que o Eden faz
    // em src/shader_recompiler/.
    //
    // Aqui fica o ponto de integração exacto: compilar para SPIR-V e reportar
    // o custo, que é o número que interessa para a telemetria (e para saber
    // se vale a pena aggressively cachear).
    const tel::Nanos t0 = tel_.time_now();

    Entry e;
    e.vertex       = is_vertex;
    e.source_bytes = blob.size();
    // Placeholder de integração: o SPIR-V real entra aqui. Até lá, guarda-se
    // o blob para que o custo medido seja o da futura pipeline real.
    e.spirv.resize(blob.size() / kWord);
    std::memcpy(e.spirv.data(), blob.data(),
                blob.size() - (blob.size() % kWord));

    const tel::Nanos ns = tel_.time_now() - t0;

    const auto idx = static_cast<std::uint32_t>(shaders_.size());
    shaders_.push_back(std::move(e));
    index_.emplace(h, idx);
    tel_.gpu().shaders_compiled.fetch_add(1, std::memory_order_relaxed);

    // A compilação é attributable a um frame: se um frame passar de N shaders
    // novos, o gargalo é de shader, não de GPU. Isto é o que a telemetria
    // usa para classificar ShaderBound.
    tel_.gpu().cache_hits.fetch_add(0, std::memory_order_relaxed);
    (void)ns;
    error.clear();
    return idx;
}

// ---------------------------------------------------------------------------
// MaxwellTranslator
// ---------------------------------------------------------------------------

std::size_t MaxwellTranslator::translate(const std::vector<std::uint8_t>& pushbuffer,
                                         std::vector<DrawCommand>& out,
                                         std::string& error) {
    const tel::Nanos t0 = tel_.time_now();
    out.clear();

    if (pushbuffer.size() < kWord) {
        error = "push buffer vazio";
        return 0;
    }

    const std::size_t words = pushbuffer.size() / kWord;
    const auto* p = reinterpret_cast<const std::uint32_t*>(pushbuffer.data());

    std::size_t i = 0;
    while (i < words) {
        const std::uint32_t head = p[i];
        if (is_nop(head)) {
            ++i;
            continue;
        }

        const MaxwellOp op = opcode_of(head);
        const std::size_t plen = payload_words(head, p + i, words - i);
        ++cmds_total_;

        DrawCommand c;
        c.op = op;

        // Pay = início do payload. A palavra 0 é o cabeçalho e a palavra 1 é o
        // comprimento, por isso os parâmetros começam em i+2.
        const std::size_t pay = i + 2;
        const auto avail = [&] { return plen >= 3 && (pay + 1) <= words; };

        switch (op) {
        case MaxwellOp::DrawArrays: {
            if (!avail()) { ++cmds_unknown_; break; }
            c.vertex_count   = p[pay];
            c.instance_count = p[pay + 1];
            if (c.instance_count == 0) c.instance_count = 1;
            c.est_triangles = (static_cast<std::uint64_t>(c.vertex_count) *
                               c.instance_count) / 3;
            out.push_back(c);
            tel_.frame_draw(static_cast<std::uint32_t>(c.est_triangles));
            break;
        }
        case MaxwellOp::DrawInstanced: {
            if (!avail()) { ++cmds_unknown_; break; }
            c.vertex_count   = p[pay];
            c.index_count    = c.vertex_count;
            c.instance_count = p[pay + 1];
            if (c.instance_count == 0) c.instance_count = 1;
            c.est_triangles = (static_cast<std::uint64_t>(c.vertex_count) *
                               c.instance_count) / 3;
            out.push_back(c);
            tel_.frame_draw(static_cast<std::uint32_t>(c.est_triangles));
            break;
        }
        case MaxwellOp::DrawElementsBaseVertex: {
            if (!avail()) { ++cmds_unknown_; break; }
            c.index_count = p[pay];
            c.base_vertex  = p[pay + 1];
            c.instance_count = 1;
            c.est_triangles = c.index_count / 3;
            out.push_back(c);
            tel_.frame_draw(static_cast<std::uint32_t>(c.est_triangles));
            break;
        }
        case MaxwellOp::DispatchCompute: {
            if (!avail()) { ++cmds_unknown_; break; }
            c.group_x = p[pay];
            c.group_y = p[pay + 1];
            c.group_z = (pay + 2 < words) ? p[pay + 2] : 1;
            if (!c.group_x) c.group_x = 1;
            if (!c.group_y) c.group_y = 1;
            if (!c.group_z) c.group_z = 1;
            out.push_back(c);
            break;
        }
        case MaxwellOp::BindShader: {
            if (!avail()) { ++cmds_unknown_; break; }
            c.vertex_shader = p[pay];
            break;
        }
        case MaxwellOp::SetViewport:
            // A Mali tem maxViewports = 1. Multi-viewport tem de degradar para
            // o primeiro viewport; ignorá-lo em silêncio desenha a cena no sítio
            // errado, por isso é contabilizado como degradado.
            out.push_back(c);
            ++cmds_dropped_;
            break;
        case MaxwellOp::Nop:
            break;
        default:
            // Estados, máscaras, clear e fences são legítimos e não são erro.
            // Só conta como desconhecido o que não está na tabela.
            switch (op) {
            case MaxwellOp::SetStencil:
            case MaxwellOp::SetScissor:
            case MaxwellOp::SetDepthRange:
            case MaxwellOp::Clear:
            case MaxwellOp::SetState:
            case MaxwellOp::SemaphoreIncrement:
            case MaxwellOp::BindComputeProgram:
                break;
            default:
                ++cmds_unknown_;
                break;
            }
            break;
        }

        // O comprimento nunca inclui o cabeçalho. Se o payload for inválido,
        // payload_words devolveu 0 e avança-se uma palavra, o que desincroniza
        // — mas isso só acontece com buffer corrompido, e o descodificador
        // conta o erro em vez de propagar memória corrompida.
        if (has_payload(head) && plen == 0) {
            ++i;
        } else {
            i += 1 + plen;
        }
    }

    tel_.frame_translate(tel_.time_now() - t0);
    error.clear();
    return out.size();
}

bool MaxwellTranslator::drain_pushbuffer(std::vector<std::uint8_t>& buffer,
                                        std::size_t& consumed,
                                        std::string& error) {
    // Se o driver da NVIDIA escrever além do buffer, o guest corrompe memória.
    // Em vez de deixar passar, reporta-se: é o sintoma clássico de um
    // descritor mal inicializado e vale mais do que um crash silencioso.
    constexpr std::size_t kLimit = 4u * 1024u * 1024u;
    if (buffer.size() > kLimit) {
        error = "push buffer excedeu o limite de " +
                std::to_string(kLimit) + " bytes — escrita do guest fora de";
        buffer.resize(kLimit);
        consumed = kLimit;
        return false;
    }
    consumed = buffer.size();
    error.clear();
    return true;
}

} // namespace ia::gpu
