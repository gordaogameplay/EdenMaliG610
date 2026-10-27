// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Teste da interface de leitura segura da memoria do guest (ETAPA 5).
#include "g610/gpu/rasterizer_g610.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    // Mock de leitura: preenche com um padrao conhecido.
    auto mock_le = [](std::uint64_t src, void* dst, std::size_t size) -> bool {
        if (src == 0 || dst == nullptr) return false;
        // Simula regiao legivel apenas para src < 0x1000.
        if (src + size > 0x1000) return false;
        std::memset(dst, 0xAB, size);
        return true;
    };

    g610::gpu::RasterizerG610 r{nullptr, nullptr, mock_le, nullptr};

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        ++pass;
        std::printf("  %s: %s\n", ok ? "ok" : "FALHOU", what);
        if (!ok) ++fail;
    };

    // Leitura valida.
    char buf[16];
    check(r.ler_do_guest(0x100, buf, 16), "leitura valida");
    check(buf[0] == (char)0xAB, "dados lidos");

    // Tamanho zero.
    check(r.ler_do_guest(0x100, buf, 0), "tamanho zero");

    // dst nullptr.
    check(!r.ler_do_guest(0x100, nullptr, 16), "dst nullptr rejeitado");

    // Overflow aritmetico.
    check(!r.ler_do_guest(UINT64_MAX - 4, buf, 16), "overflow rejeitado");

    // Regiao nao mapeada.
    check(!r.ler_do_guest(0x2000, buf, 16), "regiao nao mapeada rejeitada");

    // Tamanho excessivo.
    std::vector<char> big(64 * 1024 * 1024 + 1);
    check(!r.ler_do_guest(0x100, big.data(), big.size()), "tamanho excessivo rejeitado");

    // Sem callback.
    g610::gpu::RasterizerG610 r2{nullptr, nullptr, nullptr, nullptr};
    check(!r2.ler_do_guest(0x100, buf, 16), "sem callback rejeitado");

    // Draw com stream valido (nao chama Vulkan, apenas valida e loga).
    r.AnotaDesenho(4, 0, false, 0x200, 12);
    r.Draw(false, 1);
    check(r.contadores().draws == 1, "Draw incrementou contador");

    // Draw sem VA capturado.
    g610::gpu::RasterizerG610 r3{nullptr, nullptr, mock_le, nullptr};
    r3.Draw(false, 1);
    check(r3.contadores().draws == 1, "Draw sem VA incrementou contador");

    std::printf("\n%d verificacoes, %d falhas\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
