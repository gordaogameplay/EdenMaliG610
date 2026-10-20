/* EdenMaliG610 — trampoline de execução nativa.
 *
 * O mecanismo, numa frase: o tpidr_el0 do processador é a chave que diz, em
 * qualquer trap, se quem levantou a excepção foi o guest ou o host.
 *
 * Porquê o tpidr_el0 e não uma global? Porque os signal handlers correm em
 * contexto interruptível e precisam de encontrar o GuestContext da thread que
 * falhou. O tpidr_el0 é acessível em modo de utilizador sem syscall, é
 * por-thread, e o libc do host já o usa — por isso o motor lá coloca um
 * ThreadParameters, cujo primeiro campo é o GuestContext e o segundo é um
 * valor mágico.
 *
 * Fluxo de um SVC:
 *
 *   guest executa `BRK #n`   (o patcher trocou SVC por BRK)
 *        -> SIGTRAP
 *        -> handler: lê o magic em tpidr_el0
 *                   guest: despacha o serviço, ajusta o pc, retoma
 *                   host:  é um bug nosso — deixa o handler por omissão actuar
 *
 * NOTA SOBRE O `eret`
 * --------------------
 * Não usamos `eret`. O motor corre inteiramente em modo de utilizador (EL0):
 * o guest não tem EL1 e o host também não. Uma trap é um sinal, e o regresso
 * ao guest é um salto normal depois de o handler ter arranjado o estado.
 * Evita-se construir um ucontext completo e é mais rápido — o que importa,
 * porque cada SVC passa por aqui.
 */

#define __ASSEMBLY__

#include "g610/context_offsets.hpp"

/* Sinais usados pelo motor (valores Linux). */
#define IA_SIGTRAP   5
#define IA_SIGBUS    7
#define IA_SIGSEGV  11
#define IA_SIGUSR2  12
#define IA_SIGURG   23

/* Chamadas de sistema AArch64. */

#define IA_NR_gettid 178
#define IA_NR_tgkill 234

/* ------------------------------------------------------------------ */
/* g610_enter_guest — host -> guest                                      */
/*                                                                     */
/* void g610_enter_guest(GuestContext* ctx, ThreadParameters* tp,        */
/*                     void (*code)(void))                             */
/*                                                                     */
/* x0 = ctx, x1 = tp, x2 = endereço de arranque do guest              */
/*                                                                     */
/* Salva o contexto do host, carrega o do guest e salta. Não regressa   */
/* pelo caminho normal: `code` acaba com um BRK que devolve o controlo  */
/* ao host.                                                            */
/* ------------------------------------------------------------------ */
.section .text.g610_enter_guest, "ax", %progbits
.global g610_enter_guest
.type g610_enter_guest, %function
g610_enter_guest:
    /* Guardar o estado do host ANTES de mexer em sp: depois de escrever em
     * sp, o valor original só é recuperável a partir de um registo. */
    mov     x16, sp
    mrs     x17, tpidr_el0

    /* x18 = base do HostContext dentro do GuestContext.
     * x18 (platform register) é de uso restrito no AArch64, mas seguro
     * dentro de código de kernel de utilizador: nada aqui corre código do
     * host que dependa dele. */
    add     x18, x0, #(IA_CTX_HOSTCTX)

    /* Guardar registos general-purpose do host */
    stp     x16, x17, [x18, #(IA_HOST_SP)]
    stp     x19, x20, [x18, #(IA_HOST_REGS +  0)]
    stp     x21, x22, [x18, #(IA_HOST_REGS + 16)]
    stp     x23, x24, [x18, #(IA_HOST_REGS + 32)]
    stp     x25, x26, [x18, #(IA_HOST_REGS + 48)]
    stp     x27, x28, [x18, #(IA_HOST_REGS + 64)]
    stp     x29, x30, [x18, #(IA_HOST_REGS + 80)]

    /* Guardar registos vectoriais d8-d15 (callee-saved em AAPCS64) */
    stp     q8,  q9,  [x18, #(IA_HOST_VREGS +  0)]
    stp     q10, q11, [x18, #(IA_HOST_VREGS + 32)]
    stp     q12, q13, [x18, #(IA_HOST_VREGS + 64)]
    stp     q14, q15, [x18, #(IA_HOST_VREGS + 96)]

    /* Carregar o contexto do guest.
     *
     * O array de GPRs é contíguo (31 × 8 bytes), por isso carrega-se com
     * ldp a partir do base. Os registos x19-x30 são carregados para
     * x19-x30 directamente: o AArch64 permite escrever em qualquer registo
     * de propósito geral a partir de código de assembler, a restrição do
     * AAPCS64 aplica-se ao C, não ao assembly.
     */
    add     x16, x0, #0                    /* base do array de GPRs */

    ldp     x0,  x1,  [x16, #0]
    ldp     x2,  x3,  [x16, #16]
    ldp     x4,  x5,  [x16, #32]
    ldp     x6,  x7,  [x16, #48]
    ldp     x8,  x9,  [x16, #64]
    ldp     x10, x11, [x16, #80]
    ldp     x12, x13, [x16, #96]
    ldp     x14, x15, [x16, #112]
    ldp     x19, x20, [x16, #(19*8)]
    ldp     x21, x22, [x16, #(21*8)]
    ldp     x23, x24, [x16, #(23*8)]
    ldp     x25, x26, [x16, #(25*8)]
    ldp     x27, x28, [x16, #(27*8)]
    ldp     x29, x30, [x16, #(29*8)]

    /* sp e pc do guest */
    ldr     x17, [x0, #(IA_CTX_SP)]
    mov     sp,  x17
    /* o terceiro argumento (endereço de arranque) é o pc efectivo */
    mov     x2,  x2

    /* tpidr_el0 do guest: aponta para o ThreadParameters, que tem o magic
     * e o contexto nativo. Qualquer trap a partir daqui encontra o
     * GuestContext por este caminho. */
    ldr     x17, [x0, #(IA_CTX_TPIDR)]
    msr     tpidr_el0, x17

    /* FPCR/FPSR: instruções sem registo de fonte, não precisa de staging. */
    ldr     w16, [x0, #(IA_CTX_FPCR)]
    msr     fpcr, x16
    ldr     w16, [x0, #(IA_CTX_FPSR)]
    msr     fpsr, x16

    /* Saltar para o código do guest. */
    br      x2
.size g610_enter_guest, . - g610_enter_guest

/* ------------------------------------------------------------------ */
/* g610_yield_to_host — o guest devolve o controlo ao host              */
/*                                                                     */
/* Envia um sinal a si próprio. A volta ao host é sempre feita por um    */
/* handler, nunca por um retorno directo — é o que torna o contexto do  */
/* host recuperável.                                                   */
/*                                                                     */
/* void g610_yield_to_host(void)                                         */
/* ------------------------------------------------------------------ */
.section .text.g610_yield_to_host, "ax", %progbits
.global g610_yield_to_host
.type g610_yield_to_host, %function
g610_yield_to_host:
    mov     x8, #IA_NR_gettid
    svc     #0
    mov     x1, x0                         /* tid */
    mov     x2, #(IA_SIGUSR2)
    mov     x8, #IA_NR_tgkill
    svc     #0
    brk     #1002                          /* inalcançável */
.size g610_yield_to_host, . - g610_yield_to_host

/* ------------------------------------------------------------------ */
/* g610_lock_thread_params — serializa o acesso ao ThreadParameters     */
/*                                                                     */
/* Spinlock com LDAXR/STXR. Sem isto, duas threads a entrar e sair ao  */
/* mesmo tempo corrompem o contexto uma da outra.                      */
/*                                                                     */
/* void g610_lock_thread_params(ThreadParameters* tp)                    */
/* ------------------------------------------------------------------ */
.section .text.g610_lock_thread_params, "ax", %progbits
.global g610_lock_thread_params
.type g610_lock_thread_params, %function
g610_lock_thread_params:
    add     x0, x0, #(IA_TLS_LOCK)
1:  clrex
2:  ldaxr   w1, [x0]
    cbz     w1, 1b                         /* ocupado: espera */
    stxr    w1, wzr, [x0]
    cbnz    w1, 2b                         /* falhou: nova tentativa */
    ret
.size g610_lock_thread_params, . - g610_lock_thread_params

/* ------------------------------------------------------------------ */
/* g610_unlock_thread_params                                            */
/*                                                                     */
/* void g610_unlock_thread_params(ThreadParameters* tp)                  */
/* ------------------------------------------------------------------ */
.section .text.g610_unlock_thread_params, "ax", %progbits
.global g610_unlock_thread_params
.type g610_unlock_thread_params, %function
g610_unlock_thread_params:
    add     x0, x0, #(IA_TLS_LOCK)
    mov     w1, #1                         /* desbloqueado */
    stlr    w1, [x0]
    ret
.size g610_unlock_thread_params, . - g610_unlock_thread_params

.section .note.GNU-stack, "", %progbits
