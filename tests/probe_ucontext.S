// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* Sonda decisiva: o handler pode manipular o ucontext para o guest continuar?
 *
 * A pergunta que decide o desenho do motor. O handler recebe um ucontext com
 * o PC a apontar para o proprio BRK. Se escrever pc + 4 e regressar, o
 * kernel retoma DEPOIS do BRK e o guest continua — e todo o motor de SVC
 * pode ser escrito em C++ normal, sem troca de contexto em assembly.
 *
 * Se o kernel ignorar a alteracao, o guest re-executa o mesmo BRK em ciclo
 * infinito e o motor TEM de trocar os registos de hardware dentro do handler
 * (o caminho do Eden, com ~20 instrucoes de assembly por SVC).
 *
 * guest_continue() devolve um marcador se a execucao passou do BRK.
 */
#define __ASSEMBLY__

    .text
    .global guest_continue
    .type guest_continue, %function
guest_continue:
    mov     x0, #0          /* valor inicial, distingue-se do marcador */
    brk     #3              /* o handler deve avancar o PC */
    mov     x0, #0x600D     /* "continuou": marca de passagem */
    ret
    .size guest_continue, . - guest_continue

/* guest_noadvance: o handler NAO avanca o PC. Se voltar a zero, o kernel
 * honrou o PC original (a trap repete); se devolver o marcador, o kernel
 * ignorou a manipulacao. */
    .text
    .global guest_noadvance
    .type guest_noadvance, %function
guest_noadvance:
    mov     x0, #0
    brk     #4
    mov     x0, #0x600D
    ret
    .size guest_noadvance, . - guest_noadvance

/* guest_args: testa a passagem de argumentos e resultado.
 *   x0 = 11 (argumento)
 *   SVC #6  -> o handler soma 100 ao x0
 *   devolve x0 */
    .text
    .global guest_args
    .type guest_args, %function
guest_args:
    mov     x0, #11
    brk     #6
    ret
    .size guest_args, . - guest_args

.section .note.GNU-stack, "", %progbits
