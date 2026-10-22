// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — motor de execução nativa (medido, não suposto).
//
// O mecanismo, verificado no Orange Pi 5 Pro com `tests/probe_ucontext.cpp`:
//
//   Quando o guest executa um `BRK` (que o patcher obteve de um `SVC`), o
//   kernel levanta SIGTRAP e entrega um `ucontext_t` cujo `pc` aponta para o
//   próprio BRK. Se o handler escrever `pc + 4`, o kernel retoma DEPOIS do
//   BRK e o guest continua. Medido: uma trap, execução completa.
//
//   Os argumentos do serviço leem-se em `regs[0..5]` e o resultado escreve-se
//   em `regs[0]`. O guest vê o valor novo. Também medido.
//
// Isto significa que **o motor não precisa de trocar registos de hardware
// dentro do handler**. A alternativa — salvar e restaurar 31 GPRs, 8 registos
// vectoriais, sp, tpidr_el0 e fpcr/fpsr por chamada de serviço — é o que o
// Eden faz, e é necessário lá porque o Eden precisa de pré-emptão entre
// threads. Para este motor, é custo sem benefício.
//
// O `tpidr_el0` como chave de TLS fica reservado para quando houver
// threads de guest mapeadas uma a uma para threads do host. Não é requisito
// para arrancar.
#pragma once

#include <csignal>
#include <cstdint>
#include <csetjmp>
#include <string>

#include "g610/guest_context.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

// Corre codigo do guest numa thread nova, com o `sp` que o guest deu.
//
// O codigo do guest e' NATIVO, logo o `sp` e' o do registo da CPU — e nao
// pode ser mudado em C++. A rotocao esta' em ensamblador, no `.cpp`.
extern "C" void g610_run_guest(std::uint64_t entry, std::uint64_t arg,
                               std::uint64_t sp, std::uint64_t platform_reg);

namespace g610 {

// ── O dono do despacho, partilhado com o SVC que precisa estacionar ───────
//
// O SvcDispatcher é serializado por um dono atómico (`g_dispatch_owner` em
// `native_engine.cpp`, passo 3). Um serviço que bloqueia (ArbitrateLock é o
// primeiro: o Eden estaciona em `KConditionVariable::WaitForAddress`,
// `k_condition_variable.cpp:156`) tem de SOLTAR o dono durante a espera —
// senão o thread que o desbloqueia não consegue despachar o
// `ArbitrateUnlock`, e o processo trava. É o que um kernel faz: a syscall
// bloqueante abandona a CPU e volta a tomá-la quando acorda.
//
// O token é o que o `native_engine.cpp` usa (0 = livre, 1 = a principal,
// GuestThreadState* = as threads de guest). Convenção: ler o token ANTES de
// soltar (outro despacho pode trocá-lo); repor o MESMO ao acordar.

// O token do despacho EM CURSO nesta thread. 0 fora dela.
std::uint64_t dispatch_owner_itself();

// Soltar o dono (só tem efeito se o dono em curso for o caller). Zera o
// token em curso.
void dispatch_owner_vacate();

// Voltar a tomar, com o mesmo limite de spin do engine. true = tomado.
bool dispatch_owner_claim();

// Repor o token em curso (chamado pela thread que acorda, com o valor que
// leu antes de soltar).
void dispatch_owner_set(std::uint64_t token);

// O que o guest fez na última trap. Registado para o relatório e para
// distinguir uma paragem por serviço de uma por fault. Definido aqui, antes
// de `GuestThreadState`, porque o estado de thread tem um campo deste tipo
// (o passo 1 deixava-o abaixo, e o struct filho ficava a referenciar um tipo
// ainda por declarar).
enum class TrapKind : std::uint8_t {
    None = 0,
    SvcCall,
    GuestFault,
    Breakpoint,
    Yield,
};

// O estado de UMA thread que executa codigo de guest.
//
// Tudo o que o handler de sinais escreve e' por thread: uma thread de guest
// que faultou nao pode reescrever o relatorio da principal, e o regresso ao
// host tem de ser para a frame DA thread que faultou.
//
// **Nao e' um `__thread`.** A razao e' especifica deste motor, e foi
// medida: enquanto o guest corre, o `TPIDR_EL0` vale o TLS do GUEST (o motor
// troca-o), e um `__thread` do host lido nesse intervalo daria o valor de um
// bloco errado. E `thread_local` (C++) em PIC passa por `__tls_get_addr`,
// que aloca - e alocar dentro de um handler de sinais e' caminho para o OOM
// (medido: o processo morria no primeiro `SendSyncRequest`).
//
// Como o handler encontra a sua thread? Pelo `TPIDR_EL0` de entrada: enquanto
// o guest corre, o registo vale a base do TLS do guest DESSA thread. Logo, o
// valor do registo identifica a thread - e le-se com um `mrs`, sem libc.
struct GuestThreadState {
    // O TLS do guest desta thread (o valor instalado em TPIDR_EL0 enquanto o
    // guest corre) e o TLS da libc desta thread. Sao trocados a volta de cada
    // trap: o handler e' codigo do host e precisa da libc, e o guest precisa
    // do TLS DELE.
    std::uint64_t guest_tls = 0;
    std::uint64_t host_tls  = 0;

    // Contadores.
    std::uint64_t svc_calls   = 0;
    std::uint64_t traps       = 0;
    std::uint64_t faults      = 0;
    std::uint64_t host_faults = 0;
    std::uint64_t svc_time_ns = 0;
    TrapKind      last        = TrapKind::None;

    // O ultimo fault DESTA thread.
    bool          limit_hit        = false;
    bool          faulted          = false;
    bool          guest_returned   = false;
    bool          illegal_insn     = false;
    std::uint64_t fault_pc         = 0;
    std::uint64_t fault_sp         = 0;
    std::uint64_t fault_addr       = 0;

    // **Os registos no momento do fault** ✗ — sem eles ✗ nao se sabe o
    // que o guest procurava ✗ ✗ (o id ✗, o ponteiro ✗) ✗ ✗.
    std::uint64_t fault_regs[22]   = {0};

    // O `x18` DESTA thread — o "platform register" (stack canary).
    //
    // O Eden gera um valor POR THREAD: `ctx.r[18] = Random64(0) | 1` em cada
    // `ResetThreadContext64` (`k_thread.cpp:58`, Eden no Pi). A canary é
    // guardada pela frame que a viu no prólogo, logo só tem de ser
    // consistente DENTRO da thread — e threads diferentes terem canaries
    // diferentes é o que o Eden produz. O handler repõe o `x18` desta
    // estrutura depois de cada SVC.
    std::uint64_t platform_reg     = 0;

    // O ponto de regresso ao host desta thread, e a stack onde a recuperacao
    // de faults corre. A stack e' propria porque a do guest pode estar
    // corrompida - e o regresso tem de ser para a frame desta thread.
    sigjmp_buf    jmp{};
    bool          have_jmp = false;
    alignas(16) std::uint8_t recovery_stack[64 * 1024];
};

// Resumo de uma execução do guest.
struct RunResult {
    std::uint64_t svc_calls     = 0;  // quantos serviços foram despachados
    std::uint64_t traps         = 0;
    std::uint64_t faults        = 0;
    std::uint64_t host_faults   = 0;  // traps que NAO eram do guest
    std::uint64_t svc_time_ns   = 0;
    std::uint64_t guest_x0      = 0;  // registo deixado pelo guest
    bool          exited        = false;
    int           exit_code     = 0;
    TrapKind      last_trap     = TrapKind::None;

    // O endereço que o CPU tentou acecer no último fault, e o PC onde isso
    // aconteceu. O PC sozinho não serve para nada: um fault de conteúdo e um
    // fault de mapeamento dão o mesmo PC e padrões completamente diferentes.
    std::uint64_t fault_pc      = 0;
    std::uint64_t fault_addr    = 0;
    std::string   error;
};

// Ponteiros instalados pelo handler de sinais. São file-scope (e não
// thread-local) porque um handler tem de os encontrar sem saber que thread o
// levantou.
//
// Com threads de guest, o arranjo é: **a thread principal conta aqui** (é a
// que corre em `run()`); cada thread criada pelo jogo conta no SEU
// `GuestThreadState` (tabela `g_thread_slots` em `native_engine.cpp`). Este
// struct passa a levar sobretudo CONFIGURAÇÃO — despacho, observador,
// plataforma, limite de traps, recuperação — e os TLS da principal.
struct EngineHooks {
    SvcDispatcher* svc      = nullptr;
    tel::Telemetry* tel     = nullptr;
    std::uint64_t   svc_calls   = 0;
    std::uint64_t   traps      = 0;
    std::uint64_t   faults     = 0;
    std::uint64_t   host_faults = 0;
    std::uint64_t   svc_time_ns = 0;
    TrapKind        last       = TrapKind::None;
    bool            armed      = false;
    bool            limit_hit  = false;

    // Observer de SVC. Vive aqui e não na instância porque o handler de
    // sinais é estático e não sabe que instância o instalou.
    using Observer = void (*)(std::uint32_t number, SvcContext& ctx,
                              std::uint64_t pc, std::uint64_t sp, void* user);
    Observer        observer      = nullptr;
    void*           observer_user = nullptr;

    // Chamado a cada trap, seja SVC ou outra. Para diagnóstico de "o guest
    // corre mas não avança": sem ver o PC não há forma de distinguir um
    // guest parado de um guest em ciclo.
    using Trapper = void (*)(int sig, std::uint64_t pc, std::uint64_t sp,
                             void* user);
    Trapper         trapper      = nullptr;
    void*           trapper_user = nullptr;

    // Recuperação de fault: o handler salta para `recovery_addr` com
    // `recovery_sp`, e daí um siglongjmp regressa ao host.
    //
    // Porque não se deixa o sigreturn repetir a instrução que levantou o
    // fault: isso entra em ciclo até o processo ser morto, e o stack trace
    // original — o que interessa para diagnosticar — desaparece.
    // x18 do guest (stack canary). Reposto em cada SVC porque um `ret` do
    // guest o clobbera, e o próximo SVC que verificar a canary encontraria um
    // valor diferente do posto no arranque.
    std::uint64_t   platform_reg = 0;

    // Base do TLS do guest, e o TLS da thread do host, trocados à volta da
    // execução do guest.
    //
    // O guest lê o seu TLS de `TPIDR_EL0` (é o registo que no Switch o kernel
    // programa por thread). Como o guest corre NATIVO no mesmo CPU que o host,
    // esse registo pertence à thread do Linux. O `mrs` devolve um endereço do
    // host, o `ldr` seguinte acerta fora do espaço do guest, e o motor reporta
    // um fault que não tem nada a ver com o código do jogo.
    //
    // Trocar o registo é preferível a descodificar o `mrs`: o registo é
    // trocado em modo de utilizador, custa um ciclo, e não exige emulação de
    // nenhuma instrução — que era o princípio do motor.
    //
    // A troca tem de acontecer dentro do handler de sinais, não só à volta de
    // `run()`: o handler chama `printf` e telemetria, e isso é código do host
    // a correr com o TLS do guest instalado.
    std::uint64_t   guest_tls = 0;
    std::uint64_t   host_tls  = 0;

    // O x1 do guest no entry point: o handle da thread principal.
    //
    // Ver `g610_call_guest.S`. Zero é o valor que o motor usava, e zero é
    // **o valor de um mutex livre** — o que fazia o SDK da Nintendo abortar.
    std::uint64_t   guest_x1 = 0;

    // 0 = sem limite. Ao exceder, o handler sai pela rotina de recuperação.
    std::uint64_t   trap_limit   = 0;

    std::uint64_t   recovery_pc = 0;
    std::uint64_t   recovery_sp   = 0;
    std::uint64_t   fault_pc      = 0;  // para o relatório
    std::uint64_t   fault_sp      = 0;

    // O ENDEREÇO que o CPU tentou aceder, não o PC. São coisas diferentes, e a
    // diferença é o diagnóstico inteiro: o PC não diz se o guest foi ler ao
    // sítio certo com a permissão errada, ou ao sítio errado.
    std::uint64_t   fault_addr    = 0;
    bool            faulted       = false;

    // **Os registos no momento do fault** ✗ — sem eles ✗ nao se sabe o
    // que o guest procurava ✗ ✗ (o id ✗, o ponteiro ✗) ✗ ✗.
    std::uint64_t   fault_regs[22] = {0};

    // O guest fez `ret` sem ter chamado ExitProcess. Distingue esse bug de
    // uma saída limpa e de um fault, para o relatório não mentir.
    bool            guest_returned = false;

    // O guest executou uma instrução que o CPU não reconhece (SIGILL). Num
    // jogo real significa descodificação errada ou patch mal formado.
    bool            illegal_insn = false;
};

class NativeEngine {
public:
    NativeEngine(tel::Telemetry& tel, SvcDispatcher& svc);
    ~NativeEngine();

    NativeEngine(const NativeEngine&) = delete;
    NativeEngine& operator=(const NativeEngine&) = delete;

    // A base do TLS do guest, que o motor vai instalar em `TPIDR_EL0`.
    //
    // Só o boot o sabe: o motor não conhece o layout do programa, e um teste
    // unitário que corra um guest sintético não tem bloco nenhum. Deixar a 0
    // significa "não tocar no `TPIDR_EL0`", que é o comportamento certo
    // quando não há guest — e é o que evita que a libc do host fique com um
    // `fs` a apontar para memória que não existe.
    void set_guest_tls(std::uint64_t base) noexcept { hooks_.guest_tls = base; }

    // Corre o codigo do guest **nesta** thread, com o `TPIDR_EL0` apontado
    // para o TLS `tls` e com o ponto de regresso **dela**.
    //
    // Devolve 0 se o guest voltou, e 1 se o interrompeu um fault — nesse
    // caso, a thread acaba aqui e o processo continua.
    static int run_guest_thread(std::uint64_t entry, std::uint64_t arg,
                                std::uint64_t sp, std::uint64_t tls);

    // O x1 do guest no entry point. Ver `EngineHooks::guest_x1`.
    void set_guest_x1(std::uint64_t v) noexcept { hooks_.guest_x1 = v; }

    // Instala os handlers. Uma vez. Sem isto um BRK mata o processo.
    bool install_handlers(std::string& error);

    // Executa `code` como se fosse o entry point do guest, com `sp` como pilha.
    // `code` deve terminar com um BRK ou um SVC ExitProcess.
    //
    // Retorna quando o guest chama ExitProcess, levanta uma trap que não sabe
    // tratar, ou cai numa região sem permissão.
    RunResult run(void (*code)(), std::uint64_t sp);

    // Limite de traps. Ao excedê-lo, o guest é parado e o relatório diz porquê.
    //
    // Não é só conveniência de diagnóstico: um guest em ciclo infinito é um
    // processo a 100% de CPU sem forma de o parar a partir de fora, e a única
    // saída seria matar o processo do host — o que perde a telemetria e o
    // relatório que explicam *porque* está em ciclo.
    //
    // Zero (o omisso) significa sem limite. Para um jogo real convém pôr um:
    // um jogo não faz 10^8 traps num arranque.
    void set_trap_limit(std::uint64_t n) noexcept { hooks_.trap_limit = n; }

    // Prepara os registos de plataforma antes de entrar no guest.
    //
    // x18 é o "platform register" na AAPCS64: o guest usa-o como stack
    // canary. Sem o inicializar, o guest lê lixo, a verificação da canary
    // falha, e o código de recuperação chama `ReturnFromException` — que é
    // exactamente o primeiro SVC que o rtld nos pediu, com um x0 que não é
    // um endereço de 39 bits.
    //
    // O Eden faz o mesmo em `ResetThreadContext64` (k_thread.cpp:58):
    //
    //     ctx.r[18] = Common::Random::Random64(0) | 1;
    //
    // O bit 0 a 1 é deliberado: o LSB de uma canary nunca é zero, para que
    // uma string de canary não possa ficar com um byte NUL no fim.
    void set_platform_register(std::uint64_t value) noexcept {
        platform_reg_ = value | 1u;
    }

    // Chamado pelo handler a cada SVC, com o número e o contexto já lidos.
    //
    // Existe para o traçador de arranque: o guest real diz quais serviços
    // pede, e isso só se sabe deixando-o correr. Um hook é melhor do que
    // imprimir de dentro do handler — `printf` não é async-signal-safe, e
    // chamar código arbitrário de um handler durante o arranque de um guest
    // real é exactamente o tipo de coisa que transforma um bug num crash sem
    // diagnóstico.
    void set_observer(EngineHooks::Observer fn, void* user) noexcept {
        hooks_.observer      = fn;
        hooks_.observer_user = user;
        publish_hooks();
    }

    void set_trapper(EngineHooks::Trapper fn, void* user) noexcept {
        hooks_.trapper      = fn;
        hooks_.trapper_user = user;
        publish_hooks();
    }

    bool armed() const noexcept { return hooks_.armed; }

private:
    static void signal_trampoline(int sig, siginfo_t* info, void* raw);

    // O corpo do handler, sem a troca de TLS. Ver `signal_trampoline` para
    // porque é que a troca está à volta e não dentro.
    //
    // `st` diz de quem é a trap: `nullptr` = trap da thread principal (conta
    // nos contadores de `EngineHooks`, como sempre) ou do próprio host. Nas
    // threads de guest registadas, os contadores e o fault vão para o SEU
    // `GuestThreadState` — uma thread a faultar nunca sobrescreve o
    // relatório da outra.
    static void handle_guest_signal(int sig, siginfo_t* info, void* raw,
                                    GuestThreadState* st);

    // Copia os hooks para a cópia que o handler de sinais lê. Qualquer
    // alteração depois de `install_handlers` tem de passar por aqui: o handler
    // é estático e não vê a instância, por isso um campo alterado só na
    // instância é silenciosamente ignorado.
    void publish_hooks();

    // Repõe os contadores na cópia que o handler de sinais escreve.
    void reset_counters();

    tel::Telemetry& tel_;
    SvcDispatcher&  svc_;
    EngineHooks     hooks_;
    std::uint64_t   platform_reg_ = 0;

    // O x1 do guest no entry point. Ver `EngineHooks::guest_x1`.
    std::uint64_t   guest_x1_ = 0;


};

// Executa `code` com `sp` no registo de stack, como `void()`.
//
// Definida em assembly (src/g610_call_guest.S), daí o `extern "C"` sem o qual o
// linker procura um nome com agressão de nomes.
//
// O terceiro argumento é o platform register (x18) — a stack canary do
// guest. Não é opcional: sem ele o primeiro `__stack_chk_fail` do guest
// falha e chama `ReturnFromException`.
//
// Não há return: a função só volta se o código do guest RETORNAR, o que num
// guest real não acontece (ele chama ExitProcess, ou fica em ciclo). O
// `[[noreturn]]` documenta a intenção e evita que o optimizador assuma um
// regresso que nunca vem.
extern "C" [[noreturn]] void g610_call_guest(void (*code)(), std::uint64_t sp,
                                            std::uint64_t platform_reg,
                                            std::uint64_t guest_x1);

} // namespace g610
