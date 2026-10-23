// tests/fast_path_sandbox.cpp
//
// Sandbox isolado do fast-path de sinais. NAO entra no build do projecto
// (o g610_tests tem a sua main; o cmake seria reconfigurado). Compilar e
// rodar sozinho, no board emuos (aarch64):
//
//     g++ -O2 tests/fast_path_sandbox.cpp -o /tmp/ff_fast_path
//     /tmp/ff_fast_path
//
// O que mede: o custo POR TRAP do handler minimo (le o pc do ucontext,
// valida o word do site contra o opcode esperado, injeta o valor no
// registo certo, avanca pc em 4 e volta) contra o mesmo despacho + a
// copia do ucontext que o motor faz hoje por trap. O ganho sai em
// ns/trap e traps/s.
//
// Nota de honestidade sobre o opcode: neste host, `mrs x?, cntfrq_el0` e'
// LEGAL em EL0 — o CPU executa e nao faulta. A trap que o motor ganha no
// guest vem do patcher (os stubs do sdk.text: 0x83b0bb64 = `brk #0x21`; o
// brk chega como SIGTRAP no Linux arm64). O sandbox modeia o par
// site+handler: o site e' um `brk #imm` cujo imm codifica o registo
// destino da leitura cntfrq simulada:
//
//     imm = 0xE000 | Rt          (os bits [15:5] do encoding de
//                                 mrs x{Rt}, cntfrq_el0  = 0xD53BE000|Rt,
//                                 cujo Rt ocupa os bits [4:0])
//
// O handler le o word em pc, valida contra o brk esperado re-computado,
// descobre o Rt do imm, injeta em regs[Rt] e avanca pc += 4. Os dois
// sinais (SIGILL e SIGTRAP) ficam registados com o mesmo corpo: no motor
// servirao os dois caminhos (udf -> SIGILL, brk -> SIGTRAP); neste run o
// que ocorrera' e' o brk (SIGTRAP), quando o patcher escreve um stub.
//
// LIMITACAO MEDIDA deste sandbox: as variantes de Rt ficam em {0, 3, 15}
// (todos caller-saved do A64 PCS). Uma variante com Rt callee-saved
// (ex.: x19) CORROMPE o frame do main de C por baixo: o handler injeta
// em regs[19], o sigreturn repoe o registo da FRAME do chamador e o
// chamador acaba com um ponteiro estragado — medido aqui como SIGBUS na
// segunda trap (dbg_rt19). No motor de verdade o guest nao tem esse tipo
// de frame C por baixo, por isso a restricao e' do sandbox, nao do
// fast-path. x18 fica de fora por ser platform register (o motor da
// canary).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <signal.h>

// ---------------------------------------------------------------------------
// Encodings computados, nao adivinhados (a mesma formula dos stubs do
// sdk.text — o `brk #0x21` do 0x83b0bb64 decodifica com ela):
//
//     brk  #imm16      = 0xD4200000 | (imm16 << 5)          (bits [20:5] = imm)
//     mov  Xd, Xm      = 0xAA0003E0 | (Xm << 16) | Xd       (alias ORR, Rn=xzr)
//     subs Xn, Xn, #1  = 0xF1000000 | (1 << 10) | (Xn << 5) | Xn
//     cbnz Xn, -N      = 0xB5000000 | ((neg & 0x7FFFF) << 5) | Xn
//     ret              = 0xD65F03C0
// ---------------------------------------------------------------------------
constexpr std::uint32_t kBrkBase       = 0xD4200000u; // brk #imm16
constexpr std::uint32_t kMrsCntfrqBase = 0xD53BE000u; // mrs x{Rt}, cntfrq_el0
constexpr std::uint32_t kMovReg        = 0xAA0003E0u; // mov Xd, Xm
constexpr std::uint32_t kSubsImm1      = 0xF1000000u; // subs Xn, Xn, #imm
constexpr std::uint32_t kCbnzBase      = 0xB5000000u; // cbnz Xn, imm19
constexpr std::uint32_t kRet           = 0xD65F03C0u;

static inline std::uint32_t marker_imm(std::uint32_t rt) {
    return 0xE000u | (rt & 0x1F);
}
static inline std::uint32_t brk_word(std::uint32_t imm) {
    return kBrkBase | (imm << 5);
}
static inline std::uint32_t mov_word(std::uint32_t rd, std::uint32_t rm) {
    return kMovReg | (rm << 16) | rd;
}
static inline std::uint32_t subs1_word(std::uint32_t rn) {
    return kSubsImm1 | (1u << 10) | (rn << 5) | rn;
}
static inline std::uint32_t cbnz_word(std::uint32_t rn, std::int32_t rel4) {
    return kCbnzBase | (static_cast<std::uint32_t>(rel4 & 0x7FFFF) << 5) | rn;
}

// ---------------------------------------------------------------------------
// Estado partilhado com o handler. Sem locks, sem alocacao, sem libc no
// corpo do handler — so' aritmetica e escrita no ucontext.
// ---------------------------------------------------------------------------
struct Counters {
    volatile std::uint64_t apanhados;   // sites validos tratados
    volatile std::uint64_t rejeitados;  // word fora do padrao (o generico
                                        // de hoje teria de assumir)
};
static Counters g_ctr = {0, 0};
static volatile std::uint64_t g_seq;    // o "valor simulado" que se injeta

// ---------------------------------------------------------------------------
// O handler rapido.
// ---------------------------------------------------------------------------
static void trap_handler(int sig, siginfo_t*, void* uctx_raw) {
    (void)sig;
    ucontext_t* uc = static_cast<ucontext_t*>(uctx_raw);
    const std::uint64_t pc = uc->uc_mcontext.pc;
    if (pc < 4) {
        ++g_ctr.rejeitados;
        return;
    }

    const std::uint32_t word =
        *reinterpret_cast<const std::uint32_t*>(pc);

    // (1) e' um brk imediato? bits [31:21] = 0xD4200000>>21
    if ((word & 0xFFE00000u) != kBrkBase) {
        ++g_ctr.rejeitados;
        return;
    }

    // (2) a leitura cntfrq que o site representa: 0xD53BE000|Rt
    const std::uint32_t imm = (word >> 5) & 0xFFFFu;
    if ((imm & 0xFFE0u) != 0xE000u) {
        ++g_ctr.rejeitados;
        return;
    }
    const std::uint32_t rt = imm & 0x1Fu;

    // (3) confere o word do site contra o brk re-computado a partir do Rt
    // (o mesmo teste bitwise do opcode — apanha qualquer falha de dekod):
    //     site      = brk #(0xE000|Rt)
    //     guest     = mrs x{Rt}, cntfrq_el0 = 0xD53BE000|Rt
    if (word != brk_word(marker_imm(rt))) {
        ++g_ctr.rejeitados;
        return;
    }

    // (4) injeta o valor simulado e avanca so' o pc. O kernel honra as duas
    // escritas ao retomar — medido no tests/probe_ucontext.cpp.
    ++g_seq;
    uc->uc_mcontext.regs[rt] = g_seq;
    uc->uc_mcontext.pc = pc + 4;
    ++g_ctr.apanhados;
}

// O handler pesado: o mesmo despacho + a copia do ucontext (e um memset)
// que o motor faz hoje por trap. E' o baseline; serve so' para o delta.
static void trap_handler_pesado(int sig, siginfo_t* info, void* uctx_raw) {
    static std::uint8_t espelho[sizeof(ucontext_t)];
    std::memcpy(espelho, uctx_raw, sizeof(ucontext_t));
    std::memset(espelho, 0x11, 512);    // o passe de telemetria do motor
    trap_handler(sig, info, uctx_raw);
}

static void install(int sig, void (*fn)(int, siginfo_t*, void*)) {
    struct sigaction sa = {};
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, nullptr) != 0) {
        std::perror("sigaction");
        std::exit(1);
    }
}

// ---------------------------------------------------------------------------
// Codegen: uma funcao por variante de Rt na pagina executavel. 8 words:
//
//     +0  mov  x9, x0          ; contador = iters (o argumento)
//     +4  brk  #(0xE000|Rt)    ; O SITE = 1 instrucao; onde a trap nasce
//     +8  mov  x10, x{Rt}      ; copia o valor injetado (a prova da via)
//     +12 subs x9, x9, #1
//     +16 cbnz x9, -3          ; (imm19 -3: 12 bytes para tras = o brk)
//     +20 mov  x0, x10
//     +24 ret
//     +28 ret                  ; padding de alinhamento
// ---------------------------------------------------------------------------
static void emit_fn(std::uint32_t* p, std::uint32_t rt) {
    *p++ = mov_word(9, 0);
    *p++ = brk_word(marker_imm(rt));
    *p++ = mov_word(10, rt & 0x1Fu);
    *p++ = subs1_word(9);
    *p++ = cbnz_word(9, -3);
    *p++ = mov_word(0, 10);
    *p++ = kRet;
    *p++ = kRet;
}

using GuestFn = std::uint64_t (*)(std::uint64_t iters);

static double now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return static_cast<double>(t.tv_sec) * 1e9 +
           static_cast<double>(t.tv_nsec);
}

// Roda n traps, verifica a via, devolve o melhor tempo das passadas.
static double medir(const char* rotulo, GuestFn fn, std::uint64_t n) {
    (void)fn(128);                      // aquecimento (cache e icache)
    g_ctr.apanhados = 0;
    g_ctr.rejeitados = 0;
    const std::uint64_t seq_antes = g_seq;

    std::uint64_t ultima_prova = 0;
    double melhor = 1e30;
    for (int passada = 0; passada < 3; ++passada) {
        const double t0 = now_ns();
        ultima_prova = fn(n);
        const double dt = now_ns() - t0;
        if (dt < melhor) melhor = dt;
    }

    const bool via_ok = (ultima_prova == seq_antes + 3ull * n);
    (void)via_ok;
    std::printf("  %-6s melhor de 3: n=%llu  %6.1f ns/trap  %6.3f Mtraps/s"
                "  apanhados=%llu rejeitados=%llu\n",
                rotulo,
                static_cast<unsigned long long>(n),
                melhor / static_cast<double>(n),
                static_cast<double>(n) / (melhor / 1e9) / 1e6,
                static_cast<unsigned long long>(g_ctr.apanhados),
                static_cast<unsigned long long>(g_ctr.rejeitados));
    if (ultima_prova != seq_antes + 3ull * n) {
        std::printf("     FALHA: o ultimo valor injetado = %llu, esperava %llu\n",
                    static_cast<unsigned long long>(ultima_prova),
                    static_cast<unsigned long long>(seq_antes + 3ull * n));
        std::exit(2);
    }
    return melhor;
}

int main() {
    g_seq = 0;

    // 1. a pagina executavel (uma so' para as 3 variantes de Rt)
    void* code = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        // algumas kernels recusam W|X de uma vez: mapeia RW e mprotect
        code = mmap(nullptr, 0x1000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED) {
            std::perror("mmap");
            return 1;
        }
        if (mprotect(code, 0x1000, PROT_READ | PROT_EXEC) != 0) {
            std::perror("mprotect");
            return 1;
        }
    }
    auto* words = static_cast<std::uint32_t*>(code);
    // {0, 3, 15}: caller-saved — ver a LIMITACAO MEDIDA no cabecalho
    const std::uint32_t rts[3] = {0, 3, 15};
    GuestFn fns[3];
    for (int i = 0; i < 3; ++i) {
        emit_fn(words, rts[i]);
        fns[i] = reinterpret_cast<GuestFn>(words);
        words += 8;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + 0x1000);

    std::printf("=== fast-path sandbox (sites = brk #0xE000|Rt) ===\n");
    std::printf("sizeof(ucontext_t) = %zu bytes (o peso da copia do baseline)\n",
                sizeof(ucontext_t));
    constexpr std::uint64_t kN = 200000;

    // 2. modo rapido
    install(SIGILL,  trap_handler);
    install(SIGTRAP, trap_handler);
    std::printf("\n[FAST — so' o despacho minimo]\n");
    const double t_fast[3] = {medir("rt=0",  fns[0], kN),
                              medir("rt=5",  fns[1], kN),
                              medir("rt=19", fns[2], kN)};

    // 3. modo pesado (o motor de hoje: mesma coisa + copia do ucontext)
    install(SIGILL,  trap_handler_pesado);
    install(SIGTRAP, trap_handler_pesado);
    std::printf("\n[PESADO — + memcpy do ucontext + memset]\n");
    const double t_pesado[3] = {medir("rt=0",  fns[0], kN),
                                medir("rt=5",  fns[1], kN),
                                medir("rt=19", fns[2], kN)};

    // 4. o ganho
    std::printf("\n[GANHO] heavy/fast por variante:\n");
    for (int i = 0; i < 3; ++i) {
        std::printf("  rt=%-2d  %.2fx  (fast %.1f ns vs pesado %.1f ns)\n",
                    rts[i], t_pesado[i] / t_fast[i],
                    t_fast[i] / static_cast<double>(kN),
                    t_pesado[i] / static_cast<double>(kN));
    }
    std::printf("\nfim: seq=%llu (a via de injecao percorreu ate' ao fim)\n",
                static_cast<unsigned long long>(g_seq));
    return 0;
}

// ---------------------------------------------------------------------------
// Medida no board (RK3588, emuos@192.168.2.51, g++ -O2, 23/10/2026):
//
//     FAST:    1411.9 / 1414.7 / 1413.8 ns/trap   = 0.708 Mtraps/s
//     PESADO:  1561.0 / 1559.2 / 1562.2 ns/trap   = 0.641 Mtraps/s
//     ganho:   1.10-1.11x — a copia do ucontext (4560 bytes) + memset custa
//              ~150 ns por trap; o piso da entrega sincrona de sinal
//              (~1.4 us por brk) domina.
//     total:   3.600.768 traps no run completo (3 variantes de Rt x
//              {fast, pesado}), seq verificado em todos os pontos,
//              rejeitados = 0.
//
// Interpretacao para o motor: no spin de ~200 kHz (orcamento de ~5 us por
// trap), o fast-path rende ~0.15-0.5 us por trap — o resto do custo
// (despacho do svc, o mapeamento de IPC, os logs) e' o alvo seguinte.
// ---------------------------------------------------------------------------
