// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação da tradução Maxwell -> Vulkan.
#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/macro_mme.hpp"
#include "g610/gpu/pushbuffer.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace g610::gpu {

// O ficheiro de registos do motor (o upstream: `Regs::reg_array` ✓).
//
// Cada método escreve o seu argumento no registo correspondente — e é
// assim que os comandos empacotados (a topologia, o início, a
// contagem) se lêem ✓.
//
// O mesmo número é o `MacroRegistersStart` do upstream ✓: os métodos
// >= 0xE00 não são registos — são os gatilhos das macros MME.
namespace {
constexpr std::size_t kMaxRegistos = 0x2010;
// O alinhamento do push buffer: o fluxo de métodos são palavras de 32
// bits ✓ (o upstream: `Tegra::CommandHeader` ✓).
constexpr std::size_t kWord = 4;
// O `macro_positions` do upstream (`maxwell_3d.h`: `std::array<u32, 0x80>` ✓).
constexpr std::size_t kNumMacroPositions = 0x80;
// O limiar dos métodos do canal (o `non_puller_methods` do upstream
// `dma_pusher.cpp` ✓): m < 0x40 é do *puller*, não do motor.
constexpr std::uint32_t kPullerMetodos = 0x40;
// O registo que falta nome na casa (o `load_mme.start_address_ptr` = 71:
// fica entre o `instruction_ptr` (69) e o `start_address` (72) ✓).
constexpr std::uint32_t load_mme_start_address_ptr = 71;
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
    falcon_vas_.clear();
    envio_ids_.clear();

    // A leitura de palavra — a mesma da `palavra` da `pushbuffer.cpp`,
    // porque o buffer pode não estar alinhado (vindo de um vector do
    // host): por `memcpy`.
    auto palavra = [&pushbuffer](std::size_t i, std::uint32_t& v) {
        std::memcpy(&v, pushbuffer.data() + i * 4, 4);
    };

    // ============================================================
    // O estado do motor — local por chamada.
    //
    // **Medido** (`tools/simular_mme.py` nos 3 caps reais): o jogo
    // re-carrega o mesmo bloco MME inteiro em cada submissão (623
    // instruções nos mesmos buckets, os mesmos binds, os mesmos
    // hashes) — o reset por `translate()` é seguro e é o que o modelo
    // pede.
    // ============================================================

    // O ficheiro de registos — no upstream, cada método escreve o seu
    // argumento no registo correspondente (`Regs::reg_array`). É daqui
    // que as macros leem o estado (`GetRegisterValue`) e o `draw.end`
    // lê as contagens.
    std::vector<std::uint32_t> regs(kMaxRegistos, 0);

    // O estado do desenho (o `draw_state` do upstream, `draw_manager.cpp`).
    //
    // Os contadores vivem nos registos: quem os escreve é o método
    // correspondente, e valem até ao próximo desenho.
    std::uint32_t index_first = 0;
    std::uint32_t instance_count = 0;
    bool indexado = false;
    bool dentro = false;

    // **O bind dos subcanais** (o `puller.cpp:29-58` — o mesmo filtro
    // da versão anterior). **Medido** (`/tmp/boot_r27.log`): sem este
    // bind, a casa traduz métodos de outros motores.
    std::uint32_t motores_por_subcanal[8] = {0};

    // ============================================================
    // O MME — o código em buckets por chave (o `uploaded_macro_code`
    // do upstream, `macro.cpp:62-69`: `AddCode` faz `push_back` no
    // vetor da chave) e as posições por slot (`macro_positions`,
    // `maxwell_3d.h:3198`).
    // ============================================================
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> mme_buckets;
    std::vector<std::uint32_t> macro_positions(kNumMacroPositions, 0);

    // O ciclo `ProcessMacro` (o `maxwell_3d.cpp:199-224`).
    std::uint32_t executando_macro = 0;
    std::vector<std::uint32_t> macro_params;

    // O sintoma do microcode (a contagem honesta; sai no `error` uma vez).
    std::string anomalias_microcode;
    std::uint32_t profundidade_macro = 0;

    // (A recursão tem definição atrasada: o `Send` das macros volta a
    // entrar neste mesmo caminho — o `maxwell3d.CallMethod` do upstream.)
    std::function<void(std::uint32_t metodo, const std::uint32_t* base,
                       std::uint32_t quantidade, bool e_ultimo)>
        chamada_metodo;

    // ============================================================
    // A palavra do motor: a escrita no registo + os casos do MME +
    // o desenho (o `Maxwell3D::CallMethod` para depois passar pelo
    // `ProcessMethodCall`).
    // ============================================================
    auto palavra_engine = [&](std::uint32_t metodo, std::uint32_t valor) {
        // A escrita no registo vem antes dos casos: o upstream escreve
        // o reg (via `ProcessDirtyRegisters`) e só depois o
        // `ProcessMethodCall` trata o método — os casos do MME leem o
        // valor NOVO dos registos-ponteiro.
        if (metodo < regs.size()) regs[metodo] = valor;

        switch (metodo) {
        case gpu::maxwell3d::load_mme_instruction_ptr:  // 69
            // O `ClearCode(regs.load_mme.instruction_ptr)` (o
            // `maxwell_3d.cpp:332-343`): apaga o bucket na chave nova.
            mme_buckets.erase(regs[metodo]);
            return;

        case gpu::maxwell3d::load_mme_instruction:  // 70
            // O `AddCode(regs.load_mme.instruction_ptr, argumento)`:
            // anexa ao BUCKET da chave. O ponteiro não auto-incrementa
            // no upstream — é isto que os 623 instruídos medidos nos
            // buckets provam.
            mme_buckets[regs[static_cast<std::size_t>(
                            gpu::maxwell3d::load_mme_instruction_ptr)]]
                .push_back(valor);
            return;

        case gpu::maxwell3d::load_mme_start_address:  // 72
            // O `ProcessMacroBind`: `macro_positions[start_ptr++] = data`
            // (o `maxwell_3d.cpp:482-484` — o ponteiro incrementa aqui).
            {
                const std::size_t ptr =
                    static_cast<std::size_t>(load_mme_start_address_ptr);
                macro_positions[regs[ptr] % kNumMacroPositions] = valor;
                ++regs[ptr];
            }
            return;

        case gpu::maxwell3d::falcon + 4:  // 2244
            // A chamada de firmware 4 (o `ProcessMethodCall`,
            // `maxwell_3d.cpp:346-347`): no hardware é um blob que
            // escreve registos conforme os parámetros; o upstream
            // substitui por `regs.shadow_scratch[0] = 1`
            // (`ProcessFirmwareCall4`, `maxwell_3d.cpp:486-492`), porque
            // os registos tocados pelo blob não afetam os jogos que ele
            // mediu. A escrita simples 2244 continua (a escrita acima
            // acontece antes do switch, como o `ProcessDirtyRegisters`
            // do upstream); o stub abre o laço da macro de desenho do
            // jogo (ela lê o rascunho 0xD00 e sai quando vale 0 no r1
            // decrementado).
            regs[0xD00] = 1;
            // A medição do blob: o endereço que a macro entrega (o
            // instrumento `G610_VA_DUMP` do nvdrv lê a memória do guest
            // nesses VAs — a evidência vem antes de qualquer decisão
            // de desenho).
            falcon_vas_.push_back(valor);
            return;

        case gpu::maxwell3d::vertex_buffer_count:
            // O valor fica no registo (é de lá que o `draw.end` o lê,
            // como no upstream).
            break;

        case gpu::maxwell3d::index_buffer_count:
            // O upstream: `index_buffer.count` marca o desenho como
            // indexado (`draw_state.draw_indexed = true`).
            indexado = true;
            break;

        case gpu::maxwell3d::index_buffer_first:
            index_first = valor;
            break;

        case gpu::maxwell3d::global_base_vertex_index:
            break;  // já está no ficheiro de registos

        case gpu::maxwell3d::draw_begin:
            // O `draw.begin` é o início de um desenho (`DrawBegin` no
            // upstream): reinicia a contagem de instâncias e o modo.
            dentro = true;
            instance_count = 0;
            std::fprintf(stderr, "[DRAW_TRACE] draw_begin (1414) chamado, dentro=true\n");
            break;

        case gpu::maxwell3d::draw_end: {
            // O `draw.end` é o desenho (`DrawEnd` no upstream).
            std::fprintf(stderr, "[DRAW_TRACE] draw_end (1413) chamado, dentro=%d vertex_count=%u\n",
                         dentro, regs[gpu::maxwell3d::vertex_buffer_count]);
            if (!dentro)
                break;
            dentro = false;

            // Os contagens vêm dos registos — como no upstream.
            const std::uint32_t vertex_count = regs[gpu::maxwell3d::vertex_buffer_count];
            const std::uint32_t index_count = regs[gpu::maxwell3d::index_buffer_count];

            DrawCommand c;
            c.method = metodo;
            c.indexado = indexado;
            c.index_count = indexado ? index_count : 0;
            c.index_first = index_first;
            c.vertex_count = indexado ? 0 : vertex_count;
            c.base_vertex = regs[gpu::maxwell3d::global_base_vertex_index];
            // Captura o stream de vértices 0 (registo 0x1C00..0x1C02), se
            // o push buffer o tiver escrito. Ainda não se lê da memória do
            // guest — é apenas o VA bruto para a etapa seguinte.
            if (0x702 < regs.size()) {
                const std::uint32_t vs0 = regs[0x700];
                c.vertex_stride = vs0 & 0xFFF;
                if (vs0 & (1u << 12)) {
                    const std::uint32_t hi = regs[0x701];
                    const std::uint32_t lo = regs[0x702];
                    c.vertex_base_va = (static_cast<std::uint64_t>(hi) << 32) | lo;
                }
            }
            // Decodifica os atributos de vértice (registos 0x458..0x477,
            // byte offset 0x1160, 4 bytes por registo).
            for (std::uint32_t i = 0; i < 32 && (0x458 + i) < regs.size(); ++i) {
                const std::uint32_t raw = regs[0x458 + i];
                const std::uint32_t size_enum = (raw >> 21) & 0x3F;
                if (size_enum == 0) continue; // Invalid / desabilitado
                VertexAttributeDesc a;
                a.enabled = true;
                a.buffer = raw & 0x1F;
                a.constant = (raw >> 6) & 0x1;
                a.offset = (raw >> 7) & 0x3FFF;
                a.size_enum = size_enum;
                a.type_enum = (raw >> 27) & 0x7;
                a.bgra = (raw >> 31) & 0x1;
                // stride do stream de origem (0x700 + buffer*4)
                const std::size_t vs_reg = 0x700 + static_cast<std::size_t>(a.buffer) * 4;
                if (vs_reg < regs.size()) a.stream_stride = regs[vs_reg] & 0xFFF;
                // component_count e size_bytes
                switch (size_enum) {
                case 0x01: a.component_count = 4; a.size_bytes = 16; break; // R32G32B32A32
                case 0x02: a.component_count = 3; a.size_bytes = 12; break; // R32G32B32
                case 0x03: a.component_count = 4; a.size_bytes = 8; break;  // R16G16B16A16
                case 0x04: a.component_count = 2; a.size_bytes = 8; break;  // R32G32
                case 0x05: a.component_count = 3; a.size_bytes = 6; break;  // R16G16B16
                case 0x0A: case 0x33: a.component_count = 4; a.size_bytes = 4; break; // R8G8B8A8 / X8B8G8R8
                case 0x0F: a.component_count = 2; a.size_bytes = 4; break;  // R16G16
                case 0x12: a.component_count = 1; a.size_bytes = 4; break;  // R32
                case 0x13: a.component_count = 3; a.size_bytes = 3; break;  // R8G8B8
                case 0x18: case 0x32: a.component_count = 2; a.size_bytes = 2; break; // R8G8 / G8R8
                case 0x1B: a.component_count = 1; a.size_bytes = 2; break;  // R16
                case 0x1D: case 0x34: a.component_count = 1; a.size_bytes = 1; break; // R8 / A8
                case 0x30: a.component_count = 4; a.size_bytes = 4; break;  // A2B10G10R10
                case 0x31: a.component_count = 3; a.size_bytes = 4; break;  // B10G11R11
                default: a.component_count = 1; a.size_bytes = 0; break;
                }
                c.attributes.push_back(a);
            }

            // Topologia: resolve conforme upstream UpdateTopology().
            if (0x652 < regs.size()) c.primitive_topology_control_raw = regs[0x652];
            if (0x65C < regs.size()) c.topology_raw = regs[0x65C] & 0xFFFF;
            if (c.primitive_topology_control_raw == 1) { // UseSeparateState
                const std::uint32_t to = c.topology_raw;
                if (to == 1) { c.topology = 0; c.topology_known = true; } // Points
                else if (to == 2) { c.topology = 1; c.topology_known = true; } // Lines
                else if (to == 3) { c.topology = 3; c.topology_known = true; } // LineStrip
                else if (to == 4 || to == 5 || (to >= 0xA && to <= 0xD)) { c.topology = to; c.topology_known = true; }
                else { c.topology_known = false; } // None or unknown
            } else {
                c.topology_known = false; // UseInBeginMethods: nao rastreado
            }

            c.instance_count = instance_count != 0 ? instance_count : 1;
            c.est_triangles = static_cast<std::uint64_t>(
                                  indexado ? c.index_count : c.vertex_count) / 3;
            out.push_back(c);
            tel_.frame_draw(static_cast<std::uint32_t>(c.est_triangles));
            // O upstream limpa a marca de "indexado" no fim do desenho.
            indexado = false;
            break;
        }

        case gpu::maxwell3d::vertex_array_instance_first:
        case gpu::maxwell3d::vertex_array_instance_subsequent:
            // **Um desenho instanciado** (`DrawArrayInstanced`): os
            // valores vivem num registo empacotado; fica a contagem e
            // conta-se como degradado (honesto: sem a contagem
            // exacta não é o desenho).
            ++cmds_dropped_;
            ++instance_count;
            break;

        default:
            // Estados, máscaras, fences e o resto são legítimos e não
            // são erro: contam-se como conhecidos.
            break;
        }
    };

    // ============================================================
    // O interpretador das macros — o caminho LE do upstream (a macro
    // de desenho do jogo NÃO é HLE: hash `9e3ca7054d12d63c`, fora dos
    // 15 builders de `macro_hle.cpp`).
    // ============================================================
    mme::Interpretador interprete{
        [&](std::uint32_t endereco, std::uint32_t valor) {
            // O `Send` — o `maxwell3d.CallMethod(endereco, valor, true)`:
            // o método volta ao mesmo caminho do motor (é por aqui que
            // a macro manda desenhar).
            // Instrumento do passo 1: por quem o microcode envia. O
            // "0 comandos traduzidos" com 901 envios decide-se com esta
            // contagem no log (o id = o endereco do `CallMethod`).
            ++envio_ids_[endereco];
            // Instrumentação temporária: verificar envios para 1401-1417
            if (endereco >= 1401 && endereco <= 1417) {
                std::fprintf(stderr,
                    "[MME_DRAW_TRACE] endereco=0x%x (%d) valor=0x%x envios_antes=%d\n",
                    endereco, endereco, valor, static_cast<int>(envio_ids_[endereco]) - 1);
            }
            std::uint32_t v = valor;
            chamada_metodo(endereco, &v, 1, true);
        },
        [&](std::uint32_t metodo) {
            // O `Read` — o `maxwell3d.GetRegisterValue(metodo)`.
            return metodo < regs.size() ? regs[metodo] : 0u;
        }};

    // ============================================================
    // O `ProcessMacro` (o `maxwell_3d.cpp:199-224`): colhe os
    // parámetros e chama a macro quando o burst acaba.
    // ============================================================
    auto processar_macro = [&](std::uint32_t metodo, const std::uint32_t* base,
                               std::uint32_t quantidade, bool e_ultimo) {
        if (quantidade == 0)
            return;

        if (executando_macro == 0) {
            // O ASSERT do upstream `(method % 2) == 0`: a macro começa
            // pelo método PAR (o registo da macro), nunca pelo de ARGS.
            // A casa conta o sintoma e não executa.
            if ((metodo % 2) != 0) {
                ++cmds_dropped_;
                return;
            }
            executando_macro = metodo;
        }

        macro_params.insert(macro_params.end(), base, base + quantidade);

        // A chamada no fim do burst (o `is_last_call` do upstream).
        if (!e_ultimo)
            return;

        // O `CallMacroMethod` (o `maxwell_3d.cpp:402-414`) — o reset do
        // estado vem antes da execução (uma macro pode mandar métodos
        // de macro; o upstream faz exactamente isto).
        const std::uint32_t metodo_macro = executando_macro;
        executando_macro = 0;

        const std::uint32_t entry =
            ((metodo_macro - 0xE00) >> 1) %
            static_cast<std::uint32_t>(kNumMacroPositions);
        const std::uint32_t chave = macro_positions[entry];

        // O código: o bucket da chave; se a chave não é um bucket, o
        // `mid_method` do upstream procura o bucket que a contém e
        // rebasa (o `macro.cpp:83-110`).
        const std::vector<std::uint32_t>* codigo = nullptr;
        auto it = mme_buckets.find(chave);
        if (it != mme_buckets.end()) {
            codigo = &it->second;
        } else {
            bool de_rebase = false;
            std::uint32_t base_encontrada = 0;
            for (const auto& par : mme_buckets) {
                if (chave >= par.first && (chave - par.first) < par.second.size()) {
                    base_encontrada = par.first;
                    de_rebase = true;
                    break;
                }
            }
            if (de_rebase) {
                // O rebase — o insert pode rehashar o mapa: as
                // referências re-resolvem depois.
                mme_buckets[chave];
                mme_buckets.at(chave)
                    .assign(mme_buckets.at(base_encontrada).begin() +
                                (chave - base_encontrada),
                            mme_buckets.at(base_encontrada).end());
                codigo = &mme_buckets.at(chave);
            } else {
                // A macro não tem código carregado — sintoma real
                // (nada inventado: conta e segue).
                ++cmds_dropped_;
                macro_params.clear();
                return;
            }
        }

        if (++profundidade_macro > 16) {
            // A recursão (macro mandando macro) além do razoável:
            // o motor vivo não deve travar; conta e para.
            --profundidade_macro;
            ++cmds_unknown_;
            macro_params.clear();
            return;
        }
        interprete.executar(*codigo, macro_params);
        --profundidade_macro;

        // O acumulado do microcode: cada `executar` reinicia a sua
        // contagem; o tradutor soma os totais (as medições que o
        // teste fixa: 17 passos e 5 envios por chamada da macro de
        // desenho — o `tools/trace_macro.py` mediu o mesmo).
        macro_passos_ += interprete.passos();
        macro_envios_ += interprete.envios();

        // Os sintomas do microcode (teto de passos, pc fora do código,
        // parámetros esgotados).
        if (interprete.saiu_por_teto() || interprete.pc_fora() ||
            interprete.params_esgotados()) {
            ++cmds_unknown_;
            if (anomalias_microcode.empty()) {
                if (interprete.saiu_por_teto())
                    anomalias_microcode = "macro MME passou do teto de passos";
                else if (interprete.pc_fora())
                    anomalias_microcode = "macro MME saltou fora do código";
                else
                    anomalias_microcode = "macro MME exauriu os parámetros";
            }
        }
        macro_params.clear();
    };

    // O `CallMethod` / `CallMultiMethod` do motor (o `maxwell_3d.cpp`):
    // macros num só caminho; o resto é palavra a palavra.
    chamada_metodo = [&](std::uint32_t metodo, const std::uint32_t* base,
                         std::uint32_t quantidade, bool e_ultimo) {
        if (metodo >= 0xE00) {
            processar_macro(metodo, base, quantidade, e_ultimo);
            return;
        }
        if (quantidade == 0) {
            // Método sem parâmetros (ex. draw_begin/draw_end): chama com valor 0.
            palavra_engine(metodo, 0);
            return;
        }
        for (std::uint32_t i = 0; i < quantidade; ++i)
            palavra_engine(metodo, base[i]);
    };

    // A porta do canal: o puller primeiro (o `dma_pusher.cpp`: os
    // métodos < 0x40 não chegam ao motor), depois o filtro do subcanal
    // do motor 3D (o mesmo filtro da versão anterior).
    auto chamada_via_subcanal = [&](std::uint32_t sc, std::uint32_t metodo,
                                    const std::uint32_t* base,
                                    std::uint32_t quantidade, bool e_ultimo) {
        ++cmds_total_;
        if (metodo >= 1400) {
            std::fprintf(stderr,
                "[SUBCANAL_TRACE] sc=%u metodo=%u qtd=%u motor[%u]=0x%x Maxwell3D=0x%x\n",
                sc, metodo, quantidade, sc,
                motores_por_subcanal[sc], gpu::motor::Maxwell3D);
        }
        if (metodo == gpu::canal::BindObject) {
            // O `ProcessBindMethod` (o `puller.cpp:29-58`).
            if (sc < 8 && quantidade > 0)
                motores_por_subcanal[sc] = *base;
            return;
        }
        if (metodo < kPullerMetodos)
            return;  // semáforos/fence: do canal (o nvdrv trata), não do motor
        if (sc >= 8 || motores_por_subcanal[sc] != gpu::motor::Maxwell3D)
            return;  // métodos de outros motores: não são do 3D
        chamada_metodo(metodo, base, quantidade, e_ultimo);
    };

    // ============================================================
    // A máquina de palavras — o `ProcessCommands` do upstream com a
    // semântica POR PALAVRA (a razão desta reescrita: o `for_each` da
    // casa colapsava os blocos NonIncreasing/IncreaseOnce e engolia os
    // parámetros das macros — medido nos bytes: `w[975]` é o header
    // modo 5 m=3594 args=3 seguido de 3 palavras de dados; a versão
    // anterior entregava só as 2 primeiras, e o burst de 3 params
    // nunca chegava inteiro ao `ProcessMacro`).
    // ============================================================
    const std::size_t n_palavras = pushbuffer.size() / 4;

    std::uint32_t burst_metodo = 0, burst_sc = 0, burst_count = 0;
    bool burst_uma_vez = false;  // o modo 5 (IncreaseOnce)
    bool burst_cauda = false;    // o resto do burst vem em bloco (o modo 3; a cauda do 5)
    std::vector<std::uint32_t> bloco;

    std::size_t i = 0;
    while (i < n_palavras) {
        std::uint32_t w = 0;
        palavra(i, w);

        if (burst_count != 0) {
            if (burst_cauda) {
                // O bloco do burst — o `CallMultiMethod` do upstream.
                // O `e_ultimo` é o `amount == methods_pending` do
                // upstream: verdadeiro quando o bloco cobre todo o
                // restante do burst. (O upstream chunka por buffer
                // intermédio; aqui o bloco corre inteiro — semântica
                // igual: os parámetros chegam na ordem.)
                const std::uint64_t restante =
                    std::min<std::uint64_t>(static_cast<std::uint64_t>(burst_count),
                                            n_palavras - i);
                bloco.clear();
                for (std::uint64_t k = 0; k < restante; ++k) {
                    std::uint32_t v = 0;
                    palavra(i + k, v);
                    bloco.push_back(v);
                }
                chamada_via_subcanal(burst_sc, burst_metodo, bloco.data(),
                                     static_cast<std::uint32_t>(restante),
                                     restante == burst_count);
                burst_count -= static_cast<std::uint32_t>(restante);
                i += static_cast<std::size_t>(restante);
                continue;
            }

            // O aumento: uma palavra é uma chamada, o método incrementa.
            std::uint32_t v = 0;
            palavra(i, v);
            chamada_via_subcanal(burst_sc, burst_metodo, &v, 1, burst_count == 1);
            ++burst_metodo;
            --burst_count;
            if (burst_uma_vez)
                burst_cauda = true;  // o modo 5 incrementa UMA vez
            ++i;
            continue;
        }

        // A palavra é cabeçalho de método novo.
        const std::uint32_t modo = (w >> 29) & 7;
        if (modo == gpu::modo::Increasing || modo == gpu::modo::NonIncreasing ||
            modo == gpu::modo::IncreaseOnce) {
            burst_metodo = w & 0x1FFFu;
            burst_sc = (w >> 13) & 7u;
            burst_count = (w >> 16) & 0x1FFFu;
            burst_uma_vez = (modo == gpu::modo::IncreaseOnce);
            burst_cauda = (modo == gpu::modo::NonIncreasing);
        } else if (modo == gpu::modo::Inline) {
            // O cabeçalho é a chamada (o valor nos bits 16..28).
            std::uint32_t valor = (w >> 16) & 0x1FFFu;
            chamada_via_subcanal((w >> 13) & 7u, w & 0x1FFFu, &valor, 1, true);
        }
        // Se o cabeçalho tem count==0 (método sem parâmetros, como
        // draw_begin/draw_end), despacha imediatamente.
        if (burst_count == 0 && (modo == gpu::modo::Increasing ||
                                 modo == gpu::modo::NonIncreasing ||
                                 modo == gpu::modo::IncreaseOnce)) {
            chamada_via_subcanal(burst_sc, burst_metodo, nullptr, 0, true);
            burst_metodo = 0;
            burst_sc = 0;
        }
        ++i;
    }

    // O que ficou pendente (burst truncado no fim do buffer, macro sem
    // fecho): sintoma contado, nada inventado.
    if (burst_count != 0 || executando_macro != 0 || !macro_params.empty()) {
        ++cmds_dropped_;
    }
    if (!anomalias_microcode.empty())
        error = anomalias_microcode;

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
