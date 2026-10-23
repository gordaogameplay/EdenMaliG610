// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — motor de execução nativa.
#include "g610/guest_placement.hpp"
#include "g610/native_engine.hpp"
#include "g610/guest_program.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>

// ---------------------------------------------------------------------------
// O estado das threads de guest: uma TABELA fixa, nao um `__thread`
// ---------------------------------------------------------------------------
//
// O passo 1 tentou resolver isto com tres variaveis `__thread` (initial-exec,
// medidas razao por razao no PROGRESSO.md): `g_thread_de_guest`,
// `g_guest_tls`, `g_host_tls`. As variaveis em si estavam certas — eram
// escritas E lidas no contexto da thread certa (`run_guest_thread`). O que
// nao tinha alguma resposta e' o caso do HANDLER: levantou-se um sei la'
// que trap numa thread de guest — **que thread e'?** O trampolim do passo 1
// respondia "os valores `g_hooks.guest_tls`/`g_hooks.host_tls`" — que sao os
// da principal, publicados no `run()`. Medido (`wip/passo-1-thread`):
// a trap de uma thread de guest INSTALAVA o TLS da principal, o guest dessa
// thread corria com o TLS dela, e o `str x0,[x1,#0x1f8]` do SDK — que grava
// o ponteiro de TLS no proprio bloco — escreveu o `tls_tp` DA PRINCIPAL.
//
// Nem da para resolver com `__thread` no handler: o registo que a libc usa
// (`fs`) e o registo que o guest usa (`TPIDR_EL0`) sao O MESMO registo
// fisico. Enquanto o guest corre, o motor programa-o com o TLS do guest, e
// uma leitura de `__thread` dentro do handler nessa janela devolve o bloco
// do guest — nao o do host. (E `thread_local` do C++ em PIC passa por
// `__tls_get_addr`, que pode alocar — dentro de um handler, OOM medido:
// o processo morria no primeiro `SendSyncRequest`.)
//
// A marca que resolve isto e' a que o propio Eden usa no NCE: **o
// `TPIDR_EL0` de entrada do handler IDENTIFICA a thread.** O kernel
// preserva o registo no caminho do sinal — enquanto o guest corre, o
// registo vale o TLS DESSA thread (medido em quatro vias no passo 0:
// ver PROGRESSO.md). Entao o handler le o registo — `mrs`, uma instrucao,
// sem libc — e procura na tabela:
//
//     TPIDR_EL0 = TLS do guest DESSA thread  →  GuestThreadState
//
// A procura tem de correr sem lock e sem alocacao — dentro do handler nem
// uma coisa nem outra sao seguras. Por isso: 32 slots fixos, estado
// estatico, e o unico acesso sincronizado e' o `in_use` por slot, trocado
// com `compare_exchange`. O registo de um slot acontece ANTES de o guest
// dessa thread correr (na criacao da thread); o des-registo, depois do
// ultimo trap dela — nunca no meio do handler.

namespace g610 {

// A pagina do watchpoint — ver `SvcDispatcher::ArmarWatchpoint`.
extern volatile std::uint64_t g_watch_pagina;
// **O watchpoint ainda nao foi armado** — porque o rtld
// escreve na pagina (as relocacoes).
void ReporWatchpoint();
// **Ler o global do jogo** — o handler de sinais e' estatico.
bool LerGlobalDoJogo(std::uint64_t* valor);
void ReArmarWatchpoint();
void ArmarWatchpointNoProximoTrap();
// **A flag de re-armar** — ver `svc_dispatcher.cpp`.
extern volatile bool g_rearmar_watchpoint;

namespace {

// Os hooks são file-scope porque um handler de sinais não sabe que thread o
// levantou. Contêm a CONFIGURAÇÃO (despacho, observador, limite, recuperação,
// TLS da principal) e os contadores da thread principal; as threads de guest
// criadas via `CreateThread` contam NO SEU `GuestThreadState`.
EngineHooks g_hooks{};

// A TABELA de estado por thread. Tamanho fixo por duas razoes: (a) o handler
// nao pode alocar (OOM medido: `thread_local` → `__tls_get_addr`); (b) a
// procura dentro do handler tem de ser uma varredura linear de memória
// estática — sem mapa, sem lock, async-signal-safe.
//
// 32 é folga grossa para o que o jogo faz hoje: o arranque criou 3 threads
// (medido no `wip/passo-1-thread`).
constexpr int kMaxGuestThreads = 32;

struct GuestThreadSlot {
    std::atomic_bool in_use{false};
    GuestThreadState st{};
};

GuestThreadSlot g_thread_slots[kMaxGuestThreads];

// Toma um slot para uma thread nova. Async-signal-safe por construção:
// um test-and-set atómico, nenhuma alocação, nenhum lock.
GuestThreadState* register_guest_thread_state(std::uint64_t guest_tls) {
    for (auto& slot : g_thread_slots) {
        bool expect = false;
        if (!slot.in_use.compare_exchange_strong(expect, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
            continue;
        // Repõe o payload inteiro: um slot reusado de uma execução anterior
        // não pode trazer contadores nem fault de outra vida.
        slot.st = GuestThreadState{};
        slot.st.guest_tls = guest_tls;
        return &slot.st;
    }
    return nullptr;
}

// Devolve o slot. Chama-se no fim da vida da thread — nunca com traps
// pendentes desta: nenhum trap novo desta thread pode chegar depois do
// des-registo (o guest dela já parou).
void unregister_guest_thread_state(GuestThreadState* st) {
    if (st == nullptr)
        return;
    for (auto& slot : g_thread_slots) {
        if (&slot.st == st) {
            slot.in_use.store(false, std::memory_order_release);
            return;
        }
    }
}

// A THREAD cuja trap é, pelo valor de `TPIDR_EL0` no momento do trap.
//
// Chamada no princípio do handler. Busca falha não é erro: ou é a trap da
// thread principal (que conta em `g_hooks`, como sempre fez), ou é um fault
// do host — ambos seguem o caminho antigo, sem a troca de TLS por estado.
GuestThreadState* find_guest_thread_state(std::uint64_t tp) {
    if (tp == 0)
        return nullptr;
    for (auto& slot : g_thread_slots) {
        // `acquire`: ver o payload completo posto pelo registo; o
        // `compare_exchange` do registo tem `release` nessa troca.
        if (slot.in_use.load(std::memory_order_acquire) &&
            slot.st.guest_tls == tp)
            return &slot.st;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// TPIDR_EL0: o TLS da thread, lido e escrito pelo guest
// ---------------------------------------------------------------------------
//
// O guest vai buscar o seu TLS com `mrs x, tpidrro_el0`. Esse registo não
// existe para o guest neste kernel — medido quatro vias, ver PROGRESSO.md —
// por isso o patcher reescreve essa instrução para `mrs x, tpidr_el0`, e é
// este registo que o motor programa.
//
// `TPIDR_EL0` é o `fs` do AArch64 e é por thread de hardware, o que dá a
// multi-thread de graça: cada thread de host guarda o seu e instala o seu
// bloco de TLS de guest.
//
// Consequência que obriga a cuidado: enquanto o guest corre, `TPIDR_EL0` não é
// o TLS da libc. Qualquer código de host que corra nesse intervalo e use TLS
// rebenta — medido: `snprintf` com o `fs` apontado para o bloco do guest dá
// segfault. O único código de host que corre aí dentro é o handler de sinais,
// e é por isso que a troca vive aí e não só à volta de `run()`.

inline std::uint64_t read_thread_pointer() {
    std::uint64_t v = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

inline void write_thread_pointer(std::uint64_t v) {
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(v) : "memory");
}


// Emula `MRS <Xt>, TPIDRRO_EL0` quando o fault foi provoked por ela.
//
// Porque é preciso: ver o caso SIGSEGV em `handle_guest_signal`. O resumo é
// que neste host não há forma de PROGRAMAR o registo — só de o ler, e devolve
// sempre 0.
//
// Os encodings não são adivinhados. Foram montados com o assembler e lidos do
// object file:
//
//     mrs x0, tpidrro_el0   ->  d53bd060
//     mrs x8, tpidrro_el0   ->  d53bd068
//     mrs x3, tpidrro_el0   ->  d53bd063
//     msr tpidrro_el0, x0   ->  d51bd060     <- NÃO colide com o MRS
//
// O destino é o registo em `Xt`, que ocupa os bits [4:0]. A máscara tem de
// apagar SÓ esses cinco: `0xffffffe0`.
//
// A primeira versão usou `0xfffffc1f`, que também apaga os bits [5:6] — e aí
// `0xd53bd068` dava `0xd53bd040`, que não batia certo com o alvo e a
// emulação nunca disparava. A máscara foi escrita a olho, não medida; foi o
// Bits a olhar para os bytes que a apanhou.
inline bool emulate_tls_read(ucontext_t* uc, std::uint64_t pc,
                             GuestThreadState* st) {
    if (pc < 4)
        return false;

    std::uint32_t insn = 0;

    constexpr std::uint32_t kMask = 0xFFFFFFE0u;
    constexpr std::uint32_t kMrsTpidrro = 0xD53BD060u;

    // Recuar mais do que uma instrução.
    //
    // Medido no `sdk` (offset 0x32f900):
    //
    //     mrs  x8, tpidrro_el0      ; pc-8
    //     ldr  w9, [x0]             ; pc-4   <- um spill
    //     ldr  x8, [x8, #0x1f8]     ; pc     <- fault
    //
    // O `mrs` não levanta trap — lê 0 e segue. O fault só chega duas
    // instruções depois. Por isso se olha para trás algumas instruções, e se
    // recomeça a execução no `mrs` já corrigido.
    //
    // A condição `registo == 0` é o que impede o ciclo: depois de corrigido, o
    // registo já não é zero e a procura falha. E limita o alcance: se o
    // registo tiver mudado de valor, a causa não era o TLS e o fault é
    // reportado como fault.
    constexpr int kMaxLookback = 4;
    for (int back = 1; back <= kMaxLookback; ++back) {
        const std::uint64_t at = pc - 4 * static_cast<std::uint64_t>(back);
        if (at < 4)
            break;

        std::memcpy(&insn, reinterpret_cast<const void*>(at), sizeof(insn));
        if ((insn & kMask) != kMrsTpidrro)
            continue;

        const std::size_t reg = insn & 0x1Fu;
        if (reg >= 32 || uc->uc_mcontext.regs[reg] != 0)
            continue;

        // O TLS emulado é o DA THREAD que faultou (o seu slot da tabela),
        // nunca o da principal — o passo 1 usava `g_hooks.guest_tls`.
        uc->uc_mcontext.regs[reg] =
            st != nullptr ? st->guest_tls : g_hooks.guest_tls;
        uc->uc_mcontext.pc = at + 4;
        return true;
    }
    return false;
}

// Handlers por omissão, guardados para repor quando um trap não for nosso.
struct sigaction g_old_trap{};
struct sigaction g_old_segv{};
struct sigaction g_old_bus{};

// BRK emitido por `g610_guest_returned` (src/g610_call_guest.S) quando o guest
// faz `ret` sem ter chamado ExitProcess. É uma trap para que este caso
// patológico passe pelo mesmo mecanismo de recuperação dos restantes — só o
// `siglongjmp` restaura o `sp` do host.
constexpr std::uint32_t kBrkGuestReturned = 0xFFFD;
// Paragem pedida pelo tracer: um BRK com este número também sai do guest.
constexpr std::uint32_t kBrkForceStop = 0xFFFC;

// Stack e rotina de recuperação de faults.
//
// Um SIGSEGV numa instrução do guest não pode retomar: repetir a mesma
// instrução entraria em ciclo até o processo ser morto, e o stack trace
// original desapareceria. Em vez disso o handler salta para
// `_fault_recovery`, que corre com esta stack e regressa ao host por
// siglongjmp.
alignas(16) std::uint8_t g_recovery_stack[64 * 1024];

// sigjmp_buf para regressar ao host a partir da recuperação de faults.
sigjmp_buf g_run_jmp;
volatile bool g_have_run_jmp = false;

// Estado do registo do guest no momento do fault, para o relatório.
volatile std::uint64_t g_fault_pc = 0;
volatile std::uint64_t g_fault_sp = 0;

// Rotinas de saída do guest.
//
// Tanto um fault de memória como um ExitProcess precisam de regressar ao host
// sem o guest voltar a executar. Ambos usam siglongjmp para o ponto de
// regresso DE QUEM faultou:
//
//   - thread de guest registada  →  `sigjmp_buf` DO SEU `GuestThreadState`;
//   - thread principal / guest sintético  →  `g_run_jmp`, posto por `run()`.
//
// Como se sabe de quem é a saída? O `TPIDR_EL0`: o trampolim repõe o TLS do
// guest DESSA thread antes do sigreturn, e é esse o valor que o registo tem
// aqui dentro — a busca é a mesma do handler.
[[noreturn]] void exit_recovery() {
    GuestThreadState* st = find_guest_thread_state(read_thread_pointer());
    if (st != nullptr) {
        if (st->have_jmp)
            siglongjmp(st->jmp, 1);
        // Thread registada mas sem ponto de regresso: continuar a executar
        // com o `sp` de recuperação é lixo garantido — parar conscientemente.
        _exit(71);
    }
    if (g_have_run_jmp)
        siglongjmp(g_run_jmp, 1);
    // Sem ponto de regresso: sair é melhor do que executar lixo.
    _exit(70);
}

// A mesma vista de campos DE ESTADO, sobre duas estruturas.
//
// O corpo do handler escreve contadores e fault em UMA via só — que é o
// estado de quem fez a trap. `GuestThreadState` e `EngineHooks` têm de
// propósito os mesmos nomes nestes campos; a vista evita duplicar o corpo do
// handler (uma cópia esquecida era exactamente o bug do passo 1, que lia o
// estado da thread errada).
struct TrapState {
    std::uint64_t* svc_calls;
    std::uint64_t* traps;
    std::uint64_t* faults;
    std::uint64_t* host_faults;
    std::uint64_t* svc_time_ns;
    std::uint64_t* fault_pc;
    std::uint64_t* fault_sp;
    std::uint64_t* fault_addr;
    std::uint64_t (*fault_regs)[22];
    std::uint64_t* platform_reg;
    TrapKind*      last;
    bool*          limit_hit;
    bool*          faulted;
    bool*          guest_returned;
    bool*          illegal_insn;

    explicit TrapState(EngineHooks& h)
        : svc_calls(&h.svc_calls), traps(&h.traps), faults(&h.faults),
          host_faults(&h.host_faults), svc_time_ns(&h.svc_time_ns),
          fault_pc(&h.fault_pc), fault_sp(&h.fault_sp),
          fault_addr(&h.fault_addr), fault_regs(&h.fault_regs),
          platform_reg(&h.platform_reg),
          last(&h.last), limit_hit(&h.limit_hit), faulted(&h.faulted),
          guest_returned(&h.guest_returned), illegal_insn(&h.illegal_insn) {}

    explicit TrapState(GuestThreadState& s)
        : svc_calls(&s.svc_calls), traps(&s.traps), faults(&s.faults),
          host_faults(&s.host_faults), svc_time_ns(&s.svc_time_ns),
          fault_pc(&s.fault_pc), fault_sp(&s.fault_sp),
          fault_addr(&s.fault_addr), fault_regs(&s.fault_regs),
          platform_reg(&s.platform_reg),
          last(&s.last), limit_hit(&s.limit_hit), faulted(&s.faulted),
          guest_returned(&s.guest_returned), illegal_insn(&s.illegal_insn) {}
};

// ---------------------------------------------------------------------------
// O dono do despacho: UM despacho de SVC de cada vez no processo inteiro
// ---------------------------------------------------------------------------
//
// O `SvcDispatcher` é UM por processo (`g_hooks.svc`), e desde o passo 2 há
// threads de guest a despachar AO MESMO TEMPO. Medição A/B no Pi (3
// execuções do MESMO binário): o processo morre com `double free or
// corruption (!prev/(!out))` em posições diferentes (SVC final 261/262; e o
// número de `[thrd] trap` variou 195 → 31 de uma execução para outra) —
// corrida, não determinismo do jogo. Suspeitos no estado interno do
// despacho: `handles_` (std::map), `new KObject` em `svc_create_thread`, e
// o buffer de mensagem de IPC que o Eden partilha por processo
// (`Bridge::thread()`, `k_server_session.cpp:379`).
//
// A serialização é FEITA AQUI (à volta da chamada `dispatch`), não dentro
// do repositório: é o mesmo papel de um só kernel despachando uma sysCall
// de cada vez, que é exactamente o modelo do Eden/NCE.
//
// O dono é por thread, com um caso explícito: se a MESMA thread voltar a
// entrar NESTED (um fault do host dentro do C++ do despacho — os handlers
// têm SA_NODEFER), ela não re-entra; trata a trap como fault e sai pela
// recuperação. Por isso o release é manual nos pontos de saída: o
// `siglongjmp` da recuperação NÃO desmonta o frame do despacho (nenhum
// destructor corre), e um spinlock locker por RAII ficaria enleixado lá
// dentro.
constexpr std::uint64_t kOwnerFree = 0;
constexpr std::uint64_t kOwnerMain = 1; // a thread principal, em `run()`

std::atomic<std::uint64_t> g_dispatch_owner{kOwnerFree};

// O token desta thread no mapa de donos: as threads de guest usam o
// ponteiro do SEU `GuestThreadState` (estável durante a vida da thread), a
// principal usa `kOwnerMain`.
inline std::uint64_t dispatch_token(GuestThreadState* st) {
    return st != nullptr ? reinterpret_cast<std::uint64_t>(st) : kOwnerMain;
}

// Tenta tomar o poder de despachar. Devolve false se (a) o dono é a MESMA
// thread — trap aninhada — ou (b) o teto de spin estourou. Em ambos os
// casos o chamador trata a trap como fault e sai pela recuperação.
inline bool dispatch_take(std::uint64_t me) {
    // **Espera pelo dono** — em vez de desistir ao fim de um spin.
    //
    // Medido (`boot_p9s.log`): o jogo tem várias threads, e as correm
    // como threads do host (em paralelo). Quando duas fazem um SVC ao
    // mesmo tempo, a segunda encontra a posse tomada e — com o spin
    // antigo — batia no "teto de spin" e a trap era tratada como
    // **fault** (`fault_pc` = o BRK do SVC, `fault_addr` = 0). Era
    // esse o fault: não era o guest a ler de um ponteiro nulo, era a
    // segunda thread a ser recusada.
    //
    // A posse é transferida quando o dono acaba o despacho ou quando
    // estaciona (um SVC bloqueante larga o dono antes de dormir ✓),
    // por isso esperar é o comportamento certo.
    const auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::uint64_t expect = kOwnerFree;
        if (g_dispatch_owner.compare_exchange_strong(expect, me,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
            return true;
        if (g_dispatch_owner.load(std::memory_order_acquire) == me)
            return false; // trap aninhada: o próprio dono
        if (std::chrono::steady_clock::now() > limite)
            return false; // o dono nao largou a posse em 5 s
        // Dorme: a espera e' por um despacho **breve** (o dono so' a
        // segura enquanto responde a um SVC).
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// Liberta o dono. Só o próprio dono liberta; o `expect` do
// compare_exchange dá essa garantia sem lock.
inline void dispatch_release(std::uint64_t me) {
    std::uint64_t expect = me;
    g_dispatch_owner.compare_exchange_strong(expect, kOwnerFree,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
}

} // namespace

// ── Exportações do dono do despacho (ver native_engine.hpp) ──────────────
//
// Implementa-se aqui, e não no SvcDispatcher, porque o estado (`g_dispatch_owner`)
// vive neste TU. Os helpers anónimos acima são visíveis no mesmo TU a partir
// da sua declaração, e as exportações são as únicas portas para fora.
//
// O token EM CURSO (`g_dispatch_owner_itself`) é distinto do dono atómico:
// o dono muda de thread quando o serviço bloqueante solta; o token em curso
// é o da thread PHYSICA que chamou. Uma thread estacionada sai do "em curso"
// (aquela thread não está a despachar), e ao acordar repõe o LIDO antes de
// soltar — o valor do despacho que ainda tem de terminar.

namespace {

// Nota de thread-safety: `thread_local` — cada thread física tem o SEU
// token em curso, e não há global partilhado para colidir. Uma thread que
// soltou e estacionou NÃO toca no dela até acordar. A inicialização é
// constante (= 0), portanto sem guarda dinâmica de TLS no caminho da trap.
//
// Por que thread_local e não um global: uma thread estacionada (dono
// largado) continua com o SEU token válido; um global único seria
// sobrescrito pela thread que tomou o dono enquanto aquela espera.

thread_local std::uint64_t t_dispatch_in_flight = 0;

} // namespace

std::uint64_t dispatch_owner_itself() {
    return t_dispatch_in_flight;
}

void dispatch_owner_vacate() {
    const std::uint64_t me = t_dispatch_in_flight;
    if (me != 0) {
        dispatch_release(me);
        t_dispatch_in_flight = 0;
    }
}

bool dispatch_owner_claim() {
    const std::uint64_t me = t_dispatch_in_flight;
    if (me == 0)
        return false;
    return dispatch_take(me);
}

void dispatch_owner_set(std::uint64_t token) {
    t_dispatch_in_flight = token;
}

NativeEngine::NativeEngine(tel::Telemetry& tel, SvcDispatcher& svc)
    : tel_(tel), svc_(svc) {}

NativeEngine::~NativeEngine() {
    if (!hooks_.armed)
        return;
    ::sigaction(SIGTRAP, &g_old_trap, nullptr);
    ::sigaction(SIGSEGV, &g_old_segv, nullptr);
    ::sigaction(SIGBUS, &g_old_bus, nullptr);
    ::sigaction(SIGILL, nullptr, nullptr);
    ::sigaction(SIGALRM, nullptr, nullptr);
    hooks_.armed = false;
    g_hooks.armed = false;
}

// Publica os hooks para o handler estático. Qualquer alteração depois de
// instalado tem de passar por aqui — o handler não vê a instância.
void NativeEngine::publish_hooks() {
    // Copia campo a campo em vez de `g_hooks = hooks_`.
    //
    // Só a CONFIGURAÇÃO é copiada: para onde despachar, quem observar, e as
    // rotinas de recuperação. O estado (contadores, flags de trap) NÃO é —
    // esse vive só em `g_hooks`, porque é o handler que o escreve e `run()` o
    // lê de volta no fim.
    //
    // A distinção não é preciosismo: com `g_hooks = hooks_`, o `run()`
    // reverteria o estado a cada chamada, e um observer instalado depois
    // desaparecia no `install_handlers` seguinte.
    g_hooks.svc              = hooks_.svc;
    g_hooks.tel              = hooks_.tel;
    g_hooks.observer         = hooks_.observer;
    g_hooks.observer_user    = hooks_.observer_user;
    g_hooks.trapper          = hooks_.trapper;
    g_hooks.trapper_user     = hooks_.trapper_user;
    g_hooks.platform_reg     = hooks_.platform_reg;
    g_hooks.recovery_pc      = hooks_.recovery_pc;
    g_hooks.recovery_sp      = hooks_.recovery_sp;
    g_hooks.armed            = hooks_.armed;
    g_hooks.trap_limit       = hooks_.trap_limit;
}

// Repõe os contadores na cópia que o handler escreve.
//
// Têm de ser repostos em `g_hooks` e NÃO em `hooks_`: o handler só conhece
// `g_hooks`. Repô-los só na instância e depois publicar não os limpava, e o
// segundo `run()` reportava os contadores do primeiro — que é o número que
// esconde bugs.
//
// Isto repõe SÓ o estado da thread principal. As threads de guest contam no
// SEU slot da tabela (`g_thread_slots`), e o slot é reposto no registo da
// thread — `register_guest_thread_state` põe um payload a zero — não aqui,
// porque uma thread ainda a correr durante um segundo arranque não pode ter
// os contadores da PRINCIPAL a interferir com os dela.
void NativeEngine::reset_counters() {
    g_hooks.svc_calls   = 0;
    g_hooks.traps       = 0;
    g_hooks.faults      = 0;
    g_hooks.host_faults = 0;
    g_hooks.svc_time_ns = 0;
    g_hooks.last        = TrapKind::None;
    g_hooks.faulted     = false;
    g_hooks.guest_returned = false;
    g_hooks.illegal_insn = false;
    g_hooks.limit_hit   = false;
    g_hooks.fault_pc    = 0;
    g_hooks.fault_addr  = 0;
    g_hooks.fault_sp    = 0;
}

bool NativeEngine::install_handlers(std::string& error) {
    ::sigaction(SIGTRAP, nullptr, &g_old_trap);
    ::sigaction(SIGSEGV, nullptr, &g_old_segv);
    ::sigaction(SIGBUS, nullptr, &g_old_bus);

    struct sigaction sa{};
    sa.sa_sigaction = &NativeEngine::signal_trampoline;
    // SA_SIGINFO: precisamos de siginfo_t para distinguir um BRK nosso de
    //              outro tipo de trap.
    // SA_NODEFER: uma trap dentro do handler não se auto-ignora. Um BRK
    //              aninhado é um bug e tem de ser visível.
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    if (::sigaction(SIGTRAP, &sa, nullptr) != 0) {
        error = "sigaction(SIGTRAP) falhou";
        return false;
    }
    if (::sigaction(SIGSEGV, &sa, nullptr) != 0) {
        error = "sigaction(SIGSEGV) falhou";
        return false;
    }
    if (::sigaction(SIGBUS, &sa, nullptr) != 0) {
        error = "sigaction(SIGBUS) falhou";
        return false;
    }
    // SIGILL nao e' uma trap de guest, mas sem handler o processo morre sem
    // diagnostico. Trata-se como host_fault para que o motor reporte onde.
    ::sigaction(SIGILL, &sa, nullptr);
    // SIGALRM nao e' uma trap de guest, mas sem handler um spin de codigo
    // nunca mais devolve o controlo ao host. O tracer usa-o como ultimo
    // recurso.
    ::sigaction(SIGALRM, &sa, nullptr);

    hooks_.svc = &svc_;
    hooks_.tel = &tel_;
    // Endereço e stack da rotina de recuperação de faults.
    hooks_.recovery_pc = reinterpret_cast<std::uint64_t>(&exit_recovery);
    // O topo da stack, alinhado a 16 bytes como a AAPCS64 exige.
    hooks_.recovery_sp = reinterpret_cast<std::uint64_t>(
        g_recovery_stack + sizeof(g_recovery_stack));
    hooks_.armed = true;
    publish_hooks();
    // NOTA: `publish_hooks` copia a estrutura inteira. Por isso os hooks que
    // o utilizador instala têm de ser postos ANTES de `install_handlers`, ou
    // republished depois — se não, esta cópia apaga-os.

    error.clear();
    return true;
}

RunResult NativeEngine::run(void (*code)(), std::uint64_t sp) {
    RunResult r;

    // O guest corre NESTA thread — o motor e' um loop de traps sincrono, e
    // nao ha `pthread_create` em lado nenhum. Sem isto o jogo herda o nucleo
    // onde o processo calhou, e o RK3588 pode calhar num A55.
    //
    // O Switch corre o jogo em 3 A57 a 1.020 MHz. Um A76 a 2.4 GHz e' outra
    // coisa, e e' por isso que se vai la. Ver `guest_placement.cpp`.
    fixar_guest_nos_nucleos_grandes();

    if (!hooks_.armed) {
        r.error = "handlers nao instalados";
        return r;
    }

    // Estado do dispatcher: um `exited_` que ficou a true contaminaria a
    // execução seguinte e faria um fault parecer uma saída limpa.
    svc_.reset_run_state();

    // **O watchpoint** — o global do jogo que guarda a base da tabela
    // que o guest consultou (`0x824B8232`, lido em `0x8177d0c8`)
    //.
    //
    // **So' depois do rtld** — antes disso, o rtld escreve na
    // pagina (as relocacoes) e o watchpoint disparava em
    // Write.
    //
    // `IA_WATCH=rtld` — arma no **primeiro SVC**, que e' depois
    // do rtld (o rtld nao faz SVC — so' o jogo).
    //
    // **O guarda e' obrigatorio** — medido: sem ele, a
    // pagina do global fica em so' leitura desde o **primeiro**
    // SVC — e o primeiro SVC e' o `QueryMemory` do rtld, que e'
    // **antes** das relocacoes. As relocacoes do rtld (a
    // instrucao `str x11, [x12, x8]` em `0x84461980`) escrevem nessa
    // pagina e levantavam um `SIGSEGV` (SEGV_ACCERR) — o
    // guest ficava a repetir a mesma escrita ate ao limite de traps
    //e o GOT do `main` ficava por relocar (todos os slots com
    // o mesmo valor).
    if (std::getenv("IA_WATCH") != nullptr) {
        svc_.ArmarWatchpointSePedir(0x824B5CA0ull, 8);
    }

    // **O valor do global ANTES do jogo** — para comparar com o de
    // depois.
    {
        std::uint64_t antes = 0;
        if (svc_.read_guest(0x824B5CA0ull, &antes, sizeof antes)) {
            std::fprintf(stderr, "[watch] global no arranque = 0x%llx\n",
                         (unsigned long long)antes);
        }
    }

    // Reinicia os contadores: um segundo arranque não deve herdar o primeiro.
    //
    // NÃO se faz `hooks_ = g_hooks` aqui. O handler de sinais vive em
    // `g_hooks`, e uma cópia de volta apagaria o `armed` e os hooks que o
    // utilizador instalou — foi exactamente o que aconteceu: o observer e o
    // trapper desapareciam no segundo arranque e o guest ficava mudo.
    reset_counters();

    // Ponto de regresso para a recuperação de traps.
    //
    // `sigsetjmp(buf, 1)` salva também a máscara de sinais: sem o segundo
    // argumento, o longjmp deixaria os handlers bloqueados e um segundo fault
    // mataria o processo em vez de ser reportado.
    //
    // Todas as saídas do guest passam por aqui. Nunca por um retorno normal
    // da chamada: se o guest fizesse `ret`, o epílogo de `run()` verificaria o
    // canário do stack protector sobre a stack do guest (medido — ver
    // src/g610_call_guest.S). `g610_guest_returned` transforma esse caso numa
    // trap, e o trap é tratado aqui como os restantes.
    // x18 é a stack canary do guest (o "platform register" da AAPCS64). Sem
    // isto o guest verifica a canary contra lixo, a verificação falha, e o
    // código de recuperação chama ReturnFromException. Foi o primeiro SVC que
    // o rtld pediu, com um x0 que não era um endereço de 39 bits — o sintoma
    // não apontava para a causa.
    //
    // O valor tem de ser o mesmo em todas as entradas, e o Eden usa
    // `Random64(0) | 1` (o bit 0 impede que uma canary termine em NUL).
    if (platform_reg_ == 0) {
        std::uint64_t seed = 0;
        std::FILE* f = std::fopen("/dev/urandom", "rb");
        if (f != nullptr) {
            if (std::fread(&seed, 1, sizeof(seed), f) != sizeof(seed))
                seed = 0x9E3779B97F4A7C15ull;
            std::fclose(f);
        } else {
            seed = 0x9E3779B97F4A7C15ull;
        }
        platform_reg_ = seed | 1u;
    }
    hooks_.platform_reg = platform_reg_;

    if (::getenv("IA_TRACE")) {
        char b[96];
        const int k = std::snprintf(b, sizeof b,
            "[run] platform_reg=0x%llx hooks=0x%llx\n",
            (unsigned long long)platform_reg_,
            (unsigned long long)hooks_.platform_reg);
        ssize_t w = ::write(2, b, (size_t)k); (void)w;
    }

    // A base do TLS do guest, decidida pelo boot (`set_guest_tls`). Zero quando
// não há programa — e nesse caso o `TPIDR_EL0` não é tocado.
g_hooks.guest_tls = hooks_.guest_tls;
g_hooks.host_tls  = read_thread_pointer();
guest_x1_         = hooks_.guest_x1;

const int jumped = sigsetjmp(g_run_jmp, 1);

    // O handler de sinais escreve em `g_hooks` (não tem instância). Trazer só
    // os CONTADORES de volta — nunca a estrutura inteira.
    //
// `hooks_ = g_hooks` seria o erro óbvio e ERRADO: `g_hooks` é uma cópia
    // que o handler actualiza campo a campo, e pode ter visto
    // `g_hooks.observer` ou `g_hooks.trapper` ainda a zero se o utilizador os
    // instalou depois de `install_handlers()`. Copiar a estrutura inteira
    // apagava-os, e o handler saía no `if (!g_hooks.armed) return;` — o guest
    // ficava mudo e sem diagnóstico. Foi o que aconteceu: o rtld levantava a
    // trap, o handler via-a, e não fazia nada.
    hooks_.svc_calls   = g_hooks.svc_calls;
    hooks_.traps       = g_hooks.traps;
    hooks_.faults      = g_hooks.faults;
    hooks_.host_faults = g_hooks.host_faults;
    hooks_.svc_time_ns = g_hooks.svc_time_ns;
    hooks_.last        = g_hooks.last;
    hooks_.faulted     = g_hooks.faulted;
    hooks_.guest_returned = g_hooks.guest_returned;
    hooks_.illegal_insn = g_hooks.illegal_insn;
    hooks_.limit_hit    = g_hooks.limit_hit;
    hooks_.fault_pc    = g_hooks.fault_pc;
    hooks_.fault_addr  = g_hooks.fault_addr;
    // **Os registos do fault** — o handler escreve em `g_hooks` e o
    // relatorio le em `hooks_`: sem esta copia, o dump lia zeros
    // (e foi exactamente o que acontecia).
    for (int i = 0; i < 22; ++i) {
        hooks_.fault_regs[i] = g_hooks.fault_regs[i];
    }
    hooks_.fault_sp    = g_hooks.fault_sp;

    // Já estamos no host. O `siglongjmp` deixou o TLS do guest instalado, e o
    // resto de `run()` — telemetria, relatório — é código de host.
    if (g_hooks.guest_tls != 0)
        write_thread_pointer(g_hooks.host_tls);

    // Se o dono do despacho ficou enleixo DA PRINCIPAL (a trap aninhada
    // abandonou o frame do despacho por `siglongjmp`, que não corre
    // destructors), liberta aqui. Em execuções sãs o CAS não faz nada.
    dispatch_release(kOwnerMain);

    if (jumped == 0) {
        g_have_run_jmp = true;

        // Instalar o TLS do guest. Daqui até ao `sigreturn` que segue
        // qualquer trap, o `mrs x, tpidr_el0` do guest devolve a base do TLS
        // DELE — e não o da libc.
        if (g_hooks.guest_tls != 0)
            write_thread_pointer(g_hooks.guest_tls);

        g610_call_guest(code, sp, platform_reg_, guest_x1_);
        g_have_run_jmp = false;
        // inalcançável em operação normal: se o guest tivesse regressado,
        // a stack estaria corrompida e isto nem sequer seria seguro escrever.
        r.last_trap = TrapKind::Breakpoint;
        r.error = "o guest regressou sem chamar ExitProcess";
    } else if (g_hooks.limit_hit) {
        g_have_run_jmp = false;
        r.last_trap = TrapKind::Breakpoint;
        char msg[192];
        if (hooks_.trap_limit != 0)
            std::snprintf(msg, sizeof(msg),
                          "limite de traps atingido (%llu) — o guest esta em ciclo",
                          (unsigned long long)hooks_.trap_limit);
        else
            std::snprintf(msg, sizeof(msg),
                          "alarme de seguranca — o guest ficou preso em codigo "
                          "(sem levantar traps) durante tempo suficiente");
        r.error = msg;
    } else if (svc_.exited()) {
        g_have_run_jmp = false;
        // Saída limpa: o guest chamou ExitProcess.
        r.last_trap = TrapKind::SvcCall;
    } else if (g_hooks.guest_returned) {
        g_have_run_jmp = false;
        // O guest fez `ret` sem chamar ExitProcess: bug do guest, não saída.
        r.last_trap = TrapKind::Breakpoint;
        r.error = "o guest regressou sem chamar ExitProcess";
    } else if (g_hooks.illegal_insn) {
        g_have_run_jmp = false;
        // SIGILL no guest: instrução que o CPU não reconhece. Num jogo real
        // isto significa uma descodificação errada — ou uma trap nossa.
        r.last_trap = TrapKind::Breakpoint;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "instrucao ilegal do guest em 0x%llx (sp=0x%llx)",
                      (unsigned long long)g_hooks.fault_pc,
                      (unsigned long long)g_hooks.fault_sp);
        r.error = msg;
    } else {
        g_have_run_jmp = false;
        // Fault de memória no guest.
        r.last_trap = TrapKind::GuestFault;
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "fault de memoria do guest em 0x%llx (endereco 0x%llx, sp=0x%llx)",
                      (unsigned long long)g_hooks.fault_pc,
                      (unsigned long long)g_hooks.fault_addr,
                      (unsigned long long)g_hooks.fault_sp);
        r.error = msg;

        // **A stack do guest no fault.** O SDK usa um "contexto" whose
        // o ponteiro está num slot da stack (`[sp+0x1f8]`, no dump do
        // código) — e é null no fault. O dump mostra o que está à volta.
        {
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[fault] stack sp=0x%llx:",
                               (unsigned long long)g_hooks.fault_sp));
            for (std::size_t off = 0; off <= 0x200; off += 8) {
                std::uint64_t v = 0;
                if (!svc_.read_guest(g_hooks.fault_sp + off, &v, sizeof v))
                    continue;
                if (v == 0)
                    continue;
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " [%llx]=0x%llx",
                                   (unsigned long long)off,
                                   (unsigned long long)v));
                if (n + 20 >= sizeof(linha))
                    break;
            }
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // **Os bytes do código no PC do fault.** O VA do guest é plano
        // (a reserva é um `mmap` de gama fixa — o endereço do guest é
        // o endereço do host), por isso se lêem direto. É o que diz o
        // que a instrução está a fazer — sem símbolos, os bytes são a
        // única pista.
        {
            // A leitura e' a segura do dispatcher: o acesso do host nao
            // pode faultar (o `read` do guest usa um processo filho).
            //
            // **64 bytes antes e 64 depois do PC**: o codigo do guest esta
            // *patched* (as "ips patches" do NCA), por isso o NSO
            // descomprimido **nao serve** para o desassemblar — e' o
            // codigo em memoria que diz o que a instrucao faz.
            std::uint8_t codigo[128] = {0};
            if (svc_.read_guest(g_hooks.fault_pc - 64, codigo, sizeof codigo)) {
                char linha[700];
                std::size_t n = 0;
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "[fault] cod (pc-64) 0x%llx:",
                                   (unsigned long long)(g_hooks.fault_pc - 64)));
                for (std::size_t i = 0; i < sizeof codigo && n + 4 < sizeof(linha); ++i)
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, " %02x", codigo[i]));
                if (n + 2 < sizeof(linha))
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, "\n"));
                const ssize_t w = ::write(2, linha, n);
                (void)w;
            }
        }

        // **A tabela que o guest estava a consultar** — o lookup que
        // devolveu nulo.
        //
        // **O `x0` do fault NAO e' a base** — e' o **resultado** do
        // lookup (que foi nulo). A base e' o campo `+16` do
        // objecto (`x19`) — o mesmo que o caller leu.
        //
        // **O `id`** e' o `u16` em `x19 + 50`.
        {
            const std::uint64_t obj = hooks_.fault_regs[19];
            std::uint64_t base = 0;
            std::uint16_t id = 0;
            std::uint32_t tam = 0;
            svc_.read_guest(obj + 16, &base, sizeof base);
            svc_.read_guest(obj + 50, &id, sizeof id);
            svc_.read_guest(obj + 52, &tam, sizeof tam);
            char linha[400];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n,
                               "[fault] tabela: obj=0x%llx base=0x%llx id=%u tam=%u",
                               (unsigned long long)obj, (unsigned long long)base,
                               (unsigned)id, (unsigned)tam));
            // **O byte de "presente"** — e' o que o lookup testa.
            std::uint8_t presente = 0;
            if (svc_.read_guest(base + id + 0x1888, &presente, 1)) {
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n,
                                   " presente=%u (em 0x%llx)", (unsigned)presente,
                                   (unsigned long long)(base + id + 0x1888)));
            }
            // **A lista de ids** — a tabela paralela.
            std::uint64_t lista = 0;
            if (svc_.read_guest(base + id * 8 + 0x18AC, &lista, sizeof lista)) {
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " lista=0x%llx",
                                   (unsigned long long)lista));
                std::uint8_t ids[24]{};
                if (lista != 0 && svc_.read_guest(lista, ids, sizeof ids)) {
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, " ids="));
                    for (std::size_t i = 0; i < sizeof ids; ++i)
                        n += static_cast<std::size_t>(
                            std::snprintf(linha + n, sizeof(linha) - n, " %u",
                                           (unsigned)ids[i]));
                }
            }
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // **O global do jogo** — o `0x824B8232`, que e' onde o
        // jogo guarda a base da tabela (`adrp x8, 0x824b5000` +
        // `ldr x8, [x8, #3232]`, em `0x8177d0c8`).
        //
        // **Se estiver a zero**, o lookup e' feito sobre a base 0
        // — que e' o que o dump mostra.
        {
            std::uint64_t global = 0;
            char linha[1024];
            std::size_t n = 0;
            if (svc_.read_guest(0x824B5CA0ull, &global, sizeof global)) {
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n,
                                   "[fault] global 0x824B5CA0 = 0x%llx",
                                   (unsigned long long)global));
            }
            // **A volta do global** — os bytes crus (e lidos tres
            // vezes — para se ver se o valor muda: o guest tem
            // varias threads e uma delas pode estar a escrever).
            {
                std::uint8_t cru[64]{};
                for (int tentativa = 0; tentativa < 3; ++tentativa) {
                    std::memset(cru, 0, sizeof cru);
                    svc_.read_guest(0x824B5C80ull, cru, sizeof cru);
                    n += static_cast<std::size_t>(
                        std::snprintf(linha + n, sizeof(linha) - n, "\n[fault] 0x824B5C80:"));
                    for (std::size_t i = 0; i < sizeof cru; ++i) {
                        n += static_cast<std::size_t>(
                            std::snprintf(linha + n, sizeof(linha) - n, " %02x", cru[i]));
                        if ((i % 16) == 15)
                            n += static_cast<std::size_t>(
                                std::snprintf(linha + n, sizeof(linha) - n, "\n           "));
                    }
                }
            }
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }
    }

    r.exited    = svc_.exited();
    r.exit_code = svc_.exit_code();
    r.svc_calls   = hooks_.svc_calls;
    r.traps       = hooks_.traps;
    r.faults      = hooks_.faults;
    r.host_faults = hooks_.host_faults;
    r.fault_pc   = hooks_.fault_pc;
    r.fault_addr = hooks_.fault_addr;
    r.svc_time_ns = hooks_.svc_time_ns;

    // O relatório soma os contadores DAS THREADS DE GUEST: cada uma conta no
    // SEU slot; o que está acima é só o que a principal contou. Leitura por
    // varrimento sem lock — os números de uma thread ainda a viver são os que
    // ela já reportou (os slots vivos continuam a conta; é um apontador de
    // progresso, não uma contabilidade exacta).
    for (const auto& slot : g_thread_slots) {
        if (!slot.in_use.load(std::memory_order_acquire))
            continue;
        r.svc_calls   += slot.st.svc_calls;
        r.traps       += slot.st.traps;
        r.faults      += slot.st.faults;
        r.host_faults += slot.st.host_faults;
        r.svc_time_ns += slot.st.svc_time_ns;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Handler de sinais
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Correr codigo do guest numa thread nova
// ---------------------------------------------------------------------------
//
// O codigo do guest e' NATIVO: corre directamente na CPU. Logo, uma thread de
// guest e' uma thread do host a correr o entry point, com o `sp` que o guest
// deu no `CreateThread` e com o `TPIDR_EL0` apontado para o TLS *dela*.
//
// O `sp` nao pode ser mudado em C++ (o compilador mete-o no meio), por isso
// ha uma rotocao em ensamblador.
//
// **Os registos do guest ficam a zero**, como no Eden
// (`KThread::InitializeContext`: tudo a zero, menos o `x0` — o argumento —
// e o `x18`, o platform register). Sem isto, a thread nova herda os registos
// do host: o entry point que o SDK passa le o `x7` e o `x19`, e com valores
// do host ele salta para um endereco do host.
extern "C" __attribute__((naked)) void g610_run_guest(std::uint64_t entry,
                                                      std::uint64_t arg,
                                                      std::uint64_t sp,
                                                      std::uint64_t platform_reg) {
    __asm__ volatile(
        "sub  sp, sp, #0x30\n"
        "str  x0, [sp, #0]\n"     // o entry point
        "str  x1, [sp, #8]\n"     // o argumento
        "str  x2, [sp, #16]\n"    // o `sp` do guest
        "str  x3, [sp, #24]\n"    // o platform register
        "mov  x0, xzr\n"
        "mov  x1, xzr\n"
        "mov  x2, xzr\n"
        "mov  x3, xzr\n"
        "mov  x4, xzr\n"
        "mov  x5, xzr\n"
        "mov  x6, xzr\n"
        "mov  x7, xzr\n"
        "mov  x8, xzr\n"
        "mov  x9, xzr\n"
        "mov  x10, xzr\n"
        "mov  x11, xzr\n"
        "mov  x12, xzr\n"
        "mov  x13, xzr\n"
        "mov  x14, xzr\n"
        "mov  x15, xzr\n"
        "mov  x16, xzr\n"
        "mov  x17, xzr\n"
        "mov  x19, xzr\n"
        "mov  x20, xzr\n"
        "mov  x21, xzr\n"
        "mov  x22, xzr\n"
        "mov  x23, xzr\n"
        "mov  x24, xzr\n"
        "mov  x25, xzr\n"
        "mov  x26, xzr\n"
        "mov  x27, xzr\n"
        "mov  x28, xzr\n"
        "mov  x29, xzr\n"
        "mov  x30, xzr\n"
        "ldr  x18, [sp, #24]\n"    // o platform register
        "ldr  x0, [sp, #8]\n"     // o argumento
        "ldr  x16, [sp, #16]\n"   // o `sp` do guest
        "ldr  x17, [sp, #0]\n"    // o entry point
        "mov  sp, x16\n"         // o `sp` do guest
        "blr  x17\n"             // o entry point
        "brk  #0\n");            // a thread do guest nao volta
}

int NativeEngine::run_guest_thread(std::uint64_t entry, std::uint64_t arg,
                                   std::uint64_t sp, std::uint64_t tls) {
    // O estado DESTA thread, num slot da tabela — nunca `__thread` (ver o
    // bloco de comentário acima de `g_hooks` para as medições que obrigam a
    // isso). O slot é registrado antes do guest desta thread correr, e
    // libertado nos dois fins abaixo, depois do último trap dela.
    GuestThreadState* st = register_guest_thread_state(tls);
    if (st == nullptr) {
        // A tabela esgotou: esta thread fica sem correr. Partilhar o estado
        // de outra thread (o comportamento do passo 1) era o bug medido —
        // melhor uma thread a menos do que duas a escrever no mesmo relatório.
        std::fprintf(stderr,
                     "[thrd] sem slot de estado para o TLS 0x%llx — a thread nao corre\n",
                     (unsigned long long)tls);
        return 2;
    }
    st->host_tls = read_thread_pointer();
    st->guest_tls = tls;

    // O `x18` é POR THREAD: o Eden põe `Random64(0) | 1` no contexto de cada
    // uma (`ResetThreadContext64`, `k_thread.cpp:58`). A canary é preservada
    // pela frame que a guardou no prólogo, logo o valor só precisa de ser
    // consistente dentro desta thread — e diferente entre threads é o que o
    // Eden produz. Isto corre FORA do handler: `/dev/urandom` aqui é seguro.
    {
        std::uint64_t seed = 0;
        std::FILE* f = std::fopen("/dev/urandom", "rb");
        if (f != nullptr) {
            if (std::fread(&seed, 1, sizeof(seed), f) != sizeof(seed))
                seed = tls ^ entry;
            std::fclose(f);
        } else {
            seed = tls ^ entry;
        }
        st->platform_reg = seed | 1u;
    }

    // O `sigsetjmp` tem de estar numa frame que **sobreviva** ao codigo do
    // guest: um `siglongjmp` para o `sigjmp_buf` de uma frame ja regressada
    // e' comportamento indefinido. O buffer é o DESSA thread — um buffer
    // global partilhado (o `g_thread_jmp` do passo 1) era exactamente o que
    // o `exit_recovery` não podia escolher: a busca por `TPIDR_EL0` decide
    // para que frame regressar.
    if (sigsetjmp(st->jmp, 1) != 0) {
        // Um fault, ExitProcess ou o limite de traps trouxe-nos para aqui —
        // nesta thread, porque foi NELA que o guest parou. O lugar do fault
        // é a informação mais valiosa que temos, e agora é o DELE: o passo 1
        // lia os campos GLOBAIS (da principal), e por isso imprimia
        // "pc=0xa... (addr 0x0)" — lixo de outra thread.
        st->have_jmp = false;
        write_thread_pointer(st->host_tls);
        // A thread sai — e se o dono do despacho ficou enleixo DELA (o caso
        // da trap aninhada abandonou o frame do despacho por `siglongjmp`,
        // que não corre destructors), liberta aqui.
        dispatch_release(reinterpret_cast<std::uint64_t>(st));
        std::fprintf(stderr,
                     "[thrd] a thread de guest parou: pc=0x%llx addr=0x%llx "
                     "sp=0x%llx (traps=%llu faults=%llu)\n",
                     (unsigned long long)st->fault_pc,
                     (unsigned long long)st->fault_addr,
                     (unsigned long long)st->fault_sp,
                     (unsigned long long)st->traps,
                     (unsigned long long)st->faults);
        unregister_guest_thread_state(st);
        return 1;
    }
    st->have_jmp = true;

    // **O TLS do guest tem de estar instalado ANTES de o guest correr.** O
    // handler de sinais troca o TLS do host pelo do guest *durante* as traps
    // — mas o guest le o `TPIDR_EL0` a toda a hora, e fora de uma trap o
    // registo e' o da `libc` do host.
    write_thread_pointer(st->guest_tls);

    g610_run_guest(entry, arg, sp, st->platform_reg);

    write_thread_pointer(st->host_tls);
    st->have_jmp = false;
    dispatch_release(reinterpret_cast<std::uint64_t>(st));
    unregister_guest_thread_state(st);
    return 0;
}

void NativeEngine::signal_trampoline(int sig, siginfo_t* info, void* raw) {
    // QUE thread faultou? O próprio `TPIDR_EL0` de entrada responde: o kernel
    // preserva-o do momento do trap, e enquanto o guest corre o registo vale
    // o TLS DESSA thread (quatro vias de medição no passo 0). Encontrar o
    // slot ANTES de qualquera troca — o registo ainda está por repor.
    //
    // Foi aqui que o passo 1 falhou: trocava o TLS por `g_hooks.guest_tls` /
    // `g_hooks.host_tls` — os valores da thread PRINCIPAL, postos no `run()`.
    // A trap de uma thread de guest instalava o TLS da principal, e o `str
    // x0,[x1,#0x1f8]` do SDK escreveu o `tls_tp` DA PRINCIPAL (medido no
    // `wip/passo-1-thread`).
    GuestThreadState* st = find_guest_thread_state(read_thread_pointer());

    if (st != nullptr) {
        // Duas trocas, DESSA thread, e pelo mesmo motivo nos dois sentidos:
        //
        //   - o handler é código do HOST (telemetria, despacho, e o
        //     `fprintf` dos contadores) e precisa da libc DELA;
        //   - o `sigreturn` que se segue retoma o GUEST DELA, que espera o
        //     SEU TLS em `TPIDR_EL0`.
        //
        // Sem a segunda troca, o guest retoma com a libc e lê o próprio
        // ThreadLocalRegion no sítio errado — um fault que não parece fault.
        write_thread_pointer(st->host_tls);
        handle_guest_signal(sig, info, raw, st);
        if (st->guest_tls != 0)
            write_thread_pointer(st->guest_tls);
        return;
    }

    if (g_hooks.guest_tls == 0) {
        // Sem programa carregado não há bloco de TLS de guest — e instalar
        // um endereço não mapeado rebenta a libc dentro do handler. Os testes
        // unitários correm um guest sintético sem programa, e ficavam a
        // bloquear por causa disto.
        handle_guest_signal(sig, info, raw, nullptr);
        return;
    }

    // A thread principal: os `g_hooks.guest_tls` / `g_hooks.host_tls` são os
    // DELA — postos no `run()` — e o caminho é o mesmo de sempre.
    write_thread_pointer(g_hooks.host_tls);
    handle_guest_signal(sig, info, raw, nullptr);
    write_thread_pointer(g_hooks.guest_tls);
}

void NativeEngine::handle_guest_signal(int sig, siginfo_t* info, void* raw,
                                       GuestThreadState* st) {
    if (!g_hooks.armed)
        return; // nunca devia acontecer: handlers não instalados

    auto* uc = static_cast<ucontext_t*>(raw);
    const auto pc = static_cast<std::uint64_t>(uc->uc_mcontext.pc);

    // O estado que isto ACTUALIZA é o de quem faultou: uma thread de guest
    // registada escreve no SEU slot — nunca sobrescreve o relatório de outra
    // thread nem o da principal. A principal e os guests sintéticos
    // continuam (bem) em `g_hooks`. Era aqui, no passo 1, que uma thread
    // faultada fazia a principal perder o próprio relatório.
    TrapState ts = st != nullptr ? TrapState(*st) : TrapState(g_hooks);

    // O topo da stack de recuperação DE QUEM FAULTOU: a das threads de guest
    // é a delas; a da principal é a global. Um `siglongjmp` para o
    // `sigjmp_buf` de outra thread é comportamento indefinido — e o `sp`
    // segue a mesma regra do buffer.
    const std::uint64_t rec_sp =
        st != nullptr
            ? reinterpret_cast<std::uint64_t>(st->recovery_stack +
                                              sizeof(st->recovery_stack))
            : g_hooks.recovery_sp;

    ++*ts.traps;

    // **Re-armar o watchpoint** — a escrita que o disparou ja' passou
    // (a pagina ficou gravavel), e agora pode voltar a ser so'
    // de leitura — para apanhar a escrita **seguinte**.
    if (g_rearmar_watchpoint) {
        g_rearmar_watchpoint = false;
        ReArmarWatchpoint();
    }

    if (g_hooks.trapper != nullptr)
        g_hooks.trapper(sig, pc, static_cast<std::uint64_t>(uc->uc_mcontext.sp),
                        g_hooks.trapper_user);

    // Limite de traps. POR THREAD: cada uma tem a sua contagem, e um guest
    // em ciclo numa delas não pode ficar a queimar CPU para sempre — sai-se
    // pela rotina de recuperação DELA, que preserva o `sp` do host e deixa o
    // relatório explicar o que aconteceu.
    if (g_hooks.trap_limit != 0 && *ts.traps >= g_hooks.trap_limit) {
        *ts.limit_hit = true;
        *ts.last = TrapKind::Breakpoint;
        uc->uc_mcontext.sp = rec_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    if (::getenv("IA_TRACE")) {
        char b[288];
        const int k = std::snprintf(b, sizeof b,
            "[h] sig=%d pc=0x%llx addr=%p code=%d x0=0x%llx x1=0x%llx "
            "armed=%d watch=0x%llx\n", sig,
            (unsigned long long)pc,
            info ? info->si_addr : nullptr,
            info ? info->si_code : 0,
            (unsigned long long)uc->uc_mcontext.regs[0],
            (unsigned long long)uc->uc_mcontext.regs[1],
            (int)g_hooks.armed,
            (unsigned long long)g_watch_pagina);
        ssize_t w = ::write(2, b, (size_t)k); (void)w;

        // **O mapa do processo** — a regiao que contem o endereco.
        // E' a unica forma de dizer "nao esta' mapeado" de "esta' mapeado
        // mas sem escrita".
        {
            static bool feito = false;
            if (!feito && info != nullptr && sig == SIGSEGV) {
                feito = true;
                const auto alvo = reinterpret_cast<std::uintptr_t>(info->si_addr);
                FILE* f = std::fopen("/proc/self/maps", "r");
                if (f != nullptr) {
                    char linha[512];
                    char antes[8][512]{};
                    int n_antes = 0;
                    while (std::fgets(linha, sizeof linha, f) != nullptr) {
                        unsigned long long lo = 0, hi = 0;
                        if (std::sscanf(linha, "%llx-%llx", &lo, &hi) != 2)
                            continue;
                        if (alvo >= lo && alvo < hi) {
                            std::fprintf(stderr, "[maps] --- contexto ---\n");
                            for (int i = 0; i < n_antes; ++i)
                                std::fprintf(stderr, "[maps] %s", antes[i]);
                            std::fprintf(stderr, "[maps] %s", linha);
                            break;
                        }
                        std::snprintf(antes[n_antes % 8], 512, "%s", linha);
                        ++n_antes;
                    }
                    std::fclose(f);
                }
            }
        }
    }

    switch (sig) {
    case SIGTRAP: {
        // Descodificar o número de serviço do próprio BRK no PC.
        //
        // Não se usa si_value: numa trap de BRK o Linux põe si_addr = o
        // endereço da instrução, e o imediato só existe nas instruções. Ler o
        // código é o que garante que o número é o que o guest pediu.
        std::uint32_t insn = 0;
        if (pc != 0)
            std::memcpy(&insn, reinterpret_cast<const void*>(pc), 4);

        if ((insn & 0xFFE00000u) != 0xD4200000u) {
            // Não é um BRK: é outra trap (depuracao do host, ou um BRK de
            // instrumentação nosso). Contabiliza e devolve.
            *ts.last = TrapKind::Breakpoint;
            ++*ts.host_faults;
            return;
        }

        const std::uint32_t number = (insn >> 5) & 0xFFFF;

        // **O relogio do guest** — as leituras de CNTFRQ/CNTPCT/CNTVCT
        // viram `brk #(0x40|Rt)` no patcher. O SDK da Nintendo pede o tick
        // ao `cntfrq_el0` (`GetSystemTick` e' literalmente `mrs`, medido em
        // 0x83ab4fd0), que no board devolve 24_000_000 CONSTANTE — e o
        // "agora" do TimerEvent congelou: a parede do `WaitTimerEvent`
        // reentria 712 vezes de 5 ms sem chegar ao prazo
        // `ConvertToTimeSpan(24_000_000) + 5e6 = 1_255_000_000` (o x23 das
        // traps e a C do board dizem o mesmo).
        //
        // Aqui o host responde com ticks de 19,2 MHz — a escala fixa que o
        // proprio SDK usa em `GetSystemTickFrequency` — desde a primeira
        // leitura: `ns * 192 / 10000` = ns por 52,083 ns/tick. Com isto o
        // "agora" avanca em nanosegundos reais e os prazos vencem.
        //
        // Caminho curto: sem print (a leitura e' frequente) e sem despacho.
        // A estatica da epoca inicializa uma vez so' e thread-safe por
        // construcao (C++11).
        if (number >= 0x40 && number < 0x60) {
            const std::uint64_t agora = g_hooks.tel->time_now();
            static const std::uint64_t epoca = agora;
            uc->uc_mcontext.regs[number - 0x40] =
                (agora - epoca) * 192 / 10000;
            // **Medida do relogio** (`G610_TRAP_CLOCK=1`): a cada 1000
            // leituras imprime o real_ns decorrido e os ticks que o trap
            // respondeu, desde a epoca. Prova o ritmo do relogio do guest
            // contra o do host: LOG26 mediu n=1000 real_ns=990453048
            // ticks=19016698 escala=0.019 — a resposta avanca a 19,2 MHz
            // fiável, logo o relogio NAO e' a causa da cadencia da parede
            // do TimerEvent (que dorme 4,8 ms e reentra ~158x nos 1,6 s
            // de um run limitado a 4000 traps pelo tracer).
            static const bool trace_relogio =
                std::getenv("G610_TRAP_CLOCK") != nullptr;
            // **Custo por trap** (`G610_TRAP_NS=1`): o tempo entre o
            // "agora" lido no arranque do caminho rapido e o fim do
            // tratamento dele. Conta so' o trecho do handler; a entrega
            // do sinal pelo kernel (o grosso do ~1,4 us medido no
            // sandbox 980cb7f) fica fora por natureza. The a cada 5000
            // leituras para nao imprimir por trap.
            static const bool medir_custo_enviado = [] {
                static const bool en = std::getenv("G610_TRAP_NS") != nullptr;
                return en;
            }();
            if (medir_custo_enviado) {
                static std::uint64_t n_custo = 0;
                static std::uint64_t ns_custo = 0;
                static std::uint64_t ns_max = 0;
                const std::uint64_t t_fim = g_hooks.tel->time_now();
                const std::uint64_t custo = t_fim - agora;
                ns_custo += custo;
                if (custo > ns_max) ns_max = custo;
                if ((++n_custo % 5000) == 0)
                    std::fprintf(stderr,
                                 "[simclock] n=%llu ns/trap medio=%.0f max=%llu\n",
                                 (unsigned long long)n_custo,
                                 double(ns_custo) / double(n_custo),
                                 (unsigned long long)ns_max);
            }
            if (trace_relogio) {
                static std::uint32_t n_leituras = 0;
                if ((++n_leituras % 1000) == 0) {
                    const std::uint64_t ns_real = agora - epoca;
                    const std::uint64_t ticks = (agora - epoca) * 192 / 10000;
                    std::fprintf(stderr,
                                 "[simclock] n=%u real_ns=%llu ticks=%llu "
                                 "escala=%.3f\n",
                                 n_leituras,
                                 static_cast<unsigned long long>(ns_real),
                                 static_cast<unsigned long long>(ticks),
                                 static_cast<double>(ticks) /
                                     static_cast<double>(ns_real));
                }
            }
            uc->uc_mcontext.pc = pc + 4;
            return;
        }

        if (number == kBrkForceStop) {
            // Paragem pedida pelo tracer (um spin de código que não levanta
            // traps). Sai pelo mesmo caminho dos restantes.
            *ts.last = TrapKind::Breakpoint;
            *ts.limit_hit = true;
            uc->uc_mcontext.sp = rec_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        if (number == kBrkGuestReturned) {
            // O guest fez `ret` sem ExitProcess. Sair pelo mesmo caminho dos
            // faults para que o `siglongjmp` restaure o `sp` do host: sem
            // isto, o epílogo de `run()` verificaria o canário do stack
            // protector sobre a stack do guest.
            *ts.last = TrapKind::Breakpoint;
            *ts.guest_returned = true;
            uc->uc_mcontext.sp = rec_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        // **A thread principal** — os traps dela nao tinham registo
        // (o dump de baixo e' so' para as threads de guest), e e'
        // nela que o rtld fica preso.
        //
        // **O PC e' a pista** — diz ONDE o guest esta'.
        if (st == nullptr && ::getenv("IA_TRACE") != nullptr) {
            std::fprintf(stderr,
                         "[main] trap #%u pc=0x%llx brk=%u x0=0x%llx x1=0x%llx "
                         "x2=0x%llx\n",
                         number, (unsigned long long)pc, number,
                         (unsigned long long)uc->uc_mcontext.regs[0],
                         (unsigned long long)uc->uc_mcontext.regs[1],
                         (unsigned long long)uc->uc_mcontext.regs[2]);
        }

        // Numa thread de guest, cada trap e' uma pista: e' o unico registo
        // do que o guest esta' a fazer la dentro.
        if (st != nullptr) {
            // x22 = ponteiro do TickManager interno (levado no WaitTimerEvent),
            // x23 = o valor bruto do "mrs" logo antes do leito e x24 = a foto do
            // contador do evento: os tres invertem (ou confirmam) um "agora"
            // congelado. A base do print em x28 fica como ancoragem antiga.
            std::fprintf(stderr,
                         "[thrd] trap #%u pc=0x%llx sp=0x%llx x0=0x%llx x1=0x%llx "
                         "x2=0x%llx x3=0x%llx x7=0x%llx x19=0x%llx "
                         "x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx "
                         "x24=0x%llx x28=0x%llx\n",
                         number, (unsigned long long)pc,
                         (unsigned long long)uc->uc_mcontext.sp,
                         (unsigned long long)uc->uc_mcontext.regs[0],
                         (unsigned long long)uc->uc_mcontext.regs[1],
                         (unsigned long long)uc->uc_mcontext.regs[2],
                         (unsigned long long)uc->uc_mcontext.regs[3],
                         (unsigned long long)uc->uc_mcontext.regs[7],
                         (unsigned long long)uc->uc_mcontext.regs[19],
                         (unsigned long long)uc->uc_mcontext.regs[20],
                         (unsigned long long)uc->uc_mcontext.regs[21],
                         (unsigned long long)uc->uc_mcontext.regs[22],
                         (unsigned long long)uc->uc_mcontext.regs[23],
                         (unsigned long long)uc->uc_mcontext.regs[24],
                         (unsigned long long)uc->uc_mcontext.regs[28]);
            // **Cadeia de chamados** (env `G610_TRAP_FP=1`): x30 e' o retorno
            // do stub (o site que chamou o servico) e os pares [x29]/[x29+8]
            // dao a cadeia de frames do chamador — o mesmo esquema do
            // `G610_SVC_SIGNAL_FP`, mas do lado da espera. Leve: 3 leituras
            // de 16 bytes por trap, so' com o env posto.
            static const bool trap_fp = std::getenv("G610_TRAP_FP") != nullptr;
            if (trap_fp) {
                const std::uint64_t fp = uc->uc_mcontext.regs[29];
                constexpr std::uint64_t largura = 0x200000ull;
                constexpr std::uint64_t topo = 0x800000000ull;
                std::uint64_t f[6] = {0, 0, 0, 0, 0, 0};
                const bool ok = fp >= largura && fp < topo &&
                                g_hooks.svc->read_guest(fp, f, 2 * sizeof f[0]);
                if (ok && f[0] >= largura && f[0] < topo)
                    (void)g_hooks.svc->read_guest(f[0], f + 2, 2 * sizeof f[0]);
                if (ok && f[2] >= largura && f[2] < topo)
                    (void)g_hooks.svc->read_guest(f[2], f + 4, 2 * sizeof f[0]);
                std::fprintf(stderr,
                             "[thrd] trap cadeia #%u x30=0x%llx lvl1=0x%llx "
                             "lvl2=0x%llx lvl3=0x%llx\n",
                             number,
                             (unsigned long long)uc->uc_mcontext.regs[30],
                             (unsigned long long)f[1], (unsigned long long)f[3],
                             (unsigned long long)f[5]);
            }
        }

        // Despachar o serviço: argumentos em x0-x5, resultado em x0.
        SvcContext c{};
        for (int i = 0; i < 6; ++i)
            c.x[i] = uc->uc_mcontext.regs[i];
        for (int i = 0; i < 31; ++i)
            c.gpr[i] = uc->uc_mcontext.regs[i];
        c.pc = pc;
        c.sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);

        // O observer ve o contexto ANTES do dispatch, para poder registar o
        // que o guest pediu. Se registasse depois, veria o resultado em x0 e
        // não saberia o que foi pedido — que é a informação útil.
        if (g_hooks.observer != nullptr)
            g_hooks.observer(number, c, pc,
                             static_cast<std::uint64_t>(uc->uc_mcontext.sp),
                             g_hooks.observer_user);

        const auto t0 = g_hooks.tel->time_now();
        const std::uint64_t me = dispatch_token(st);
        bool locked = dispatch_take(me);
        if (locked) {
            // O token em curso parte antes do serviço: um SVC bloqueante
            // (ArbitrateLock) larga o dono e repõe `me` ao acordar. `me` e o
            // token em curso coincidem aqui; no fim, ambos voltam a 0.
            dispatch_owner_set(me);
            // **O TLS da thread que pede.** A resposta do IPC vai para o TLS
            // da thread do *cliente* (o Eden, `hle_ipc.cpp:330`) — e a casa
            // tem um só `KThread` para todas as threads de guest. Sem isto,
            // a resposta de uma thread de guest vai para o TLS da principal
            // e o guest lê-a do sítio errado (lixo) — o sintoma é um abort
            // do SDK com um erro de CMIF.
            //
            // A thread principal não tem slot (o motor conta-a à parte), e
            // o seu TLS é o `guest_tls` dos hooks.
            g_hooks.svc->set_tls_da_thread(st != nullptr ? st->guest_tls : g_hooks.guest_tls);
            // **O valor do global** — a cada SVC, para se ver
            // **quando** muda.
            if (std::getenv("IA_WATCH") != nullptr) {
                static std::uint32_t n = 0;
                if ((++n % 100) == 0) {
                    std::uint64_t v = 0;
                    if (LerGlobalDoJogo(&v)) {
                        std::fprintf(stderr, "[watch] global = 0x%llx\n",
                                     (unsigned long long)v);
                    }
                }
            }
            g_hooks.svc->dispatch(number, c);
            dispatch_release(me);
            dispatch_owner_set(0);
        } else {
            // O despacho não aconteceu: trap aninhada do próprio dono (um
            // fault do host no C++ do despacho), ou teto de spin estourou.
            // Trata como fault — a thread sai pela recuperação, que liberta
            // o dono DELA nos seus pontos de fim. Não incrementa `svc_calls`
            // nem `svc_time_ns`: o serviço não correu.
            *ts.last = TrapKind::GuestFault;
            *ts.fault_pc = pc;
            *ts.fault_sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
            *ts.fault_addr = 0;
            *ts.faulted = true;
            uc->uc_mcontext.sp = rec_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }
        *ts.svc_time_ns +=
            static_cast<std::uint64_t>(g_hooks.tel->time_now() - t0);
        ++*ts.svc_calls;

        // A ORDA IMPORTA: x0 é o registo de RETORNO na ABI do Switch.
        //
        // Primeiro os argumentos (x1-x5), que o serviço pode ter mudado como
        // canal de saída. DEPOIS o resultado em x0.
        //
        // Ao contrário, `c.ret` era escrito primeiro e o ciclo seguinte
        // sobrescrevia-o com `c.x[0]` — que para a maioria dos serviços é o
        // endereço de saída, não o retorno. O guest via um endereço onde
        // esperava um Result, e o rtld tratava isso como erro:
        //
        //     4dc:  cbnz w0, 0x708     ; Result != 0 -> unreachable
        //
        // Por isso x0 NÃO é copiado de `c.x`. Quem devolve um valor escreve
        // em `ctx.ret`.
        for (int i = 1; i < 6; ++i)
            uc->uc_mcontext.regs[i] = c.x[i];
        uc->uc_mcontext.regs[0] = c.ret;

        // Repõe x18. O `ret` do guest clobbera o platform register, e o
        // próximo SVC que verificar a canary encontraria um valor diferente
        // do que foi posto no arranque — que é o DESTA thread: cada thread
        // de guest tem a sua (o Eden gera um por thread em
        // `ResetThreadContext64`).
        if (*ts.platform_reg != 0)
            uc->uc_mcontext.regs[18] = *ts.platform_reg;

        *ts.last = TrapKind::SvcCall;

        // ExitProcess: o guest pediu para terminar. Escrever pc+4 e regressar
        // NÃO chega — o guest continuaria a executar depois do ExitProcess.
        // A saída faz-se como a dos faults: saltar para uma rotina com a
        // stack DE QUEM PEDIU, que regressa ao host por siglongjmp.
        if (g_hooks.svc->exited()) {
            uc->uc_mcontext.sp = rec_sp;
            uc->uc_mcontext.pc = g_hooks.recovery_pc;
            return;
        }

        // Saltar o BRK. O kernel honra esta escrita — medido em
        // tests/probe_ucontext.cpp.
        uc->uc_mcontext.pc = pc + 4;
        return;
    }

    case SIGSEGV:
    case SIGBUS: {
        // Antes de contar um fault, tentar o caso que NÃO é um fault: o guest
        // ter lido o seu TLS de um registo de sistema que o host também usa.
        //
        // O SDK do jogo faz `mrs x8, tpidrro_el0` para chegar ao bloco de TLS.
        // Numa execução nativa esse registo é o `fs` da thread do Linux. Neste
        // host vale 0 (medido), e a instrução seguinte — `ldr x8, [x8, #0x1F8]`
        // — acerta em 0x1f8, fora do espaço do guest: SIGSEGV.
        //
        // Trocar o registo seria a solução limpa, e foi a primeira tentativa:
        // neste kernel `msr tpidrro_el0, xn` levanta SIGILL, e
        // `arch_prctl(ARCH_SET_FS)` devolve -1. Medido nos dois, num programa
        // de 10 linhas. Não há caminho de escrita.
        //
        //resta a emular a LEITURA, que é uma instrução e não um caminho de
        //execução. Continua a ser nativo: o CPU não sabe que isto aconteceu.
        const std::uint64_t tls_de_quem = st != nullptr ? st->guest_tls
                                                        : g_hooks.guest_tls;
        if (tls_de_quem != 0 && emulate_tls_read(uc, pc, st))
            return;   // retoma o guest no PC seguinte

        // **O watchpoint** — a pagina armada em so' leitura.
        // Uma escrita nela e' o guest a escrever — e e' o que estamos
        // a procurar.
        const auto endereco_do_fault =
            reinterpret_cast<std::uintptr_t>(info->si_addr);
        if (g_watch_pagina != 0 &&
            (endereco_do_fault >= g_watch_pagina &&
             endereco_do_fault < g_watch_pagina + 0x1000)) {
            // **O valor antigo** — o que estava no sitio antes da
            // escrita.
            //
            // A instrucao e' um `str` — mas descodificar todas as
            // variantes nao vale o effort. O que interessa e' o **PC**
            // (que da o rtld) e o valor **depois**.
            // **O valor a escrever** — o `str x11, [x12, x8]`
            // (em `0x84461980`) escreve o `x11`.
            char b[512];
            int n = std::snprintf(b, sizeof b,
                                  "[watch] escrita na pagina 0x%llx (pc=0x%llx) "
                                  "x8=0x%llx x11=0x%llx x12=0x%llx dest=0x%llx\n",
                                  (unsigned long long)endereco_do_fault,
                                  (unsigned long long)pc,
                                  (unsigned long long)uc->uc_mcontext.regs[8],
                                  (unsigned long long)uc->uc_mcontext.regs[11],
                                  (unsigned long long)uc->uc_mcontext.regs[12],
                                  (unsigned long long)(uc->uc_mcontext.regs[12] +
                                                      uc->uc_mcontext.regs[8]));
            for (int i = 0; i < 8; ++i) {
                n += std::snprintf(b + n, sizeof(b) - n, " x%d=0x%llx", i,
                                   (unsigned long long)uc->uc_mcontext.regs[i]);
            }
            n += std::snprintf(b + n, sizeof(b) - n, " x30=0x%llx\n",
                               (unsigned long long)uc->uc_mcontext.regs[30]);
            ssize_t w = ::write(2, b, (size_t)n);
            (void)w;
            // A pagina volta a ser de leitura-escrita — para o guest
            // repetir a instrucao.
            ReporWatchpoint();
            // E volta a armar — no **proximo trap** (a escrita
            // actual ainda nao aconteceu).
            ArmarWatchpointNoProximoTrap();
            // O guest repete a instrucao.
            uc->uc_mcontext.pc = pc;
            uc->uc_mcontext.sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
            return;
        }

        if (::getenv("IA_TRACE")) {
            char b[224];
            int n = std::snprintf(b, sizeof b, "[tls] falhou em pc=0x%llx\n",
                                  (unsigned long long)pc);
            if (pc >= 16) {
                for (int i = 4; i >= 1; --i) {
                    std::uint32_t w = 0;
                    std::memcpy(&w, reinterpret_cast<const void*>(pc - 4 * i), 4);
                    n += std::snprintf(b + n, sizeof(b) - n,
                                       "  pc-0x%x: %08x  x0=0x%llx x8=0x%llx "
                                       "tpidrro=0x%llx\n", 4 * i, w,
                                       (unsigned long long)uc->uc_mcontext.regs[0],
                                       (unsigned long long)uc->uc_mcontext.regs[8],
                                       (unsigned long long)read_thread_pointer());
                }
            }
            ssize_t w = ::write(2, b, (size_t)n); (void)w;
        }

        ++*ts.faults;
        *ts.last = TrapKind::GuestFault;
        g_hooks.tel->note_page_fault();

        // **Instrumento: um fault do MOTOR, distinguido do fault do
        // GUEST.**
        //
        // O PC do fault fora da reserva do guest (`[0x60000000,
        // 0x310000000)`) é código **do motor** — tipicamente o handler
        // de um SVC, a meio do serviço. Sem esta linha, o relatório
        // dizia "fault de memória do guest" e o PC era um número sem
        // dono; o diagnóstico parava aí.
        //
        // O `dladdr` não é async-signal-safe (o standard diz que não
        // pode ser chamado de um handler), mas só **lê** a tabela de
        // símbolos dinâmica, sem alocar — e o caminho já é o da morte
        // do processo, onde a alternativa é não saber nada.
        if (pc < 0x60000000ull || pc >= 0x310000000ull) {
            Dl_info dl{};
            if (::dladdr(reinterpret_cast<void*>(pc), &dl) != 0 &&
                dl.dli_sname != nullptr) {
                char b[256];
                const int n = std::snprintf(
                    b, sizeof(b),
                    "[host-fault] pc=0x%llx em %s (+0x%llx) — codigo do MOTOR\n",
                    (unsigned long long)pc, dl.dli_sname,
                    (unsigned long long)(pc - (std::uintptr_t)dl.dli_saddr));
                const ssize_t w = ::write(2, b, (size_t)n);
                (void)w;
            }
            // O backtrace: o `dladdr` da só a função do topo, e o que
            // interessa é quem chamou. `backtrace_symbols_fd` escreve
            // directo no fd (sem alocar), o que é o que se pode fazer
            // de um handler.
            void* frames[24]{};
            const int nframes = ::backtrace(frames, 24);
            ::backtrace_symbols_fd(frames, nframes, 2);
        }

        // Um fault de memória não pode continuar: deixar o sigreturn
        // re-executar a mesma instrução entraria em ciclo até o processo ser
        // morto, engolindo o stack trace — o pior diagnóstico possível.
        *ts.fault_pc = pc;
        *ts.fault_sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
        *ts.fault_addr = reinterpret_cast<std::uint64_t>(info->si_addr);
        for (int i = 0; i < 22; ++i) {
            (*ts.fault_regs)[i] = uc->uc_mcontext.regs[i];
        }
        *ts.faulted  = true;

        // **Os registos** — sem eles nao se sabe o que o guest
        // procurava (o id, o ponteiro).
        {
            char linha[400];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[fault] registos:"));
            for (int i = 0; i < 8; ++i) {
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " x%d=0x%llx", i,
                                   (unsigned long long)uc->uc_mcontext.regs[i]));
            }
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, " x19=0x%llx x20=0x%llx x21=0x%llx",
                               (unsigned long long)uc->uc_mcontext.regs[19],
                               (unsigned long long)uc->uc_mcontext.regs[20],
                               (unsigned long long)uc->uc_mcontext.regs[21]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        uc->uc_mcontext.sp = rec_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    case SIGALRM: {
        // Alarme de segurança: o guest ficou preso em código (um spin que não
        // levanta traps) e o tracer quer que o programa volte ao host para
        // imprimir o relatório. Sem isto, um spin silencioso truncava a
        // saída e perdia-se o único rasto do que aconteceu.
        //
        // **O PC** — é a única pista de ONDE o guest ficou preso
        // (um `b,` — o rtld do jogo, à espera de um módulo).
        if (::getenv("IA_TRACE") != nullptr) {
            char b[192];
            const int k = std::snprintf(b, sizeof b,
                "[alarme] o guest esta preso em pc=0x%llx x0=0x%llx x1=0x%llx "
                "x2=0x%llx x19=0x%llx\n",
                (unsigned long long)pc,
                (unsigned long long)uc->uc_mcontext.regs[0],
                (unsigned long long)uc->uc_mcontext.regs[1],
                (unsigned long long)uc->uc_mcontext.regs[2],
                (unsigned long long)uc->uc_mcontext.regs[19]);
            const ssize_t w = ::write(2, b, (size_t)k);
            (void)w;
        }
        *ts.limit_hit = true;
        *ts.last = TrapKind::Breakpoint;
        uc->uc_mcontext.sp = rec_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    case SIGILL: {
        // Uma instrucao que o CPU nao reconhece. Num guest isto significa uma
        // descodificacao errada ou um patch mal formado — e nao uma saida.
        // Registado a parte para o relatorio dizer qual dos dois.
        ++*ts.faults;
        ++*ts.host_faults;
        *ts.last = TrapKind::Breakpoint;
        *ts.illegal_insn = true;
        *ts.fault_pc = pc;
        *ts.fault_sp = static_cast<std::uint64_t>(uc->uc_mcontext.sp);
        uc->uc_mcontext.sp = rec_sp;
        uc->uc_mcontext.pc = g_hooks.recovery_pc;
        return;
    }

    default:
        return;
    }
}

} // namespace g610
