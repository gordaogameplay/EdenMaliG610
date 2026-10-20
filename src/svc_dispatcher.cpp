// EdenMaliIA — implementação do despachante de SVC.
#include "ia/svc_dispatcher.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace ia {

namespace {
std::uint64_t host_tick_ns() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// O guest valida strings pela zona terminador. Esta leitura é defensiva para
// não rebentar o processo do host com um apontador do guest sem NUL.
//
// Nota: `addr` é um endereço do espaço de 39 bits do guest. Numa configuração
// real isto tem de passar por um tradutor de memória do guest em vez de um
// dereference directo — ver a nota sobre GuestMemory em nso_loader.hpp. Está
// assim para manter este ficheiro legível; o tradutor entra em fase posterior.
} // namespace

bool SvcDispatcher::read_guest(std::uint64_t addr, void* dst, std::size_t size) const {
    if (mem_ != nullptr)
        return mem_->read(addr, dst, size, g_err_scratch);
    // Sem espaço ligado: não se pode garantir que o endereço é válido. Um
    // dereference directo aqui seria um SIGSEGV dentro de um handler de
    // sinais, que é o pior sítio possível para um crash.
    return false;
}

bool SvcDispatcher::write_guest(std::uint64_t addr, const void* src,
                                std::size_t size) {
    if (mem_ == nullptr || size == 0)
        return false;
    return mem_->write(addr, src, size, g_err_scratch);
}

namespace {

bool safe_copy_string(SvcDispatcher& d, std::uint64_t guest_addr, std::string& out,
                      std::size_t max) {
    if (guest_addr == 0)
        return false;
    out.assign(max, '\0');
    if (!d.read_guest(guest_addr, out.data(), max))
        return false;
    // O comprimento vem do guest e é exactamente o número de bytes a ler. O
    // terminador não precisa de estar dentro desse intervalo: o driver do
    // Switch passa o comprimento, não um strlen.
    return true;
}
} // namespace

std::string to_string(Svc s) {
    switch (s) {
    case Svc::SetHeapSize:             return "SetHeapSize";
    case Svc::SetMemoryPermission:     return "SetMemoryPermission";
    case Svc::SetMemoryAttribute:      return "SetMemoryAttribute";
    case Svc::MapMemory:               return "MapMemory";
    case Svc::UnmapMemory:             return "UnmapMemory";
    case Svc::QueryMemory:             return "QueryMemory";
    case Svc::ExitProcess:             return "ExitProcess";
    case Svc::ExitThread:              return "ExitThread";
    case Svc::SignalEvent:             return "SignalEvent";
    case Svc::ClearEvent:              return "ClearEvent";
    case Svc::MapSharedMemory:         return "MapSharedMemory";
    case Svc::UnmapSharedMemory:       return "UnmapSharedMemory";
    case Svc::CreateTransferMemory:    return "CreateTransferMemory";
    case Svc::CloseHandle:             return "CloseHandle";
    case Svc::GetSystemTick:           return "GetSystemTick";
    case Svc::ConnectToNamedPort:      return "ConnectToNamedPort";
    case Svc::SendSyncRequestLight:    return "SendSyncRequestLight";
    case Svc::SendSyncRequest:         return "SendSyncRequest";
    case Svc::GetProcessId:            return "GetProcessId";
    case Svc::GetThreadId:             return "GetThreadId";
    case Svc::Break:                   return "Break";
    case Svc::OutputDebugString:       return "OutputDebugString";
    case Svc::GetInfo:                 return "GetInfo";
    case Svc::FlushEntireDataCache:    return "FlushEntireDataCache";
    case Svc::FlushDataCache:          return "FlushDataCache";
    case Svc::MapPhysicalMemory:       return "MapPhysicalMemory";
    case Svc::UnmapPhysicalMemory:     return "UnmapPhysicalMemory";
    }
    return "Svc#" + std::to_string(static_cast<std::uint32_t>(s));
}

std::string to_string(Result r) {
    switch (r) {
    case Result::Success:           return "Success";
    case Result::Invalid:           return "Invalid";
    case Result::NotInitialized:    return "NotInitialized";
    case Result::NotSupported:      return "NotSupported";
    case Result::InvalidHandle:     return "InvalidHandle";
    case Result::OutOfMemory:       return "OutOfMemory";
    case Result::NotFound:          return "NotFound";
    case Result::InvalidAddress:    return "InvalidAddress";
    case Result::PermissionDenied:  return "PermissionDenied";
    case Result::OutOfResource:     return "OutOfResource";
    case Result::Unsupported:       return "Unsupported";
    default:                        return "Result#" + std::to_string((std::uint32_t)r);
    }
}

// ---------------------------------------------------------------------------

std::uint32_t SvcDispatcher::connect_named_port(const std::string& name) {
    if (name.empty())
        return 0;
    auto it = port_by_name_.find(name);
    if (it != port_by_name_.end())
        return it->second;

    const std::uint32_t h = next_handle_++;
    port_by_name_.emplace(name, h);
    name_by_handle_.emplace(h, name);
    return h;
}

void SvcDispatcher::close_handle(std::uint32_t handle) {
    auto it = name_by_handle_.find(handle);
    if (it == name_by_handle_.end())
        return;
    port_by_name_.erase(it->second);
    name_by_handle_.erase(it);
}

// ---------------------------------------------------------------------------
// dispatch — o caminho quente.
// ---------------------------------------------------------------------------

void SvcDispatcher::dispatch(std::uint32_t number, SvcContext& ctx) {
    const tel::Nanos t0 = tel_.time_now();
    tel_.note_svc();
    tel_.exec().svc_calls.fetch_add(1, std::memory_order_relaxed);

    Result r;
    switch (static_cast<Svc>(number)) {
    case Svc::SetHeapSize:          r = svc_set_heap_size(ctx); break;
    case Svc::ExitProcess:          r = svc_exit_process(ctx);  break;
    case Svc::CloseHandle:          r = svc_close_handle(ctx);  break;
    case Svc::SetMemoryPermission:  r = svc_control_memory(ctx); break;
    case Svc::GetSystemTick:        r = svc_get_system_tick(ctx); break;
    case Svc::GetThreadId:          r = svc_get_thread_id(ctx); break;
    case Svc::GetProcessId:         r = svc_get_process_id(ctx); break;
    case Svc::ConnectToNamedPort:   r = svc_connect_port(ctx);   break;
    case Svc::MapMemory:            r = svc_map_memory(ctx);     break;
    case Svc::UnmapMemory:          r = svc_unmap_memory(ctx);   break;
    case Svc::MapSharedMemory:      r = svc_map_shared_memory(ctx); break;
    case Svc::UnmapSharedMemory:    r = svc_unmap_memory(ctx);   break;
    case Svc::QueryMemory:          r = svc_query_memory(ctx);   break;
    case Svc::GetInfo:              r = svc_get_info(ctx);       break;
    default:                        r = svc_unsupported(ctx);    break;
    }

    // Só escreve o Result se o serviço não tiver devolvido um valor.
    // Ver `SvcContext::has_ret`.
    if (!ctx.has_ret)
        ctx.ret = static_cast<std::uint64_t>(r);

    // A medição é o produto principal do motor: cada SVC entra no histograma
    // com o seu custo, e o resumo aponta o mais caro.
    tel_.services().record(number, tel_.time_now() - t0);
}

Result SvcDispatcher::svc_set_heap_size(SvcContext& ctx) {
    // ABI do Switch: SetHeapSize(u64 size) -> u64 heap_base
    //
    // x0 à entrada é o TAMANHO pedido; x0 à saída é o ENDEREÇO BASE do heap.
    // São coisas diferentes, e confundir as duas é o que fazia esta função
    // devolver 0 — o que faz o rtld tratar o heap como se estivesse na página
    // zero, e o primeiro acesso do guest a 0 ser um fault de memória sem
    // nenhum contexto.
    //
    // O rtld usa este endereço como base para os blocos que pede depois com
    // MapMemory, portanto tem de estar dentro da reserva que fizémos.
    const std::uint64_t requested = ctx.x[0];
    if (requested == 0)
        return Result::Invalid;

    // Não temos uma política de heap sophisticated: devolvemos o início da
    // reserva que o layout definiu. É o que o Eden faz para NCE (o heap é
    // gerido pelo próprio runtime do guest, não pelo emulador), e é
    // suficiente para o rtld arrancar.
    std::uint64_t base = heap_base_;
    if (base == 0) {
        // Sem layout ligado: ainda assim devolver um endereço dentro dos 39
        // bits é melhor que 0. Um endereço plausível deixa o rtld avançar até
        // ao primeiro MapMemory, onde o erro é visível.
        base = kDefaultHeapBase;
    }

    heap_size_ = requested;
    // O endereço base é o VALOR DE RETORNO. Na ABI do Switch, x0 é o registo
    // de retorno; escrevê-lo em ctx.x[0] não chega ao guest porque o motor
    // substitui x0 por ctx.ret.
    ctx.ret     = base;
    ctx.has_ret = true;
    return Result::Success;
}

Result SvcDispatcher::svc_exit_process(SvcContext& ctx) {
    exited_    = true;
    exit_code_ = static_cast<int>(ctx.x[0]);
    return Result::Success;
}

Result SvcDispatcher::svc_close_handle(SvcContext& ctx) {
    close_handle(static_cast<std::uint32_t>(ctx.x[0]));
    return Result::Success;
}

Result SvcDispatcher::svc_control_memory(SvcContext&) {
    // O heap do guest é gerido pelo próprio runtime com mmap directo; o SVC
    // ControlMemory do Switch é gerido pelo processador de heap, e aqui a
    // alocação de heap é partilhada. Não é seguro inventar semântica aqui.
    return Result::Success;
}

Result SvcDispatcher::svc_get_system_tick(SvcContext& ctx) {
    // O Switch conta em 19.2 MHz. A origem do host é o monotonic clock do
    // Linux, que está em ns. Converter mantém a progressão monótona, que é o
    // que o guest usa para await e timeouts.
    const auto ns = host_tick_ns();
    constexpr std::uint64_t kHz = 19'200'000;
    ctx.ret     = (ns / 1000) * kHz / 1'000'000'000;
    ctx.has_ret = true;
    return Result::Success;
}

Result SvcDispatcher::svc_get_process_info(SvcContext& ctx) {
    // O guest valida o tamanho antes de escrever. Respeitar o pedido evita
    // corromper memória do host.
    const auto out_size = static_cast<std::size_t>(ctx.x[1]);
    if (out_size < 0x18)
        return Result::InvalidSize;
    // O ProcessInfo é escrito pelo serviço no espaço do guest; o retorno é o
    // Result. Deixar x0 intacto aqui devolveria o endereço de saída ao guest.
    return Result::Success;
}

Result SvcDispatcher::svc_get_thread_id(SvcContext& ctx) {
    ctx.ret     = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    ctx.has_ret = true;
    return Result::Success;
}

Result SvcDispatcher::svc_connect_port(SvcContext& ctx) {
    // ABI do Switch: svcConnectToNamedPort(u32* out_port, const char* name,
    // u32 name_len) — x0 é o ponteiro de saída, x1 o nome, x2 o comprimento.
    std::string name;
    const std::size_t len = static_cast<std::size_t>(ctx.x[2]);
    if (len == 0 || len > 256)
        return Result::Invalid;
    if (!safe_copy_string(*this, ctx.x[1], name, len))
        return Result::Invalid;

    const std::uint32_t h = connect_named_port(name);
    if (h == 0)
        return Result::NotFound;

    // Escrever no ESPAÇO DO GUEST, não num ponteiro do host.
    //
    // A versão anterior fazia `*reinterpret_cast<u32*>(ctx.x[0]) = h`. Numa
    // configuração de teste, onde x0 é um ponteiro do host, isso funciona. Em
    // produção x0 é um endereço de 39 bits do guest e a escrita vai para o
    // sítio errado — ou, pior, para memória viva do processo do host.
    //
    // Um teste com ponteiros do host passa e o jogo rebenta. A mesma classe
    // de erro do `x[18]`: um valor que parece funcionar porque o teste está
    // a testar outra coisa.
    if (ctx.x[0] != 0 && !write_guest(ctx.x[0], &h, sizeof(h)))
        return Result::Invalid;
    return Result::Success;
}

// ---------------------------------------------------------------------------
// Serviços de memória
//
// Estes são os que o rtld usa antes de tocar em código do jogo. Sem eles o
// guest não arranca, e o sintoma é um SIGSEGV num endereço que não pertence a
// nada — sem nenhum contexto que diga quem o pediu.
// ---------------------------------------------------------------------------

// ABI do Switch: MapMemory(dst, src, size)
//   x0 = endereço virtual de destino no espaço do guest
//   x1 = endereço físico de origem (ignorado: num guest nativo o "físico" é o
//        nosso próprio endereço de host)
//   x2 = tamanho em bytes
//
// O rtld usa isto para projectar os segmentos do main e da sdk emendereços
// que ele próprio escolhe, dentro da região de heap.
//
// O que devolve: Success se o destino estiver dentro da reserva que fizémos.
Result SvcDispatcher::svc_map_memory(SvcContext& ctx) {
    const std::uint64_t dst  = ctx.x[0];
    const std::uint64_t size = ctx.x[2];

    if (mem_ == nullptr)
        return Result::NotInitialized;
    if (size == 0)
        return Result::Invalid;

    // O endereço é do guest: tem de estar dentro do que reservámos. Mapear
    // fora disso é mapear por cima de outra coisa do processo do host, e o
    // kernel do Switch rejeitaria. É a mesma razão por que se usa
    // MAP_FIXED_NOREPLACE — um endereço errado tem de ser um erro visível,
    // não uma corruption silenciosa.
    if (!mem_->owns(dst) || !mem_->owns(dst + size - 1))
        return Result::Invalid;

    // A memória dentro da reserva já é do processo e já é acessível. O
    // "mapeamento" que o Switch faria (traduzir físico -> virtual) aqui é um
    // no-op, porque o guest e o host partilham o espaço de endereçamento.
    ++map_memory_calls_;
    return Result::Success;
}



Result SvcDispatcher::svc_map_shared_memory(SvcContext& ctx) {
    // Mesma semântica que MapMemory num guest nativo: "partilhar" memória
    // entre processos dentro do mesmo espaço é um no-op, porque já é a mesma
    // memória física.
    return svc_map_memory(ctx);
}

Result SvcDispatcher::svc_unmap_memory(SvcContext&) {
    // Desfazer o mapeamento exigiria mudar as permissões da página, e o rtld
    // pode pedir páginas que ele próprio mapeou. Um no-op é mais seguro do que
    // um desmapeamento parcial que deixa o guest a ler memória sem acesso.
    // Registado para que o contador mostre que aconteceu.
    ++unmap_memory_calls_;
    return Result::Success;
}

// ---------------------------------------------------------------------------
// QueryMemory (0x06) — o PRIMEIRO serviço que o rtld pede.
//
//   x0 = endereço onde escrever a MemoryInfo (8+8+4+4+4+4+4+4 = 40 bytes)
//   x1 = endereço onde escrever a PageInfo   (4 bytes)
//   x2 = o endereço a interrogar
//
// Este é o serviço que destravou o arranque: sem resposta, o rtld fica a
// espera e nunca chega ao código do jogo. E é o primeiro a aparecer porque é
// o primeiro que precisa de saber que a memória do guest é válida.
//
// O layout é o de `Svc::MemoryInfo` (svc_types.h:478 no Eden), copiado
// literalmente:
//
//     u64 base_address;  u64 size;  MemoryState state;
//     MemoryAttribute attribute;    MemoryPermission permission;
//     u32 ipc_count;  u32 device_count;  u32 padding;
//
// Não há tabelas de páginas no guest nativo, por isso a resposta vem do
// layout: o bloco é a região do espaço onde o endereço cai. É a mesma
// informação que um page table daria, sem o custo de manter um.
Result SvcDispatcher::svc_query_memory(SvcContext& ctx) {
    const std::uint64_t out_info  = ctx.x[0];
    const std::uint64_t out_page  = ctx.x[1];
    const std::uint64_t query     = ctx.x[2];

    if (mem_ == nullptr)
        return Result::NotInitialized;

    struct MemoryInfo {
        std::uint64_t base_address;
        std::uint64_t size;
        std::uint32_t state;
        std::uint32_t attribute;
        std::uint32_t permission;
        std::uint32_t ipc_count;
        std::uint32_t device_count;
        std::uint32_t padding;
    };
    static_assert(sizeof(MemoryInfo) == 0x28, "MemoryInfo tem 40 bytes");

    MemoryInfo info{};
    info.base_address = 0;
    info.size         = 0;
    info.state        = 0;   // Free
    info.attribute    = 0;
    info.permission   = 0;   // None
    info.ipc_count    = 0;
    info.device_count = 0;
    info.padding      = 0;

    // A resposta vem da tabela de regiões, e não da reserva inteira.
    //
    // Isto não é um detalhe. O rtld descriptor de módulos com esta chamada:
    // começa no endereço 0 e avança `base + size`, e só aceita uma região se
    // `state == Code` (3) e `permission == Read|Execute` (5). Com uma única
    // região RWX ele viajava um bloco só, não viajava nenhum — e o sintoma
    // era `Unresolved symbol: nn::init::Start`, que é o rtld a dizer que
    // carregou zero módulos.
    //
    // Sem a tabela (os testes unitários), caímos no comportamento antigo:
    // a reserva inteira, como uma região normal.
    if (!regions_.empty()) {
        for (const GuestRegion& r : regions_) {
            if (r.size == 0)
                continue;
            if (query >= r.base && query < r.base + r.size) {
                info.base_address = r.base;
                info.size         = r.size;
                info.state        = r.state;
                info.permission   = r.permission;
                break;
            }
        }
    } else if (mem_ != nullptr && mem_->owns(query)) {
        info.base_address = mem_->base();
        info.size         = mem_->size();
        info.state        = GuestRegion::kNormal;
        info.permission   = GuestRegion::kRead | GuestRegion::kWrite |
                            GuestRegion::kExecute;
    }

    if (!write_guest(out_info, &info, sizeof(info)))
        return Result::Invalid;

    // PageInfo: um único u32 de flags. Zero = sem flags, que é o normal.
    // As PageInfo voltam em x1, NÃO escritas em *out_page.
    //
    // Não é palpite: é o que o stub gerado pelo SDK do jogo faz. Lido do
    // `rtld` real (offset 0x16e4):
    //
    //     str   x1, [sp, #-16]!     ; guarda &flags
    //     svc   #0x6                 ; QueryMemory
    //     ldr   x2, [sp]             ; x2 = &flags
    //     str   w1, [x2]             ; *(&flags) = w1  <-- de x1!
    //     ret                        ; o Result volta em x0
    //
    // O stub é que escreve. Se o kernel também escrevesse, o valor seria o
    // mesmo — mas o registo tem de vir preenchido, senão o guest lê lixo.
    //
    // Sem isto o guest guardava em `flags` o valor que x1 tivesse à entrada
    // (o endereço de saída), e o resultado era um endereço onde se espera um
    // número. O rtld tratava isso como um sinal de erro e seguia para um
    // `__builtin_unreachable()`.
    const std::uint32_t page_flags = 0u;
    ctx.x[1] = page_flags;

    // Escrever também é inofensivo (o stub vai escrever o mesmo valor) e
    // protege o caso de um SDK que não faça a escrita.
    if (out_page != 0 && !write_guest(out_page, &page_flags, sizeof(page_flags)))
        return Result::Invalid;

    ++query_memory_calls_;
    return Result::Success;
}

Result SvcDispatcher::svc_get_process_id(SvcContext& ctx) {
    // x0 = ponteiro de saída (u64). O rtld grava o id do processo e volta a
    // usá-lo em chamadas IPC; um valor inventado serve, desde que estável
    // dentro da execução — e é escrito no espaço do guest, que é o que
    // interessa.
    std::uint64_t out = 1;
    if (!read_guest(ctx.x[0], &out, sizeof(out)))
        return Result::Invalid;
    return Result::Success;
}

// ---------------------------------------------------------------------------
// GetInfo
// ---------------------------------------------------------------------------
//
// Porque é preciso já: o SDK do jogo pede isto no arranque, ANTES de qualquer
// serviço. Pediu `RandomEntropy` duas vezes e `AslrRegionAddress` uma, e com
// a resposta em erro ficou com a região ASLR a null — e desreferenciou-a.
// O sintoma foi `svc Break(x0=0)`, o crash handler do próprio jogo.
//
// A forma do serviço não é adivinhada. Vem de `SvcWrap_GetInfo64` no Eden
// (svc.cpp:1537-1545):
//
//     info_type   = GetArg64(args, 1);      // x1
//     info_handle = GetArg64(args, 2);      // x2
//     subtype     = GetArg64(args, 3);      // x3
//     SetArg64(args, 0, ret);               // x0 = Result
//     SetArg64(args, 1, out);               // x1 = o VALOR
//
// Ou seja: o valor volta em x1, no registo que era o tipo. Não é escrito num
// ponteiro — e um palpite que o tratasse como `x0 = saída` devolveria lixo sem dar
// erro.
//
// Os números dos InfoType são os do Eden (`svc_types.h:130-147`).
//
// STUB, e é preciso dizê-lo: a região ASLR que devolvemos é o NOSSO heap. No
// Switch é uma região separada, gerida pelo `AslrSpaceManager` do SDK. Para
// o crt0 chegar ao fim não interessa — o que interessa é que seja um endereço
// dentro da reserva e que caiba. Se o SDK começar a alocar muito dali, é este
// o número a suspectar.
Result SvcDispatcher::svc_get_info(SvcContext& ctx) {
    const std::uint32_t type    = static_cast<std::uint32_t>(ctx.x[1]);
    const std::uint32_t subtype = static_cast<std::uint32_t>(ctx.x[3]);

    const std::uint64_t heap_base = heap_base_ != 0 ? heap_base_ : kDefaultHeapBase;
    const std::uint64_t heap_size = heap_size_  != 0 ? heap_size_  : 0x0800'0000ull;

    std::uint64_t value = 0;
    switch (type) {
    case 2:  value = 0;                    break;  // AliasRegionAddress
    case 3:  value = 0x4000'0000ull;       break;  // AliasRegionSize
    case 4:  value = heap_base;            break;  // HeapRegionAddress
    case 5:  value = heap_size;            break;  // HeapRegionSize
    case 6:  value = heap_base + heap_size;break;  // TotalMemorySize
    case 7:  value = 0;                    break;  // UsedMemorySize
    case 11: value = random_u64();         break;  // RandomEntropy
    case 12: value = heap_base;            break;  // AslrRegionAddress
    case 13: value = heap_size;            break;  // AslrRegionSize
    case 14: value = stack_base_;         break;  // StackRegionAddress
    case 15: value = stack_size_;         break;  // StackRegionSize
    case 16: value = 4;                    break;  // PersonalWordSize (AArch64)
    default:
        // Um tipo que não sabemos é melhor assim do que com um valor
        // inventado: o guest vê um Result de erro e pode livrar-se.
        return Result::Unsupported;
    }
    (void)subtype;

    // O valor vai em x1. `ctx.ret` fica para o Result, que o motor escreve
    // em x0 — são coisas diferentes, e é por aqui que esta SVC custa mais
    // uma vez às outras: o mesmo erro que o `x0` nunca receber o Result.
    ctx.x[1]  = value;
    ctx.ret   = 0;
    ctx.has_ret = true;
    return Result::Success;
}

std::uint64_t SvcDispatcher::random_u64() {
    // Sem estado, sem semente: `/dev/urandom` é a única fonte que não depende
    // de nada que este motor ainda não tenha. E o SDK pede `RandomEntropy`
    // duas vezes seguidas — se devolvêssemos o mesmo valor, o RNG do jogo
    // arrancaria com uma semente previsível, o que é pior do que não ter RNG.
    std::uint64_t v = 0;
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f != nullptr) {
        if (std::fread(&v, 1, sizeof(v), f) != sizeof(v))
            v = 0;
        std::fclose(f);
    } else {
        v = entropy_counter_++;
    }
    return v;
}

Result SvcDispatcher::svc_unsupported(SvcContext&) {
    return Result::Unsupported;
}

} // namespace ia
