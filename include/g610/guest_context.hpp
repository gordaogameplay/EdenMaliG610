// EdenMaliG610 — contexto do guest e do host.
//
// A execução nativa funciona trocando de contexto: o guest tem registos
// próprios, o host tem os seus. Quando o guest levanta uma trap, o estado do
// host tem de ser salvo e o do guest carregado; e o inverso no regresso.
//
// O que torna isto seguro em vez de frágil é a verificação de offsets: o
// assembly em `g610_trampoline.S` indexa directamente estes campos, e um
// `static_assert` em cada um garante que o C++ e o assembly concordam. Se um
// campo mudar de lugar, o build falha em vez de correr lixo no A76.
//
// Disposição: os GPRs x0-x30 e SP. Os vector registers (v0-v31) são mantidos
// pelo próprio hardware durante a execução normal — só precisam de troca nos
// pontos de-yield, onde estão no mesmo contexto de registos.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "g610/context_offsets.hpp"

namespace g610 {

// Marcador que distingue um tpidr_el0 de guest de um de host.
//
// Sem isto, um SIGSEGV do próprio host (um bug no motor) seria interpretado
// como um fault do guest e "tratado" como memória inválida — mascarando o
// bug em vez de o expor. O valor é verificado nos signal handlers antes de
// qualquer decisão.
inline constexpr std::uint32_t kTlsMagic = 0x555A5559u;

// Os offsets vivem em context_offsets.hpp, que é o único header que o
// assembly inclui — ver a razão lá.

// Estado do host, escondido no tpidr_el0 enquanto o guest corre.
//
// É a chave do truque: o kernel AArch64 usa tpidr_el0 para o TLS do
// utilizador. Se o motor colocar aí o seu próprio GuestContext, todo o código
// C++ do host passa a funcionar com o tpidr_el0 correcto (o libc usa-o), e o
// guest, que corre no mesmo nível de excepção, tem o contexto guardado sem
// custo adicional.
struct alignas(16) HostContext {
    std::array<std::uint64_t, 12> saved_regs{}; // x19-x30
    std::array<std::uint64_t, 8>  saved_vregs{}; // d8-d15
    std::uint64_t sp{};
    std::uint64_t tpidr_el0{};
    std::uint64_t pad[1]{};
};

// Motivo pelo qual o guest parou.
enum class HaltReason : std::uint32_t {
    None = 0,
    SvcCall,        // SVC reescrito para BRK
    GuestFault,     // SIGSEGV/SIGBUS dentro do espaço do guest
    DebugException, // BRK directo (sem SVC por baixo)
    Yield,          // YIELD — opportunity de yield
    WaitForInterrupt, // WFI — o guest espera por I/O
    PreemptionRequest, // de outra thread
};

// O contexto completo do guest.
//
// A ordem dos campos NÃO é livre: `g610_trampoline.S` indexa-os por offset
// constante. Os static_assert abaixo fixam essa correspondência — se um campo
// mudar de sítio, o build falha em vez de o assembly escrever no sítio errado.
struct alignas(16) GuestContext {
    std::array<std::uint64_t, 31> cpu_registers{}; // x0..x30
    std::uint64_t sp{};
    std::uint64_t pc{};      // endereço de retorno (após a instrução)
    std::uint64_t tpidrro_el0{};
    std::uint64_t tpidr_el0{};
    std::uint32_t fpcr{};
    std::uint32_t fpsr{};
    std::uint32_t pstate{};
    std::uint32_t svc{};     // número de SVC em curso
    std::uint32_t halt_reason = 0;
    HostContext host_ctx{};
    void* system{};          // contexto de serviços
    bool  guest_running = false;

};

// ThreadParameters — o que o tpidr_el0 aponta enquanto o guest corre.
//
// Layout pensado para caber nos 16 bytes que o kernel reserva para o TLS do
// utilizador: contexto nativo + magic + spinlock. É tudo o que um handler
// precisa para decidir se a trap é do guest ou do host.
struct ThreadParameters {
    GuestContext* ctx;          // kTlsCtx
    std::uint32_t magic;        // kTlsMagicWord
    std::uint32_t lock;         // kTlsLock
};

// ---------------------------------------------------------------------------
// Verificacao de layout.
//
// Estas comparacoes ficam aqui, e nao dentro das structs, por uma razao
// tecnica: durante a definicao de uma struct com `alignas(16)`, o tipo esta
// ainda em construcao e o compilador trata-o como abstracto — um
// `sizeof` ou `offsetof` ali dentro falha a compilar. Depois da struct estar
// fechada, o layout e completo e as comparacoes sao validas.
//
// Sao elas que tornam o assembly seguro: cada offset usado em
// g610_trampoline.S e verificado contra a struct real. Se um campo mudar de
// sitio, o build falha em vez de o assembly escrever no sitio errado.
// ---------------------------------------------------------------------------
static_assert(offsetof(HostContext, saved_regs)  == off::kHostRegs);
static_assert(offsetof(HostContext, saved_vregs) == off::kHostVregs);
static_assert(offsetof(HostContext, sp)          == off::kHostSp);
static_assert(offsetof(HostContext, tpidr_el0)   == off::kHostTpidr);
static_assert(sizeof(HostContext)                == off::kHostCtxSz,
              "HostContext: tamanho nao bate com o assembly");

static_assert(offsetof(GuestContext, cpu_registers) == off::kCtxGpr);
static_assert(offsetof(GuestContext, sp)           == off::kCtxSp);
static_assert(offsetof(GuestContext, pc)           == off::kCtxPc);
static_assert(offsetof(GuestContext, tpidrro_el0)  == off::kCtxTpidrroEl0);
static_assert(offsetof(GuestContext, tpidr_el0)    == off::kCtxTpidrEl0);
static_assert(offsetof(GuestContext, fpcr)         == off::kCtxFpcr);
static_assert(offsetof(GuestContext, fpsr)         == off::kCtxFpsr);
static_assert(offsetof(GuestContext, pstate)       == off::kCtxPstate);
static_assert(offsetof(GuestContext, svc)          == off::kCtxSvc);
static_assert(offsetof(GuestContext, halt_reason)  == off::kCtxHalt);
static_assert(offsetof(GuestContext, host_ctx)     == off::kCtxHostCtx);

static_assert(sizeof(ThreadParameters) == off::kTlsCtxSize,
              "ThreadParameters tem de caber nos 16 bytes do TLS");
static_assert(offsetof(ThreadParameters, ctx)   == off::kTlsCtx);
static_assert(offsetof(ThreadParameters, magic) == off::kTlsMagicWord);
static_assert(offsetof(ThreadParameters, lock)  == off::kTlsLock);

} // namespace g610
