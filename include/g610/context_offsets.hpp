// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
/* EdenMaliG610 — offsets partilhados entre C++ e assembly.
 *
 * Este header é o ÚNICO que o assembly inclui. Não pode ter nada de C++:
 * o assembler do GCC não tem <array>, <cstdint> nem <cstddef>.
 *
 * A separação existe para que a correspondência entre o layout em C++ e os
 * índices usados no assembly seja verificável. Cada constante aqui é
 * verificada por um `static_assert` em `guest_context.hpp`; se um campo
 * mudar de sítio, o build falha em vez de o assembly escrever no sítio
 * errado — que num contexto de registos significa um salto para lixo.
 */
#pragma once

/* Este header tem duas audiences, e por isso e dividido em duas:
 *
 *  - O C++ le as constantes `constexpr` e usa-as nos `static_assert` que
 *    verificam o layout.
 *  - O assembly (src/g610_trampoline.S) nao percebe `namespace` nem
 *    `constexpr`, por isso usa as macros `IA_*` definidas mais abaixo, cujo
 *    valor tem de ser igual ao das constantes.
 *
 * As duas metades sao verificadas uma contra a outra pelos static_assert no
 * fim deste header. E por isso que nao basta uma unica fonte: o assembler do
 * GCC so aceita a sintaxe de preprocessador, nao a de C++.
 */
#pragma once

#define IA_PP_CAT_(a, b) a##b
#define IA_PP_CAT(a, b)  IA_PP_CAT_(a, b)

/* --- metade do assembly ------------------------------------------------ */
/* Os valores reais sao os medidos por sizeof/offsetof nas structs reais. */
#define IA_HOST_REGS   0x00
#define IA_HOST_VREGS  0x60
#define IA_HOST_SP     0xA0
#define IA_HOST_TPIDR  0xA8
#define IA_HOST_CTXSZ  0xC0

#define IA_CTX_GPR       0x000
#define IA_CTX_SP        0x0F8
#define IA_CTX_PC        0x100
#define IA_CTX_TPIDRRO   0x108
#define IA_CTX_TPIDR     0x110
#define IA_CTX_FPCR      0x118
#define IA_CTX_FPSR      0x11C
#define IA_CTX_PSTATE    0x120
#define IA_CTX_SVC       0x124
#define IA_CTX_HALT      0x128
#define IA_CTX_HOSTCTX   0x130

#define IA_TLS_CTX      0x00
#define IA_TLS_MAGIC    0x08
#define IA_TLS_LOCK     0x0C
#define IA_TLS_SIZE     0x10

#ifndef __ASSEMBLY__

namespace g610 {

namespace off {

/* Dentro de HostContext.
 *
 * saved_regs ocupa 12×8 = 0x60 bytes e saved_vregs 8×8 = 0x40, logo sp fica
 * em 0xA0. O pad leva o tamanho a 0xC0, que é um múltiplo de 16 e portanto
 * alinha bem antes do HostContext de dentro do GuestContext. */
inline constexpr unsigned kHostRegs  = IA_HOST_REGS;
inline constexpr unsigned kHostVregs = IA_HOST_VREGS;
inline constexpr unsigned kHostSp    = IA_HOST_SP;
inline constexpr unsigned kHostTpidr = IA_HOST_TPIDR;
inline constexpr unsigned kHostCtxSz = IA_HOST_CTXSZ;

/* Dentro de GuestContext.
 *
 * cpu_registers ocupa 31×8 = 0xF8, por isso sp cai exactamente no limite e
 * pc em 0x100. O HostContext (0xC0, alinhado a 16) começa em 0x130. */
inline constexpr unsigned kCtxGpr        = IA_CTX_GPR;
inline constexpr unsigned kCtxSp         = IA_CTX_SP;
inline constexpr unsigned kCtxPc         = IA_CTX_PC;
inline constexpr unsigned kCtxTpidrroEl0 = IA_CTX_TPIDRRO;
inline constexpr unsigned kCtxTpidrEl0   = IA_CTX_TPIDR;
inline constexpr unsigned kCtxFpcr       = IA_CTX_FPCR;
inline constexpr unsigned kCtxFpsr       = IA_CTX_FPSR;
inline constexpr unsigned kCtxPstate     = IA_CTX_PSTATE;
inline constexpr unsigned kCtxSvc        = IA_CTX_SVC;
inline constexpr unsigned kCtxHalt       = IA_CTX_HALT;
inline constexpr unsigned kCtxHostCtx    = IA_CTX_HOSTCTX;

/* Dentro do ThreadParameters — o que o tpidr_el0 aponta. */
inline constexpr unsigned kTlsCtx       = IA_TLS_CTX;
inline constexpr unsigned kTlsMagicWord = IA_TLS_MAGIC;
inline constexpr unsigned kTlsLock      = IA_TLS_LOCK;
inline constexpr unsigned kTlsCtxSize   = IA_TLS_SIZE;

} // namespace off

} // namespace g610

#endif // __ASSEMBLY__
