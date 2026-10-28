// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — ponte entre o tradutor Maxwell e o RasterizerG610.
//
// Este e' o UNICO TU que inclui `rasterizer_g610.hpp` (e, por via dele,
// `vendor/eden/src/core/memory.h`). Nao inclui `memory_bridge.h`,
// evitando o conflito de definicao de `Core::Memory::Memory`.

#include "g610/gpu/draw_consumer.hpp"
#include "g610/gpu/rasterizer_g610.hpp"
#include "g610/gpu/shader_spv.hpp"

namespace g610::gpu {

void processar_draw_comandos(std::vector<DrawCommand>& comandos, void* contexto,
                             EscreveNoGuestCb escreve_no_guest,
                             SinalizaFenceCb sinaliza_fence,
                             LeDoGuestCb le_do_guest,
                             std::shared_ptr<VulkanBackend> backend) {
    static RasterizerG610 rasterizer{
        [contexto, escreve_no_guest](
            std::uint64_t endereco, const void* dados,
            std::size_t tamanho) -> bool {
            return escreve_no_guest(contexto, endereco, dados, tamanho);
        },
        [contexto, sinaliza_fence](std::uint32_t syncpoint) {
            sinaliza_fence(contexto, syncpoint);
        },
        [contexto, le_do_guest](std::uint64_t src, void* dst, std::size_t size) -> bool {
            if (le_do_guest == nullptr) return false;
            return le_do_guest(contexto, src, dst, size);
        },
        backend};

    // Define os shaders SPIR-V na primeira invocacao.
    static bool shaders_definidos = false;
    if (!shaders_definidos) {
        std::vector<char> vs(vertex_spv, vertex_spv + vertex_spv_len);
        std::vector<char> fs(fragment_spv, fragment_spv + fragment_spv_len);
        rasterizer.set_shaders(vs, fs);
        shaders_definidos = true;
    }

    for (auto& cmd : comandos) {
        rasterizer.AnotaDesenho(cmd.vertex_count, cmd.index_count, cmd.indexado,
                                cmd.vertex_base_va, cmd.vertex_stride,
                                cmd.attributes, cmd.topology_known, cmd.topology);
        rasterizer.Draw(cmd.indexado, cmd.instance_count);
    }
}

} // namespace g610::gpu
