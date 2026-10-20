// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* guest_test.S contem um SVC #3 (ExitProcess). Ao executa-lo nativamente
 * fora do motor, o SVC levanta SIGILL/SIGTRAP no kernel Linux de utilizador.
 * Este teste documenta o que o kernel entrega ao handler:
 *   - que sinal
 *   - qual o PC no ucontext
 *   - se o SVC# foi lido antes ou depois do patch (BRK)
 *
 * E o que determina se o handler pode alterar o contexto de forma util.
 */
#define __ASSEMBLY__

    .text
    .global probe_trap
    .type probe_trap, %function
probe_trap:
    mov     x0, #42
    /* SVC cru: o kernel de utilizador trata como illegal instruction */
    svc     #0
    brk     #1000
    ret
    .size probe_trap, . - probe_trap

/* guest_test_brk: mesma coisa mas com BRK, que o patcher produz.
 * BRK gera SIGTRAP com si_addr = o endereco do proprio BRK. */
    .text
    .global probe_brk
    .type probe_brk, %function
probe_brk:
    mov     x0, #42
    brk     #3
    mov     x1, #7
    ret
    .size probe_brk, . - probe_brk
