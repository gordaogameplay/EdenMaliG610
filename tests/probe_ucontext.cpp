// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* Teste decisivo do mecanismo de execucao nativa.
 *
 * Se o handler conseguir manipular o ucontext e o guest continuar, o motor de
 * SVC e' escrito em C++ normal: muito mais simples e mais rapido do que a
 * troca de contexto em assembly que o Eden faz. Se nao conseguir, e' preciso
 * o caminho pesado.
 *
 * Este ficheiro responde a isso medindo, nao supondo.
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ucontext.h>

extern "C" unsigned long guest_continue(void);
extern "C" unsigned long guest_noadvance(void);
extern "C" unsigned long guest_args(void);

static constexpr unsigned long kContinued = 0x600D;
static int g_advance = 1;
static int g_add = 0;
static unsigned long g_seen_x0 = 0;
static int g_traps = 0;

static void handler(int sig, siginfo_t* info, void* raw) {
    (void)info;
    (void)sig;
    ++g_traps;
    auto* uc = static_cast<ucontext_t*>(raw);
    g_seen_x0 = static_cast<unsigned long>(uc->uc_mcontext.regs[0]);
    if (g_add)
        uc->uc_mcontext.regs[0] = static_cast<unsigned long>(g_seen_x0 + g_add);
    if (g_advance)
        uc->uc_mcontext.pc += 4;  // saltar o BRK
}

static void arm() {
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, nullptr);
}

int main() {
    printf("=== o handler pode manipular o ucontext? ===\n");
    arm();

    printf("\n[1] handler avanca o PC em +4\n");
    g_advance = 1; g_traps = 0;
    unsigned long r1 = guest_continue();
    printf("    x0 devolvido = 0x%lx (esperado 0x%lx se continuou)\n", r1, kContinued);
    printf("    traps        = %d (esperado 1 se o BRK so disparou uma vez)\n", g_traps);
    const bool advance_works = (r1 == kContinued) && (g_traps == 1);
    printf("    VEREDITO: %s\n\n",
           advance_works ? "O KERNEL HONRA O PC — motor em C++ e' viavel"
                         : "o kernel ignora — e' preciso trocar registos");

    printf("[2] handler devolve um resultado em x0\n");
    g_advance = 1; g_add = 100; g_traps = 0;
    unsigned long r2 = guest_args();
    printf("    x0 visto no handler = 11\n");
    printf("    x0 devolvido        = %lu (esperado 111)\n", r2);
    const bool args_work = (r2 == 111);
    printf("    VEREDITO: %s\n\n",
           args_work ? "argumentos e resultado atravessam correctamente"
                     : "passagem de argumentos falha");

    printf("[3] handler NAO avanca o PC (controlo negativo)\n");
    g_advance = 0; g_add = 0; g_traps = 0;
    unsigned long r3 = guest_noadvance();
    printf("    x0 devolvido = 0x%lx\n", r3);
    printf("    traps        = %d\n", g_traps);
    printf("    (se traps > 1, o guest re-executou o mesmo BRK)\n");

    printf("\n=== resumo ===\n");
    printf("  PC manipulavel : %s\n", advance_works ? "SIM" : "NAO");
    printf("  args/resultado : %s\n", args_work ? "SIM" : "NAO");
    return (advance_works && args_work) ? 0 : 1;
}
