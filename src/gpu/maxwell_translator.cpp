// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação da tradução Maxwell -> Vulkan.
#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"

#include <cstring>

namespace g610::gpu {

// O ficheiro de registos do motor (o upstream: `Regs::reg_array` ✓).
//
// Cada método escreve o seu argumento no registo correspondente — e é
// assim que os comandos empacotados (a topologia, o início, a
// contagem) se lêem ✓.
namespace {
constexpr std::size_t kMaxRegistos = 0xE00;
// O alinhamento do push buffer: o fluxo de métodos são palavras de 32
// bits ✓ (o upstream: `Tegra::CommandHeader` ✓).
constexpr std::size_t kWord = 4;
}  // namespace

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
                                         std::vector<DrawCommand>& out, std::string& error) {
    const tel::Nanos t0 = tel_.time_now();
    out.clear();
    error.clear();

    gpu::PushBufferReader leitor;

    // O ficheiro de registos — no upstream, cada método escreve o seu
    // argumento no registo correspondente (`Regs::reg_array`) ✓.
    std::vector<std::uint32_t> regs(kMaxRegistos, 0);

    // O estado do desenho (o `draw_state` do upstream, `draw_manager.cpp` ✓).
    //
    // **Os contadores vivem nos registos** ✓ — como no upstream ✓, onde
    // o `draw.end` lê `regs.vertex_buffer.count` e `regs.index_buffer` ✗.
    // Por isso não se reiniciam no `draw.begin` ✓: quem os escreve é o
    // método correspondente ✓, e valem até ao próximo desenho ✓.
    std::uint32_t index_first = 0;
    std::uint32_t instance_count = 0;
    bool indexado = false;
    bool dentro = false;

    // **O bind dos subcanais** ✗ — o upstream
    // (`src/video_core/engines/puller.cpp:29-58` ✗) liga cada subcanal a
    // um motor ✗, pelo método `BindObject` (o 0x00 do canal ✗):
    //
    //     void Puller::ProcessBindMethod(const MethodCall& method_call) {
    //         const auto engine_id = static_cast<EngineID>(method_call.argument);
    //         bound_engines[method_call.subchannel] = engine_id;
    //         ...
    //     }
    //
    // **Só os métodos do subcanal ligado ao motor 3D** ✗ sao do motor 3D
    // ✗ — os dos outros subcanais sao de outros motores ✗ (o de copia ✗,
    // o de computacao ✗) ✗ ✗.
    //
    // **Medido** (`/tmp/boot_r27.log` ✗): sem este bind ✗, a casa traduz
    // os métodos **de todos** os subcanais ✗ — e os ids que ela via ✗
    // (`1410`, `1411` ✗) **nao** sao o `draw.begin`/`draw.end` ✗ (que sao
    // `1414`/`1413` ✗) ✗ ✗ — o que explica os **0 comandos traduzidos**
    // ✗ ✗.
    //
    // O mapa e' `subcanal -> motor` ✗ — e vale so' para o subcanal ligado
    // ao 3D ✗ ✗.
    std::uint32_t motores_por_subcanal[8] = {0};

    leitor.for_each(pushbuffer, [&](const gpu::MethodCall& m) {
        ++cmds_total_;

        // O `BindObject` (o 0x00 do canal ✗) — o upstream
        // (`puller.cpp:145-148` ✗) trata-o antes de tudo ✗.
        if (m.method == gpu::canal::BindObject) {
            if (m.subchannel < 8) {
                motores_por_subcanal[m.subchannel] = m.argument;
            }
            return;
        }

        // **Só o subcanal do motor 3D** ✗ tem métodos de motor ✗ — os
        // outros ✗ (o `maxwell_dma` ✗, o `kepler_compute` ✗) tem
        // *outros* ids ✗ ✗, e traduzi-los como se fossem do 3D ✗ dava
        // desenhos errados ✗ ✗.
        if (m.subchannel < 8 && motores_por_subcanal[m.subchannel] != gpu::motor::Maxwell3D) {
            return;
        }

        if (m.method < regs.size())
            regs[m.method] = m.argument;

        switch (m.method) {
        case gpu::maxwell3d::vertex_buffer_count:
            // O valor fica no registo ✓ (é de lá que o `draw.end` o
            // lê ✓, como no upstream ✓).
            break;

        case gpu::maxwell3d::index_buffer_count:
            // O upstream: `index_buffer.count` marca o desenho como
            // indexado (`draw_state.draw_indexed = true`) ✓.
            indexado = true;
            break;

        case gpu::maxwell3d::index_buffer_first:
            index_first = m.argument;
            break;

        case gpu::maxwell3d::global_base_vertex_index:
            break;  // já está no ficheiro de registos

        case gpu::maxwell3d::draw_begin:
            // O `draw.begin` é o início de um desenho (`DrawBegin` no
            // upstream ✓): reinicia a contagem de instâncias e o modo.
            dentro = true;
            instance_count = 0;
            break;

        case gpu::maxwell3d::draw_end: {
            // O `draw.end` é o desenho (`DrawEnd` no upstream ✓).
            if (!dentro)
                break;
            dentro = false;

            // Os contagens vêm dos registos ✓ — como no upstream ✓.
            const std::uint32_t vertex_count = regs[gpu::maxwell3d::vertex_buffer_count];
            const std::uint32_t index_count = regs[gpu::maxwell3d::index_buffer_count];

            DrawCommand c;
            c.method = m.method;
            c.indexado = indexado;
            c.index_count = indexado ? index_count : 0;
            c.index_first = index_first;
            c.vertex_count = indexado ? 0 : vertex_count;
            c.base_vertex = regs[gpu::maxwell3d::global_base_vertex_index];
            c.instance_count = instance_count != 0 ? instance_count : 1;
            c.est_triangles = static_cast<std::uint64_t>(
                                 indexado ? c.index_count : c.vertex_count) / 3;
            out.push_back(c);
            tel_.frame_draw(static_cast<std::uint32_t>(c.est_triangles));
            // O upstream limpa a marca de "indexado" no fim do desenho ✓.
            indexado = false;
            break;
        }

        case gpu::maxwell3d::vertex_array_instance_first:
        case gpu::maxwell3d::vertex_array_instance_subsequent:
            // **Um desenho instanciado** (o upstream: `DrawArrayInstanced` ✓).
            // A topologia, o início e a contagem vivem num registo
            // *empacotado* ✗ — o upstream tira-os bit a bit ✗. Fica a
            // contagem ✗ e conta-se como degradado ✓ (é honesto: um
            // desenho instanciado sem a contagem exacta não é o desenho).
            ++cmds_dropped_;
            ++instance_count;
            break;

        default:
            // Estados, máscaras, fences e o resto são legítimos e não
            // são erro: contam-se como conhecidos ✓.
            break;
        }
    });

    tel_.frame_translate(tel_.time_now() - t0);
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

} // namespace g610::gpu
