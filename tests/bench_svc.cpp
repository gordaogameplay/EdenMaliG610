// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — medicao do custo do despacho de servicos.
//
// ## O que mede
//
// O **custo por SVC**, que e' a unidade de trabalho do motor: cada pedido
// do guest ao sistema passa por aqui.
//
// A medicao e' do **despacho** (a parte C++): ler os argumentos, decidir
// o que fazer, escrever a resposta. A **trap** em si (o `brk`, o handler
// de sinais, a restauracao do contexto) e' do motor de execucao nativa e
// mede-se a parte de cima.
//
// ## Como correr
//
//     ./bench_svc [numero_de_chamadas]
//
// A saida e' o numero de SVCs por segundo, e o tempo por chamada.

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

namespace {

long long agora_ns() {
    struct timespec t {};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<long long>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t n = 10'000'000;
    if (argc > 1)
        n = std::strtoull(argv[1], nullptr, 0);

    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    g610::SvcDispatcher svc(tel);

    // O `GetThreadId` (0x25) e' o SVC mais barato que ha: nao le' nem
    // escreve memoria do guest, e devolve um inteiro. Serve de medida de
    // base — o custo do despacho, sem o custo de I/O.
    g610::SvcContext ctx{};
    ctx.x[0] = 0;

    // Aquecer: a primeira chamada paga o page fault das paginas do
    // codigo, e isso nao e' o que se quer medir.
    for (int i = 0; i < 1000; ++i)
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::GetThreadId), ctx);

    const auto t0 = agora_ns();
    for (std::uint64_t i = 0; i < n; ++i)
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::GetThreadId), ctx);
    const auto t1 = agora_ns();

    const auto total = t1 - t0;
    const double por_chamada = static_cast<double>(total) / static_cast<double>(n);
    const double por_segundo = static_cast<double>(n) * 1e9 / static_cast<double>(total);

    std::printf("despacho de SVC: %llu chamadas em %.3f ms\n", (unsigned long long)n,
                total / 1e6);
    std::printf("  por chamada: %.1f ns\n", por_chamada);
    std::printf("  por segundo: %.0f SVC/s\n", por_segundo);
    std::printf("  x0 devolvido: 0x%llx\n", (unsigned long long)ctx.x[0]);
    return 0;
}
