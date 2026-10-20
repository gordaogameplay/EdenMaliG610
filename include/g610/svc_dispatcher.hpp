// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — despachante de SVC.
//
// A execução nativa funciona assim: o binário ARM64 do jogo corre tal e qual no
// Cortex-A76, em modo de utilizador. Não há JIT nem intérprete. A única coisa
// que é interceptado é o `SVC` — a instrução com que o guest chama o sistema.
//
// Quando o patcher StaticPatcher encontra um `SVC #imm` na carga do programa,
// reescreve-o para `BRK #imm`. O `BRK` gera uma excepção, o tratador de sinais
// identifica o número, chama este despachante, e regressa ao guest com
// `svc_return`. O registo x0 em modo de saída é o resultado.
//
// O ganho sobre um JIT não é "executar mais depressa" — é não executar trabalho
// de tradução em lado nenhum. O que resta no caminho são estes SVC.
//
// Este ficheiro implementa os serviços de controlo do Horizon OS. Os serviços
// basados em porta (nvdrv, vi, am, audio, ...) são encaminhados para o
// SvcRegistry e respondidos pelo motor de IPC.
#pragma once

#include "g610/guest_memory.hpp"
#include "g610/kobject.hpp"

#include <cstdint>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "g610/telemetry.hpp"

namespace g610 {

// Heap por omissão quando não há programa ligado. Dentro dos 39 bits e longe
// do zero, porque devolver 0 faz o guest faultar no primeiro acesso.
inline constexpr std::uint64_t kDefaultHeapBase = 0x0000'0000'9000'0000ull;

// Prioridade da thread principal quando o NPDM nao diz nada.
//
// O libnx diz que 0x2C e' "a prioridade habitual da thread principal"
// (thread.h:39). E' um valor de referência, nao uma medicao: se o NPDM
// trouxer `main_thread_priority`, usa-se essa.
inline constexpr std::uint32_t kDefaultMainPriority = 0x2C;

// Threads livres que o processo pode criar. O Eden nao impõe um limite ao
// nosso motor — nao temos scheduler — mas o SDK le este numero ANTES de criar
// threads, e zero e' uma resposta que o faria desistir.
inline constexpr std::uint64_t kFreeThreadCount = 0x3E;

// Números de SVC do Horizon OS (ABI Nintendo). Só os que este motor
// implementa directamente; o resto devolve Result::Unsupported.
// Números de serviço.
//
// FONTE: src/core/hle/kernel/svc.h do Eden (enum SvcId), que é a implementação
// de referência. Copiados de lá, não de memória nem de documentação
// secundária — e a diferença importa: a versão anterior deste ficheiro tinha
// ExitProcess = 0x03 quando o valor real é 0x07, e GetSystemTick = 0x6E quando
// é 0x1E. Com números errados, um SVC do rtld caía no `default` e o guest
// recebia "não implementado" para uma chamada que devia funcionar — sem
// qualquer erro visível, só um arranque que não acontece.
//
// Nomes não implementados ainda assim estão listados: o patcher e a
// telemetria precisam de saber que o número é conhecido, mesmo que o
// comportamento ainda não exista.
enum class Svc : std::uint32_t {
    SetHeapSize          = 0x01,
    SetMemoryPermission  = 0x02,
    SetMemoryAttribute   = 0x03,
    MapMemory            = 0x04,
    UnmapMemory          = 0x05,
    QueryMemory          = 0x06,
    ExitProcess          = 0x07,
    CreateThread         = 0x08,
    StartThread          = 0x09,
    ExitThread           = 0x0A,
    SleepThread          = 0x0B,
    GetThreadPriority    = 0x0C,
    SetThreadPriority    = 0x0D,
    GetThreadCoreMask    = 0x0E,
    SetThreadCoreMask    = 0x0F,
    GetCurrentProcessorNumber = 0x10,
    SignalEvent          = 0x11,
    ClearEvent           = 0x12,
    MapSharedMemory      = 0x13,
    UnmapSharedMemory    = 0x14,
    CreateTransferMemory = 0x15,
    CloseHandle          = 0x16,
    ResetSignal          = 0x17,
    WaitSynchronization  = 0x18,
    CancelSynchronization = 0x19,
    ArbitrateLock        = 0x1A,
    ArbitrateUnlock      = 0x1B,
    WaitProcessWideKeyAtomic = 0x1C,
    SignalProcessWideKey = 0x1D,
    GetSystemTick        = 0x1E,
    ConnectToNamedPort   = 0x1F,
    SendSyncRequestLight = 0x20,
    SendSyncRequest      = 0x21,
    SendSyncRequestWithUserBuffer = 0x22,
    SendAsyncRequestWithUserBuffer = 0x23,
    GetProcessId         = 0x24,
    GetThreadId          = 0x25,
    Break                = 0x26,
    OutputDebugString    = 0x27,
    ReturnFromException  = 0x28,
    GetInfo              = 0x29,
    FlushEntireDataCache = 0x2A,
    FlushDataCache       = 0x2B,
    MapPhysicalMemory    = 0x2C,
    UnmapPhysicalMemory  = 0x2D,
};


// Códigos de resultado do Horizon. O guest verifica estes valores, portanto a
// semântica importa: devolver 0 é sucesso, qualquer outro é erro.
enum class Result : std::uint32_t {
    Success              = 0,
    Invalid              = 1,
    NotInitialized       = 2,
    NotSupported         = 3,
    InvalidHandle        = 7,
    OutOfMemory          = 8,
    NotFound             = 14,
    InvalidSize          = 16,
    Busy                  = 16 + 1,
    InvalidAddress        = 22,
    Busy2                = 24,
    OutOfResource        = 25,
    PermissionDenied     = 26,
    NotInitialized2      = 30,
    InvalidMemorySize     = 31,
    Unsupported           = 64,
    // `ResultTimedOut` do Eden (`svc_results.h:30`): modulo Kernel (1),
    // descricao 117. Vale 1 | (117 << 9) = 0x3AE1.
    //
    // E' o que o `WaitSynchronization` devolve quando o prazo expira sem
    // nada acontecer — e o que o SDK sabe interpretar.
    TimedOut              = 1 | (117 << 9),
};

// Contexto de um SVC. x0–x5 são os argumentos (o kernel do Switch passa-os
// pela x0 e pela x1..x5); o resultado é escrito em x0 ao regressar.
struct SvcContext {
    // x0-x5: os argumentos do serviço, pela ABI do Switch. São os únicos que
    // o despachante usa para decidir o que fazer.
    std::uint64_t x[6]  = {};

    // gpr[0..30]: TODOS os registos do guest no momento da chamada.
    //
    // Existe para diagnóstico, e a distinção importa. Ler `x[18]` quando o
    // que se quer é o registo x18 é um erro que já foi cometido: `x` só tem 6
    // entradas, e `x[18]` lê memória a seguir ao struct — que num processo com
    // outros dados dá valores plausíveis e completamente falsos.
    //
    // Foi o que aconteceu ao diagnosticar o arranque do rtld: os registos
    // "contenham ASCII" e pareceu uma pistaChanged até perceber que o ASCII
    // vinha do meu próprio heap.
    std::uint64_t gpr[31] = {};

    std::uint64_t ret   = 0; // valor devolvido em x0

    // O serviço pôs `ret`? Então NÃO é um Result a escrever por cima.
    //
    // Na ABI do Switch, x0 tanto pode ser um Result (a maioria dos serviços)
    // como um VALOR de retorno (SetHeapSize devolve o endereço do heap,
    // GetSystemTick devolve o tick, CloseHandle devolve o handle).
    //
    // Sem esta distinção, `ctx.ret = resultado` no fim de `dispatch()`
    // deitava fora o valor que o serviço tinha posto — e o `SetHeapSize`
    // devolvia 0 em vez do endereço do heap.
    bool          has_ret = false;
    std::uint64_t pc    = 0; // PC do guest, para diagnóstico
    std::uint64_t sp    = 0; // sp do guest
    bool          threaded = true;
};

std::string to_string(Svc s);
std::string to_string(Result r);

// ---------------------------------------------------------------------------
// Serviço de porta.
//
// `nvdrv` é o que interessa à GPU: é por aí que o driver NVIDIA do Switch
// recebe os comandos Maxwell. A tradução para Valhall acontece a jusante.
// ---------------------------------------------------------------------------
struct PortService {
    std::string name;
    std::uint32_t handle = 0;
    bool         connected = false;
};

// ---------------------------------------------------------------------------
// Despachante.
// ---------------------------------------------------------------------------
// Uma região do espaço do guest tal como o kernel do Switch a descreve.
//
// Não é abstracção: é o que `svcQueryMemory` devolve ao guest, e o rtld
// decide o que fazer com ela a partir de DOIS campos — `state` e
// `permission`. As permissões e os estados abaixo são os valores que o Eden
// declara em `svc_types.h:19-24` e `47-52`.
//
// Um R-X com state=Code é o que o rtld trata como módulo: em cada um deles
// lê um cabeçalho `MOD0` e extrai as tabelas de símbolos. Todo o resto é
// ignorado.
struct GuestRegion {
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    std::uint32_t state = 0;       // MemoryState
    std::uint32_t permission = 0;  // MemoryPermission

    // MemoryState (Eden, svc_types.h:19)
    static constexpr std::uint32_t kFree    = 0x00;
    static constexpr std::uint32_t kStatic  = 0x02;
    static constexpr std::uint32_t kCode    = 0x03;
    static constexpr std::uint32_t kNormal  = 0x05;

    // MemoryPermission (Eden, svc_types.h:57)
    static constexpr std::uint32_t kRead     = 1u << 0;
    static constexpr std::uint32_t kWrite    = 1u << 1;
    static constexpr std::uint32_t kExecute  = 1u << 2;
    static constexpr std::uint32_t kReadOnly = kRead;
    static constexpr std::uint32_t kReadExec = kRead | kExecute;
    static constexpr std::uint32_t kReadWrite = kRead | kWrite;
};

// O sistema do Eden, quando está ligado. Ver `attach_eden`.
//
// **Declarado no namespace, e não dentro de `SvcDispatcher`.** Declarado
// dentro, seria `SvcDispatcher::EdenSystem` — um tipo diferente do
// `g610::EdenSystem` que `eden_bridge.cpp` define, e o `unique_ptr` daria
// "invalid application of sizeof to incomplete type" ao destruir. É o mesmo
// erro do `KObjectNameGlobalData`, e pela mesma razão: uma declaração à
// frente dentro de uma classe nomeia a classe, não o namespace.
class EdenSystem;

class SvcDispatcher {
public:
    // Declarado aqui e definido no `.cpp`, como o destructor: um construtor
    // definido no cabeçalho tem de poder DESTRUIR os membros se algo
    // disparar uma excepção a meio, e isso exige o `sizeof` do `EdenSystem`
    // em cada translation unit.
    explicit SvcDispatcher(tel::Telemetry& tel);

    // Definido no `.cpp`, e não aqui.
    //
    // O `unique_ptr<EdenSystem>` tem de destruir um tipo que aqui só está
    // declarado. O destructor implícito de `SvcDispatcher` seria gerado em
    // EVERYWHERE este cabeçalho aparece — incluindo os testes, que não incluem
    // `ia/eden_bridge.hpp` — e em cada um deles o `sizeof` de um tipo
    // incompleto é um erro de compilação. Declarar o destructor aqui e
    // defini-lo no `.cpp` é a forma de o `sizeof` só acontecer num sítio onde
    // o tipo está completo.
    ~SvcDispatcher();

    // Liga o espaço de endereçamento do guest.
    //
    // Sem isto, os SVC que recebem um endereço do guest (MapMemory,
    // ConnectToNamedPort, GetInfo) desreferenciam-no directamente — e o
    // resultado depende de o endereço cair ou não dentro de algum mapeamento
    // do host. Num espaço de 39 bits isso dá SIGSEGV sem contexto, num
    // endereço que não pertence a nada.
    //
    // A mesma limitação se aplica à LEITURA: `safe_copy_string` desreferencia
    // um endereço do guest. Isso é seguro aqui porque o espaço do guest é
    // realmente memória do host, mas é uma dependência implícita que fica
    // explícita quando o despachante sabe qual é o espaço.
    void bind_memory(GuestMemory* mem) noexcept { mem_ = mem; }
    GuestMemory* memory() const noexcept { return mem_; }

    // Liga a camada de IPC do Eden (o `sm:` a sério, as portas e as sessões).
    //
    // Devolve `false` — e fica tudo como estava — quando não há memória do
    // guest ligada. É o caso dos testes que não montam um programa, e é
    // melhor que uma falha: o dispatcher continua a responder com o caminho
    // antigo, e o teste que estava a passar continua a passar.
    //
    // **Quando isto succeeds, `ConnectToNamedPort` e `SendSyncRequest` deixam
    // de ser nossos.** Deixam de ser o `connect_named_port` (uma tabela
    // handle→nome e quatro comandos de `sm` escritos à mão) e passam a ser o
    // `KObjectName` e o `KClientSession` do Eden. Ver `vendor/eden/README.md`.
    bool attach_eden(std::uint64_t pid, std::uint64_t tid);
    bool eden_attached() const noexcept { return eden_ != nullptr; }

    // O handle da thread principal, no espaço de handles do Eden.
    //
    // Vai para o x1 do entry point do guest, que é onde o Eden o põe
    // (`k_process.cpp:1006-1010`) e onde o SDK da Nintendo o lê.
    std::uint32_t main_thread_handle() const noexcept;

    // A base da TLS do guest, para o `KThread`.
    //
    // `KThread::GetTlsAddress` é o que o `KClientSession::SendSyncRequest`
    // usa para encontrar a `MessageBuffer` — é por isso que este valor tem de
    // chegar ao `EdenSystem` e não só ao `NativeEngine`.
    //
    // Vem depois de `attach_eden` porque a TLS só está montada depois de o
    // programa estar carregado.
    // (definido no .cpp: `EdenSystem` so esta completo la)")
    //
    // **E escreve aqui o handle da thread principal, em TLS+0x110.** É o Eden,
    // medido (`k_process.cpp:1014`):
    //
    //     this->GetMemory().Write32(GetInteger(main_thread->GetTlsAddress()) + 0x110,
    //                                thread_handle);
    //
    // E é o elo que faltava na cadeia toda:
    //
    //   1. o kernel escreve o handle em TLS+0x110
    //   2. o rtld lê-o e passa-o no x1 do entry do `main`
    //   3. o crt0 do SDK copia-o para `nn::os::detail::g_OsBootParamter`
    //   4. o `SdkMutexType::TryLock` compara o dono do mutex com esse valor
    //
    // Se o passo 1 der zero, o passo 4 compara zero com zero — o valor de um
    // mutex **livre** — e o SDK aborta a dizer que o mutex já é seu. Foi
    // exactamente o que aconteceu, e o sintoma (`this == NULL`) não aponta
    // para nada disto.
    //
    // **O mesmo handle que vai no x1** — o Eden usa uma variável para os dois,
    // e é o que o loader espera.
    void set_eden_tls(std::uint64_t endereco) noexcept;

    // Onde começa o heap que SetHeapSize devolve.
    //
    // Vem do layout do programa (`g610::layout::kHeapBase`). Definir aqui
    // separado permite ao despachante funcionar sem um programa — que é o
    // caso dos testes, e o caso de um dia em que o heap seja gerido de outra
    // forma.
    void set_heap_base(std::uint64_t base) noexcept { heap_base_ = base; }

    // O tamanho do heap do guest.
    //
    // **O SDK dimensiona o heap do jogo a partir do `GetInfo(21)`**
    // (`TotalNonSystemMemorySize`), e o que pede no `SetHeapSize` tem de
    // caber no que o motor mapeou. Por isso o valor tem de ser o do
    // layout, e nao o da reserva inteira.
    void set_heap_size(std::uint64_t size) noexcept { heap_size_ = size; }
    std::uint64_t heap_base() const noexcept { return heap_base_; }

    // A pilha do guest, para `GetInfo(StackRegionAddress/Size)`.
    //
    // Passado por setter como o heap, e nao lido de `layout::` directamente:
    // `guest_program.hpp` inclui este header, e ler o layout aqui seria um
    // include circular. A dependencia fica na mao de quem chama, que e quem
    // sabe o layout.
    // A região de alias, para `GetInfo(AliasRegionAddress/Size)`.
    void set_alias_region(std::uint64_t base, std::uint64_t size) noexcept {
        alias_base_ = base;
        alias_size_ = size;
    }

    // A região ASLR: onde o código do processo está mapeado.
    //
    // **Não é o heap, e não é a região de alias.** No Eden
    // (`svc_info.cpp:76-82`):
    //
    //     case InfoType::AslrRegionAddress:
    //         *result = GetInteger(process->GetPageTable().GetAliasCodeRegionStart());
    //     case InfoType::AslrRegionSize:
    //         *result = process->GetPageTable().GetAliasCodeRegionSize();
    //
    // E uma versão anterior deste motor devolvia o **heap** em 12 e 13 — o
    // que se vê no commentário de `case 16`, que fala de um `PersonalWordSize`
    // que não existe em lado nenhum. A conta estava errada e o resultado
    // também.
    //
    // A consequência medida: o SDK da Nintendo usa esta região para instalar
    // o seu heap e o seu estado. Com o heap devolvido no sítio da região ASLR,
    // o `nn::os::detail::g_OsBootParamter` do SDK ficava a zeros, e o
    // `SdkMutexType::TryLock` comparava o dono do mutex (0, livre) com esse
    // zero — concluía que o mutex já era seu, e abortava com `this == NULL`.
    void set_aslr_region(std::uint64_t base, std::uint64_t size) noexcept {
        aslr_base_ = base;
        aslr_size_ = size;
    }
    std::uint64_t aslr_base() const noexcept { return aslr_base_; }
    std::uint64_t aslr_size() const noexcept { return aslr_size_; }


    // O que `GetInfo` responde a "TotalMemorySize" e "UsedMemorySize".
    void set_memory_totals(std::uint64_t total, std::uint64_t used) noexcept {
        mem_total_ = total;
        mem_used_  = used;
    }

    // `SystemResourceSizeTotal` vem do NPDM (`system_resource_size`, 0x14).
    void set_system_resource_size(std::uint64_t v) noexcept { system_resource_size_ = v; }

    void set_stack_region(std::uint64_t base, std::uint64_t size) noexcept {
        stack_base_ = base;
        stack_size_ = size;
    }

    // A prioridade da thread principal, vinda do NPDM (`main_thread_priority`).
    // E' o que `GetThreadPriority(handle 0)` devolve: handle 0 e' a thread
    // corrente, e a thread corrente no arranque e' a principal.
    void set_main_thread_priority(std::uint32_t p) noexcept { main_priority_ = p; }
    std::uint32_t main_thread_priority() const noexcept { return main_priority_; }

    // As regiões do espaço de endereçamento, por ordem de base.
    //
    // `svcQueryMemory` responde a partir desta tabela, e a resposta é o que o
    // rtld usa para encontrar módulos: ele começa por interrogar o endereço
    // 0 e vai avançando de `base + size` em `base + size`, aceitando uma
    // região só se `state == Code` e `permission == Read|Execute`. Tudo o que
    // não cumpra isso é saltado.
    //
    // Duas consequências, ambas medidas no rtld real (binário, offset 0x4cc):
    //
    //   - a tabela tem de cobrir o espaço SEM BURACOS. Um buraco devolveria
    //     "Free" com base=0 e size=0, e o rtld parava de avançar.
    //   - a primeira região tem de começar em 0, pelo mesmo motivo: a
    //     primeira interrogação é ao endereço 0.
    //
    // Ver `GuestProgram::memory_regions()`.
    void set_regions(std::vector<GuestRegion> regions) { regions_ = std::move(regions); }
    const std::vector<GuestRegion>& regions() const noexcept { return regions_; }

    // Liga a telemetria. Toda a chamada a dispatch() é cronometrada e
    // atribuída ao slot do serviço correspondente — é esta atribuição que
    // permite responder "qual serviço do Horizon está a custar mais".
    void dispatch(std::uint32_t number, SvcContext& ctx);

    // Portas de serviço nomeadas. Devolve um handle ou 0.
    std::uint32_t connect_named_port(const std::string& name);

    // A tabela de objectos. Substitui o contador `next_handle_`, que
    // fabricava handles sem geração — e `linear_id == 0` é inválido pela
    // regra do Eden, por isso nenhum deles era um handle a sério.
    HandleTable handles_;
    std::size_t vivos_ = 0;
    const std::string& last_port() const noexcept { return last_port_; }

    // O último comando IPC despachado, e quantos houve. O tracer usa isto
    // para mostrar o que o guest perguntou — sem ele, "o jogo não arrancou" é
    // a única informação que sobra.
    const std::string& last_ipc() const noexcept { return last_ipc_; }
    std::uint64_t sync_request_calls() const noexcept { return sync_request_calls_; }
    void          close_handle(std::uint32_t handle);

    // O jogo pediu para terminar?
    bool exited() const noexcept { return exited_; }
    int  exit_code() const noexcept { return exit_code_; }
    std::uint64_t heap_size() const noexcept { return heap_size_; }

    // Lê `size` bytes do espaço do guest. Devolve false se o endereço não
    // pertencer à reserva — o que evita um SIGSEGV dentro de um handler de
    // sinais, que é o pior sítio possível para um crash.
    bool read_guest(std::uint64_t addr, void* dst, std::size_t size) const;

    // Escreve no espaço do guest.
    //
    // Qualquer SVC que devolva dados TEM de passar por aqui: devolver um
    // ponteiro para memória do host seria o host a escrever no sítio errado,
    // e o guest leria lixo. O endereço é do guest — 39 bits — e a validação
    // é a mesma que na leitura.
    bool write_guest(std::uint64_t addr, const void* src, std::size_t size);

    // Repõe o estado de execução para um novo arranque.
    //
    // Sem isto, um `exited_` que ficou a true de uma execução anterior
    // contamina a seguinte: o motor veria "o guest pediu para sair" num
    // arranque que nem sequer chamou ExitProcess, e classificaria um fault de
    // memória como saída limpa. Foi exactamente o que aconteceu nos testes —
    // um falso "não chegou a chamar ExitProcess" num guest que levava um
    // fault de propósito.
    //
    // As portas nomeadas NÃO são repostas: são estado do sistema, não do
    // processo, e um `SetHeapSize` seguido de `ConnectToNamedPort` tem de ver
    // o heap que o primeiro definiu.
    void reset_run_state() {
        exited_    = false;
        exit_code_ = 0;
        map_memory_calls_   = 0;
        unmap_memory_calls_ = 0;
        query_memory_calls_ = 0;
    }

    // Chamadas de mapeamento desde o último reset.
    //
    // O rtld faz dezenas no arranque, e vê-las a crescer é a forma mais
    // directa de saber que progrediu do loader até ao código do jogo. É uma
    // métrica de progresso, não um detalhe interno.
    std::uint64_t map_memory_calls()   const noexcept { return map_memory_calls_; }
    std::uint64_t unmap_memory_calls() const noexcept { return unmap_memory_calls_; }
    std::uint64_t query_memory_calls() const noexcept { return query_memory_calls_; }

private:
    // Cada grupo devolve Result; o resultado é escrito em ctx.ret.
    Result svc_set_heap_size(SvcContext&);
    Result svc_signal_process_wide_key(SvcContext&);
    Result svc_exit_process(SvcContext&);
    Result svc_close_handle(SvcContext&);
    Result svc_control_memory(SvcContext&);
    Result svc_get_system_tick(SvcContext&);
    Result svc_get_thread_id(SvcContext&);
    Result svc_get_process_info(SvcContext&);
    Result svc_connect_port(SvcContext&);
    Result svc_get_process_id(SvcContext&);
    Result svc_map_memory(SvcContext&);
    Result svc_unmap_memory(SvcContext&);
    Result svc_map_shared_memory(SvcContext&);
    Result svc_query_memory(SvcContext&);

    // `svcGetInfo`. O VALOR volta em x1, o Result em x0 — ver a
    // implementacao para o porque de isso ser contra-intuitivo.
    Result svc_get_info(SvcContext&);

    // Entropia para `InfoType::RandomEntropy`. Reabre `/dev/urandom` a cada
    // chamada: sao duas por arranque, e nenhum estado para gerir.
    std::uint64_t random_u64();

    // GetThreadPriority (0x0C). O valor volta em x1, como no GetInfo.
    Result svc_get_thread_priority(SvcContext& ctx);

    // SetThreadPriority (0x0D). Aceitar e' melhor que falhar: o guest trata
    // um erro como motivo para abortar, e um stub transparente deixa-o
    // seguir. A prioridade e' anotada para o relatorio.
    Result svc_set_thread_priority(SvcContext& ctx);

    Result svc_unsupported(SvcContext&);

    // ── IPC mínimo ────────────────────────────────────────────────────────
    //
    // O transporte do `sm:`. Ver `src/ipc_min.cpp` para o layout, que foi
    // medido no pedido real e conferido contra `ipc.h` do Eden.
    static constexpr std::size_t kIpcBufferMax = 0x200;

    // Despacha um pedido e escreve a resposta no MESMO buffer do guest.
    // Devolve false se não foi um pedido SMART válido.
    bool ipc_dispatch(std::uint64_t buffer, std::size_t size);

    // A geometria da resposta, lida de `IA_IPC_VARIANT`.
    //
    // Existe porque três palpites seguidos falharam e deduzir mais um ia ser
    // adivinhar. Testam-se todas as combinações e vê-se qual deixa o guest
    // avançar — que é uma forma de medir em vez de supor.
    static int ipc_variant();

    Result svc_send_sync_request(SvcContext& ctx);
    // `WaitSynchronization(handles, num_handles, timeout_ns)`.
    //
    // A ABI, do Eden (`svc.cpp:1420`):
    //
    //     x0 = timeout em nanossegundos
    //     x1 = ponteiro para o array de handles
    //     x2 = numero de handles
    //     saida: x0 = Result, x1 = o indice do handle sinalizado
    Result svc_wait_synchronization(SvcContext& ctx);
    Result svc_send_sync_request_light(SvcContext& ctx);

    // Escreve uma resposta vazia com um Result — o "não sei responder" honesto.
    void write_empty_reply(std::uint64_t buffer, std::size_t size, std::uint32_t result);

    // Handle <-> nome de serviço. É a única "sessão" que existe: um handle é
    // um inteiro e um nome, e nada mais. STUB declarado.
    std::string service_name_for_handle(std::uint32_t handle) const;

    // O objecto que um handle aponta, ou nullptr se o handle não for válido.
    // `obter()` valida a forma, o índice e a geração.
    KObject* object_for_handle(std::uint32_t handle) const;

    static constexpr std::uint32_t kResultNotImplemented = 0x00040001;

    std::string       last_ipc_;
    std::uint64_t     sync_request_calls_ = 0;



    tel::Telemetry& tel_;
    GuestMemory* mem_ = nullptr;

    // Ver `EdenSystem` acima, e `attach_eden`.
    std::unique_ptr<EdenSystem> eden_;

    // A tabela de regiões que `svcQueryMemory` consulta. Ver `set_regions()`.
    std::vector<GuestRegion> regions_;
    // Scratch reutilizável para as mensagens de erro de read_guest(). Não é
    // const, e read_guest() é const porque é uma consulta.
    mutable std::string g_err_scratch;
    std::unordered_map<std::string, Handle> port_by_name_;
    std::unordered_map<std::uint32_t, std::string> name_by_handle_;
    std::uint32_t    next_handle_ = 0x1000;
    std::uint64_t    entropy_counter_ = 0x9E3779B97F4A7C15ull;
    // A última porta a que o guest se ligou, para o relatório. STUB: não há
    // IPC por trás do handle.
    std::string       last_port_;

    // `ConnectToNamedPort` fabrica handles a partir de 0x1000. `name_by_handle_`
    // é o que permite responder a um `SendSyncRequest` sabendo a que serviço
    // ele é dirigido.

    std::uint64_t    mem_total_   = 0;
    std::uint64_t    mem_used_    = 0;
    std::uint64_t    system_resource_size_ = 0;
    std::uint64_t    alias_base_   = 0;
    std::uint64_t    alias_size_   = 0;

    std::uint64_t    aslr_base_    = 0;
    std::uint64_t    aslr_size_    = 0;
    std::uint64_t    stack_base_   = 0;
    std::uint64_t    stack_size_   = 0;
    std::uint32_t    main_priority_ = 0;
    std::int32_t     last_priority_ = 0;

    // O id do processo e o da thread principal. Fixos: o SDK usa-os como
    // chave do estado por-thread, e um id que muda entre chamadas faz o jogo
    // achar que está numa thread diferente.
    std::uint64_t    process_id_ = 1;
    std::uint64_t    thread_id_  = 1;
    std::uint64_t    heap_size_   = 0;
    std::uint64_t    heap_base_   = 0;
    std::uint64_t    map_memory_calls_   = 0;
    std::uint64_t    unmap_memory_calls_ = 0;
    std::uint64_t    query_memory_calls_ = 0;
    bool             exited_      = false;
    int              exit_code_   = 0;
};

} // namespace g610
