/* EdenMaliIA — entrada no guest.
 *
 * Troca o registo de stack pelo do guest e salta para o código do guest. A
 * partir daí o código AArch64 corre nativamente no Cortex-A76: sem tradução,
 * sem interpreter, sem blocos de JIT. As instruções que precisam de tratamento
 * já foram reescritas em memória pelo patcher estático (SVC -> BRK), por isso
 * não há nada a reescrever aqui.
 *
 * ---------------------------------------------------------------------------
 * PORQUÊ ISTO ESTÁ EM ASSEMBLY E NÃO EM C++
 * ---------------------------------------------------------------------------
 * Trocar `sp` dentro de uma função C++ destrói o stack protector do caller.
 *
 * O problema foi medido, não suposto. Com `-fstack-protector-strong` (o
 * default do Ubuntu), a função `run()` põe um canário no seu frame. Se o
 * trampoline fizer `mov sp, sp_do_guest`, e a execução voltar a `run()` por
 * um caminho que não restaure `sp`, o epílogo verifica o canário numa stack
 * que não é a sua:
 *
 *     *** stack smashing detected ***: terminated
 *
 * Guardar o `sp` original em x19 não resolve — o guest clobbera x19, e a
 * verificação acontece no regresso, com o canário já inalcançável.
 *
 * ---------------------------------------------------------------------------
 * CONTRATO DE ENTRADA — O QUE O GUEST ESPERA ENCONTRAR
 * ---------------------------------------------------------------------------
 * O entry de um programa de Switch NÃO é uma função C. Recebe um contexto
 * preparado por quem o chamou, e a primeira coisa que faz é TESTÁ-lo:
 *
 *     cmp     x0, #0
 *     b.ne    <outro caminho>
 *
 * x0 é um ARGUMENTO, não o endereço do entry. É o que o Eden faz em
 * `ResetThreadContext64` (src/core/hle/kernel/k_thread.cpp:58):
 *
 *     ctx = {};                    // TODOS os registos a zero
 *     ctx.r[0]  = arg;             // arg = 0 (ver KProcess::Run)
 *     ctx.r[18] = Random64() | 1;  // platform register = stack canary
 *     ctx.pc    = entry_point;
 *     ctx.sp    = stack_top;
 *
 * Duas consequências que custaram tempo a descobrir:
 *
 *   1. x18 tem de ser uma canary válida. Sem isso o primeiro
 *      `__stack_chk_fail` do guest chama `svc #ReturnFromException` — que foi
 *      exactamente o primeiro SVC que o rtld real nos pediu.
 *
 *   2. TODOS os registos têm de estar a zero. O `br x0` do trampoline
 *      deixava x0 = endereço do entry; o `cmp x0, #0` falhava, o guest ia
 *      pelo caminho errado e acabava num `b .` a girar para sempre sem
 *      levantar uma única trap. O sintoma — "o guest não faz nada" — não
 *      aponta de forma nenhuma para "os registos de entrada estão errados".
 *
 * Custo: ~35 instruções por ENTRADA no guest, não por chamada de serviço.
 * Irrelevante face a guardar 31 GPRs em cada SVC.
 *
 * ---------------------------------------------------------------------------
 * O QUE ACONTECE QUANDO O GUEST REGRESSA
 * ---------------------------------------------------------------------------
 * Um guest real nunca regressa: termina com `svc #ExitProcess` ou leva um
 * fault, e ambos chegam ao host por `siglongjmp`, que restaura `sp` e os
 * registos callee-saved. Nesse caminho o canário de `run()` é válido.
 *
 * O caso patológico é o guest fazer `ret` sem ExitProcess. Para que também
 * ele passe pelo mecanismo de recuperação, `ia_call_guest` empurra para a
 * stack do guest o endereço de `ia_guest_returned`:
 *
 *   guest faz `ret`   -> salta para ia_guest_returned
 *   ia_guest_returned -> brk #0xFFFD
 *   handler           -> escreve sp = recovery_sp, pc = recovery_pc
 *   recovery_pc       -> siglongjmp de volta a run()
 *
 * ---------------------------------------------------------------------------
 * IMPLEMENTAÇÃO
 * ---------------------------------------------------------------------------
 * O endereço de entrada é guardado NA STACK DO GUEST, não num registo nem num
 * global. Porquê: a rotina abaixo limpa todos os registos, e um registo (ou
 * um global apontado por um) seria apagado antes de ser usado. A stack do
 * guest é o único sítio que sobrevive ao cleaning, e é dela que o próprio
 * guest vai ler quando fizer `ret`.
 */

/* void ia_call_guest(void (*code)(), unsigned long sp, unsigned long platform_reg,
 *                    unsigned long guest_x1)
 *
 * x0 = endereço de entrada do guest
 * x1 = valor para sp (topo da stack do guest)
 * x2 = valor para x18 (platform register = stack canary do guest)
 * x3 = valor para x1 do guest (o handle da thread principal)
 *
 * ---------------------------------------------------------------------------
 * O x1 DO GUEST
 * ---------------------------------------------------------------------------
 * O Eden passa o handle da thread principal no x1 do entry point
 * (`k_process.cpp:1006-1010`):
 *
 *     } else {                                  // entrada NSO, sem ABI homebrew
 *         main_thread->GetContext().r[0] = 0;
 *         main_thread->GetContext().r[1] = thread_handle;
 *     }
 *
 * E o SDK da Nintendo depende disso: o crt0 dele copia o handle para
 * `nn::os::detail::g_OsBootParamter`, e o `SdkMutexType::TryLock` compara o
 * dono do mutex com esse valor.
 *
 * Sem isto, o handle chegava a zero — e zero é **exactamente** o valor que um
 * mutex livre tem no campo "dono". O SDK comparava 0 com 0, concluía que o
 * mutex já era seu, e abortava com `this == NULL`.
 *
 * A medição que levou aqui: o `g_OsBootParamter` do SDK estava todo a zeros, e
 * o dono do mutex também.
 */
.section .text.ia_call_guest, "ax", %progbits
.global ia_call_guest
.type ia_call_guest, %function
ia_call_guest:
    /* Endereço para onde um `ret` do guest deve cair. */
    adrp    x4, ia_guest_returned
    add     x4, x4, :lo12:ia_guest_returned

    /* Duas palavras de 16 bytes abaixo do topo da stack do guest. Subtrair
     * 16 (e não 8) preserva o alinhamento a 16 exigido pela AAPCS64.
     *
     * Layout, do topo para baixo:
     *   sp + 0   endereço de entrada (para o trampoline recuperar)
     *   sp + 16  endereço de retorno de segurança (para o guest)
     *   sp + 32  handle da thread principal (nosso; lido antes do salto)
     *
     * A terceira palavra é a novidade: o handle do x1 do guest.
     *
     * **Não pode ir num registo.** A primeira versão punha-o em x16
     * (`mov x16, x3`) e o loop de limpeza a seguir apagava-o — o x1 do guest
     * chegava ao rtld a zero, e o SDK abortava. A medição: o
     * `nn::os::detail::g_OsBootParamter` do SDK ficava a zeros e o dono do
     * mutex também, e o `SdkMutexType::TryLock` comparava zero com zero.
     *
     * E **não muda o sp do guest**: a reserva passa a 48 bytes e o `add sp`
     * final passa a 32, o que dá o mesmo `stack_top - 16` de antes. A palavra
     * do handle fica ACIMA do sp do guest, numa zona que o entry não usa.
     */
    sub     x1, x1, #48
    str     x0, [x1]          /* entrada */
    str     x4, [x1, #16]      /* retorno de segurança */
    str     x3, [x1, #32]      /* handle da thread principal */
    mov     sp, x1

    mov     x18, x2            /* platform register: stack canary do guest */

    /* ------------------------------------------------------------------
     * Limpar os registos — o contrato do guest.
     *
     * x18 fica de fora (é a canary) e x0 é o último a ser limpo, porque é
     * logo a seguir reposto a partir da stack.
     * ------------------------------------------------------------------ */
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr          /* fp */
    mov     x30, xzr          /* lr: quem chamou foi o kernel, não uma função */

    /* x0 tem de ser 0: é o ARGUMENTO (KProcess::Run passa 0), e a PRIMEIRA
     * instrução do entry faz `cmp x0, #0` sobre ele.
     *
     * O endereço de entrada NÃO pode ir em x0. Foi o que aconteceu durante
     * muito tempo — `mov x0, xzr` seguido de `ldr x0, [sp]` — e o guest via
     * `cmp x0, #0` falhar, saltava para o caminho alternativo e acabava num
     * `b .` a girar para sempre. O sintoma — "o guest não faz nada" — não
     * aponta de forma nenhuma para "estou a carregar o endereço no registo
     * errado".
     *
     * O destino vai em x17 (IP1), que a AAPCS64 declara como descartável:
     * qualquer código tem de assumir que o chamador o estragou. É o registo
     * mais expênsico de todos, e por isso o mais seguro para transportar o
     * destino durante o cleaning.
     */
    mov     x0,  xzr          /* o argumento: 0 */
    add     sp,  sp, #32      /* sp do guest fica 32 bytes acima */
    ldr     x1,  [sp]         /* o handle da thread principal (ver o cabecalho) */
    ldr     x17, [sp, #-32]   /* endereço de entrada ([sp-32], nao [sp-16]) */
    br      x17
.size ia_call_guest, . - ia_call_guest

/* ---------------------------------------------------------------------------
 * ia_guest_returned — o guest fez `ret` sem ter chamado ExitProcess
 *
 * Só é alcançado por um `ret` do guest que consuma o endereço que
 * `ia_call_guest` deixou na stack. Produz uma trap em vez de saltar para o
 * vazio, para que o handler tenha uma saída limpa e o relatório diga o que
 * aconteceu.
 * --------------------------------------------------------------------------- */
.section .text.ia_guest_returned, "ax", %progbits
.global ia_guest_returned
.type ia_guest_returned, %function
ia_guest_returned:
    /* BRK #0xFFFD — o handler reconhece como "o guest regressou". */
    brk     #0xFFFD
.size ia_guest_returned, . - ia_guest_returned

.section .note.GNU-stack, "", %progbits