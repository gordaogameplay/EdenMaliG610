// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O `nvdrv` — o serviço do driver da NVIDIA — **nosso**, fino.
//
// ## O que é
//
// O `nvdrv` é o serviço que dá ao jogo a GPU. O SDK pede-o ao `sm:` durante
// o arranque, e é por ele que o jogo abre os "dispositivos" (`/dev/nvhost-*`)
// e lhes manda os ioctls que submeteram trabalho à GPU.
//
// ## Porque este ficheiro existe
//
// **Medido** (`boot_p6l.log`): o guest pede `GetService("nvdrv")`, o `sm:`
// defere o pedido (o serviço não está registado), e o deferimento no motor
// nativo devolve uma resposta de **0 palavras** — o reader CMIF do guest
// aborta com `SetTerminateResult: modulo=162 desc=1` (o `UserlandAssert`
// da libnn).
//
// ## A forma é do Eden, à letra
//
// A tabela de comandos e os **IDs** são os do upstream
// (`core/hle/service/nvdrv/nvdrv_interface.cpp`), e as **formas das
// respostas** também. É o contrato com o guest: o `NvResult` que o Eden
// devolve é um `u32` com os valores da tabela de `nvdata.h`, e o guest
// compara-os com os seus.
//
// ## O conteúdo é o mínimo — e o que falta está escrito
//
// O `nvdrv` do Eden arrasta o container de dispositivos, o `nvmap`, o
// gestor de syncpoints e sete tipos de dispositivo (`nvhost-ctrl`,
// `nvhost-gpu`, `nvhost-prof-gpu`, `nvhost-nvdec`, `nvhost-nvjpg`,
// `nvhost-vic`, `nvdisp-disp0`) — ou seja, o driver inteiro.
//
// Aqui o que há é a **interface**: os comandos existem, respondem com a
// forma certa, e o `Initialize` fica feito. O trabalho que eles fariam —
// submeter o push buffer à GPU — é o **tradutor Maxwell -> Vulkan** da
// casa (`src/gpu/`), que ainda não está ligado ao IPC.
//
// ## O que o log diz
//
// Cada `Ioctl` é registado com o seu comando, o seu grupo e o seu
// comprimento. Essa linha é a **lista de trabalho** do caminho da GPU: é
// por aí que se vê o que o jogo submete, e em que volume.
//
// SPDX-FileCopyrightText: 2018 Yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

namespace g610::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// O `NvResult` — o resultado do driver, do Eden (`nvdata.h`).
//
// É um `u32` e o guest compara-o com os **seus** valores. A tabela é a do
// upstream, à letra; só os valores que o Eden usa nos comandos que
// implementamos estão nomeados.
// ─────────────────────────────────────────────────────────────────────────────
enum class NvResult : std::uint32_t {
    Success           = 0x0,
    NotImplemented    = 0x1,
    NotSupported      = 0x2,
    NotInitialized    = 0x3,
    BadParameter      = 0x4,
    Timeout           = 0x5,
    InsufficientMemory = 0x6,
    ReadOnlyAttribute = 0x7,
    InvalidState      = 0x8,
    InvalidAddress    = 0x9,
    InvalidSize       = 0xA,
    BadValue          = 0xB,
    AlreadyAllocated  = 0xD,
    Busy              = 0xE,
    ResourceError     = 0xF,
    CountMismatch     = 0x10,
    OverFlow          = 0x11,
    FileOperationFailed = 0x30003,
    DeviceNotFound    = 0x30011,
    AccessDenied      = 0x30010,
};

// O `DeviceFD` do Eden (`nvdata.h`): um `s32`, e `-1` é o inválido.
using DeviceFD = std::int32_t;
constexpr DeviceFD kInvalidFd = -1;

// O `Ioctl` do Eden (`nvdata.h`): um `u32` com o comando, o grupo, o
// comprimento e as duas bandeiras de direcção.
//
//     BitField<0, 8, u32>   cmd;
//     BitField<8, 8, u32>   group;
//     BitField<16, 14, u32> length;
//     BitField<30, 1, u32>  is_in;
//     BitField<31, 1, u32>  is_out;
struct Ioctl {
    std::uint32_t raw = 0;

    [[nodiscard]] std::uint32_t cmd() const { return raw & 0xFF; }
    [[nodiscard]] std::uint32_t group() const { return (raw >> 8) & 0xFF; }
    [[nodiscard]] std::uint32_t length() const { return (raw >> 16) & 0x3FFF; }
    [[nodiscard]] bool is_out() const { return (raw >> 31) & 1; }
};

// ─────────────────────────────────────────────────────────────────────────────
// As características da GPU — a struct do Eden
// (`nvhost_ctrl_gpu.h`, `IoctlGpuCharacteristics`).
//
// **A ordem dos campos é o contrato.** O guest lê esta struct byte a
// byte, e o Eden declara-a com os campos por ordem (`u32`, `u64`, ...).
// Numa máquina little-endian (o Orange Pi 5 Pro é), os tipos `u32_le`/`u64_le`
// do Eden são os tipos nativos — por isso a struct de C++ abaixo tem o
// mesmo tamanho e a mesma disposição.
//
// O `static_assert` é a prova: 160 bytes, como o Eden exige
// (`nvhost_ctrl_gpu.h`: `sizeof(IoctlGpuCharacteristics) == 160`).
// ─────────────────────────────────────────────────────────────────────────────
struct IoctlGpuCharacteristics {
    std::uint32_t arch;                        // 0x120 (GM200)
    std::uint32_t impl;                        // 0xB (GM20B)
    std::uint32_t rev;                         // 0xA1 (revisão A1)
    std::uint32_t num_gpc;                     // 0x1
    std::uint64_t l2_cache_size;               // 0x40000
    std::uint64_t on_board_video_memory_size;  // 0x0 (não usado)
    std::uint32_t num_tpc_per_gpc;             // 0x2
    std::uint32_t bus_type;                    // 0x20 (AXI)
    std::uint32_t big_page_size;               // 0x20000
    std::uint32_t compression_page_size;       // 0x20000
    std::uint32_t pde_coverage_bit_count;      // 0x1B
    std::uint32_t available_big_page_sizes;    // 0x30000
    std::uint32_t gpc_mask;                    // 0x1
    std::uint32_t sm_arch_sm_version;          // 0x503
    std::uint32_t sm_arch_spa_version;         // 0x503
    std::uint32_t sm_arch_warp_count;          // 0x80
    std::uint32_t gpu_va_bit_count;            // 0x28
    std::uint32_t reserved;                    // 0
    std::uint64_t flags;                       // 0x55
    std::uint32_t twod_class;                  // 0x902D
    std::uint32_t threed_class;                // 0xB197
    std::uint32_t compute_class;               // 0xB1C0
    std::uint32_t gpfifo_class;                // 0xB06F
    std::uint32_t inline_to_memory_class;      // 0xA140
    std::uint32_t dma_copy_class;              // 0xB0B5
    std::uint32_t max_fbps_count;              // 0x1
    std::uint32_t fbp_en_mask;                 // 0x0 (desligado)
    std::uint32_t max_ltc_per_fbp;             // 0x2
    std::uint32_t max_lts_per_ltc;             // 0x1
    std::uint32_t max_tex_per_tpc;             // 0x0 (não suportado)
    std::uint32_t max_gpc_count;               // 0x1
    std::uint32_t rop_l2_en_mask_0;            // 0x21D70
    std::uint32_t rop_l2_en_mask_1;            // 0x0
    std::uint64_t chipname;                    // 0x6230326D67 ("gm20b")
    std::uint64_t gr_compbit_store_base_hw;    // 0x0 (não suportado)
};
static_assert(sizeof(IoctlGpuCharacteristics) == 160,
              "IoctlGpuCharacteristics tem de ter 160 bytes, como no Eden");

// E o invólucro do ioctl (`nvhost_ctrl_gpu.h`, `IoctlCharacteristics`): o
// tamanho e o endereço que o guest mandou, e a struct.
struct IoctlCharacteristics {
    std::uint64_t gpu_characteristics_buf_size;  // 0xA0
    std::uint64_t gpu_characteristics_buf_addr;  // não nulo
    IoctlGpuCharacteristics gc;
};
static_assert(sizeof(IoctlCharacteristics) == 176,
              "IoctlCharacteristics tem de ter 176 bytes, como no Eden");

// A `VaRegion` do Eden (`nvhost_as_gpu.h`): uma região do espaço de
// endereços virtual da GPU.
struct VaRegion {
    std::uint64_t offset;
    std::uint32_t page_size;
    std::uint32_t _pad0_;
    std::uint64_t pages;
};
static_assert(sizeof(VaRegion) == 0x18, "tamanho errado");

// O `IoctlGetVaRegions` do Eden: onde estão as regiões e quantas.
struct IoctlGetVaRegions {
    std::uint64_t buf_addr;  // (ignorado)
    std::uint32_t buf_size;  // forçado a 2*sizeof(VaRegion)
    std::uint32_t reserved;
    VaRegion regions[2];
};
static_assert(sizeof(IoctlGetVaRegions) == 16 + sizeof(VaRegion) * 2, "tamanho errado");

// O `IoctlAllocSpace` do Eden: o pedido de espaço.
struct IoctlAllocSpace {
    std::uint32_t pages;
    std::uint32_t page_size;
    std::uint32_t flags;
    std::uint32_t padding;
    std::uint64_t offset;  // (a union com `align`)
};
static_assert(sizeof(IoctlAllocSpace) == 24, "tamanho errado");

// ─────────────────────────────────────────────────────────────────────────────
// As structs do `nvmap` (do Eden, `devices/nvmap.h`).
// ─────────────────────────────────────────────────────────────────────────────

// `IocCreateParams`: o tamanho pedido e o handle que sai.
struct IocCreateParams {
    std::uint32_t size;
    std::uint32_t handle;
};
static_assert(sizeof(IocCreateParams) == 8, "tamanho errado");

// `IocAllocParams`: a alocação de um handle.
struct IocAllocParams {
    std::uint32_t handle;
    std::uint32_t heap_mask;
    std::uint32_t flags;
    std::uint32_t align;
    std::uint8_t kind;
    std::uint8_t padding[7];
    std::uint64_t address;
};
static_assert(sizeof(IocAllocParams) == 32, "tamanho errado");

// `IocParamParams`: um parâmetro de um handle (tamanho, alinhamento, …).
struct IocParamParams {
    std::uint32_t handle;
    std::uint32_t param;
    std::uint32_t result;
};
static_assert(sizeof(IocParamParams) == 12, "tamanho errado");

// `IocFreeParams`: o_HANDLE e o que a libertação devolve.
struct IocFreeParams {
    std::uint32_t handle;
    std::uint8_t padding[4];
    std::uint64_t address;
    std::uint32_t size;
    std::uint32_t flags;
};
static_assert(sizeof(IocFreeParams) == 24, "tamanho errado");

// `IocFromIdParams` e `IocGetIdParams`: o handle e o id (no Eden são o
// mesmo valor).
struct IocFromIdParams {
    std::uint32_t id;
    std::uint32_t handle;
};
static_assert(sizeof(IocFromIdParams) == 8, "tamanho errado");

struct IocGetIdParams {
    std::uint32_t id;
    std::uint32_t handle;
};
static_assert(sizeof(IocGetIdParams) == 8, "tamanho errado");

// O `IoctlMapBufferEx` do Eden (`nvhost_as_gpu.h`): mapear a memória de
// um handle do `nvmap` dentro do VA da GPU.
struct IoctlMapBufferEx {
    std::uint32_t flags;          // bit0: offset fixo, bit2: cacheável
    std::uint32_t kind;           // -1 é o default
    std::uint32_t handle;         // o handle do `nvmap`
    std::uint32_t page_size;      // 0 = não importa
    std::int64_t buffer_offset;
    std::uint64_t mapping_size;
    std::int64_t offset;          // o VA (entrada e saída)
};
static_assert(sizeof(IoctlMapBufferEx) == 40, "tamanho errado");

// O `IoctlFreeSpace` do Eden: libertar um espaço do VA.
struct IoctlFreeSpace {
    std::uint64_t offset;
    std::uint32_t pages;
    std::uint32_t page_size;
};
static_assert(sizeof(IoctlFreeSpace) == 16, "tamanho errado");

// ─────────────────────────────────────────────────────────────────────────────
// As outras structs de ioctl que o guest pede.
//
// Todas do Eden (`nvhost_ctrl_gpu.h`, `nvhost_as_gpu.h`), e todas com o
// `static_assert` do tamanho — que é o que o Eden exige.
// ─────────────────────────────────────────────────────────────────────────────

// `IoctlZcullGetCtxSize`: o tamanho do contexto de ZCull.
struct IoctlZcullGetCtxSize {
    std::uint32_t size;
};
static_assert(sizeof(IoctlZcullGetCtxSize) == 4, "tamanho errado");

// `IoctlGpuGetTpcMasksArgs`: o tamanho do buffer de máscaras e a máscara.
struct IoctlGpuGetTpcMasksArgs {
    std::uint32_t mask_buffer_size;
    std::uint32_t tpc_mask;
    std::uint32_t padding[4];
};
static_assert(sizeof(IoctlGpuGetTpcMasksArgs) == 24, "tamanho errado");

// `IoctlNvgpuGpuZcullGetInfo`: as|ALIGNAMENTOS de píxeis do ZCull.
struct IoctlZcullGetInfo {
    std::uint32_t width_align_pixels;
    std::uint32_t height_align_pixels;
    std::uint32_t pixel_squares_by_aliquots;
    std::uint32_t aliquot_total;
    std::uint32_t region_byte_multiplier;
    std::uint32_t region_header_size;
    std::uint32_t subregion_header_size;
    std::uint32_t subregion_width_align_pixels;
    std::uint32_t subregion_height_align_pixels;
    std::uint32_t subregion_count;
};
static_assert(sizeof(IoctlZcullGetInfo) == 40, "tamanho errado");

// `IoctlAllocAsEx` (`nvhost_as_gpu.h`): o espaço de endereços virtual
// que o guest pede para a GPU.
struct IoctlAllocAsEx {
    std::uint32_t flags;            // usualmente 1
    std::int32_t as_fd;             // ignorado
    std::uint32_t big_page_size;
    std::uint32_t reserved;         // ignorado
    std::uint64_t va_range_start;
    std::uint64_t va_range_end;
    std::uint64_t va_range_split;
};
static_assert(sizeof(IoctlAllocAsEx) == 40, "tamanho errado");

// ─────────────────────────────────────────────────────────────────────────────
// O `nvdrv` propriamente dito.
// ─────────────────────────────────────────────────────────────────────────────
class NvDrv final : public Service::ServiceFramework<NvDrv> {
public:
    explicit NvDrv(Core::System& system, const char* nome)
        : ServiceFramework{system, nome} {
        // A tabela do Eden (`nvdrv_interface.cpp`), à letra — os IDs, os
        // nomes, e o mesmo conjunto de comandos implementados.
        static const FunctionInfo functions[] = {
            {0, &NvDrv::Open, "Open"},
            {1, &NvDrv::Ioctl1, "Ioctl"},
            {2, &NvDrv::Close, "Close"},
            {3, &NvDrv::Initialize, "Initialize"},
            {4, &NvDrv::QueryEvent, "QueryEvent"},
            {5, nullptr, "MapSharedMem"},
            {6, &NvDrv::GetStatus, "GetStatus"},
            {7, nullptr, "SetAruidForTest"},
            {8, &NvDrv::SetAruid, "SetAruid"},
            {9, &NvDrv::DumpGraphicsMemoryInfo, "DumpGraphicsMemoryInfo"},
            {10, nullptr, "InitializeDevtools"},
            {11, &NvDrv::Ioctl2, "Ioctl2"},
            {12, &NvDrv::Ioctl3, "Ioctl3"},
            {13, &NvDrv::SetGraphicsFirmwareMemoryMarginEnabled,
             "SetGraphicsFirmwareMemoryMarginEnabled"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0: abrir um dispositivo. O nome vem no **buffer 0** (o Eden usa
    // `ctx.ReadBuffer()` e tira o nome de lá).
    //
    // O Eden devolve o `DeviceFD` e o `NvResult`. Sem `Initialize` antes,
    // devolve `NotInitialized` — que é o que o Eden devolve, e o guest
    // sabe tratar.
    void Open(HLERequestContext& ctx) {
        const auto buffer = ctx.ReadBufferCopy(0);
        std::string nome(reinterpret_cast<const char*>(buffer.data()),
                         buffer.size());
        // O nome vem terminado em NUL.
        const auto nulo = nome.find('\0');
        if (nulo != std::string::npos)
            nome.resize(nulo);

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);

        if (!initialized_) {
            rb.Push<DeviceFD>(0);
            rb.PushEnum(NvResult::NotInitialized);
            LOG_WARNING(Service_NVDRV, "Open: NvServices ainda não foi inicializado");
            return;
        }

        // O profiler não existe em produção (o Eden recusa) — e o nosso
        // tradutor também não tem.
        if (nome == "/dev/nvhost-prof-gpu") {
            rb.Push<DeviceFD>(0);
            rb.PushEnum(NvResult::NotSupported);
            LOG_WARNING(Service_NVDRV, "Open: '{}' não é suportado", nome);
            return;
        }

        // Um dispositivo novo. O `fd` é um número que o guest usa para
        // os ioctls seguintes — e que tem de ser **estável** enquanto o
        // dispositivo estiver aberto.
        const DeviceFD fd = proximo_fd_++;
        dispositivos_.emplace(fd, nome);
        LOG_DEBUG(Service_NVDRV, "Open: '{}' -> fd={}", nome, fd);

        rb.Push<DeviceFD>(fd);
        rb.PushEnum(NvResult::Success);
    }

    // 1, 11 e 12: os ioctls. A diferença entre eles é quantos buffers
    // de entrada têm (o Eden: `Ioctl` tem um, `Ioctl2` tem dois — um
    // "inlined" — e `Ioctl3` tem dois de saída).
    //
    // **O que se faz aqui**: regista-se o comando (é a lista de trabalho
    // do caminho da GPU) e responde-se com a forma certa. O trabalho de
    // verdade — ler o push buffer e traduzi-lo para Vulkan — é o
    // tradutor da casa (`src/gpu/`), que ainda não está ligado.
    void Ioctl1(HLERequestContext& ctx) {
        IoctlComum(ctx, 1);
    }

    void Ioctl2(HLERequestContext& ctx) {
        IoctlComum(ctx, 2);
    }

    void Ioctl3(HLERequestContext& ctx) {
        IoctlComum(ctx, 3);
    }

    // 2: fechar um dispositivo.
    void Close(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const DeviceFD fd = rp.Pop<DeviceFD>();

        if (!initialized_) {
            LOG_WARNING(Service_NVDRV, "Close: NvServices ainda não foi inicializado");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::NotInitialized);
            return;
        }

        const auto it = dispositivos_.find(fd);
        if (it == dispositivos_.end()) {
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::DeviceNotFound);
            return;
        }
        LOG_DEBUG(Service_NVDRV, "Close: fd={} ('{}')", fd, it->second);
        dispositivos_.erase(it);

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }

    // 3: inicializar. O Eden lê o handle do processo e o da memória
    // transferida (que empresta ao driver como buffer de trabalho) e o
    // tamanho desse buffer.
    //
    // Depois de isto, os `Open` passam a devolver um `fd` a sério.
    void Initialize(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        // Os dois handles de cópia do pedido (o processo e a memória
        // transferida) são lidos pelo Eden com `ctx.GetCopyHandle(0/1)`.
        // O nosso `RequestParser` não os lê — e não precisa: o que
        // interessa é o **tamanho** do buffer de trabalho, que vem a
        // seguir.
        const std::uint32_t tamanho = rp.Pop<std::uint32_t>();

        initialized_ = true;
        LOG_DEBUG(Service_NVDRV, "Initialize: buffer de trabalho de {} bytes", tamanho);

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }

    // 4: o evento de um dispositivo. O Eden devolve um **handle de
    // cópia** (o `KReadableEvent`) e o `NvResult`.
    void QueryEvent(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const DeviceFD fd = rp.Pop<DeviceFD>();
        const std::uint32_t evento = rp.Pop<std::uint32_t>();
        (void)fd;

        if (!initialized_) {
            LOG_WARNING(Service_NVDRV, "QueryEvent: NvServices ainda não foi inicializado");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::NotInitialized);
            return;
        }

        // O evento: um `KEvent` como o do `applet_oe` — o objecto por
        // trás do handle de cópia.
        if (!evento_) {
            auto& kernel = system.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_ = new Kernel::KEvent(kernel);
            evento_->Initialize(kernel, &processo);
        }

        ResponseBuilder rb{ctx, 3, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
        rb.PushEnum(NvResult::Success);
    }

    // 6: o estado do driver. O Eden responde `Success` e mais nada.
    void GetStatus(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }

    // 8: o APU id do processo.
    void SetAruid(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const std::uint64_t pid = rp.Pop<std::uint64_t>();
        LOG_DEBUG(Service_NVDRV, "SetAruid: pid=0x{:016X}", pid);
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }

    // 9: despejar a informação da memória gráfica. No Switch real isto
    // não faz nada (é o que o switchbrew diz), e o Eden também não.
    void DumpGraphicsMemoryInfo(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 13: a margem de memória do firmware gráfico.
    void SetGraphicsFirmwareMemoryMarginEnabled(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // O `GetTPCMasks1` (grupo `'G'`, comando 0x06) — as máscaras de TPC.
    //
    // O Eden: `params.tcp_mask = 3` quando o buffer de máscaras não é
    // vazio (`nvhost_ctrl_gpu.cpp`, `GetTPCMasks1`).
    void GetTPCMasks1(HLERequestContext& ctx) {
        IoctlGpuGetTpcMasksArgs params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "GetTPCMasks1: buffer de máscaras de {} bytes",
                  params.mask_buffer_size);
        if (params.mask_buffer_size != 0)
            params.tpc_mask = 3;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O `ZCullGetCtxSize` (grupo `'G'`, comando 0x01) — o tamanho do
    // contexto de ZCull. O Eden: `params.size = 0x1`.
    void ZCullGetCtxSize(HLERequestContext& ctx) {
        IoctlZcullGetCtxSize params{};
        LerEntrada(ctx, params);
        params.size = 0x1;
        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O `ZCullGetInfo` (grupo `'G'`, comando 0x02) — os alinhamentos de
    // píxeis do ZCull. Os valores são os do Eden.
    void ZCullGetInfo(HLERequestContext& ctx) {
        IoctlZcullGetInfo params{};
        LerEntrada(ctx, params);

        params.width_align_pixels             = 0x20;
        params.height_align_pixels            = 0x20;
        params.pixel_squares_by_aliquots       = 0x400;
        params.aliquot_total                  = 0x800;
        params.region_byte_multiplier         = 0x20;
        params.region_header_size             = 0x20;
        params.subregion_header_size          = 0xC0;
        params.subregion_width_align_pixels   = 0x20;
        params.subregion_height_align_pixels  = 0x40;
        params.subregion_count                = 0x10;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O `AllocAsEx` (grupo `'A'`, comando 0x09) — o espaço de
    // endereços virtual da GPU.
    //
    // O Eden cria aqui o *memory manager* da Tegra (`system.GPU().
    // InitAddressSpace`) e os seusCk昕allocadores de páginas. **Aqui não
    // há nada para criar**: o tradutor Maxwell -> Vulkan da casa
    // (`src/gpu/`) trabalha com os recursos que o jogo submete, não com um
    // espaço de endereços virtual gerido pelo kernel.
    //
    // O que se faz é **registar o pedido** — o log diz o que o guest
    // pediu — e responder com sucesso. O `NvResult` que o Eden devolve
    // é `Success`, e é esse que o guest espera.
    void AllocAsEx(HLERequestContext& ctx) {
        IoctlAllocAsEx params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV,
                  "AllocAsEx: flags={} big_page_size=0x{:X} va=[0x{:X}, 0x{:X}) split=0x{:X}",
                  params.flags, params.big_page_size, params.va_range_start,
                  params.va_range_end, params.va_range_split);

        // O Eden: se o guest **não** escolheu o espaço, o driver usa os
        // valores por omissão (`nvhost_as_gpu.cpp`, `AllocAsEx`: "If
        // this is unspecified then default values should be used").
        //
        // **Aqui o espaço por omissão é o da casa** — a região de alias
        // da reserva — e não o do Eden ([32 MiB, 8 GiB)), porque o VA
        // tem de ser memória do **guest** (ver a nota da `VmDaGpu`).
        //
        // O tamanho de página grande, esse é o do guest (0x10000) — e o
        // Eden só o aceita se for uma potência de dois dentro das
        // suportadas (`SUPPORTED_BIG_PAGE_SIZES` = 0x30000).
        if (params.big_page_size != 0 &&
            (params.big_page_size & (params.big_page_size - 1)) == 0 &&
            (params.big_page_size & 0x30000u) != 0) {
            va_.pagina_grande = params.big_page_size;
        }
        va_.iniciado = true;
        espaco_va_ = params;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O `GetVARegions1` (grupo `'A'`, comando 0x08) — as regiões do
    // espaço de endereços virtual da GPU.
    //
    // O Eden (`nvhost_as_gpu.cpp`, `GetVARegionsImpl`) devolve **duas**
    // regiões: a das páginas pequenas e a das grandes, e força
    // `buf_size = 2*sizeof(VaRegion)`.
    void GetVARegions1(HLERequestContext& ctx) {
        IoctlGetVaRegions params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "GetVARegions1: va=[0x{:X}, 0x{:X}) split=0x{:X}",
                  va_.base, va_.fim, va_.split);

        params.buf_size = 2 * sizeof(VaRegion);
        params.reserved = 0;
        params.regions[0] = VaRegion{
            .offset = va_.base,
            .page_size = kPaginaPequena,
            ._pad0_ = 0,
            .pages = (va_.split - va_.base) / kPaginaPequena,
        };
        params.regions[1] = VaRegion{
            .offset = va_.split,
            .page_size = va_.pagina_grande,
            ._pad0_ = 0,
            .pages = (va_.fim - va_.split) / va_.pagina_grande,
        };

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O `AllocateSpace` (grupo `'A'`, comando 0x02) — alocar espaço
    // no VA da GPU.
    //
    // **O VA da GPU é o espaço de endereços do próprio guest.** Num
    // motor de execução nativa não há tradução: a "página física" de uma
    // região é o seu endereço. Por isso o VA que o driver dá ao guest é
    // uma zona **da reserva** — e o guest escreve lá directamente, e o
    // tradutor Maxwell -> Vulkan lê o push buffer de lá.
    //
    // A alocação é um **contador** (o Eden usa um `FlatAllocator`; aqui
    // basta o seguinte, porque o guest não liberta durante o arranque).
    void AllocateSpace(HLERequestContext& ctx) {
        IoctlAllocSpace params{};
        LerEntrada(ctx, params);

        if (!va_.iniciado) {
            LOG_WARNING(Service_NVDRV, "AllocateSpace: o espaço de VA ainda não foi criado");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        LOG_DEBUG(Service_NVDRV, "AllocateSpace: pages={} page_size=0x{:X} flags=0x{:X}",
                  params.pages, params.page_size, params.flags);

        // O Eden: o tamanho de página tem de ser o das páginas
        // pequenas ou o das grandes (`nvhost_as_gpu.cpp`,
        // `AllocateSpace`); o resto é `BadValue`.
        std::uint64_t* proximo = nullptr;
        std::uint64_t base = 0;
        std::uint64_t passo = 0;
        std::uint64_t limite = 0;
        if (params.page_size == kPaginaPequena) {
            proximo = &va_.proximo_pequeno;
            base = va_.base;
            passo = kPaginaPequena;
            limite = va_.split;
        } else if (params.page_size == va_.pagina_grande) {
            proximo = &va_.proximo_grande;
            base = va_.split;
            passo = va_.pagina_grande;
            limite = va_.fim;
        } else {
            LOG_WARNING(Service_NVDRV, "AllocateSpace: tamanho de página inválido 0x{:X}",
                        params.page_size);
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        const std::uint64_t tamanho = static_cast<std::uint64_t>(params.pages) * passo;
        if (*proximo + tamanho > limite) {
            LOG_WARNING(Service_NVDRV, "AllocateSpace: sem espaço no VA da GPU");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::InsufficientMemory);
            return;
        }

        params.offset = *proximo;
        *proximo += tamanho;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // ── o `nvmap`: os handles de memória da GPU ──
    //
    // O guest abre o `/dev/nvmap` e pede handles (`IocCreate`), aloca
    // memória para eles (`IocAlloc`), interroga parâmetros (`IocParam`)
    // e liberta (`IocFree`). Os **IDs dos comandos** são os do Eden
    // (`devices/nvmap.cpp`, grupo 0x01).
    //
    // **A memória do handle é a do VA da GPU.** Num motor nativo não há
    // tradução: a "memória do dispositivo" é memória do guest, e é por
    // isso que o guest escreve lá directamente e o tradutor Maxwell ->
    // Vulkan lê o push buffer de lá. Por isso a alocação sai do **mesmo
    // espaço** que o `AllocateSpace` dá ao guest.
    struct HandleNvmap {
        std::uint32_t tamanho = 0;
        std::uint32_t align = 0;
        std::uint64_t endereco = 0;
        bool alocado = false;
    };
    std::map<std::uint32_t, HandleNvmap> nvmap_;
    std::uint32_t proximo_nvmap_ = 1;
    // O par (VA da GPU -> endereco do guest), que o tradutor Maxwell ->
    // Vulkan usa para traduzir os comandos do push buffer.
    std::map<std::uint64_t, std::uint64_t> mapeamentos_;

    // 0x01/0x01: `IocCreate` — criar um handle de `size` bytes.
    void IocCreate(HLERequestContext& ctx) {
        IocCreateParams params{};
        LerEntrada(ctx, params);

        const std::uint32_t id = proximo_nvmap_++;
        HandleNvmap h{};
        h.tamanho = params.size;
        nvmap_.emplace(id, h);

        LOG_DEBUG(Service_NVDRV, "nvmap: IocCreate size=0x{:X} -> handle={}", params.size, id);

        params.handle = id;
        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x01/0x04: `IocAlloc` — alocar a memória de um handle.
    void IocAlloc(HLERequestContext& ctx) {
        IocAllocParams params{};
        LerEntrada(ctx, params);

        const auto it = nvmap_.find(params.handle);
        if (it == nvmap_.end() || it->second.alocado) {
            LOG_WARNING(Service_NVDRV, "nvmap: IocAlloc: handle {} inválido", params.handle);
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        // O alinhamento mínimo é a página (o Eden: `params.align <
        // YUZU_PAGESIZE` → sobe para a página).
        std::uint32_t align = params.align != 0 ? params.align : kPaginaPequena;
        if (align < kPaginaPequena)
            align = kPaginaPequena;

        // A alocação sai da região das páginas pequenas do VA da GPU.
        const std::uint64_t tamanho =
            (static_cast<std::uint64_t>(it->second.tamanho) + kPaginaPequena - 1) &
            ~static_cast<std::uint64_t>(kPaginaPequena - 1);
        if (va_.proximo_pequeno + tamanho > va_.split) {
            LOG_WARNING(Service_NVDRV, "nvmap: IocAlloc: sem espaço no VA da GPU");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::InsufficientMemory);
            return;
        }

        it->second.endereco = va_.proximo_pequeno;
        it->second.align = align;
        it->second.alocado = true;
        va_.proximo_pequeno += tamanho;

        LOG_DEBUG(Service_NVDRV, "nvmap: IocAlloc handle={} tamanho=0x{:X} -> 0x{:X}",
                  params.handle, it->second.tamanho, it->second.endereco);

        params.address = it->second.endereco;
        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x01/0x09: `IocParam` — um parâmetro de um handle.
    //
    // Os tipos são os do Eden (`devices/nvmap.h`, `HandleParameterType`):
    // 1 = tamanho, 2 = alinhamento, 3 = base, 4 = heap, 5 = tipo.
    void IocParam(HLERequestContext& ctx) {
        IocParamParams params{};
        LerEntrada(ctx, params);

        const auto it = nvmap_.find(params.handle);
        if (it == nvmap_.end()) {
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        switch (params.param) {
        case 1:  // tamanho
            params.result = it->second.tamanho;
            break;
        case 2:  // alinhamento
            params.result = it->second.align;
            break;
        case 3:  // base — o Eden devolve -22 (posix EINVAL)
            params.result = static_cast<std::uint32_t>(-22);
            break;
        case 4:  // heap
            params.result = it->second.alocado ? 0x40000000u : 0u;
            break;
        case 5:  // tipo
            params.result = 0;
            break;
        default:
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x01/0x05: `IocFree` — libertar a memória de um handle.
    //
    // **O Eden devolve o endereço e o tamanho libertados**
    // (`nvmap.cpp`, `IocFree`: `params.address = freeInfo->address;
    // params.size = freeInfo->size;`). O guest usa-os — e com o endereço
    // a zeros, desreferenciava um ponteiro nulo (medido: um fault em
    // `0x20`, que é `0 + 0x20`).
    void IocFree(HLERequestContext& ctx) {
        IocFreeParams params{};
        LerEntrada(ctx, params);

        const auto it = nvmap_.find(params.handle);
        if (it != nvmap_.end()) {
            params.address = it->second.endereco;
            params.size = it->second.tamanho;
            params.flags = 0;
            LOG_DEBUG(Service_NVDRV, "nvmap: IocFree handle={} -> 0x{:X} tamanho=0x{:X}",
                      params.handle, params.address, params.size);
            it->second.alocado = false;
            nvmap_.erase(it);
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x01/0x03: `IocFromId` — o handle a partir do id (no Eden, o mesmo
    // valor).
    void IocFromId(HLERequestContext& ctx) {
        IocFromIdParams params{};
        LerEntrada(ctx, params);

        if (params.id == 0 || nvmap_.count(params.id) == 0) {
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }
        params.handle = params.id;
        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x01/0x0e: `IocGetId` — o id a partir do handle.
    void IocGetId(HLERequestContext& ctx) {
        IocGetIdParams params{};
        LerEntrada(ctx, params);

        if (params.handle == 0 || nvmap_.count(params.handle) == 0) {
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::AccessDenied);
            return;
        }
        params.id = params.handle;
        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x41/0x06: `MapBufferEx` — mapear a memória de um handle do
    // `nvmap` dentro do VA da GPU.
    //
    // **No motor nativo o mapeamento é a identidade.** A memória do
    // handle já é do guest (foi alocada no VA da GPU), por isso o VA
    // que o guest pede e o endereço da memória são o mesmo número. O que
    // fica registado é o **par** (VA → endereço do guest), que é o que o
    // tradutor Maxwell -> Vulkan vai usar para traduzir os comandos do
    // push buffer.
    void MapBufferEx(HLERequestContext& ctx) {
        IoctlMapBufferEx params{};
        LerEntrada(ctx, params);

        const auto it = nvmap_.find(params.handle);
        if (it == nvmap_.end() || !it->second.alocado) {
            LOG_WARNING(Service_NVDRV, "MapBufferEx: handle {} não está alocado", params.handle);
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::BadValue);
            return;
        }

        const std::uint64_t endereco = it->second.endereco + params.buffer_offset;
        LOG_DEBUG(Service_NVDRV,
                  "MapBufferEx: handle={} va=0x{:X} -> 0x{:X} tamanho=0x{:X} flags=0x{:X}",
                  params.handle, static_cast<std::uint64_t>(params.offset), endereco,
                  params.mapping_size, params.flags);

        // O VA devolvido: o do guest, se pediu um fixo; o da memória, se
        // não (a identidade).
        params.offset = (params.flags & 0x1u) != 0
                            ? params.offset
                            : static_cast<std::int64_t>(endereco);
        mapeamentos_.emplace(static_cast<std::uint64_t>(params.offset), endereco);

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x41/0x03: `FreeSpace` — libertar um espaço do VA.
    //
    // O Eden devolve a memória ao *allocator*. Aqui o allocator é um
    // contador (a memória não volta a ser usada), por isso o que fica é
    // o registo.
    void FreeSpace(HLERequestContext& ctx) {
        IoctlFreeSpace params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "FreeSpace: va=0x{:X} pages={} page_size=0x{:X}",
                  params.offset, params.pages, params.page_size);

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x41/0x01: `BindChannel` — ligar o canal do `nvhost-gpu` ao
    // memory manager da GPU.
    //
    // O Eden (`nvhost_as_gpu.cpp`, `BindChannel`) põe o `memory_manager`
    // no estado do canal. Aqui não há memory manager para ligar (a
    // memória é a do guest, e o tradutor lê-a do push buffer), por isso
    // o que fica é o registo do canal.
    void BindChannel(HLERequestContext& ctx) {
        struct IoctlBindChannel {
            std::int32_t fd;
        };
        static_assert(sizeof(IoctlBindChannel) == 4, "tamanho errado");

        IoctlBindChannel params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "BindChannel: fd={}", params.fd);

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // O ioctl comum aos três comandos.
    //
    // A forma da resposta é a do Eden: `Result` + `NvResult`, e — quando
    // o comando tem saída — o buffer de saída escrito **antes** da
    // resposta.
    void IoctlComum(HLERequestContext& ctx, int variante) {
        RequestParser rp{ctx};
        const DeviceFD fd = rp.Pop<DeviceFD>();
        const Ioctl comando{rp.PopRaw<std::uint32_t>()};

        LOG_DEBUG(Service_NVDRV,
                  "Ioctl{}: fd={} grupo=0x{:02X} cmd=0x{:02X} tamanho={} saida={}",
                  variante, fd, comando.group(), comando.cmd(), comando.length(),
                  comando.is_out() ? 1 : 0);

        if (!initialized_) {
            LOG_WARNING(Service_NVDRV, "Ioctl: NvServices ainda não foi inicializado");
            ResponseBuilder rb{ctx, 3};
            rb.Push(ResultSuccess);
            rb.PushEnum(NvResult::NotInitialized);
            return;
        }

        // O despacho pelos grupos e comandos — o do Eden
        // (`nvhost_ctrl_gpu.cpp` e `nvhost_as_gpu.cpp`, `Ioctl1`).
        //
        // O grupo `'G'` (0x47) é o do `nvhost-ctrl-gpu` e o `'A'` (0x41) o
        // do `nvhost-as-gpu` — os dois dispositivos que o jogo abre.
        if (comando.group() == 'G') {
            switch (comando.cmd()) {
            case 0x1:
                ZCullGetCtxSize(ctx);
                return;
            case 0x2:
                ZCullGetInfo(ctx);
                return;
            case 0x5:
                GetCharacteristics1(ctx);
                return;
            case 0x6:
                GetTPCMasks1(ctx);
                return;
            default:
                break;
            }
        } else if (comando.group() == 'A') {
            switch (comando.cmd()) {
            case 0x1:
                BindChannel(ctx);
                return;
            case 0x2:
                AllocateSpace(ctx);
                return;
            case 0x3:
                FreeSpace(ctx);
                return;
            case 0x6:
                MapBufferEx(ctx);
                return;
            case 0x8:
                GetVARegions1(ctx);
                return;
            case 0x9:
                AllocAsEx(ctx);
                return;
            default:
                break;
            }
        } else if (comando.group() == 0x01) {
            // O grupo do `nvmap` (`devices/nvmap.cpp`).
            switch (comando.cmd()) {
            case 0x1:
                IocCreate(ctx);
                return;
            case 0x3:
                IocFromId(ctx);
                return;
            case 0x4:
                IocAlloc(ctx);
                return;
            case 0x5:
                IocFree(ctx);
                return;
            case 0x9:
                IocParam(ctx);
                return;
            case 0xe:
                IocGetId(ctx);
                return;
            default:
                break;
            }
        }

        // O que não está implementado: o Eden responde `NotImplemented` e
        // **não escreve nada** no buffer de saída
        // (`nvhost_ctrl_gpu.cpp`: `UNIMPLEMENTED_MSG(...); return
        // NvResult::NotImplemented;`).
        //
        // **Não é "sucesso com zeros".** Medido (`boot_p6s.log`): com um
        // sucesso e um buffer a zeros, o guest usou os zeros como se
        // fossem dados válidos e saltou para um ponteiro lixo
        // (`0x40011e2d407c3`) — um fault de memória.
        //
        // O `NotImplemented` é um resultado que o driver **conhece**: o
        // guest sabe o que fazer com ele, e o log fica a dizer qual
        // comando falta.
        LOG_WARNING(Service_NVDRV, "Ioctl: grupo=0x{:02X} cmd=0x{:02X} NÃO IMPLEMENTADO",
                    comando.group(), comando.cmd());

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::NotImplemented);
    }

    // O `GetCharacteristics1` (grupo `'G'`, comando 0x05) — as
    // características da GPU.
    //
    // **A mecânica é a do Eden** (`ioctl_serialization.h`, `WrapGeneric`):
    // a struct é lida do buffer de **entrada**, o handler enche-a, e a
    // struct enchida é escrita no buffer de **saída**. O Eden enche-a com
    // os valores do GM20B (`nvhost_ctrl_gpu.cpp`, `GetCharacteristics1`).
    //
    // **Porque isto não é um detalhe**: com o buffer de saída a zeros, o
    // `arch` que o guest lia era 0 — e o driver de NVN do guest escolhe a
    // tabela de funções pela arquitectura. Com `arch = 0` a tabela não
    // tem entrada, o ponteiro é nulo, e o guest desreferenciou `0x10`:
    // o fault medido em `boot_p6m.log`.
    void GetCharacteristics1(HLERequestContext& ctx) {
        LOG_DEBUG(Service_NVDRV, "GetCharacteristics1: pedido");

        // A struct, tal como o guest a mandou.
        const auto entrada = ctx.ReadBufferCopy(0);
        IoctlCharacteristics params{};
        const std::size_t lido = std::min(entrada.size(), sizeof(params));
        if (lido > 0)
            std::memcpy(&params, entrada.data(), lido);

        // Os valores do Eden, à letra (`nvhost_ctrl_gpu.cpp`).
        params.gc.arch                       = 0x120;
        params.gc.impl                       = 0xB;
        params.gc.rev                        = 0xA1;
        params.gc.num_gpc                    = 0x1;
        params.gc.l2_cache_size              = 0x40000;
        params.gc.on_board_video_memory_size = 0x0;
        params.gc.num_tpc_per_gpc            = 0x2;
        params.gc.bus_type                   = 0x20;
        params.gc.big_page_size              = 0x20000;
        params.gc.compression_page_size      = 0x20000;
        params.gc.pde_coverage_bit_count     = 0x1B;
        params.gc.available_big_page_sizes   = 0x30000;
        params.gc.gpc_mask                   = 0x1;
        params.gc.sm_arch_sm_version         = 0x503;
        params.gc.sm_arch_spa_version        = 0x503;
        params.gc.sm_arch_warp_count         = 0x80;
        params.gc.gpu_va_bit_count           = 0x28;
        params.gc.reserved                   = 0x0;
        params.gc.flags                      = 0x55;
        params.gc.twod_class                 = 0x902D;
        params.gc.threed_class               = 0xB197;
        params.gc.compute_class              = 0xB1C0;
        params.gc.gpfifo_class               = 0xB06F;
        params.gc.inline_to_memory_class     = 0xA140;
        params.gc.dma_copy_class             = 0xB0B5;
        params.gc.max_fbps_count             = 0x1;
        params.gc.fbp_en_mask                = 0x0;
        params.gc.max_ltc_per_fbp            = 0x2;
        params.gc.max_lts_per_ltc            = 0x1;
        params.gc.max_tex_per_tpc            = 0x0;
        params.gc.max_gpc_count              = 0x1;
        params.gc.rop_l2_en_mask_0           = 0x21D70;
        params.gc.rop_l2_en_mask_1           = 0x0;
        params.gc.chipname                   = 0x6230326D67;  // "gm20b"
        params.gc.gr_compbit_store_base_hw   = 0x0;

        params.gpu_characteristics_buf_size = sizeof(IoctlGpuCharacteristics);  // 0xA0
        // O Eden põe 0xdeadbeef, com o comentário "Cannot be 0 (UNUSED)".
        // É o mesmo valor, e a mesma razão: o guest não o usa, mas não
        // pode ser zero.
        params.gpu_characteristics_buf_addr = 0xdeadbeef;

        // A struct enchida vai para o buffer de saída — o que o guest
        // vai ler.
        const std::size_t saida = ctx.GetWriteBufferSize(0);
        const std::size_t n = std::min(saida, sizeof(params));
        if (n > 0)
            ctx.WriteBuffer(&params, n);

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }

    bool initialized_ = false;
    DeviceFD proximo_fd_ = 1;
    std::map<DeviceFD, std::string> dispositivos_;
    Kernel::KEvent* evento_ = nullptr;
    // O espaço de endereços virtual que o guest pediu para a GPU (o
    // `AllocAsEx`). Fica registado para o log e para o caminho da GPU.
    IoctlAllocAsEx espaco_va_{};

    // A "VM" da GPU — o espaço de endereços virtual, em **páginas do
    // próprio guest**.
    //
    // **Porquê o espaço do guest?** Num motor de execução nativa não há
    // tradução de endereços: a página física de uma região é o seu
    // endereço. O push buffer que o guest submete é lido pelo tradutor
    // Maxwell -> Vulkan (`src/gpu/`) de lá, e os comandos que lá estão
    // referenciam o VA da GPU. Se o VA fosse um espaço à parte, o
    // tradutor teria de manter uma tabela de tradução — e o mais simples
    // é o VA **ser** o espaço do guest.
    //
    // **A zona:** de 0xA0000000 (depois dos 126 MiB que o guest pediu
    // como heap) até à base da stack (0x290000000) — 4,75 GiB.
    //
    // **Não há colisão com o heap do guest**, e a razão é do layout da
    // casa: a região do heap é declarada como `kNormal` (não `Free`) nas
    // `memory_regions()`, por isso o `AddressSpaceAllocator` do SDK
    // (queallocate por `QueryMemory`) **não a toma** — o espaço entre o
    // heap declarado e a stack é nosso.
    //
    // E o tamanho: **medido** (`boot_p6v.log`), o guest pede **65536
    // páginas de 64 KiB = 4 GiB** de páginas grandes. Com 512 MiB (a
    // região de alias) a alocação falhava — e o SDK, com o VA sem
    // espaço, desreferenciava um ponteiro nulo (`0x10`).
    struct VmDaGpu {
        bool iniciado = false;
        std::uint64_t base = 0x98000000ull;  // depois do heap do guest
        std::uint64_t split = 0xC0000000ull;  // a partir daqui: páginas grandes
        std::uint64_t fim = 0x290000000ull;   // a base da stack
        std::uint32_t pagina_grande = 0x10000;  // o que o guest pediu
        std::uint64_t proximo_pequeno = 0x98000000ull;
        std::uint64_t proximo_grande = 0xC0000000ull;
    };
    VmDaGpu va_{};

    // A página pequena do Eden (`VM::YUZU_PAGESIZE`).
    static constexpr std::uint32_t kPaginaPequena = 0x1000;

    // ── os três passos de um ioctl ──
    //
    // O Eden (`ioctl_serialization.h`, `WrapGeneric`): a struct é lida do
    // buffer de **entrada**, o handler enche-a, e a struct enchida é
    // escrita no buffer de **saída**.
    template <typename T>
    static void LerEntrada(HLERequestContext& ctx, T& params) {
        const auto entrada = ctx.ReadBufferCopy(0);
        const std::size_t lido = std::min(entrada.size(), sizeof(params));
        if (lido > 0)
            std::memcpy(&params, entrada.data(), lido);
    }

    template <typename T>
    static void EscreverSaida(HLERequestContext& ctx, const T& params) {
        const std::size_t saida = ctx.GetWriteBufferSize(0);
        const std::size_t n = std::min(saida, sizeof(params));
        if (n > 0)
            ctx.WriteBuffer(&params, n);
    }

    // A resposta de um ioctl que correu bem: `Result` + `NvResult`.
    static void RespostaOk(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(NvResult::Success);
    }
};

} // namespace

// Regista o `nvdrv` no `sm:` — o mesmo caminho do `appletOE`, do `time:u`
// e do `fsp-srv`.
//
// O Eden regista quatro nomes ("nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t" —
// para as aplicações, os applets, os módulos de sistema e os testes). O
// jogo pede o primeiro; os outros são o mesmo serviço com outro nome.
void registar_nvdrv(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("nvdrv", std::make_shared<NvDrv>(system, "nvdrv"));
    server_manager->RegisterNamedService("nvdrv:a", std::make_shared<NvDrv>(system, "nvdrv:a"));
    server_manager->RegisterNamedService("nvdrv:s", std::make_shared<NvDrv>(system, "nvdrv:s"));
    server_manager->RegisterNamedService("nvdrv:t", std::make_shared<NvDrv>(system, "nvdrv:t"));
    Service::ServerManager::RunServer(std::move(server_manager));
}

} // namespace g610::services
