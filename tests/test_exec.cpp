// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — teste de execução nativa de ponta a ponta.
//
// Compila o guest sintético (tests/guest_test.S) para AArch64, extrai o
// .text, aplica o patcher estático — que transforma os SVC em BRK — e
// executa-o através do motor. Depois verifica se aconteceu mesmo o que se
// espera:
//
//   * as instruções AArch64 correram nativamente (não há tradução);
//   * os SVC levantaram trap e foram despachados;
//   * os argumentos atravessaram do guest para o host;
//   * o resultado voltou para o guest;
//   * a execução continuou depois da trap;
//   * ExitProcess terminou o guest.
//
// É o teste que responde "o motor executa código?", que até agora ninguém
// tinha medido.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "exec_mem.hpp"
#include "g610/native_engine.hpp"
#include "g610/patcher.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

// Rotinas do guest, escritas em assembly (tests/guest_test.S). Executam
// codigo AArch64 nativo — sao o guest que o motor vai correr.
extern "C" void guest_entry();
extern "C" void guest_fault();
extern "C" void guest_one_svc();

// Tamanhos das rotinas, gerados no build a partir do objecto
// (ver tests/gen_guest_sizes.sh). Evita subtrair enderecos de funcoes em C++,
// o que nao e uma expressao constante.
#include "guest_sizes.h"

using g610::test::mmap_exec;
using g610::test::munmap_exec;

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const char* what) {
    if (ok) { ++g_pass; std::printf("  ok: %s\n", what); }
    else    { ++g_fail; std::printf("  FALHOU: %s\n", what); }
}

} // namespace

int main() {
    std::printf("══════ EdenMaliG610 — execução nativa ══════\n");

    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    tel.start("0000000000000000", "guest-test");

    g610::SvcDispatcher svc(tel);
    g610::NativeEngine engine(tel, svc);

    std::string err;
    if (!engine.install_handlers(err)) {
        std::printf("ERRO: %s\n", err.c_str());
        return 1;
    }
    std::printf("  handlers de sinais instalados\n\n");
    // temporario: confirmar que chegamos aqui

    // ── guest principal ──────────────────────────────────────────────────
    std::printf("[1] execução nativa com SVC\n");
    {
        // O patcher reescreve SVC->BRK in-place no código do guest. É
        // exactamente o que faz ao programa de um jogo, e tem de funcionar
        // equally aqui.
        g610::StaticPatcher patcher(tel);
        g610::PatchReport pr{};
        std::string pe;

        std::vector<std::uint8_t> image(kGuestTextSize);
        std::memcpy(image.data(), (const void*)&guest_entry, kGuestTextSize);

        if (!patcher.patch(image, 0, kGuestTextSize, pr, pe)) {
            std::printf("  FALHOU: patcher: %s\n", pe.c_str());
            return 1;
        }

        check(pr.saw_any_svc, "o patcher encontrou SVC no guest");
        check(pr.svc_patched >= 3, "os três SVC foram reescritos para BRK");

        // Executar. O código está em `image`, e é preciso correr *dali* —
        // o patch foi escrito em image, não no .text original.
        // Copiar de volta não é opção (o .text é read-only), por isso o
        // motor tem de correr a partir de um bloco executável próprio.
        void* block = mmap_exec(image.data(), image.size());
        if (!block) {
            std::printf("  FALHOU: não foi possível criar memória executável\n");
            return 1;
        }

        auto entry = reinterpret_cast<void (*)()>(block);

        // Stack do guest: 256 KB alinhado a 16 bytes.
        constexpr std::size_t kStack = 256 * 1024;
        alignas(16) std::uint8_t stack[kStack];

        const g610::RunResult r = engine.run(entry,
                                           reinterpret_cast<std::uint64_t>(
                                               stack + kStack));

        check(r.traps >= 3, "houve pelo menos 3 traps");
        check(r.svc_calls == 3, "os três SVC foram despachados");
        check(r.exited, "ExitProcess terminou o guest");
        check(r.last_trap == g610::TrapKind::SvcCall,
              "a última trap foi um SVC, não um fault");
        check(r.error.empty(), "sem erro");

        std::printf("    svc_calls=%llu traps=%llu faults=%llu svc_ns=%llu\n",
                    (unsigned long long)r.svc_calls,
                    (unsigned long long)r.traps,
                    (unsigned long long)r.faults,
                    (unsigned long long)r.svc_time_ns);

        munmap_exec(block, image.size());
    }

    // ── argumento atravessa ──────────────────────────────────────────────
    std::printf("\n[2] argumento e resultado\n");
    {
        // guest_one_svc põe x0=123 e chama ExitProcess. O dispatcher regista
        // o exit_code a partir de x0 — se o argumento não tiver chegado,
        // o código de saída seria 0 em vez de 123.
        g610::StaticPatcher patcher(tel);
        g610::PatchReport pr{};
        std::string pe;

        std::vector<std::uint8_t> image(kGuestTextOneSize);
        std::memcpy(image.data(), (const void*)&guest_one_svc, kGuestTextOneSize);
        patcher.patch(image, 0, kGuestTextOneSize, pr, pe);

        void* block = mmap_exec(image.data(), image.size());
        auto entry = reinterpret_cast<void (*)()>(block);

        constexpr std::size_t kStack = 256 * 1024;
        alignas(16) std::uint8_t stack[kStack];

        const g610::RunResult r = engine.run(entry,
                                           reinterpret_cast<std::uint64_t>(
                                               stack + kStack));

        check(r.svc_calls == 1, "um SVC despachado");
        check(r.exit_code == 123,
              "o argumento x0=123 chegou ao dispatcher de ExitProcess");
        std::printf("    exit_code = %d (esperado 123)\n", r.exit_code);

        munmap_exec(block, image.size());
    }

    // ── recuperação de fault ────────────────────────────────────────────
    std::printf("\n[3] recuperação de fault de memória\n");
    {
        g610::StaticPatcher patcher(tel);
        g610::PatchReport pr{};
        std::string pe;

        std::vector<std::uint8_t> image(kGuestTextFaultSize);
        std::memcpy(image.data(), (const void*)&guest_fault, kGuestTextFaultSize);
        patcher.patch(image, 0, kGuestTextFaultSize, pr, pe);

        void* block = mmap_exec(image.data(), image.size());
        auto entry = reinterpret_cast<void (*)()>(block);

        constexpr std::size_t kStack = 256 * 1024;
        alignas(16) std::uint8_t stack[kStack];

        const g610::RunResult r = engine.run(entry,
                                           reinterpret_cast<std::uint64_t>(
                                               stack + kStack));

        check(r.faults == 1, "o fault foi registado uma vez");
        check(r.last_trap == g610::TrapKind::GuestFault,
              "a paragem foi classificada como fault de guest");
        check(!r.error.empty(), "o fault produziu uma descrição");
        check(!r.exited, "o guest não chegou a chamar ExitProcess");
        std::printf("    %s\n", r.error.c_str());

        munmap_exec(block, image.size());
    }

    std::printf("\n%d verificações, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
