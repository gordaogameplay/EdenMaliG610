// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* Terceira sonda: medir em vez de esperar por timeout.
 *
 * Os testes anteriores rebentaram com timeout porque o handler reentrava em
 * ciclo: o guest re-executava o mesmo BRK, infinitamente. Aqui põe-se um
 * limite de traps, e quando e' atingido o handler deixa de avancar o PC e
 * salta para uma pilha de sandbox com um stub de saida. Assim obtem-se uma
 * resposta em vez de um timeout.
 *
 * Objectivo: determinar qual destas abordagens faz o guest continuar:
 *   A) escrever uc_mcontext.pc          (o kernel restaura ou ignora?)
 *   B) PSTATE.SS single-step             (um sinal por instrucao)
 *   C) trocar o PC com um valor de saida (so para diagnosticar)
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ucontext.h>
#include <sys/ucontext.h>
#include <setjmp.h>

extern "C" unsigned long guest_continue(void);

static constexpr unsigned long kContinued = 0x600D;
static constexpr int kMaxTraps = 5;
static int g_traps = 0;
static int g_mode = 1;

/* Rotina de saida:corre na stack do handler, salta para um longjmp. */
static sigjmp_buf g_jmp;
static volatile unsigned long g_result = 0;
static volatile unsigned long g_first_pc = 0;
static volatile unsigned long g_second_pc = 0;

static void handler(int sig, siginfo_t* info, void* raw) {
    (void)info; (void)sig;
    auto* uc = static_cast<ucontext_t*>(raw);
    if (g_traps == 0) g_first_pc = (unsigned long)uc->uc_mcontext.pc;
    if (g_traps == 1) g_second_pc = (unsigned long)uc->uc_mcontext.pc;
    ++g_traps;

    if (g_traps >= kMaxTraps) {
        /* Chegou ao limite: sair, em vez de repetir. */
        siglongjmp(g_jmp, 1);
    }

    if (g_mode == 1)
        uc->uc_mcontext.pc += 4;
    else if (g_mode == 2)
        uc->uc_mcontext.pstate |= (1UL << 20);  /* PSTATE.SS */
}

static void arm() {
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, nullptr);
}

int main() {
    arm();
    printf("=== o kernel honra a escrita do PC? (limite %d traps) ===\n", kMaxTraps);

    for (g_mode = 1; g_mode <= 2; ++g_mode) {
        g_traps = 0;
        printf("\n[modo %d] %s\n", g_mode,
               g_mode == 1 ? "escrever uc_mcontext.pc += 4"
                           : "PSTATE.SS (single-step)");

        if (sigsetjmp(g_jmp, 1) == 0) {
            g_result = guest_continue();
            printf("    RETORNOU x0 = 0x%lx %s\n", g_result,
                   g_result == kContinued ? "(continuou!)" : "(nao continuou)");
        } else {
            printf("    LIMITE DE TRAPS ATINGIDO (%d)\n", g_traps);
        }
        printf("    traps = %d, pc1 = 0x%lx, pc2 = 0x%lx\n",
               g_traps, g_first_pc, g_second_pc);
        if (g_first_pc == g_second_pc && g_traps > 1)
            printf("    => o PC NAO avancou: o kernel restaurou-o.\n");
        else if (g_traps > 1)
            printf("    => o PC avancou: o kernel HONROU a escrita.\n");
    }

    printf("\n=== fim ===\n");
    return 0;
}
