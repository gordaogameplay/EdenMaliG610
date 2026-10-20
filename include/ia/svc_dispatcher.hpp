// EdenMaliIA — despachante de SVC.
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

#include "ia/guest_memory.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ia/telemetry.hpp"

namespace ia {

// Heap por omissão quando não há programa ligado. Dentro dos 39 bits e longe
// do zero, porque devolver 0 faz o guest faultar no primeiro acesso.
inline constexpr std::uint64_t kDefaultHeapBase = 0x0000'0000'9000'0000ull;

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

class SvcDispatcher {
public:
    explicit SvcDispatcher(tel::Telemetry& tel) : tel_(tel) {}

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

    // Onde começa o heap que SetHeapSize devolve.
    //
    // Vem do layout do programa (`ia::layout::kHeapBase`). Definir aqui
    // separado permite ao despachante funcionar sem um programa — que é o
    // caso dos testes, e o caso de um dia em que o heap seja gerido de outra
    // forma.
    void set_heap_base(std::uint64_t base) noexcept { heap_base_ = base; }
    std::uint64_t heap_base() const noexcept { return heap_base_; }

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
    Result svc_unsupported(SvcContext&);



    tel::Telemetry& tel_;
    GuestMemory* mem_ = nullptr;

    // A tabela de regiões que `svcQueryMemory` consulta. Ver `set_regions()`.
    std::vector<GuestRegion> regions_;
    // Scratch reutilizável para as mensagens de erro de read_guest(). Não é
    // const, e read_guest() é const porque é uma consulta.
    mutable std::string g_err_scratch;
    std::unordered_map<std::string, std::uint32_t> port_by_name_;
    std::unordered_map<std::uint32_t, std::string> name_by_handle_;
    std::uint32_t    next_handle_ = 0x1000;
    std::uint64_t    heap_size_   = 0;
    std::uint64_t    heap_base_   = 0;
    std::uint64_t    map_memory_calls_   = 0;
    std::uint64_t    unmap_memory_calls_ = 0;
    std::uint64_t    query_memory_calls_ = 0;
    bool             exited_      = false;
    int              exit_code_   = 0;
};

} // namespace ia
