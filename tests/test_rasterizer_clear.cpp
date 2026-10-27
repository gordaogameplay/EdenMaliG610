// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Teste de integração: RasterizerG610::Clear() executa um clear Vulkan real.
#include "g610/gpu/rasterizer_g610.hpp"
#include "g610/gpu/vulkan_backend.hpp"

#include <cstdio>
#include <memory>

int main() {
    auto backend = std::make_shared<g610::gpu::VulkanBackend>();
    std::string err;
    if (!backend->init(err)) {
        std::fprintf(stderr, "backend init falhou: %s\n", err.c_str());
        return 1;
    }

    g610::gpu::RasterizerG610 rasterizer{nullptr, nullptr, nullptr, backend};

    // Chamada recebida: contador incrementado.
    rasterizer.Clear(1);
    if (rasterizer.contadores().clears != 1) {
        std::fprintf(stderr, "contador de clears não incrementou\n");
        backend->shutdown();
        return 1;
    }

    // Verificação numérica dos pixels via backend.
    auto res = backend->clear_image();
    std::printf("ClearResult: ok=%d checked=%u mismatched=%u\n",
                res.ok ? 1 : 0, res.pixels_checked, res.pixels_mismatched);
    if (!res.error.empty()) {
        std::printf("erro: %s\n", res.error.c_str());
    }

    backend->shutdown();
    return res.ok ? 0 : 1;
}
