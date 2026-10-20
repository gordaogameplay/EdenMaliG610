// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — diagnóstico do programa guest multi-módulo.
//
// Carrega o ExeFS inteiro (rtld + main + sdk), planeia o layout do espaço de
// endereçamento, mapeia, e verifica que cada módulo fica exactamente onde o
// linker do jogo assumiu. Não executa: o objectivo é responder "o programa
// está carregado correctamente?" antes de tentar corré-lo.
//
// Existe à parte do `doctor` de módulo único porque um jogo não é um NSO. O
// `main` do Pokémon Shield tem ZERO SVC (medido) e não pode arrancar sozinho:
// o entry point real é o do `rtld`, e os SVC estão na `sdk` (62) e no `rtld`
// (4).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "g610/guest_program.hpp"
#include "g610/telemetry.hpp"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
        std::printf("  ok: %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  FALHOU: %s\n", what.c_str());
    }
}

std::string hexaddr(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

const g610::GuestModule* find(const g610::GuestProgram& p, g610::GuestModule::Kind k) {
    for (const auto& m : p.modules())
        if (m.kind == k)
            return &m;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso: %s <exefs_dir>\n", argv[0]);
        return 2;
    }
    const std::string exefs = argv[1];

    std::printf("══════ programa guest (multi-módulo) ══════\n");
    std::printf("  exefs: %s\n\n", exefs.c_str());

    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    tel.start("0000000000000000", "guest-program");

    g610::GuestProgram prog(tel);
    std::string err;

    if (!prog.load(exefs, err)) {
        std::printf("  FALHOU ao carregar: %s\n", err.c_str());
        return 1;
    }

    // ── 1. módulos presentes ─────────────────────────────────────────────
    std::printf("[1] módulos do ExeFS\n");
    check(!prog.modules().empty(), "pelo menos um módulo carregado");

    const auto* rtld = find(prog, g610::GuestModule::kRtld);
    const auto* main_m = find(prog, g610::GuestModule::kMain);
    const auto* sdk    = find(prog, g610::GuestModule::kSdk);

    check(rtld != nullptr,
          "rtld carregado — é o entry point real, não o main");
    check(main_m != nullptr, "main carregado");
    check(sdk != nullptr, "sdk carregada — é onde estão os SVC");

    for (const auto& m : prog.modules())
        check(m.nso.address_space_type == 3,
              std::string(m.name()) + ": espaço de endereçamento 39-bit");

    // ── 2. mapeamento do espaço de endereçamento ─────────────────────────
    std::printf("\n[2] espaço de endereçamento\n");
    if (!prog.map_into(128ull * 1024 * 1024, err)) {
        std::printf("  FALHOU ao mapear: %s\n", err.c_str());
        return 1;
    }

    check(prog.memory().mapped(), "espaço do guest mapeado");
    std::printf("    [%s, %s) = %llu bytes\n",
                hexaddr(prog.memory().base()).c_str(),
                hexaddr(prog.memory().base() + prog.memory().size()).c_str(),
                static_cast<unsigned long long>(prog.memory().size()));

    check(prog.memory().owns(prog.entry_point()),
          "o entry point está dentro do espaço mapeado");
    check(prog.stack_top() % g610::kStackAlign == 0,
          "topo da pilha alinhado a 16 bytes (AAPCS64)");

    // ── 3. bases calculadas ──────────────────────────────────────────────
    // As bases são decididas em map_into(); verificar antes seria verificar um
    // valor que ainda não existe.
    std::printf("\n[3] bases dos módulos\n");
    for (const auto& m : prog.modules()) {
        check(m.vaddr_base >= 0x0000'0000'8000'0000ull &&
              m.vaddr_base <= 0x0000'007F'FFFF'FFFFull,
              std::string(m.name()) + " dentro dos 39 bits (" +
                  hexaddr(m.vaddr_base) + ")");
    }
    check(rtld && main_m && rtld->vaddr_base != main_m->vaddr_base,
          "rtld e main em bases distintas");

    // ── 4. conteúdo no endereço do guest ─────────────────────────────────
    //
    // Este é o bloco que apanha o bug dos gaps: se a imagem fosse concatenada
    // (text|rodata|data) em vez de espelhar o espaço virtual, a rodata
    // aterrizaria deslocada e o rtld rebentaria ao percorrer as tabelas de
    // import — muito depois, e sem mensagem útil.
    std::printf("\n[4] conteúdo no endereço do guest\n");
    for (const auto& m : prog.modules()) {
        if (m.nso.image.empty())
            continue;

        const std::uint64_t taddr =
            m.vaddr_base + m.nso.segments[g610::kNsoText].vaddr;
        std::vector<std::uint8_t> t(64);
        if (!prog.memory().read(taddr, t.data(), 64, err)) {
            check(false, std::string(m.name()) + ": " + err);
            continue;
        }
        check(std::memcmp(t.data(),
                          m.nso.image.data() + m.nso.segments[g610::kNsoText].vaddr,
                          64) == 0,
              std::string(m.name()) + ": .text no sítio certo (" +
                  hexaddr(taddr) + ")");

        if (m.nso.rodata_size() == 0)
            continue;

        const std::uint64_t raddr =
            m.vaddr_base + m.nso.segments[g610::kNsoRodata].vaddr;
        const std::uint64_t text_end = taddr + m.nso.text_size();
        check(raddr > text_end,
              std::string(m.name()) + ": rodata " +
                  std::to_string(raddr - text_end) +
                  " bytes à frente do fim do .text (o gap)");

        std::vector<std::uint8_t> r(32);
        if (!prog.memory().read(raddr, r.data(), 32, err)) {
            check(false, std::string(m.name()) + ": rodata " + err);
            continue;
        }
        check(std::memcmp(r.data(),
                          m.nso.image.data() + m.nso.segments[g610::kNsoRodata].vaddr,
                          32) == 0,
              std::string(m.name()) + ": rodata no sítio certo (" +
                  hexaddr(raddr) + ")");
    }

    // ── 5. patch estático por módulo ─────────────────────────────────────
    std::printf("\n[5] patch estático\n");
    if (!prog.patch_all(err)) {
        std::printf("  FALHOU no patch: %s\n", err.c_str());
        return 1;
    }
    for (const auto& m : prog.modules())
        check(m.patched, std::string(m.name()) + ": .text processado");

    // ── 6. regiões que o rtld varre ──────────────────────────────────────
    //
    // Isto não é um teste de "a lista está certa". É um teste das DUAS
    // invariantes que o rtld impõe, e que nenhuma das verificações acima
    // tocaria:
    //
    //   - sem buracos: o rtld avança `base + size` e pára se a resposta for
    //     "Free". Um buraco não custa um módulo, custa todos os seguintes.
    //   - a primeira região começa em 0, porque é o primeiro endereço que o
    //     rtld interroga.
    //
    // E que o `.text` de cada módulo é Code/R-X — a única forma de o rtld o
    // reconhecer como módulo.
    std::printf("\n[6] regiões do svcQueryMemory\n");
    {
        const std::vector<g610::GuestRegion>& r = prog.memory_regions();
        check(!r.empty(), "há regiões");

        std::uint64_t cursor = 0;
        bool tiling = true;
        for (const auto& g : r) {
            if (g.base != cursor) {
                tiling = false;
                std::printf("    (lacuna ou sobreposição em %s: esperava %s, "
                            "veio %s)\n",
                            hexaddr(g.base).c_str(), hexaddr(cursor).c_str(),
                            hexaddr(g.base).c_str());
            }
            cursor = g.base + g.size;
        }
        check(tiling, "as regiões ladrilham sem buracos nem sobreposições");
        check(r.front().base == 0, "a primeira região começa no endereço 0");

        for (const auto& m : prog.modules()) {
            bool achou = false;
            for (const auto& g : r) {
                if (g.base != m.vaddr_base)
                    continue;
                achou = true;
                check(g.state == g610::GuestRegion::kCode &&
                          g.permission == g610::GuestRegion::kReadExec,
                      std::string(m.name()) + ": .text é Code/R-X (o rtld "
                                              "só aceita este par)");
            }
            check(achou, std::string(m.name()) +
                              ": a região do .text começa na base do módulo");
        }
    }

    // ── resumo ───────────────────────────────────────────────────────────
    std::printf("\n%s\n", prog.summary().c_str());
    std::printf("\n  NOTA: o entry point do programa é o do rtld (%s), não o\n"
                "  do main. O main só corre quando o rtld o chama, depois de\n"
                "  ter mapeado a memória e resolvido os imports.\n",
                hexaddr(prog.entry_point()).c_str());

    std::printf("\n%d verificações, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}