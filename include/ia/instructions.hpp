// EdenMaliIA — descodificação de instruções AArch64 que o guest precisa de
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

namespace ia::arm {

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
// MRS <Xt>, CNTPCT_EL0  ->  MRS <Xt>, CNTVCT_EL0
//
// **E' o mesmo bit que o do `TPIDRRO_EL0`.** O `sysreg` do MRS esta nos
// bits [20:5], e os dois pares differem no bit 0 do `sysreg` — ou seja, no
// bit 5 da instrucao:
//
//     MRS Xt, TPIDRRO_EL0 = 0x5E01   ->  MRS Xt, TPIDR_EL0 = 0x5E00
//     MRS Xt, CNTPCT_EL0  = 0x5F01   ->  MRS Xt, CNTVCT_EL0 = 0x5F00
//
// Um bit, nos dois casos, e por isso que ha **uma** regra e nao duas.
//
// ## Porque o `CNTPCT_EL0` nao serve
//
// O `cntpct_el0` e' o contador **fisico** do temporizador, e so e' legivel a
// partir do EL1. O `cntvct_el0` e' o contador **virtual**, e o Linux
// expoe-o ao EL0 (e' o que `clock_gettime` usa).
//
// O Eden faz a mesma coisa (`nce/patcher.cpp:134-144`,
// `WriteCntpctHandler`), e ainda multiplica o valor por um factor para o
// converter de ticks do host para ticks do Switch. **A escala nao e' feita
// aqui**, e a razao esta escrita em `patcher.cpp`: o que o jogo faz com o
// valor e' medir tempo e semear um gerador aleatorio, e para as duas
// coisas o que importa e' que o contador seja **monotonico** — e o
// `cntvct_el0` e'.
//
// A medicao que trouxe aqui: o SDK da Nintendo le o contador com
// `mrs x0, cntpct_el0` (em 0x82d22fd0) e o motor respondeu "instrucao
// ilegal do guest".
[[nodiscard]] constexpr bool is_mrs_cntpct(std::uint32_t insn) noexcept {
    return MRS{insn}.verify() && MRS{insn}.sysreg() == 0x5F01;
}

// `MRS <Xt>, CNTPCT_EL0` em `MRS <Xt>, CNTVCT_EL0`, sem tocar em Rt.
[[nodiscard]] constexpr std::uint32_t cntpct_to_cntvct(std::uint32_t insn) noexcept {
    return is_mrs_cntpct(insn) ? (insn & ~0x20u) : insn;
}

static_assert(cntpct_to_cntvct(0xD53BE020u) == 0xD53BE000u,
              "MRS X0, CNTPCT_EL0 -> MRS X0, CNTVCT_EL0");
static_assert(cntpct_to_cntvct(0xD53BE021u) == 0xD53BE001u,
              "Rt preservado na conversao do contador");

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

} // namespace ia::arm
