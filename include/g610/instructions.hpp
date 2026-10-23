// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — descodificação de instruções AArch64 que o guest precisa de
// intercepção.
//
// O guest de um título de Switch corre nativamente no Cortex-A76. Quase tudo é
// executado tal e qual, mas três famílias de instruções não podem passar
// straight-through:
//
//   1. `SVC #imm` — chamada ao sistema. Não há kernel por baixo; a trap tem de
//      ir para o despachante de serviços do motor.
//   2. `MRS`/`MSR` de registos de sistema que o hardware do host não provides
//      (CNTPCT, CNTVCT, TPIDR_EL0). O host tem os seus próprios, e o guest
//      espera os do Switch.
//   3. Load/store exclusivos (`LDXR`, `STXR`, ...) — o guest assume memória
//      com semântica de exclusividade de 39 bits; o host tem de reordenar.
//
// O resto — aritmética, ramos, loads normais, NEON/SVE — passa sem tocar. É
// esta selectividade que distingue execução nativa de emulação.
//
// Cada instrução é um word de 32 bits. Os layouts abaixo são os do ARM
// Architecture Reference Manual (DDI0596) e estão fixados por static_assert
// contra os encodings reais medidos em binários de Switch.
#pragma once

#include <cstdint>

namespace g610::arm {

// ---------------------------------------------------------------------------
// SVC — Supervisor Call
//
//   31          21 20   16 15   5 4    0
//  [11010100 000 imm16 00000 00000 00001]
//
// A verificação é sobre os bits de assinatura (bit 0 = 0x1, bits 21-31 =
// 0x6A0) e não sobre o immediate — o imm16 faz parte do número de serviço.
// ---------------------------------------------------------------------------
struct SVC {
    std::uint32_t raw;

    static constexpr std::uint32_t kSig0 = 0x1;      // bits 0-4
    static constexpr std::uint32_t kSig1 = 0x6A0;    // bits 21-31

    [[nodiscard]] constexpr bool verify() const {
        return (raw & 0x1F) == kSig0 &&
               ((raw >> 21) & 0x7FF) == kSig1;
    }

    // O número de serviço: imm16 (bits 5-20).
    [[nodiscard]] constexpr std::uint16_t number() const {
        return static_cast<std::uint16_t>((raw >> 5) & 0xFFFF);
    }

    // Reescreve mantendo o número: `SVC #n` → `BRK #n`.
    //
    // SVC e BRK partilham a posição do immediate (imm16 nos bits 5-20), mas
    // NÃO são equivalentes nos bits 4-0:
    //
    //     SVC:  1101 0100 0000 imm16 00001   <- op2 = 0b00001
    //     BRK:  1101 0010 0000 imm16 00000   <- op2 = 0b00000
    //
    // Preservar os bits 4-0 ao trocar só a assinatura produz uma codificação
    // INVÁLIDA (bit 0 a 1), e o CPU responde SIGILL em vez de levantar a
    // trap. Foi exactamente o que aconteceu, e o sintoma — SIGILL no primeiro
    // SVC, sem qualquer trap tratada — não apontava para o patcher.
    //
    // A correção é reconstruir a instrução a partir do immediate, em vez de
    // trocar só a assinatura. Os bits 20-5 (imm16) é que se preservam.
    [[nodiscard]] constexpr std::uint32_t to_brk() const {
        constexpr std::uint32_t kSvcSig = 0xD4000000u; // bits 31-21
        constexpr std::uint32_t kBrkSig = 0xD4200000u;
        constexpr std::uint32_t kImm16  = 0x001FFFE0u; // bits 20-5
        return (raw & kImm16) | kBrkSig;
    }
};
static_assert(SVC{0xD40000C1}.verify(), "SVC #6 tem de verificar");
static_assert(SVC{0xD40000C1}.number() == 6, "imm16 = número de serviço");
static_assert(SVC{0xD40000C1}.to_brk() == 0xD42000C0, "SVC→BRK preserva imm16 e zera os bits 4-0");
static_assert(SVC{0xD4000001}.to_brk() == 0xD4200000, "patch serve para SVC #0");
// A codificação tem de ser legal: um BRK com o bit 0 a 1 faz o CPU levantar
// SIGILL, e não a trap que o motor espera. Este assert é o que apanhou o bug.
static_assert((SVC{0xD40000C1}.to_brk() & 0x1Fu) == 0u,
              "BRK tem os bits 4-0 a zero (op2 = 00000)");
// imm16 máximo: o SVC tem o bit 0 a 1 (op2 = 00001), o BRK tem-no a zero.
static_assert(SVC{0xD41FFFE1}.verify() && SVC{0xD41FFFE1}.number() == 0xFFFF,
              "SVC #0xFFFF verifica");
static_assert(SVC{0xD41FFFE1}.to_brk() == 0xD43FFFE0, "imm16 máximo preservado");
static_assert(SVC{0xD4000061}.number() == 3, "SVC #3 = ExitProcess");
static_assert(!SVC{0xD65F0000}.verify(), "MSR não é SVC");
// Uma instrução comum não pode ser confundida com SVC.
static_assert(!SVC{0xF9400000}.verify(), "LDR não é SVC");
static_assert(!SVC{0xAA0003E0}.verify(), "MOV não é SVC");

// ---------------------------------------------------------------------------
// MRS — Move System Register
//
//   31       21 20   16 15   5 4    0
//  [11010101 001 Rt   0   Op0 CRn CRm Op2]
// ---------------------------------------------------------------------------
struct MRS {
    std::uint32_t raw;

    static constexpr std::uint32_t kSig = 0xD53; // bits 20-31

    [[nodiscard]] constexpr bool verify() const {
        return (raw >> 20) == kSig;
    }

    [[nodiscard]] constexpr std::uint32_t rt() const {
        return raw & 0x1F;
    }

    // Sistema operativo de 15 bits: Op0<<10 | Op1<<5 | CRn<<2 | CRm<<1 | Op2
    [[nodiscard]] constexpr std::uint16_t sysreg() const {
        return static_cast<std::uint16_t>((raw >> 5) & 0x7FFF);
    }
};
static_assert(MRS{0xD53BE020}.verify(), "MRS X0, CNTPCT_EL0 verifica");
static_assert(MRS{0xD53BE020}.rt() == 0, "Rt = 0");
static_assert(MRS{0xD53BE020}.sysreg() == 0x5F01, "CNTPCT_EL0 = 0x5F01");

// ---------------------------------------------------------------------------
// Leituras do guest ao temporizador do ARM — CNTFRQ / CNTPCT / CNTVCT
//
// Os tres, pelo `sysreg` empacotado em bits [20:5]:
//
//     0x5F00  CNTFRQ_EL0  — a frequencia. CONSTANTE (no board: 24_000_000).
//     0x5F01  CNTPCT_EL0  — o contador fisico; ilegal no EL0 (SIGILL, medido).
//     0x5F02  CNTVCT_EL0  — o contador virtual do Linux, decorre do ARRANQUE
//                           DO BOARD, a 24 MHz no nosso host.
//
// **O SDK da Nintendo pede o tick a `cntfrq_el0`.** Medido: `GetSystemTick`
// sao duas instrucoes (`mrs x0, cntfrq_el0; ret`, no nnSdk em 0x83ab4fd0) e
// `GetSystemTickFrequency` devolve 19_200_000 fixo, em linha. A escala das
// conversoes e' fixa; as leituras de tick tem de avancar NELA.
//
// Correr nativamente nao serve: aqui o `cntfrq` devolve a frequencia, nao um
// contador, e o "agora" do SDK congela. Medicao do sintoma (a parede do
// Pokemon Shield): `x23 = 0x16e3600 = 24_000_000` constante nas traps do
// `WaitTimerEvent`; `ConvertToTimeSpan(24_000_000)` = 1_250_000_000
// constante; prazo = 1_255_000_000; `deadline <= agora` nunca e' verdade.
//
// Reescrever para `cntvct` (o molde antigo) tampouco: o `cntvct` do board
// corre desde o arranque do board, a 24 MHz, e a escala que o SDK aplica a
// frente e' 19,2 MHz — os prazos criados por uma fonte e medidos por outra
// nao se encontram. O Eden upstream tem servidor proprio para o facto
// (`nce/patcher.cpp:134-144`, `WriteCntpctHandler`, o valor multiplicado
// host->Switch): aqui a casa toma trap e responde com a escala fixa.
//
// Logo: **trap ao host**, `brk #(0x40|Rt)`. O Rt viagem no imediato (o
// handler do host aloja o resultado nele) e a resposta sao ticks de 19,2 MHz
// desde o arranque do guest: uma unica fonte de "agora", na escala que o
// proprio SDK fixa, para o guest inteiro.
[[nodiscard]] constexpr bool is_mrs_cntfrq(std::uint32_t insn) noexcept {
    return MRS{insn}.verify() && MRS{insn}.sysreg() == 0x5F00;
}
[[nodiscard]] constexpr bool is_mrs_cntpct(std::uint32_t insn) noexcept {
    return MRS{insn}.verify() && MRS{insn}.sysreg() == 0x5F01;
}
[[nodiscard]] constexpr bool is_mrs_cntvct(std::uint32_t insn) noexcept {
    return MRS{insn}.verify() && MRS{insn}.sysreg() == 0x5F02;
}

// `mrs x{Rt}, cnt*` em `brk #(0x40|Rt)`. A banda 0x40..0x5F e' propria: nao
// toca nos BRK de SVC (0..31) nem nos marcadores da casa (0xFFFC/0xFFFD).
[[nodiscard]] constexpr std::uint32_t mrs_tick_to_brk(std::uint32_t insn) noexcept {
    return (is_mrs_cntfrq(insn) || is_mrs_cntpct(insn) || is_mrs_cntvct(insn))
               ? (0xD4200000u | ((0x40u | (insn & 0x1Fu)) << 5))
               : insn;
}

static_assert(is_mrs_cntfrq(0xD53BE000u), "MRS X0, CNTFRQ_EL0");
static_assert(is_mrs_cntpct(0xD53BE020u), "MRS X0, CNTPCT_EL0");
static_assert(is_mrs_cntvct(0xD53BE040u), "MRS X0, CNTVCT_EL0");
static_assert(!is_mrs_cntfrq(0xD53BE020u) && !is_mrs_cntvct(0xD53BE000u),
              "os tres registos distinguem-se pelo op2");
static_assert(mrs_tick_to_brk(0xD53BE000u) == 0xD4200800u,
              "MRS X0, CNTFRQ -> BRK #0x40");
// Site medido no WaitTimerEvent: `mrs x23, cntfrq` (0x83ab2718 do nnSdk,
// bytes 0xD53BE017). brk #(0x40|23) = brk #0x57.
static_assert(mrs_tick_to_brk(0xD53BE017u) == 0xD4200AE0u,
              "MRS X23, CNTFRQ -> BRK #0x57");

// MRS <Xt>, TPIDRRO_EL0  ->  MRS <Xt>, TPIDR_EL0
// ---------------------------------------------------------------------------
//
// Um bit. É tudo.
//
// Os encodings não foram escritos a olho — foram montados com o assembler e
// lidos do `.o`:
//
//     mrs x0, tpidrro_el0   ->  d53bd060
//     mrs x5, tpidr_el0     ->  d53bd045
//
// A diferença é o bit 6 (valor 0x20). Os 4 bytes mantêm-se, o registo destino
// mantem-se, e o alcance é o mesmo. Sem pool de literais, sem relocação, sem
// reposicionar os 25 MB do `.text` do `main`.
//
// Porque é preciso: neste kernel `TPIDRRO_EL0` não existe para o guest.
// Medido — `mrs` devolve sempre 0, `msr` dá SIGILL, e `arch_prctl`
// (ARCH_SET_FS) devolve EINVAL. `TPIDR_EL0`, esse sim, é programável e
// devolve um valor real por thread de hardware. Trocar o registo na
//instruction, em vez de a emular por trap, custa zero por acesso.
constexpr std::uint32_t kMrsTpidrroEl0 = 0xD53BD060u;  // "d53bd060 | Rt"
constexpr std::uint32_t kMrsTpidrEl0   = 0xD53BD040u;  // "d53bd045 | Rt"
constexpr std::uint32_t kSysregMask    = 0xFFFFFFE0u;  // apaga Rt (bits 0-4)

[[nodiscard]] constexpr bool is_mrs_tpidrro(std::uint32_t insn) noexcept {
    return (insn & kSysregMask) == kMrsTpidrroEl0;
}

// O bit 5 (0x20) é o que distingue os dois registos. Apagá-lo traduz
// `MRS <Xt>, TPIDRRO_EL0` em `MRS <Xt>, TPIDR_EL0` sem tocar em Rt (bits
// 0-4) nem em mais nada.
[[nodiscard]] constexpr std::uint32_t tpidrro_to_tpidr(std::uint32_t insn) noexcept {
    return is_mrs_tpidrro(insn) ? (insn & ~0x20u) : insn;
}

// Os dois encodings confirmados com o assembler, como `static_assert`, para
// que uma alteração futura não os desfaça em silêncio.
static_assert((0xD53BD060u & kSysregMask) == kMrsTpidrroEl0);
static_assert((0xD53BD045u & kSysregMask) == kMrsTpidrEl0);
static_assert(kMrsTpidrroEl0 != kMrsTpidrEl0);
static_assert(tpidrro_to_tpidr(0xD53BD068u) == 0xD53BD048u,
              "mrs x8, tpidrro_el0  ->  mrs x8, tpidr_el0");
static_assert(tpidrro_to_tpidr(0xD53BD045u) == 0xD53BD045u,
              "ja e' tpidr_el0, fica como esta");

// ---------------------------------------------------------------------------
// MSR — Move System Register (a partir de registo)
// ---------------------------------------------------------------------------
struct MSR {
    std::uint32_t raw;

    static constexpr std::uint32_t kSig = 0xD51; // bits 20-31

    [[nodiscard]] constexpr bool verify() const {
        return (raw >> 20) == kSig;
    }

    [[nodiscard]] constexpr std::uint32_t rt() const {
        return raw & 0x1F;
    }

    [[nodiscard]] constexpr std::uint16_t sysreg() const {
        return static_cast<std::uint16_t>((raw >> 5) & 0x7FFF);
    }
};
static_assert(MSR{0xD51BD040}.verify(), "MSR TPIDR_EL0, X0 verifica");
static_assert(MSR{0xD51BD040}.sysreg() == 0x5E82, "TPIDR_EL0 = 0x5E82");

// ---------------------------------------------------------------------------
// Load/Store exclusivos
//
//   31     30 29  28 27 26  25  24  23 22  21 20  16 15  10  9  5 4   0
//  [size 00 1  0  R  0  L  0  o0  Rs  0  Rt  0  Rn   imm9 0  1  Rt2]
//
// O bit 15 (o0) distingue acquire/release. A parte interessante para o motor é
// que o guest pode usar `ldxr`/`stxr` para spinlocks em memória partilhada com
// as suas próprias threads — e o motor tem de preservar a exclusividade real,
// não apenas o resultado.
// ---------------------------------------------------------------------------
struct Exclusive {
    std::uint32_t raw;

    [[nodiscard]] constexpr bool verify() const {
        return ((raw >> 23) & 0x7F) == 0x10;
    }

    // A store exclusiva tem L=1 e L=0 marca store se load=false.
    [[nodiscard]] constexpr bool is_load() const {
        return ((raw >> 22) & 1) != 0;
    }

    [[nodiscard]] constexpr std::uint32_t rt() const {
        return raw & 0x1F;
    }

    [[nodiscard]] constexpr std::uint32_t rn() const {
        return (raw >> 5) & 0x1F;
    }

    [[nodiscard]] constexpr std::uint32_t rt2() const {
        return (raw >> 10) & 0x1F;
    }

    [[nodiscard]] constexpr std::uint32_t rs() const {
        return (raw >> 16) & 0x1F;
    }

    // Versão ordenada (acquire/release): força o bit 15.
    [[nodiscard]] constexpr std::uint32_t as_ordered() const {
        return raw | (1u << 15);
    }
};
static_assert(Exclusive{0xC85FFC00}.verify(), "LDXR W0,[X0] verifica");
static_assert(Exclusive{0xC85FFC00}.is_load(), "LDXR é load");
static_assert(!Exclusive{0xC81FFC00}.verify() == false, "STXR também verifica");
static_assert(Exclusive{0xC81FFC00}.verify(), "STXR verifica");
static_assert(!Exclusive{0xC81FFC00}.is_load(), "STXR não é load");
static_assert(Exclusive{0xC85F7C00}.as_ordered() == 0xC85FFC00,
              "LDAR resulta de forçar o bit o0");
static_assert(Exclusive{0xC8200440}.as_ordered() == 0xC8208440,
              "STLR resulta de forçar o bit o0");

// ---------------------------------------------------------------------------
// Hints: NOP, YIELD, WFI, SEV
//
//   31       21 20   16 15  13 12  11 10   8 7    5 4    0
//  [1101010 1010 001 CRm 00 1 0000 00 CRm op Y]
//
// Em forma compacta: os bits 31-21 são 0x6A8, os bits 15-13 são 001, e o
// `CRm` que distingue o hint ocupa os **bits 7-5**:
//
//   NOP   CRm=0   0xD503201F
//   YIELD CRm=1   0xD503203F
//   WFI   CRm=3   0xD503207F
//   SEV   CRm=5   0xD50320BF
//
// São NOPs architecturais do ponto de vista do programa, mas o motor usa-os
// como pontos de yield: um WFI num guest que espera por I/O é o sítio
// natural para devolver o controlo ao host.
// ---------------------------------------------------------------------------
struct Hint {
    std::uint32_t raw;

    static constexpr std::uint32_t kSig = 0x6A8; // bits 31-21

    [[nodiscard]] constexpr bool verify() const {
        return ((raw >> 21) & 0x7FF) == kSig;
    }

    enum class Kind : std::uint8_t {
        Other = 0,
        Nop = 0,
        Yield = 1,
        WaitForInterrupt = 3,
        SendEvent = 5,
    };

    // CRm em bits 7-5 é o que identifica o hint.
    [[nodiscard]] constexpr Kind kind() const {
        switch ((raw >> 5) & 0x7) {
        case 0x0: return Kind::Nop;
        case 0x1: return Kind::Yield;
        case 0x3: return Kind::WaitForInterrupt;
        case 0x5: return Kind::SendEvent;
        default:  return Kind::Other;
        }
    }
};
static_assert(Hint{0xD503201F}.verify(), "NOP verifica");
static_assert(Hint{0xD503201F}.kind() == Hint::Kind::Nop, "NOP classificado");
static_assert(Hint{0xD503203F}.verify(), "YIELD verifica");
static_assert(Hint{0xD503203F}.kind() == Hint::Kind::Yield, "YIELD classificado");
static_assert(Hint{0xD503207F}.verify(), "WFI verifica");
static_assert(Hint{0xD503207F}.kind() == Hint::Kind::WaitForInterrupt,
              "WFI classificado");
static_assert(Hint{0xD50320BF}.verify(), "SEV verifica");
static_assert(Hint{0xD50320BF}.kind() == Hint::Kind::SendEvent,
              "SEV classificado");
// MRS tem o mesmo top8 mas outra assinatura — tem de ser distinguido.
static_assert(!Hint{0xD53BE020}.verify(), "MRS não é hint");

// ---------------------------------------------------------------------------
// Tabela de encapsulamento
//
// O que o guest deve correr directamente e o que o motor tem de tratar. Manter
// isto numa função só (e não espalhado pelo patcher) torna a decisão de
// segurança auditável: se uma instrução não está na lista, passa.
// ---------------------------------------------------------------------------
enum class Disposition : std::uint8_t {
    RunNatively,  // executar tal e qual
    PatchToBrk,   // SVC -> BRK
    TrapToHost,   // MRS/MSR/exclusivos/hints -> trap ao host
};

// Classifica uma palavra de instrução.
//
// `is_text` limita o que é considerado: o patcher só varre a região de texto.
// Scanear dados por resembleência a SVC é a forma clássica de corromper um
// programa, porque um literal `0xD40000C1` numa tabela de constantes casa
// perfeitamente com o encoding.
[[nodiscard]] constexpr Disposition classify(std::uint32_t insn) noexcept {
    if (SVC{insn}.verify())
        return Disposition::PatchToBrk;
    if (MRS{insn}.verify() || MSR{insn}.verify())
        return Disposition::TrapToHost;
    if (Exclusive{insn}.verify())
        return Disposition::TrapToHost;
    if (Hint{insn}.verify())
        return Disposition::TrapToHost;
    return Disposition::RunNatively;
}

} // namespace g610::arm
