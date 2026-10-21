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
        espaco_va_ = params;

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
            case 0x9:
                AllocAsEx(ctx);
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
