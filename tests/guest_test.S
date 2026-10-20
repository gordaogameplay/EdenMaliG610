// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* Guest sintético para validar a execução nativa.
 *
 * Não é código de um jogo: é o menor programa que exercita o ciclo completo
 * do motor —
 *
 *   1. executar instruções AArch64 nativamente (sem qualquer tradução)
 *   2. levantar uma trap por SVC
 *   3. o handler despachar o serviço e ler os argumentos de x0-x5
 *   4. o resultado voltar para x0
 *   5. a execução continuar na instrução seguinte
 *   6. ExitProcess terminar o guest
 *
 * Se o motor funcionar, x0 tem de acabar com 49 (42 + 7) e o host tem de
 * receber ExitProcess.
 *
 * O código de cada SVC tem de passar por `svc #n`, que o patcher transforma
 * em `brk #n`. Sem o patch, o kernel de utilizador trata um SVC como
 * illegal instruction e o ciclo não é o que estamos a testar.
 */

/* Entry point do guest. Chamado pelo trampoline com sp válido. */
    .text
    .global guest_entry
    .type guest_entry, %function
guest_entry:
    /* x0 = 42 */
    mov     x0, #42
    /* SVC #1 (SetHeapSize, número real 0x01) — Success(0) em x0 */
    svc     #1
    /* repor o valor de teste: o serviço pode ter alterado x0 */
    mov     x0, #42
    /* x1 = 7 */
    mov     x1, #7
    /* SVC #31 (ConnectToNamedPort, número real 0x1F) */
    svc     #31
    /* x0 = 42 + x1 = 49 */
    add     x0, x0, x1
    /* SVC #7 (ExitProcess, número real 0x07) — termina o guest */
    svc     #7
    /* Se chegarmos aqui, o ExitProcess não parou o guest. Este BRK é um
     * marcador que o host reconhece como falha do motor. */
    brk     #999  /* marcador: ExitProcess não parou o guest */
    ret
    guest_entry_end:
    .size guest_entry, guest_entry_end - guest_entry

/* Guest de fault: toca um endereço sem permissão, para testar a recuperação.
 * O símbolo aponta para o início; o teste escreve-o com as permissões
 * certas para ler a instrução mas falhar o acesso de dados. */
    .text
    .global guest_fault
    .type guest_fault, %function
guest_fault:
    mov     x0, #0
    ldr     x1, [x0]      /* x0 = 0 -> SIGSEGV esperado */
    mov     x0, #0xBAD     /* se chegar aqui, o fault nao travou o guest */
    ret
    guest_fault_end:
    .size guest_fault, guest_fault_end - guest_fault

/* Guest mínimo: um único SVC, para contar traps com precisão. */
    .text
    .global guest_one_svc
    .type guest_one_svc, %function
guest_one_svc:
    mov     x0, #123
    svc     #7              /* ExitProcess imediato (0x07) */
    brk     #998
    ret
    guest_one_svc_end:
    .size guest_one_svc, guest_one_svc_end - guest_one_svc
    .global guest_entry_end, guest_fault_end, guest_one_svc_end

.section .note.GNU-stack, "", %progbits
