// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/vulkan_backend.hpp"

namespace g610::gpu {

// Encaminha os `DrawCommand` para o `RasterizerG610`. Implementado em
// `draw_consumer.cpp` (o unico TU que inclui `rasterizer_g610.hpp`,
// evitando o conflito de `Core::Memory::Memory` entre
// `vendor/eden/src/core/memory.h` e `vendor/eden/core/memory_bridge.h`).
//
// `contexto` e' passado aos callbacks (tipicamente o `this` do NvDrv).
using EscreveNoGuestCb = bool (*)(void* contexto, std::uint64_t endereco,
                                  const void* dados, std::size_t tamanho);
using SinalizaFenceCb = void (*)(void* contexto, std::uint32_t syncpoint);
using LeDoGuestCb = bool (*)(void* contexto, std::uint64_t src, void* dst,
                             std::size_t size);

void processar_draw_comandos(std::vector<DrawCommand>& comandos, void* contexto,
                             EscreveNoGuestCb escreve_no_guest,
                             SinalizaFenceCb sinaliza_fence,
                             LeDoGuestCb le_do_guest = nullptr,
                             std::shared_ptr<VulkanBackend> backend = nullptr);

} // namespace g610::gpu
