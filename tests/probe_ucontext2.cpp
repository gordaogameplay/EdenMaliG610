// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* Segunda sonda: se o kernel ignora uc_mcontext.pc, o que e' que ele honra?
 *
 * O timeout anterior provou que avancar o PC no ucontext nao funciona: o
 * guest re-executa o mesmo BRK indefinidamente.
 *
 * Hipoteses a testar, do mais provavel para o menos:
 *
 *   A) sigreturn com SA_NODEFER removido / flags de estado. Talvez o kernel
 *      reponha o PC a partir de uma copia interna e nao do ucontext.
 *   B) Usar SA_SIGINFO|SA_ONSTACK e um ucontext em memoria do handler — o
 *      kernel escreve o contexto na stack de sinais, e ai o PC e' mesmo o
 *      que restauramos.
 *   C) O TRAP_SINGLE_STEP: em vez de mexer no PC, setting o bit de
 *      single-step no pstate faz o kernel saltar uma instrucao.
 *
 * A pista mais forte: o kernel restaura de ucontext_t mas talvez use
 * uc_mcontext.__reserved ou exija uma copia do contexto feita com
 * sigaltstack. Testar B e C directamente.
 */
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ucontext.h>

extern "C" unsigned long guest_continue(void);

static constexpr unsigned long kContinued = 0x600D;
static int g_mode = 0;
static int g_traps = 0;
static char altstack[64 * 1024];

/* modo 0: nada (controlo)
 * modo 1: pc += 4
 * modo 2: pstate single-step (o kernel lida uma instrucao e re-entrega)
 * modo 3: pc += 4 e zera o resto do contexto */
static void handler(int sig, siginfo_t* info, void* raw) {
    (void)info; (void)sig;
    ++g_traps;
    auto* uc = static_cast<ucontext_t*>(raw);

    if (g_mode == 2) {
        /* PSTATE.SS (single step) = bit 20 do pstate */
        uc->uc_mcontext.pstate |= (1UL << 20);
        return;
    }
    if (g_mode == 3) {
        /* Reescrever o contexto inteiro a partir de uma copia nossa. */
        static ucontext_t saved;
        std::memcpy(&saved, uc, sizeof(saved));
        uc->uc_mcontext.pc += 4;
        return;
    }
    if (g_mode == 1)
        uc->uc_mcontext.pc += 4;
}

static void arm(unsigned flags) {
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | flags;
    sigemptyset(&sa.sa_mask);
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}

int main() {
    printf("=== o que o kernel honra para retomar apos um BRK? ===\n");

    /* modo 2: single step. Este nao depende do ucontext — o hardware pula a
     * instrucao seguinte e re-entrega o sinal. Se funcionar, e' um caminho
     * lento (um sinal por instrucao) mas correcto. */
    printf("\n[modo 2] pstate single-step\n");
    g_mode = 2; g_traps = 0;
    arm(0);
    unsigned long r = guest_continue();
    printf("    x0 = 0x%lx  traps = %d\n", r, g_traps);
    printf("    %s\n\n", (r == kContinued) ? "FUNCIONA" : "nao funciona");

    return 0;
}
