// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — ponte entre o tradutor Maxwell e o RasterizerG610.
//
// Este e' o UNICO TU que inclui `rasterizer_g610.hpp` (e, por via dele,
// `vendor/eden/src/core/memory.h`). Nao inclui `memory_bridge.h`,
// evitando o conflito de definicao de `Core::Memory::Memory`.

#include "g610/gpu/draw_consumer.hpp"
#include "g610/gpu/rasterizer_g610.hpp"

namespace g610::gpu {

void processar_draw_comandos(std::vector<DrawCommand>& comandos, void* contexto,
                             EscreveNoGuestCb escreve_no_guest,
                             SinalizaFenceCb sinaliza_fence) {
    static RasterizerG610 rasterizer{
        [contexto, escreve_no_guest](
            std::uint64_t endereco, const void* dados,
            std::size_t tamanho) -> bool {
            return escreve_no_guest(contexto, endereco, dados, tamanho);
        },
        [contexto, sinaliza_fence](std::uint32_t syncpoint) {
            sinaliza_fence(contexto, syncpoint);
        },
        [contexto](std::uint64_t src, void* dst, std::size_t size) -> bool {
            // Leitura via nvdrv wrapper estático (definido em nvdrv.cpp).
            // Se não disponível, falha explicitamente.
            extern bool NvDrvLerDoGuest(void* contexto, std::uint64_t src, void* dst, std::size_t size);
            return NvDrvLerDoGuest(contexto, src, dst, size);
        }};

    for (auto& cmd : comandos) {
        rasterizer.AnotaDesenho(cmd.vertex_count, cmd.index_count, cmd.indexado,
                                cmd.vertex_base_va, cmd.vertex_stride);
        rasterizer.Draw(cmd.indexado, cmd.instance_count);
    }
}

} // namespace g610::gpu
