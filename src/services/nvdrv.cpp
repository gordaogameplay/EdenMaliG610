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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "g610/gpu/syncpoint.hpp"
#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"
#include "g610/services/binder.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/services/nvmap_registry.hpp"
#include "g610/telemetry.hpp"

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
// Os tipos de device.
//
// O Eden tem um device **por nome** (`core/hle/service/nvdrv/nvdrv.cpp`,
// `Module::Module`: o mapa `builders` vai de "/dev/nvhost-ctrl" a
// "/dev/nvhost-vic"), e cada um tem a sua tabela de ioctls.
//
// **O tipo importa**: o grupo 0x00 é do `nvhost-ctrl` (o `nvhost_ctrl`) e
// o grupo 0x00 do `nvhost_gpu` é o `GetWaitbase` — o **mesmo** grupo, o
// **mesmo** comando, e coisas completamente diferentes. Despachar só pelo
// grupo (como a versão anterior fazia) não chega: é preciso o device
// também.
// ─────────────────────────────────────────────────────────────────────────────
enum class TipoDeDevice : std::uint8_t {
    Desconhecido,
    Controle,       // /dev/nvhost-ctrl
    ControleGpu,    // /dev/nvhost-ctrl-gpu
    Gpu,            // /dev/nvhost-gpu  (o canal)
    AsGpu,          // /dev/nvhost-as-gpu
    Nvmap,          // /dev/nvmap
    Display,        // /dev/nvdisp_disp0
    Nvdec,          // /dev/nvhost-nvdec
    Nvjpg,          // /dev/nvhost-nvjpg
    Vic,            // /dev/nvhost-vic
};

// O device de um nome (os nomes são os do Eden, `nvdrv.cpp`).
TipoDeDevice DeviceDeNome(const std::string& nome) {
    if (nome == "/dev/nvhost-ctrl")     return TipoDeDevice::Controle;
    if (nome == "/dev/nvhost-ctrl-gpu") return TipoDeDevice::ControleGpu;
    if (nome == "/dev/nvhost-gpu")      return TipoDeDevice::Gpu;
    if (nome == "/dev/nvhost-as-gpu")   return TipoDeDevice::AsGpu;
    if (nome == "/dev/nvmap")           return TipoDeDevice::Nvmap;
    if (nome == "/dev/nvdisp_disp0")    return TipoDeDevice::Display;
    if (nome == "/dev/nvhost-nvdec")    return TipoDeDevice::Nvdec;
    if (nome == "/dev/nvhost-nvjpg")    return TipoDeDevice::Nvjpg;
    if (nome == "/dev/nvhost-vic")      return TipoDeDevice::Vic;
    return TipoDeDevice::Desconhecido;
}

// O estado de um **canal** (o device `/dev/nvhost-gpu`).
//
// O canal é o device por onde o trabalho chega à GPU: o guest diz-lhe
// que handle do `nvmap` é a memória, aloca o contexto de objectos, e
// submete o push buffer.
struct Canal {
    // O handle do `nvmap` que o canal usa (`SetNVMAPfd`).
    std::int32_t nvmap_fd = -1;
    // A *fence* do canal: o par (id, valor) que o Eden devolve no
    // `AllocGPFIFOEx` e que o guest usa para sincronizar. O valor
    // **incrementa** a cada submissão — é o que diz ao guest "isto já
    // chegou à GPU".
    //
    // **O `fence_id` é um syncpoint** — o mesmo objecto que o
    // `EventWait` do `/dev/nvhost-ctrl` espera. É alocado na primeira
    // `AllocGPFIFOEx` do canal (o upstream aloca-o na criação do canal
    // — `nvhost_gpu.cpp`, `channel_syncpoint`).
    std::int32_t fence_id = -1;
    std::uint32_t fence_valor = 0;
    // Os contextos de objectos, por classe (o Eden tem 6: 2D, 3D,
    // compute, kepler, DMA, gpfifo).
    std::map<std::uint32_t, bool> contextos;
    // Os parâmetros do `ZCullBind`.
    std::uint64_t zcull_va = 0;
    std::uint32_t zcull_modo = 0;
    // A prioridade do canal.
    std::uint32_t prioridade = 0;
    // A fatia de tempo do canal (`ChannelSetTimeslice`).
    std::uint32_t fatia_de_tempo = 0;
    // Os dados do cliente (`SetClientData`).
    std::uint64_t dados_cliente = 0;
    // Quantas submissões já foram feitas (para a telemetria).
    std::uint64_t submissoes = 0;
};


// ─────────────────────────────────────────────────────────────────────────────
// As structs do canal (do Eden, `devices/nvhost_gpu.h`).
// ─────────────────────────────────────────────────────────────────────────────

// `IoctlSetNvmapFD`: qual handle do `nvmap` é a memória do canal.
struct IoctlSetNvmapFD {
    std::int32_t nvmap_fd;
};
static_assert(sizeof(IoctlSetNvmapFD) == 4, "tamanho errado");

// `IoctlChannelSetTimeout`: o prazo do canal.
struct IoctlChannelSetTimeout {
    std::uint32_t timeout;
};
static_assert(sizeof(IoctlChannelSetTimeout) == 4, "tamanho errado");

// `IoctlChannelSetPriority`: a prioridade do canal.
struct IoctlChannelSetPriority {
    std::uint32_t priority;
};
static_assert(sizeof(IoctlChannelSetPriority) == 4, "tamanho errado");

// `IoctlGetWaitbase`: o valor base da espera.
struct IoctlGetWaitbase {
    std::uint32_t unknown;
    std::uint32_t value;
};
static_assert(sizeof(IoctlGetWaitbase) == 8, "tamanho errado");

// `IoctlSetErrorNotifier`: o evento de erro do canal.
struct IoctlSetErrorNotifier {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t mem;
    std::uint32_t padding;
};
static_assert(sizeof(IoctlSetErrorNotifier) == 24, "tamanho errado");

// `IoctlAllocObjCtx`: o contexto de objectos de uma classe.
struct IoctlAllocObjCtx {
    std::uint32_t class_num;
    std::uint32_t flags;
    std::uint64_t obj_id;
};
static_assert(sizeof(IoctlAllocObjCtx) == 16, "tamanho errado");

// `IoctlZCullBind`: a ligação do ZCull ao VA da GPU.
struct IoctlZCullBind {
    std::uint64_t gpu_va;
    std::uint32_t mode;
    std::uint32_t padding;
};
static_assert(sizeof(IoctlZCullBind) == 16, "tamanho errado");

// `IoctlAllocGpfifoEx`: a alocação da fila de comandos da GPU.
struct IoctlAllocGpfifoEx {
    std::uint32_t num_entries;
    std::uint32_t num_jobs;
    std::uint32_t flags;
    std::int32_t fence_id;      // NvFence::id
    std::uint32_t fence_value;  // NvFence::value
    std::uint32_t reserved[3];
};
static_assert(sizeof(IoctlAllocGpfifoEx) == 32, "tamanho errado");

// `IoctlSubmitGpfifo`: a submissão do push buffer.
//
// A disposition é a do Eden (`nvhost_gpu.h`): 16 bytes de entrada e a
// `NvFence` (8 bytes) de saída. **As bandeiras** (`flags`) são um union
// de bits: 0 = fence_wait, 1 = fence_increment, 2 = new_hw_format,
// 4 = suppress_wfi, 8 = increment_value.
struct IoctlSubmitGpfifo {
    std::uint64_t address;   // onde está a lista de cabeçalhos
    std::uint32_t num_entries;
    std::uint32_t flags;
    std::int32_t fence_id;      // NvFence::id
    std::uint32_t fence_value;  // NvFence::value
};
static_assert(sizeof(IoctlSubmitGpfifo) == 16 + 8, "tamanho errado");

// `IoctlClientData`: os dados do cliente que o canal guarda.
struct IoctlClientData {
    std::uint64_t data;
};
static_assert(sizeof(IoctlClientData) == 8, "tamanho errado");

// `IoctlSetTimeslice`: a fatia de tempo do canal.
struct IoctlSetTimeslice {
    std::uint32_t timeslice;
};
static_assert(sizeof(IoctlSetTimeslice) == 4, "tamanho errado");

// O cabeçalho de uma entrada da fila de comandos (`Tegra::
// CommandListHeader`, do Eden — `src/video_core/dma_pusher.h`).
//
// **É um `u64` com os campos em bits** — e não três `u32`. A versão
// anterior da casa tratava-o como {tipo, endereço, tamanho} (12
// bytes), e a medição do dump provou que essa leitura era falsa: os
// "endereços" que se viam eram as metades de um `u64`.
//
//   bits  0..39  o endereço (VA da GPU, em bytes — o Eden usa-o
//                directamente, sem escala: `dma_state.dma_get =
//                command_list_header.addr`)
//   bit      40  `allow_flush`
//   bit      41  `is_push_buffer`
//   bits 42..62  o tamanho, em bytes
//   bit      63  `sync`
struct CommandListHeader {
    std::uint64_t bruto = 0;

    std::uint64_t Endereco() const { return bruto & 0xFFFFFFFFFFull; }
    bool AllowFlush() const { return ((bruto >> 40) & 1ull) != 0; }
    bool EhPushBuffer() const { return ((bruto >> 41) & 1ull) != 0; }
    std::uint64_t Tamanho() const { return (bruto >> 42) & 0x1FFFFFull; }
    bool Sync() const { return ((bruto >> 63) & 1ull) != 0; }
};
static_assert(sizeof(CommandListHeader) == 8, "tamanho errado");

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

// `IoctlZbcQueryTable`: a tabela de cores do ZCull.
// O Eden (`nvhost_ctrl_gpu.h`): 52 bytes — quatro cores para a
// "data surface", quatro para a L2, e a profundidade, a contagem de
// referencias, o formato e o tipo.
struct IoctlZbcQueryTable {
    std::uint32_t color_ds[4];
    std::uint32_t color_l2[4];
    std::uint32_t depth;
    std::uint32_t ref_cnt;
    std::uint32_t format;
    std::uint32_t type;
    std::uint32_t index_size;
};
static_assert(sizeof(IoctlZbcQueryTable) == 52, "tamanho errado");

// O numero de tipos de tabela que o Eden suporta — `color` (0) e
// `colorFormat` (1) — mais um, para o `type` poder ser 2 —.
inline constexpr std::uint32_t kTiposZbc = 1;

// `IoctlActiveSlotMask`: a mascara dos slots activos.
// O Eden (`nvhost_ctrl_gpu.h`): 8 bytes — o slot (sempre 0x07) e a mascara.
struct IoctlActiveSlotMask {
    std::uint32_t slot;
    std::uint32_t mask;
};
static_assert(sizeof(IoctlActiveSlotMask) == 8, "tamanho errado");

// `IoctlGpuTime`: o tempo global da GPU, em nanossegundos.
// O Eden (`nvhost_ctrl_gpu.h`): 8 bytes.
struct IoctlGpuTime {
    std::uint64_t gpu_time;
};
static_assert(sizeof(IoctlGpuTime) == 8, "tamanho errado");

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
// O handle do `nvmap` que o guest deu ao canal (`SetNVMAPfd`) — e a
// região que ele representa.
//
// **É global de propósito**: a submissão e' um iocto do *canal*, mas a
//Lookup por fd nem sempre dá o canal (o `fd` do iocto e' o do
// dispositivo, e o canal pode ter sido aberto noutra sessao). O que
// interessa e' a regiao do buffer, e essa e' a mesma para todos.
std::uint32_t g_handle_do_buffer = 0;
std::uint64_t g_endereco_do_buffer = 0;
std::uint64_t g_tamanho_do_buffer = 0;

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

        // O **tipo** do device, pelo nome (os nomes são os do Eden,
        // `nvdrv.cpp`, `Module::Module`). O tipo é o que diz qual é a
        // tabela de ioctls — e sem ele, um ioctl do grupo 0x00 podia
        // ser tomado pelo device errado.
        const TipoDeDevice tipo = DeviceDeNome(nome);
        if (tipo == TipoDeDevice::Desconhecido) {
            // O Eden não conhece o nome: o `Open` falha.
            rb.Push<DeviceFD>(0);
            rb.PushEnum(NvResult::FileOperationFailed);
            LOG_WARNING(Service_NVDRV, "Open: '{}' — device desconhecido", nome);
            return;
        }

        // Um dispositivo novo. O `fd` é um número que o guest usa para
        // os ioctls seguintes — e que tem de ser **estável** enquanto o
        // dispositivo estiver aberto.
        const DeviceFD fd = proximo_fd_++;
        dispositivos_.emplace(fd, nome);
        tipos_.emplace(fd, tipo);
        if (tipo == TipoDeDevice::Gpu)
            canais_.emplace(fd, Canal{});
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
            RespostaErro(ctx, NvResult::NotInitialized, "ioctl");
            return;
        }

        const auto it = dispositivos_.find(fd);
        if (it == dispositivos_.end()) {
            RespostaErro(ctx, NvResult::DeviceNotFound, "ioctl");
            return;
        }
        LOG_DEBUG(Service_NVDRV, "Close: fd={} ('{}')", fd, it->second);
        dispositivos_.erase(it);

        RespostaErro(ctx, NvResult::Success, "ioctl");
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

        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    // 4: o evento de um dispositivo. O Eden devolve um **handle de
    // cópia** (o `KReadableEvent`) e o `NvResult`.
    //
    // **O `fd` diz de que device e' o evento** — o upstream
    // (`nvdrv.cpp:211-229`) procura o device pelo fd e chama o
    // `QueryEvent` dele (`nvhost_ctrl.cpp:297`), que traduz o
    // `event_id` no slot e devolve o `KEvent`.
    //
    // **A casa so' tinha um evento global** — o `evento_` — que
    // servia para o `nvhost-ctrl-gpu` mas nao para o `nvhost-ctrl`
    // (que tem 64 eventos, um por slot).
    void QueryEvent(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const DeviceFD fd = rp.Pop<DeviceFD>();
        const std::uint32_t evento = rp.Pop<std::uint32_t>();

        if (!initialized_) {
            LOG_WARNING(Service_NVDRV, "QueryEvent: NvServices ainda não foi inicializado");
            RespostaErro(ctx, NvResult::NotInitialized, "ioctl");
            return;
        }

        // O device do evento.
        const auto tipo_it = tipos_.find(fd);
        const TipoDeDevice tipo =
            tipo_it == tipos_.end() ? TipoDeDevice::Desconhecido : tipo_it->second;

        if (tipo == TipoDeDevice::Controle) {
            // **O `/dev/nvhost-ctrl`** — o evento e' o do slot.
            //
            // O `event_id` do upstream (`nvhost_ctrl.cpp:297-320`) e' um
            // `SyncpointEventValue`: um union com o slot nos bits
            // baixos e o syncpoint nos altos.
            const std::uint32_t slot = evento & 0xFFu;
            if (slot >= g610::gpu::kMaxNvEvents) {
                RespostaErro(ctx, NvResult::BadParameter, "ioctl");
                return;
            }
            auto& nv = eventos_nv_[slot];
            if (!nv.registado || nv.kevent == nullptr) {
                LOG_WARNING(Service_NVDRV, "QueryEvent: o evento NV {} não está registado", slot);
                RespostaErro(ctx, NvResult::BadParameter, "ioctl");
                return;
            }
            if (nv.fd != fd) {
                // O evento e' de outro device — o upstream
                // (`nvdrv.cpp:214-222`) falha o mesmo.
                LOG_WARNING(Service_NVDRV,
                           "QueryEvent: o evento NV {} pertence ao device {}, não ao {}",
                           slot, nv.fd, fd);
                RespostaErro(ctx, NvResult::BadParameter, "ioctl");
                return;
            }

            ResponseBuilder rb{ctx, 3, 1};
            rb.Push(ResultSuccess);
            rb.PushCopyObjects(ctx, &nv.kevent->GetReadableEvent());
            rb.PushEnum(NvResult::Success);
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
        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    // 8: o APU id do processo.
    void SetAruid(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const std::uint64_t pid = rp.Pop<std::uint64_t>();
        LOG_DEBUG(Service_NVDRV, "SetAruid: pid=0x{:016X}", pid);
        RespostaErro(ctx, NvResult::Success, "ioctl");
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

    // 0x47/0x14: `GetActiveSlotMask` — a máscara dos slots activos.
    //
    // O Eden (`nvhost_ctrl_gpu.cpp`, `GetActiveSlotMask`): devolve
    // `slot = 0x07` e `mask = 0x01` — sempre.
    //
    // **Medido**: o guest pede este ioctl (`fd=1 grupo=0x47 cmd=0x14`)
    // e a casa respondia `NotImplemented` — o que é um erro para
    // o guest.
    void GetActiveSlotMask(HLERequestContext& ctx) {
        IoctlActiveSlotMask params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "GetActiveSlotMask: pedido");

        params.slot = 0x07;
        params.mask = 0x01;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x47/0x1c: `GetGpuTime` — o tempo global da GPU, em nanossegundos.
    //
    // O Eden (`nvhost_ctrl_gpu.cpp`, `GetGpuTime`): o `CoreTiming` global.
    //
    // **A casa nao tem `CoreTiming`** — o `System` da casa e' o
    // `system_ref.h`, que nao o expoe. O que o guest quer e' um tempo
    // que **avance** — o Eden usa-o para medir o tempo de GPU — e o
    // relogio monotonico do host serve. O valor absoluto nao
    // interessa (o Eden comeca do zero).
    void GetGpuTime(HLERequestContext& ctx) {
        IoctlGpuTime params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "GetGpuTime: pedido");

        params.gpu_time = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x47/0x03: `ZBCSetTable` — o guest escreve a tabela de cores do
    // ZCull.
    //
    // O Eden (`nvhost_ctrl_gpu.cpp`, `ZBCSetTable`): valida o tipo e
    // escreve as cores. **A casa não precisa de guardar a tabela** — o
    // ZCull da casa não a consulta — mas tem de **aceitar** o
    // comando, porque um `NotImplemented` é um erro para o guest.
    void ZBCSetTable(HLERequestContext& ctx) {
        IoctlZbcQueryTable params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "ZBCSetTable: type={} index_size={}", params.type,
                  params.index_size);

        // O Eden: um tipo acima do suportado é `BadParameter`.
        if (params.type > kTiposZbc) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x47/0x04: `ZBCQueryTable` — o guest pergunta a tabela de cores.
    //
    // O Eden (`nvhost_ctrl_gpu.cpp`, `ZBCQueryTable`): devolve as cores da
    // tabela pedida — e a struct é de tamanho fixo, com as cores
    // dentro.
    void ZBCQueryTable(HLERequestContext& ctx) {
        IoctlZbcQueryTable params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "ZBCQueryTable: type={} index_size={}", params.type,
                  params.index_size);

        if (params.type > kTiposZbc) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        // As cores da tabela — as do Eden (`nvhost_ctrl_gpu.cpp`).
        static constexpr std::uint32_t kCoresZbc[3][4] = {
            {0x00000000, 0x00000000, 0x00000000, 0x00000000},
            {0xFF000000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00},
            {0xFF000000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00},
        };

        const std::uint32_t indice = params.type < 3 ? params.type : 0;
        for (std::size_t k = 0; k < 4; ++k) {
            params.color_ds[k] = kCoresZbc[indice][k];
            params.color_l2[k] = kCoresZbc[indice][k];
        }
        params.index_size = 16;

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
        // **Se o guest escolheu**, o Eden usa o **dele**
        // (`nvhost_as_gpu.cpp:127-131`):
        //
        //     if (params.va_range_start) {
        //         vm.va_range_start = params.va_range_start;
        //         vm.va_range_split = params.va_range_split;
        //         vm.va_range_end = params.va_range_end;
        //     }
        //
        // **A casa ignorava-o** e usava a sua regiao — o que
        // fazia o `MapBufferEx` devolver VAs **fora** da regiao que o
        // guest reservou.
        //
        // A regiao por omissão da casa é a da reserva do guest (a nota
        // da `VmDaGpu`) — de 0x98000000 a 0x290000000 — porque o VA
        // da GPU tem de ser memória do **guest**.
        if (params.va_range_start != 0) {
            va_.base = params.va_range_start;
            va_.split = params.va_range_split;
            va_.fim = params.va_range_end;
            va_.proximo_pequeno = va_.base;
            va_.proximo_grande = va_.split;
        }

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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
            return;
        }

        const std::uint64_t tamanho = static_cast<std::uint64_t>(params.pages) * passo;
        if (*proximo + tamanho > limite) {
            LOG_WARNING(Service_NVDRV, "AllocateSpace: sem espaço no VA da GPU");
            RespostaErro(ctx, NvResult::InsufficientMemory, "ioctl");
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
    // **O handle alocado por cada `fd` do `/dev/nvmap`** — o canal
    // (o `/dev/nvhost-gpu`) diz qual e' o seu `nvmap_fd`, e o que
    // interessa e' a regiao desse `nvmap`.
    //
    // **Medido** (`/tmp/boot_r34.log`): o guest passa `03 00 00 00` ao
    // `SetNVMAPfd` — e o `3` e' o **fd** do `/dev/nvmap` (o log:
    // `Open: '/dev/nvmap' -> fd=3`), **nao'** um handle (os
    // handles da casa sao 4, 8, 12, …).
    //
    // O Eden (`devices/nvhost_gpu.h:75-78`) tambem o chama `nvmap_fd`
    // — e o `nvhost_gpu` usa-o como o **fd** (`nvhost_gpu.cpp:140`
    //: `nvmap_fd = params.nvmap_fd`).
    //
    // A casa tratava-o como um **handle** — e por isso nao encontrava
    // a regiao (o log: `SetNVMAPfd: handle=3 — sem regiao`).
    std::map<std::int32_t, std::vector<std::uint32_t>> handles_por_fd;
    // **O primeiro handle e' o 4** — o upstream
    // (`nvmap/core/nvmap.h:165-167`) faz `fetch_add` a partir de
    // `HandleIdIncrement` (= 4):
    //
    //     static constexpr u32 HandleIdIncrement{4};
    //     std::atomic<u32> next_handle_id{HandleIdIncrement};
    //
    // A casa comecava em **0** e contava de 1 em 1 — e o handle **0**
    // existe no Eden **apenas como handle invalido**:
    //
    // * o `IocAlloc` do Eden recusa-o (`devices/nvmap.cpp:103-106`:
    //   `if (!params.handle) { ... return NvResult::BadValue; }`);
    // * o `GraphicBuffer` do Eden so' o usa se for **maior que zero**
    //   (`ui/graphic_buffer.cpp:23`: `if (this->BufferId() > 0)`).
    //
    // **Medido** (`/tmp/boot_r26.log`): o primeiro `IocCreate` da casa
    // devolveu **handle=0** — e o `MapBufferEx` desse handle passou
    // pelo `RegistoNvmap` como se fosse um handle normal.
    //
    // A numeracao e' agora **a do Eden**: 4, 8, 12, … — o handle 0
    // nunca existe, e o guest que o peca recebe um erro (como no
    // Eden) em vez de um handle silenciosamente invalido.
    std::uint32_t proximo_nvmap_ = 4;
    // O par (VA da GPU -> endereco do guest), que o tradutor Maxwell ->
    // Vulkan usa para traduzir os comandos do push buffer.
    // **O mapeamento** — o `Mapping` do upstream
    // (`nvhost_as_gpu.h:167-180`): o `ptr` (o endereco base), o
    // `size` e o `fixed`.
    //
    // **O `Remap` precisa do `ptr` base** — o upstream faz
    // `device_address = mapping->ptr + params.buffer_offset`
    // (`nvhost_as_gpu.cpp:343`). Com a casa a guardar so' o
    // endereco final, o `buffer_offset` era aplicado **duas vezes**
    // — ou nenhuma.
    struct Mapeamento {
        std::uint64_t ptr = 0;    // o endereco base (o `ptr` do upstream)
        std::uint64_t size = 0;   // o tamanho do mapeamento
        bool fixed = false;
    };
    std::map<std::uint64_t, Mapeamento> mapeamentos_;

    // 0x01/0x01: `IocCreate` — criar um handle de `size` bytes.
    void IocCreate(HLERequestContext& ctx) {
        IocCreateParams params{};
        LerEntrada(ctx, params);

        // **De 4 em 4** — o Eden (`nvmap/core/nvmap.h:165`):
        // `HandleIdIncrement` = 4. O `fetch_add` do Eden e' sobre um
        // `u32`, por isso o handle mais alto que se pode pedir e'
        // `0xFFFFFFFC` — e o Eden **enrola** (o `fetch_add` e' um
        // `relaxed` e o `u32` da casa e' como o do Eden).
        const std::uint32_t id = proximo_nvmap_;
        proximo_nvmap_ += 4;
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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
            return;
        }

        // O alinhamento mínimo é a página (o Eden: `params.align <
        // YUZU_PAGESIZE` → sobe para a página).
        std::uint32_t align = params.align != 0 ? params.align : kPaginaPequena;
        if (align < kPaginaPequena)
            align = kPaginaPequena;

        // ── O endereço é o do guest ─────────────────────────────────────────
        //
        // O Eden (`nvmap/core/nvmap.cpp:25-52`, `Handle::Alloc`):
        //
        //     if (pAddress) {
        //         flags.keep_uncached_after_free.Assign(0);
        //     } else {
        //         LOG_CRITICAL(Service_NVDRV,
        //             "Mapping nvmap handles without a CPU side address is "
        //             "unimplemented!");
        //     }
        //     ...
        //     address = pAddress;
        //
        // Ou seja: **o Eden usa o endereço que o guest pediu**. O que
        // o Eden acrescenta é o mapeamento desse endereço no espaço do
        // dispositivo (`nvmap.cpp:137-141`, `LockForMapDeviceAddressSpace`)
        // — para a GPU o poder ler.
        //
        // **A casa ignorava o endereço do guest** e devolvia um do seu
        // contador. O guest, como o Eden, escreve no endereço **que
        // pediu** — e a casa lia um sítio onde ninguém escreveu.
        //
        // Medido (`boot_v2.log`): o push buffer do `SubmitGPFIFO`
        // (0xB9112000, 2205 bytes) e a região do canal
        // (0x98011000, 328 MB) estavam **ambos a zeros** — e o
        // `SubmitGPFIFO` traduzia **0 comandos**.
        //
        // Na casa o VA da GPU **é** o espaço do guest (nota da
        // `VmDaGpu`) — por isso o endereço pedido serve tal e qual:
        // o guest escreve nele e a casa lê nele ✓.
        const std::uint64_t pedido = params.address;
        std::uint64_t endereco = 0;

        const std::uint64_t tamanho =
            (static_cast<std::uint64_t>(it->second.tamanho) + kPaginaPequena - 1) &
            ~static_cast<std::uint64_t>(kPaginaPequena - 1);

        if (pedido != 0) {
            // O guest pediu este endereço — é o dele.
            endereco = pedido;

            // E se colidir com outra alocação ? O Eden mapeia por cima
            // — e avisa.
            for (const auto& [id, h] : nvmap_) {
                if (id == params.handle || !h.alocado)
                    continue;
                const std::uint64_t fim = h.endereco + h.tamanho;
                if (pedido < fim && h.endereco < pedido + tamanho) {
                    LOG_WARNING(Service_NVDRV,
                                "nvmap: IocAlloc: o endereco pedido 0x{:X} colide com o "
                                "handle {} (0x{:X}..0x{:X})",
                                pedido, id, h.endereco, fim);
                    break;
                }
            }
        } else {
            // Sem endereço: o Eden nem isso suporta ("unimplemented"
            //). A casa dá o do contador — para o jogo ter onde meter
            // alguma coisa — e **diz**.
            LOG_WARNING(Service_NVDRV,
                        "nvmap: IocAlloc: o guest nao pediu endereco — o Eden "
                        "considera isto nao implementado; a casa usa o do VA");
            if (va_.proximo_pequeno + tamanho > va_.split) {
                LOG_WARNING(Service_NVDRV, "nvmap: IocAlloc: sem espaço no VA da GPU");
                RespostaErro(ctx, NvResult::InsufficientMemory, "ioctl");
                return;
            }
            endereco = va_.proximo_pequeno;
            va_.proximo_pequeno += tamanho;
        }

        it->second.endereco = endereco;
        it->second.align = align;
        it->second.alocado = true;

        // O registo **partilhado**: o compositor precisa de chegar ao
        // endereco do buffer quando o jogo o entrega pelo binder
        // (`SetPreallocatedBuffer` traz o handle do `nvmap`).
        RegistoNvmap::Instancia().Registar(params.handle, it->second.endereco, tamanho);

        // **O handle deste `fd`** — e' o que o `SetNVMAPfd` vai
        // procurar (que da o **fd**, nao o handle).
        handles_por_fd[fd_em_curso_].push_back(params.handle);

        LOG_DEBUG(Service_NVDRV,
                  "nvmap: IocAlloc handle={} tamanho=0x{:X} pedido=0x{:X} -> 0x{:X}",
                  params.handle, it->second.tamanho, pedido, it->second.endereco);

        // O endereço que sai é **o pedido** — como no Eden.
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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
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
            RegistoNvmap::Instancia().Remover(params.handle);
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
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
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
            LOG_WARNING(Service_NVDRV, "nvmap: IocGetId: handle {} desconhecido", params.handle);
            RespostaErro(ctx, NvResult::AccessDenied, "ioctl");
            return;
        }
        params.id = params.handle;
        LOG_DEBUG(Service_NVDRV, "nvmap: IocGetId: handle={} -> id={}", params.handle, params.id);
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

        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            const auto entrada = ctx.ReadBufferCopy(0);
            std::fprintf(stderr, "[nvdrv] MapBufferEx: entrada=%zu bytes:", entrada.size());
            for (std::size_t i = 0; i < entrada.size() && i < 40; ++i)
                std::fprintf(stderr, " %02x", entrada[i]);
            std::fprintf(stderr, "\n");
        }

        // O VA devolvido.
        //
        // **O upstream tem tres caminhos** (`nvhost_as_gpu.cpp:331-408`):
        //
        // - `Remap` (bit 8) — remapeia um VA **ja existente** e
        //     **nao altera o `params.offset`**;
        // - `Fixed` (bit 0) — o VA e' o `params.offset`;
        // - o resto — o VA e' **alocado** (o `allocator.Allocate`)
        //     e o `params.offset` passa a ser esse.
        //
        // **A casa tratava o `Remap` como se fosse o caminho normal**
        // — e devolvia o **endereco da memoria** em vez do VA do guest
        //. O guest escrevia num sitio e lia noutro — o
        // framebuffer ficava partido.
        //
        // **Medido** (`/tmp/boot_v20.log`): o guest pede
        // `MapBufferEx: handle=0 va=0xE5D39000 -> 0xF4CEF000 tamanho=0x870000
        // flags=0x100` — o `0x100` e' o `Remap`.
        constexpr std::uint32_t kMappingFixed = 1u << 0;
        constexpr std::uint32_t kMappingRemap = 1u << 8;

        if ((params.flags & kMappingRemap) != 0) {
            // ── O `Remap` ──────────────────────────────────────────────
            //
            // O upstream (`nvhost_as_gpu.cpp:331-348`):
            //
            //     auto mapping{mapping_map.at(params.offset)};
            //     if (mapping->size < params.mapping_size) { return BadValue; }
            //     u64 gpu_address{params.offset + params.buffer_offset};
            //     VAddr device_address{mapping->ptr + params.buffer_offset};
            //     gmmu->Map(gpu_address, device_address, params.mapping_size, ...);
            //
            // **O VA tem de estar mapeado** — o `mapping_map.at()`
            // **lanca** se nao estiver. A casa aceitava o `Remap` de
            // um VA desconhecido.
            const auto mapeamento_it = mapeamentos_.find(static_cast<std::uint64_t>(params.offset));
            if (mapeamento_it == mapeamentos_.end()) {
                LOG_WARNING(Service_NVDRV,
                           "MapBufferEx: Remap de um VA nao mapeado (0x{:X})", params.offset);
                RespostaErro(ctx, NvResult::BadValue, "ioctl");
                return;
            }

            const Mapeamento& mapeamento = mapeamento_it->second;
            if (mapeamento.size < params.mapping_size) {
                LOG_WARNING(Service_NVDRV,
                           "MapBufferEx: Remap de um mapeamento menor do que o pedido "
                           "(0x{:X} < 0x{:X})",
                           mapeamento.size, params.mapping_size);
                RespostaErro(ctx, NvResult::BadValue, "ioctl");
                return;
            }

            // O endereco final — o `ptr` base **mais** o `buffer_offset`
            // (o upstream).
            //
            // **O `ptr` base nao muda** — o upstream nao o altera
            // (`nvhost_as_gpu.cpp:343` usa `mapping->ptr`, o base,
            // e nao o resultado anterior). A casa acumulava —
            // cada remap somava o `buffer_offset` ao anterior.
            const std::uint64_t endereco_remap =
                mapeamento.ptr + static_cast<std::uint64_t>(params.buffer_offset);
            LOG_DEBUG(Service_NVDRV,
                      "MapBufferEx: Remap: ptr base=0x{:X} buffer_offset=0x{:X}", mapeamento.ptr,
                      static_cast<std::uint64_t>(params.buffer_offset));

            // **O VA nao muda**.
            params.offset = static_cast<std::int64_t>(params.offset);

            LOG_DEBUG(Service_NVDRV,
                      "MapBufferEx: Remap va=0x{:X} handle={} -> 0x{:X} tamanho=0x{:X}",
                      params.offset, params.handle, endereco_remap, params.mapping_size);

            EscreverSaida(ctx, params);
            RespostaOk(ctx);
            return;
        }

        // **A validacao do handle** — so' **depois** do `Remap`,
        // como no upstream (`nvhost_as_gpu.cpp:353`: o `GetHandle`
        // so' e' chamado depois).
        //
        // **Medido** (`/tmp/boot_r38.log`): o guest pede
        //
        //     MapBufferEx: flags=0x100 handle=0 ...
        //
        // — o `handle` e' **zero** porque o `Remap` nao' o usa.
        // A casa validava o handle **antes** e respondia `BadValue`
        // — e o guest ficava sem o remapeamento.

        const auto it = nvmap_.find(params.handle);
        if (it == nvmap_.end() || !it->second.alocado) {
            LOG_WARNING(Service_NVDRV, "MapBufferEx: handle {} não está alocado", params.handle);
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
            return;
        }

        // **O tamanho** — o upstream (`nvhost_as_gpu.cpp:369`):
        //
        //     u64 size{params.mapping_size ? params.mapping_size : handle->orig_size};
        //
        // **O tamanho zero significa "o handle inteiro"** — e a casa
        // usava o zero, o que mapeava **nada**.
        //
        // **Medido**: o guest pede `MapBufferEx` com `mapping_size = 0`
        // (`fd=2 grupo=0x41 cmd=0x06 tamanho=40`, `va=0x0 -> ... tamanho=0x0`
        //) — e a casa registava `tamanho=0x0`.
        const std::uint64_t tamanho =
            params.mapping_size != 0 ? params.mapping_size : it->second.tamanho;

        // **O endereco base** — o `nvmap` do handle, **mais** o
        // `buffer_offset`.
        //
        // **O `buffer_offset` conta a partir do handle** — e o upstream
        // (`nvhost_as_gpu.cpp:369-375`) faz `device_address = ptr +
        // buffer_offset`, onde o `ptr` e' o do **mapeamento**.
        const std::uint64_t endereco = it->second.endereco + params.buffer_offset;
        LOG_DEBUG(Service_NVDRV,
                  "MapBufferEx: handle={} va=0x{:X} -> 0x{:X} tamanho=0x{:X} flags=0x{:X}",
                  params.handle, static_cast<std::uint64_t>(params.offset), endereco, tamanho,
                  params.flags);

        if ((params.flags & kMappingFixed) != 0) {
            params.offset = static_cast<std::int64_t>(params.offset);
        } else {
            params.offset = static_cast<std::int64_t>(endereco);
        }
        {
            Mapeamento mapeamento{};
            mapeamento.ptr = endereco;
            mapeamento.size = tamanho;
            mapeamento.fixed = (params.flags & kMappingFixed) != 0;
            mapeamentos_[static_cast<std::uint64_t>(params.offset)] = mapeamento;
        }

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

    // ── O CANAL (`/dev/nvhost-gpu`) ──
    //
    // O canal é o device por onde o trabalho chega à GPU. Os handlers são
    // os do upstream (`devices/nvhost_gpu.cpp`).

    // 0x48/0x01: `SetNVMAPfd` — o **fd** do `/dev/nvmap` que é a memória do
    // canal.
    //
    // **O `nvmap_fd` é o FD, não um handle** — e é o que o Eden
    // assume (`devices/nvhost_gpu.h:75-78`: o campo chama-se
    // `nvmap_fd`, e o `nvhost_gpu.cpp:140` guarda-o tal e qual).
    //
    // **Medido** (`/tmp/boot_r34.log`): o guest passa `03 00 00 00` —
    // e o `3` é o **fd** do `/dev/nvmap` (o log: `Open: '/dev/nvmap'
    // -> fd=3`), **não** um handle (os handles da casa são
    // 4, 8, 12, …).
    //
    // A casa tratava-o como um handle — e por isso não encontrava a
    // região (o log: `SetNVMAPfd: handle=3 — sem região`).
    //
    // **O que é a região do canal**: a do **último handle alocado**
    // por esse `fd` — que é o push buffer (o Eden usa o
    // `nvmap_fd` para chegar ao `NvMap` e dai ao handle do canal).
    void SetNVMAPfd(HLERequestContext& ctx) {
        IoctlSetNvmapFD params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: SetNVMAPfd: nvmap_fd={}", params.nvmap_fd);
        if (auto* canal = CanalDoFd())
            canal->nvmap_fd = params.nvmap_fd;

        // **A região do handle** — é aqui que está o push buffer (o
        // guest aloca-o pelo `nvmap` e dá o handle ao canal). Despeja-la
        // é a medição que diz o formato real.
        {
            // **O handle do canal** — o do `nvmap` com este `fd`.
            //
            // **Medido** (`/tmp/boot_r35.log`): o `nvmap` tem varias
            // alocacoes (0x1000, 0x10000, 0x13900000,
            // **0xD800000**, 0x1000, 0x10000, 0x49000) e o
            // **push buffer** e' a maior — os 226 MB (0xD800000
            //), que e' o que o Eden reserva para o GPFIFO.
            //
            // O Eden (`nvhost_gpu.cpp:183-208`) **nao' cria** a memoria
            // do GPFIFO — o jogo aloca-a pelo `nvmap` — e o canal
            // usa-a pelo `memory_manager`.
            //
            // **A regra**: a regiao do canal e' a **maior** alocacao do
            // `nvmap` com este `fd` — que e' o push buffer.
            std::uint32_t handle = 0;
            std::uint64_t maior = 0;
            const auto lista = handles_por_fd.find(params.nvmap_fd);
            if (lista != handles_por_fd.end()) {
                for (const std::uint32_t h : lista->second) {
                    const auto it = nvmap_.find(h);
                    if (it != nvmap_.end() && it->second.tamanho > maior) {
                        maior = it->second.tamanho;
                        handle = h;
                    }
                }
            }

            RegistoNvmap::Regiao regiao{};
            if (handle != 0 &&
                RegistoNvmap::Instancia().Procurar(handle, regiao)) {
                LOG_DEBUG(Service_NVDRV,
                          "canal: SetNVMAPfd: nvmap_fd={} -> handle={} -> 0x{:X} ({} bytes)",
                          params.nvmap_fd, handle, regiao.endereco, regiao.tamanho);
                g_handle_do_buffer = handle;
                RegistoNvmap::Instancia().MarcarCanal(g_handle_do_buffer);
                g_endereco_do_buffer = regiao.endereco;
                g_tamanho_do_buffer = regiao.tamanho;
                Despejar(regiao.endereco, 64, "nvmap");
            } else {
                LOG_WARNING(Service_NVDRV,
                            "canal: SetNVMAPfd: nvmap_fd={} — sem região (handle {})",
                            params.nvmap_fd, handle);
            }
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x03: `ChannelSetTimeout` — o prazo do canal.
    void ChannelSetTimeout(HLERequestContext& ctx) {
        IoctlChannelSetTimeout params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: ChannelSetTimeout: 0x{:X}", params.timeout);

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x0d: `SetChannelPriority` — a prioridade do canal.
    //
    // O Eden (`nvhost_gpu.cpp`, `SetChannelPriority`) aceita três
    // prioridades — `Low`, `Medium`, `High` — e o resto é `BadParameter`.
    void SetChannelPriority(HLERequestContext& ctx) {
        IoctlChannelSetPriority params{};
        LerEntrada(ctx, params);

        // Os valores do Eden (`nvhost_gpu.h`, `ChannelPriority`).
        constexpr std::uint32_t kBaixa = 0x40;
        constexpr std::uint32_t kMedia = 0x80;
        constexpr std::uint32_t kAlta = 0xC0;

        LOG_DEBUG(Service_NVDRV, "canal: SetChannelPriority: 0x{:X}", params.priority);
        if (params.priority != kBaixa && params.priority != kMedia &&
            params.priority != kAlta) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }
        if (auto* canal = CanalDoFd())
            canal->prioridade = params.priority;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x00/0x03: `GetWaitbase` — o valor base da espera.
    void GetWaitbase(HLERequestContext& ctx) {
        IoctlGetWaitbase params{};
        LerEntrada(ctx, params);

        // O Eden: `params.value = 0; // Seems to be hard coded at 0`.
        params.value = 0;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x0c: `SetErrorNotifier` — o evento de erro do canal.
    void SetErrorNotifier(HLERequestContext& ctx) {
        IoctlSetErrorNotifier params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: SetErrorNotifier: offset=0x{:X} size=0x{:X} handle={}",
                  params.offset, params.size, params.mem);

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x09: `AllocateObjectContext` — o contexto de objectos de
    // uma classe (2D, 3D, compute, kepler, DMA, gpfifo).
    void AllocateObjectContext(HLERequestContext& ctx) {
        IoctlAllocObjCtx params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: AllocateObjectContext: classe=0x{:X} flags=0x{:X}",
                  params.class_num, params.flags);

        // O Eden (`nvhost_gpu.cpp`, `AllocateObjectContext`): o contexto
        // é adiado até o canal estar inicializado, e um contexto
        // repetido da mesma classe é `AlreadyAllocated`.
        if (auto* canal = CanalDoFd()) {
            if (canal->contextos.count(params.class_num) != 0) {
                RespostaErro(ctx, NvResult::AlreadyAllocated, "ioctl");
                return;
            }
            canal->contextos.emplace(params.class_num, true);
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x0b: `ZCullBind` — a ligação do ZCull ao VA da GPU.
    void ZCullBind(HLERequestContext& ctx) {
        IoctlZCullBind params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: ZCullBind: va=0x{:X} modo=0x{:X}", params.gpu_va,
                  params.mode);
        if (auto* canal = CanalDoFd()) {
            canal->zcull_va = params.gpu_va;
            canal->zcull_modo = params.mode;
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x18 e 0x1a: `AllocGPFIFOEx` / `AllocGPFIFOEx2` — a fila
    // de comandos da GPU.
    //
    // A *fence* que sai é o par (id, valor) do syncpoint do canal. **O
    // valor incrementa a cada submissão** — é o que diz ao guest que o
    // trabalho anterior já passou.
    void AllocGPFIFOEx(HLERequestContext& ctx) {
        IoctlAllocGpfifoEx params{};
        LerEntrada(ctx, params);
        LOG_DEBUG(Service_NVDRV, "canal: AllocGPFIFOEx: entradas={} jobs={} flags=0x{:X}",
                  params.num_entries, params.num_jobs, params.flags);

        if (auto* canal = CanalDoFd()) {
            // **O syncpoint do canal** — alocado na primeira vez (o
            // upstream aloca-o na criação do canal — `nvhost_gpu.cpp`,
            // `channel_syncpoint`). E' o id que o guest espera no
            // `EventWait`.
            if (canal->fence_id < 0) {
                canal->fence_id = static_cast<std::int32_t>(syncpoints_.Alocar());
                LOG_DEBUG(Service_NVDRV, "canal: syncpoint {} alocado", canal->fence_id);
            }
            params.fence_id = canal->fence_id;
            params.fence_value = canal->fence_valor;
        }

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x08: `SubmitGPFIFOBase1` — **a submissão do push buffer**.
    //
    // É aqui que o trabalho do jogo chega à GPU: o guest escreveu os
    // comandos Maxwell num endereço do VA da GPU e submete a lista.
    //
    // O Eden (`nvhost_gpu.cpp`, `SubmitGPFIFOBase1`): lê os
    // `CommandListHeader` do buffer, e para cada um lê os comandos.
    //
    // **No motor nativo o VA da GPU é o espaço do guest** (ver a nota do
    // `VmDaGpu`), por isso ler o push buffer é ler a memória do guest
    // nesse endereço — sem tradução nenhuma.
    void SubmitGPFIFOBase1(HLERequestContext& ctx) {
        SubmitGPFIFO(ctx, false);
    }

    // 0x48/0x1b: a submissão com os cabeçalhos **no endereço** (a
    // variante "kickoff" do Eden).
    void SubmitGPFIFOKickoff(HLERequestContext& ctx) {
        SubmitGPFIFO(ctx, true);
    }

    // A submissão — **a porta de entrada do tradutor**.
    //
    // O guest escreveu os comandos Maxwell num endereço do VA da GPU e
    // submete a lista de cabeçalhos (`CommandListHeader`: o tipo, o
    // endereço dos comandos e o tamanho).
    //
    // **As duas variantes** (o Eden, `nvhost_gpu.cpp`):
    //
    //   - 0x08 (`kickoff = false`): os cabeçalhos vêm **no pedido**,
    //     a seguir à struct fixa;
    //   - 0x1b (`kickoff = true`): os cabeçalhos estão **no endereço**
    //     (`params.address`).
    //
    // E o que o Eden faz com a *fence* (`SubmitGPFIFOImpl`): o `id` é o
    // syncpoint do canal, e o `valor` **incrementa** — o incremento é 2
    // quando o guest pede `fence_increment`, mais o valor que o guest
    // mandou, quando pede `increment_value`. E as bandeiras saem a zero.
    void SubmitGPFIFO(HLERequestContext& ctx, bool cabecalhos_no_endereco) {
        // O buffer de entrada: a struct fixa, e (na variante de ioctl
        // variável) os cabeçalhos a seguir.
        const auto entrada = ctx.ReadBufferCopy(0);
        IoctlSubmitGpfifo params{};
        const std::size_t lido = std::min(entrada.size(), sizeof(params));
        if (lido > 0)
            std::memcpy(&params, entrada.data(), lido);

        LOG_DEBUG(Service_NVDRV,
                  "canal: SubmitGPFIFO: address=0x{:X} entradas={} flags=0x{:X} kickoff={}",
                  params.address, params.num_entries, params.flags,
                  cabecalhos_no_endereco ? 1 : 0);

        // Os cabeçalhos.
        // **A região do canal, no momento da submissão.** É aqui que o
        // jogo já escreveu o push buffer (no `SetNVMAPfd` ainda estava a
        // zeros — medido: 313 MB de zeros).
        if (g_endereco_do_buffer != 0) {
            LOG_DEBUG(Service_NVDRV, "canal: a região do buffer é 0x{:X} ({} bytes)",
                      g_endereco_do_buffer, g_tamanho_do_buffer);
            Despejar(g_endereco_do_buffer, 128, "pushbuffer");
        }

        // **O despejo completo do pushbuffer** (`G610_PB_DUMP=<ficheiro>`):
        // a cada SubmitGPFIFO escreve a região do canal (limitada a 64 KiB)
        // em modo append, com um cabeçalho por submissão (endereço, tamanho,
        // num_entries, flags). É o instrumento da decodificação da GPU: o
        // log só tem 64 bytes por submissão (o suficiente para ver que
        // existe), e a decodificação exige o buffer inteiro.
        if (const char* dump_pb = std::getenv("G610_PB_DUMP")) {
            constexpr std::uint64_t kTeto = 64 * 1024;
            const std::uint64_t tam = std::min(g_tamanho_do_buffer, kTeto);
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(tam), 0);
            if (tam > 0 && LerDoGuest(g_endereco_do_buffer, buf.data(),
                                     static_cast<std::size_t>(tam))) {
                static std::uint32_t n_pb = 0;
                ++n_pb;
                std::FILE* f = std::fopen(dump_pb, "ab");
                if (f != nullptr) {
                    std::uint64_t cabec[4] = {params.address, params.num_entries,
                                               params.flags, g_tamanho_do_buffer};
                    std::fwrite(&n_pb, sizeof n_pb, 1, f);
                    std::fwrite(cabec, sizeof cabec, 1, f);
                    std::fwrite(buf.data(), 1, buf.size(), f);
                    std::fclose(f);
                }
            }
        }

        // A native window: o diagnostico de "o jogo desenhou?".
        DespejarJanelaDoCompositor();

        const std::size_t bytes_cabec =
            static_cast<std::size_t>(params.num_entries) * sizeof(CommandListHeader);
        std::vector<std::uint8_t> bruto_cabec(bytes_cabec, 0);
        if (bytes_cabec > 0 &&
            !LerDoGuest(params.address, bruto_cabec.data(), bruto_cabec.size())) {
            LOG_WARNING(Service_NVDRV, "SubmitGPFIFO: não li os cabeçalhos em 0x{:X}",
                        params.address);
        }

        // Os cabeçalhos **crus**, em hexadecimal: é o que permite
        // descodificar o formato real (que **não** é o
        // {tipo, endereço, tamanho} que se supunha).
        {
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[gpfifo] cabecalhos crus:"));
            for (std::size_t i = 0; i < bruto_cabec.size() && i < 64 && n + 4 < sizeof(linha);
                 ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", bruto_cabec[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // Os cabeçalhos **crus**: com o formato certo (8 bytes por
        // entrada), sao eles que dizem onde esta o push buffer.
        {
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[gpfifo] cabecalhos crus:"));
            for (std::size_t i = 0; i < bruto_cabec.size() && i < 64 && n + 4 < sizeof(linha);
                 ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", bruto_cabec[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        std::vector<CommandListHeader> cabecalhos;
        if (params.num_entries > 0) {
            if (cabecalhos_no_endereco) {
                const std::size_t bytes = bytes_cabec;
                std::vector<std::uint8_t> bruto(bytes, 0);
                if (!LerDoGuest(params.address, bruto.data(), bruto.size())) {
                    LOG_WARNING(Service_NVDRV, "SubmitGPFIFO: não li os cabeçalhos");
                    RespostaErro(ctx, NvResult::InvalidAddress, "ioctl");
                    return;
                }
                for (std::uint32_t i = 0; i < params.num_entries; ++i) {
                    CommandListHeader cab{};
                    std::memcpy(&cab, bruto.data() + i * sizeof(cab), sizeof(cab));
                    cabecalhos.push_back(cab);
                }
            } else {
                // A seguir à struct fixa, no próprio pedido.
                const std::size_t off = sizeof(params);
                for (std::uint32_t i = 0; i < params.num_entries; ++i) {
                    const std::size_t pos = off + i * sizeof(CommandListHeader);
                    if (pos + sizeof(CommandListHeader) > entrada.size())
                        break;
                    CommandListHeader cab{};
                    std::memcpy(&cab, entrada.data() + pos, sizeof(cab));
                    cabecalhos.push_back(cab);
                }
            }
        }

        // O push buffer: a concatenação dos comandos de cada entrada.
        //
        // **O VA da GPU é o espaço do guest** (ver a nota do `VmDaGpu`),
        // por isso ler os comandos é ler a memória do guest.
        //
        // ── O `size` é em PALAVRAS, não em bytes ──────────────────────────
        //
        // O upstream (`dma_pusher.h:77-86`, `Tegra::CommandListHeader`):
        //
        //     BitField<42, 21, u64> size;
        //
        // e o `DmaPusher::ProcessCommands` recebe
        // `std::span<const CommandHeader>` — ou seja, `size` palavras
        // de 32 bits (`GpuGuestMemory<CommandHeader>` lê `size`
        // **elementos**).
        //
        // **Prova medida**: no pedido real, a entrada 1 começa em
        // `0xE2D67000 + 0x2274` — e `0x2274` = 8820 = 2205 palavras
        // ✓✓. A casa lia `size` como **bytes** e lia 4× menos.
        //
        // **Os tamanhos são limitados.** Um cabeçalho com um `size`
        // absurdo (o guest corrompido, ou um push buffer que ainda não é
        // o que pensamos) faria alocar gigabytes — e a memória do guest
        // é um `mmap` plano: ler fora dela é um fault. O limite é o
        // mesmo que o `drain_pushbuffer` da casa usa (4 MiB), em palavras.
        constexpr std::uint64_t kLimitePorEntrada = 4u * 1024u * 1024u;   // em palavras
        constexpr std::uint64_t kLimiteTotal = 16u * 1024u * 1024u;       // em palavras

        std::vector<std::uint8_t> pushbuffer;
        for (const auto& cab : cabecalhos) {
            const std::uint64_t palavras = cab.Tamanho();
            const std::uint64_t bytes = palavras * 4u;
            LOG_DEBUG(Service_NVDRV,
                      "SubmitGPFIFO: entrada addr=0x{:X} tamanho={} palavras ({} bytes) "
                      "push_buffer={} sync={}",
                      cab.Endereco(), palavras, bytes, cab.EhPushBuffer() ? 1 : 0,
                      cab.Sync() ? 1 : 0);
            if (palavras == 0)
                continue;
            if (palavras > kLimitePorEntrada ||
                pushbuffer.size() / 4 + palavras > kLimiteTotal) {
                // **Aviso, e não erro**: devolver `InvalidSize` ao guest
                // parte-o, e o guest tem de poder continuar.
                LOG_WARNING(Service_NVDRV,
                           "SubmitGPFIFO: entrada com tamanho {} palavras acima do limite",
                           palavras);
                continue;
            }
            const std::size_t inicio = pushbuffer.size();
            pushbuffer.resize(inicio + static_cast<std::size_t>(bytes), 0);
            if (!LerDoGuest(cab.Endereco(), pushbuffer.data() + inicio,
                            static_cast<std::size_t>(bytes))) {
                LOG_WARNING(Service_NVDRV, "SubmitGPFIFO: não li o comando em 0x{:X}",
                            cab.Endereco());
                RespostaErro(ctx, NvResult::InvalidAddress, "ioctl");
                return;
            }
        }

        // ── O instrumento do dump bruto (o ambiente: `G610_GPFIFO_DUMP`) ─
        //
        // **A medição do teste determinístico**: escreve o push buffer
        // completo de cada submit num arquivo próprio (o caminho dado no
        // ambiente, com o número da submissão preso ao fim), para o
        // teste reproduzir a ordenação real do jogo — nada inventado.
        // Sem a variável no ambiente, zero custo (só `getenv`).
        {
            const char* dump_base = std::getenv("G610_GPFIFO_DUMP");
            if (dump_base != nullptr && *dump_base != '\0' && !pushbuffer.empty()) {
                static std::atomic<std::uint32_t> submissoes_dump{0};
                const std::uint32_t seq = submissoes_dump.fetch_add(1);
                char caminho[512];
                std::snprintf(caminho, sizeof caminho, "%s.%06u.bin", dump_base,
                              static_cast<unsigned>(seq));
                if (std::FILE* f = std::fopen(caminho, "wb"); f != nullptr) {
                    const std::size_t gravado =
                        std::fwrite(pushbuffer.data(), 1, pushbuffer.size(), f);
                    std::fclose(f);
                    LOG_INFO(Service_NVDRV,
                             "SubmitGPFIFO: push buffer de {} bytes ({} gravados) em {}",
                             pushbuffer.size(), gravado, caminho);
                }
            }
        }

        // ── O fluxo de métodos ──────────────────────────────────────────
        //
        // **O dump descodificado**: as primeiras palavras do push buffer
        // lidas como comandos de host (o formato do upstream,
        // `dma_pusher.h:89-97`):
        //
        // método = bits [0, 13)
        // subcanal = bits [13, 16)
        // nº de args = bits [16, 29)
        // modo = bits [29, 32)
        //
        // Se o `modo` não for um dos quatro (0, 1, 2, 3), o
        // upstream **ignora** a palavra (`dma_pusher.cpp:189-191`) —
        // e é isso que o dump deixa ver.
        {
            char linha[256];
            std::size_t n = 0;
            for (std::size_t i = 0; i + 4 <= pushbuffer.size() && i < 32 * 4; i += 4) {
                std::uint32_t bruto = 0;
                std::memcpy(&bruto, pushbuffer.data() + i, 4);
                n += static_cast<std::size_t>(std::snprintf(
                    linha + n, sizeof(linha) - n,
                    "%s[%2zu] 0x%08x m=%u sc=%u args=%u modo=%u\n", n ? " " : "[gpfifo] dec ", i / 4,
                    bruto, bruto & 0x1FFFu, (bruto >> 13) & 0x7u, (bruto >> 16) & 0x1FFFu,
                    (bruto >> 29) & 0x7u));
                if (n + 2 >= sizeof(linha))
                    break;
            }
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // O push buffer **não** é uma lista de {tipo, endereço, tamanho}:
        // é um fluxo de métodos (o upstream: `dma_pusher.cpp`,
        // `ProcessCommands`). Cada método começa por um `CommandHeader`
        // e é seguido pelos argumentos.
        //
        // Os métodos do **canal** (o *puller* do upstream) são os que o
        // driver executa por conta própria: o bind dos subcanais, os
        // semáforos e a **fence**. E a fence é o que diz ao jogo "o
        // trabalho acabou" — sem ela, o jogo fica à espera para sempre
        // (foi o que se mediu).
        {
            gpu::PushBufferReader leitor;
            // O estado do canal (o `Puller::Regs` do upstream).
            std::uint64_t sem_endereco = 0;
            std::uint32_t sem_sequencia = 0;
            std::uint32_t fence_acao = 0;

            // A contagem por metodo: e' a unica forma de dizer o que esta
            // dentro do push buffer sem adivinhar. Os metodos do canal
            // (semaphore, fence) tem id < 0x40; os do motor 3D tem id
            // >= 0x40 (upstream, dma_pusher.cpp:204).
            std::map<std::uint32_t, std::uint32_t> metodos_vistos;
            // Os ids **por subcanal** — sem isto nao se sabe qual e'
            // o motor 3D.
            std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> por_subcanal;

            const std::size_t metodos = leitor.for_each(
                pushbuffer, [&](const gpu::MethodCall& m) {
                    ++metodos_vistos[m.method];
                    ++por_subcanal[m.subchannel][m.method];
                    // **O bind dos subcanais** — o metodo 0 do canal
                    // (`BindObject`) liga o subcanal a um motor.
                    if (m.method == gpu::canal::BindObject) {
                        LOG_DEBUG(Service_NVDRV,
                                  "SubmitGPFIFO: BindObject: subcanal={} motor=0x{:X}",
                                  m.subchannel, m.argument);
                    }
                    switch (m.method) {
                    case gpu::canal::SemaphoreAddressHigh:
                        sem_endereco = (sem_endereco & 0xFFFFFFFFull) |
                                       (static_cast<std::uint64_t>(m.argument) << 32);
                        break;
                    case gpu::canal::SemaphoreAddressLow:
                        sem_endereco = (sem_endereco & 0xFFFFFFFF00000000ull) | m.argument;
                        break;
                    case gpu::canal::SemaphoreSequence:
                        sem_sequencia = m.argument;
                        break;
                    case gpu::canal::SemaphoreTrigger: {
                        const std::uint32_t op = m.argument & 0xFu;
                        if (op == gpu::sem_op::WriteLong && sem_endereco != 0) {
                            // **O payload do semáforo.** O upstream
                            // escreve o valor na memória do guest e
                            // sinaliza o syncpoint — o guest vê o valor
                            // e prossegue.
                            const std::uint32_t payload = sem_sequencia;
                            if (EscreverNoGuest(sem_endereco, &payload, sizeof payload)) {
                                SvcDispatcher::AcordarFutexNoEndereco(sem_endereco);
                                LOG_DEBUG(Service_NVDRV,
                                          "SubmitGPFIFO: payload do semáforo escrito em "
                                          "0x{:X} ({} )",
                                          sem_endereco, payload);
                            }
                        }
                        break;
                    }
                    case gpu::canal::FenceValue:
                        // O valor da fence do canal.
                        break;
                    case gpu::canal::FenceAction: {
                        fence_acao = m.argument;
                        const std::uint32_t op = fence_acao & 0x1u;
                        const std::uint32_t syncpoint = (fence_acao >> 8) & 0xFFFFFFu;
                        if (op == gpu::fence_op::Increment) {
                            // **A fence foi incrementada** (o upstream:
                            // `rasterizer->SignalSyncPoint(id)`).
                            LOG_DEBUG(Service_NVDRV,
                                      "SubmitGPFIFO: fence incrementada (syncpoint {})",
                                      syncpoint);
                            SvcDispatcher::RegistarTrabalhoGpu();
                        }
                        break;
                    }
                    default:
                        break;
                    }
                });

            LOG_DEBUG(Service_NVDRV,
                      "SubmitGPFIFO: {} métodos no push buffer ({} descartados)", metodos,
                      leitor.descartados());

            // Os ids de metodo que apareceram, e quantas vezes. Os do canal
            // (id < 0x40) sao os que o puller executa; os do motor (>= 0x40)
            // sao os que o 3D executa.
            {
                std::string linha;
                char tmp[64];
                std::uint32_t max_id = 0;
                for (const auto& [id, n] : metodos_vistos) {
                    std::snprintf(tmp, sizeof tmp, "%s%u:%u", linha.empty() ? "" : " ", id, n);
                    linha += tmp;
                    if (id > max_id)
                        max_id = id;
                }
                LOG_DEBUG(Service_NVDRV, "SubmitGPFIFO: ids de método: {}", linha);
                LOG_DEBUG(Service_NVDRV,
                          "SubmitGPFIFO: max id={} (draw_begin=1414 draw_end=1413) total={}",
                          max_id, metodos_vistos.size());
                // **Os ids por subcanal** — o que o motor 3D recebeu.
                for (const auto& [sc, ids] : por_subcanal) {
                    std::string l;
                    char tmp2[32];
                    for (const auto& [id, n] : ids) {
                        std::snprintf(tmp2, sizeof tmp2, "%s%u:%u", l.empty() ? "" : " ", id, n);
                        l += tmp2;
                    }
                    LOG_DEBUG(Service_NVDRV, "SubmitGPFIFO: subcanal {}: {}", sc, l);
                }
            }
        }

        // O push buffer **cru**, em hexadecimal: é o que permite
        // descodificar o formato real (que **não** é o
        // {tipo, endereço, tamanho} que se supunha).
        {
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[pushbuffer]"));
            for (std::size_t i = 0; i < pushbuffer.size() && i < 64 && n + 4 < sizeof(linha);
                 ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", pushbuffer[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // E a tradução — que é o **tradutor** da casa.
        std::vector<g610::gpu::DrawCommand> comandos;
        std::string erro;
        const std::size_t n = tradutor_.translate(pushbuffer, comandos, erro);
        LOG_DEBUG(Service_NVDRV,
                  "canal: SubmitGPFIFO: {} bytes, {} comandos traduzidos "
                  "(desconhecidos {}, descartados {}, passos {} envios {}){}",
                  pushbuffer.size(), n, tradutor_.commands_unknown(),
                  tradutor_.commands_dropped_feature(), tradutor_.macro_passos(),
                  tradutor_.macro_envios(), erro.empty() ? "" : (" — " + erro));

        // Instrumento do passo 1: QUE metodos o microcode envia. Os
        // 901 envios medidos com 0 comandos so' decidem com os ids na
        // mao — os 16 mais frequentes, em hexa, com as vezes.
        {
            const auto& envios = tradutor_.envio_ids();
            if (!envios.empty()) {
                std::vector<std::pair<std::uint32_t, std::uint32_t>> ordem(
                    envios.begin(), envios.end());
                std::sort(ordem.begin(), ordem.end(),
                          [](const auto& a, const auto& b) {
                              if (a.second != b.second)
                                  return a.second > b.second;
                              return a.first < b.first;
                          });
                std::string linha2;
                char tmp3[64];
                for (std::size_t i = 0; i < ordem.size() && i < 16; ++i) {
                    std::snprintf(tmp3, sizeof tmp3, "%s0x%x x%u",
                                  linha2.empty() ? "" : " ", ordem[i].first,
                                  ordem[i].second);
                    linha2 += tmp3;
                }
                LOG_DEBUG(Service_NVDRV,
                          "SubmitGPFIFO: envios de macro: n={} ids: {}",
                          envios.size(), linha2);
            }
        }

        // ── O instrumento dos parámetros do blob (o ambiente:
        // `G610_VA_DUMP`) ────────────────────────────────────────────────
        //
        // As macros entregam endereços ao `falcon[4]` (a chamada de
        // firmware 4) e o tradutor os recolhe (`falcon_vas()`). Aqui
        // lê-se a memória do guest nesses VAs — **VA de GPU é o espaço
        // do guest**, como o bloco de leitura do push buffer acima
        // documenta — para medir o conteúdo que o blob de hardware
        // receberia. A decisão do desenho vem desta evidência, não de
        // código novo. Um arquivo texto por submit (a disciplina do
        // `G610_GPFIFO_DUMP`); sem a variável, zero custo.
        {
            const char* va_base = std::getenv("G610_VA_DUMP");
            if (va_base != nullptr && *va_base != '\0' && !pushbuffer.empty()) {
                static std::atomic<std::uint32_t> va_dump_seq{0};
                const std::uint32_t seq = va_dump_seq.fetch_add(1);
                char caminho[512];
                std::snprintf(caminho, sizeof caminho, "%s.%06u.txt", va_base,
                              static_cast<unsigned>(seq));
                // O tamanho da janela lida em cada VA distinto (o
                // ambiente `G610_VA_DUMP_TAM`, hexa ou decimal; o
                // "todo o VA": quanto maior, mais do blob se ve —
                // 0x1000 por VA revela a estrutura sem esgotar).
                static const std::size_t va_tam = [&] {
                    const char* t = std::getenv("G610_VA_DUMP_TAM");
                    const unsigned long long v =
                        t != nullptr ? std::strtoull(t, nullptr, 0) : 32ull;
                    return static_cast<std::size_t>(
                        v < 32 ? 32 : (v > 0x40000 ? 0x40000 : v));
                }();
                if (std::FILE* f = std::fopen(caminho, "w"); f != nullptr) {
                    // VAs distintos com a contagem de repeticoes (as 29
                    // chamadas do submit podem repetir ou nao endereco).
                    std::vector<std::uint32_t> vistos;
                    std::vector<std::uint32_t> conts;
                    for (const std::uint32_t va : tradutor_.falcon_vas()) {
                        std::size_t i = 0;
                        bool ja = false;
                        for (const std::uint32_t v : vistos) {
                            if (v == va) {
                                ja = true;
                                break;
                            }
                            ++i;
                        }
                        if (ja) {
                            ++conts[i];
                        } else {
                            vistos.push_back(va);
                            conts.push_back(1);
                        }
                        if (vistos.size() >= 256)
                            break;
                    }
                    // A mesa do espaco de enderecos do guest: a porta de
                    // traducao de um VA de falcon (fixed => a chave = o
                    // VA de GPU; o resto => a chave = o endereco do CPU). Sem ela, `falcon` entrega VAs que o
                    // guest nao ve.
                    std::fprintf(f,
                                 "submit=%u bytes=%zu chamadas_falcon=%zu "
                                 "vas_distintos=%zu janela=%zu as=%zu\n",
                                 seq, pushbuffer.size(),
                                 tradutor_.falcon_vas().size(), vistos.size(),
                                 va_tam, mapeamentos_.size());
                    for (const auto& [chave, mapa] : mapeamentos_)
                        std::fprintf(f,
                                     "as chave=0x%llX ptr=0x%llX tam=0x%llX fixo=%u\n",
                                     static_cast<unsigned long long>(chave),
                                     static_cast<unsigned long long>(mapa.ptr),
                                     static_cast<unsigned long long>(mapa.size),
                                     mapa.fixed ? 1u : 0u);
                    std::vector<std::uint8_t> janela(va_tam, 0);
                    for (std::size_t q = 0; q < vistos.size(); ++q) {
                        const std::uint32_t va = vistos[q];
                        // A tradução do VA: a única entrada que o cobre —
                        // a mesma forma para os dois cenas (o ptr +
                        // (va - chave) vira o endereco do CPU).
                        auto cu = mapeamentos_.upper_bound(va);
                        bool coberto = false;
                        std::uint64_t cpu = va;
                        if (cu != mapeamentos_.begin()) {
                            --cu;
                            if ((va - cu->first) < cu->second.size &&
                                cu->second.size != 0) {
                                coberto = true;
                                cpu = cu->second.ptr + (va - cu->first);
                            }
                        }
                        // As entradas por cima/por baixo (o diagnóstico
                        // de um VA sem cobertura).
                        if (!coberto) {
                            auto al = mapeamentos_.lower_bound(va);
                            if (al != mapeamentos_.end())
                                std::fprintf(f, " acima=0x%llX",
                                             static_cast<unsigned long long>(al->first));
                            if (al != mapeamentos_.begin()) {
                                auto ba = std::prev(al);
                                std::fprintf(f, " abaixo=0x%llX",
                                             static_cast<unsigned long long>(ba->first));
                            }
                        }
                        // A janela adaptativa: a inteira, e se o piso
                        // do guest não tiver a pagina toda, encolhe (o
                        // buraco do mapa nao para a medida).
                        std::size_t ok_tam = 0;
                        for (std::size_t t = janela.size();;) {
                            if (LerDoGuest(cpu, janela.data(), t)) {
                                ok_tam = t;
                                break;
                            }
                            if (t <= 16) {
                                if (LerDoGuest(cpu, janela.data(), 16)) {
                                    ok_tam = 16;
                                }
                                break;
                            }
                            t /= 4;
                        }
                        std::fprintf(f, "va=0x%08X reps=%u %s cpu=0x%llX ok_tam=%zu",
                                     va, conts[q], coberto ? "coberto" : "CRU",
                                     static_cast<unsigned long long>(cpu),
                                     ok_tam);
                        for (std::size_t j = 0; j < 16 && j < janela.size();
                             j += 4) {
                            std::uint32_t d = 0;
                            std::memcpy(&d, janela.data() + j, 4);
                            std::fprintf(f, " d%zu=%08X", j / 4, d);
                        }
                        std::fprintf(f, "\n");
                        if (ok_tam == 0)
                            continue;
                        // O hexdump da janela lida (16 em 16, com a
                        // coluna de ascii).
                        for (std::size_t off = 0; off < ok_tam; off += 16) {
                            std::fprintf(f, " %06zX", off);
                            char ascii[17] = {};
                            for (std::size_t j = 0;
                                 j < 16 && off + j < ok_tam; ++j) {
                                const unsigned c = janela[off + j];
                                if (j % 4 == 0)
                                    std::fprintf(f, " ");
                                std::fprintf(f, "%02X", c);
                                ascii[j] = (c >= 0x20 && c < 0x7F)
                                               ? static_cast<char>(c)
                                               : '.';
                            }
                            std::fprintf(f, " |%s|\n", ascii);
                        }
                    }
                    std::fclose(f);
                }
            }
        }

        // **A GPU terminou.** O driver tem de acordar o guest: medido, o
        // jogo fica à espera de uma *condition variable* que ninguém
        // sinaliza — e o `SignalProcessWideKey` do próprio jogo não
        // resolve, porque o que o jogo espera aqui é o driver.
        // **Nada de "acordar todos" aqui.** A primeira versão desta
        // chamada (a `DespertarCvWaitersDoGuest`) **esvaziava as filas
        // das cv** — emedia-se a inscrição dos *waiters* do jogo, que
        // depois já não eram encontrados pelo signal (medido: o worker
        // do jogo ficou à espera para sempre). O sinal da fence é do
        // *waiter* que a espera, não um varredor de filas.

        // A fence: o `id` é o syncpoint do canal e o `valor` incrementa
        // (a regra do Eden, `SubmitGPFIFOImpl`).
        //
        // **O upstream** (`nvhost_gpu.cpp:367-372`):
        //
        //     params.fence.id = channel_syncpoint;
        //     u32 increment{(flags.fence_increment.Value() != 0 ? 2 : 0) +
        //                   (flags.increment_value.Value() != 0 ? params.fence.value : 0)};
        //     params.fence.value =
        //         syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint, increment);
        //
        // **O incremento pode ser zero** — e nesse caso o syncpoint **nao
        // sobe**. A casa **incrementava sempre** — o que faz o
        // guest ver um valor de fence que o Eden nunca devolveria.
        //
        // E o `fence.value` de saída é o valor **depois** do incremento —
        // o novo valor do syncpoint.
        constexpr std::uint32_t kFenceWait = 1u << 0;
        constexpr std::uint32_t kFenceIncrement = 1u << 1;
        constexpr std::uint32_t kIncrementValue = 1u << 8;
        if (auto* canal = CanalDoFd()) {
            std::uint32_t incremento = 0;
            if ((params.flags & kFenceIncrement) != 0)
                incremento += 2;
            if ((params.flags & kIncrementValue) != 0)
                incremento += params.fence_value;
            if (std::getenv("IA_IPC_TRACE") != nullptr) {
                std::fprintf(stderr,
                             "[nvdrv] SubmitGPFIFO: flags=0x%x fence de entrada=(%d,%u) "
                             "incremento=%u\n",
                             params.flags, params.fence_id, params.fence_value, incremento);
            }
            canal->submissoes += 1;

            // **O syncpoint avanca** — e e' isto que cumpre a espera do
            // guest.
            //
            // O upstream (`nvhost_gpu.cpp`, `SubmitGPFIFOImpl`) faz
            // `syncpoint_manager.IncrementSyncpointMaxExt(channel_syncpoint,
            // increment)` — o syncpoint sobe **quando a submissao e'
            // aceite**. O `EventWait` do `/dev/nvhost-ctrl` ve o
            // valor novo e o guest prossegue.
            //
            // **Aqui nao ha thread de GPU** — o trabalho e' "executado"
            // agora — por isso o syncpoint sobe aqui. O guest ve
            // exactamente o mesmo.
            if (canal->fence_id >= 0) {
                const std::uint32_t id = static_cast<std::uint32_t>(canal->fence_id);
                // **O incremento zero deixa o syncpoint quieto** — e o
                // "MaxExt" do upstream significa que o valor nunca
                // **desce**.
                if (incremento != 0) {
                    syncpoints_.Incrementar(id, incremento);
                }
                canal->fence_valor = syncpoints_.Valor(id);
                LOG_DEBUG(Service_NVDRV,
                          "canal: syncpoint {} Advanced para {} ({} submissões)",
                          canal->fence_id, canal->fence_valor, canal->submissoes);
            }

            // A fence de saída — o par (syncpoint, valor **actual**).
            params.fence_id = canal->fence_id;
            params.fence_value = canal->fence_valor;
        }
        // As bandeiras saem a zero (o Eden faz o mesmo).
        params.flags = 0;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x47/0x14 e 0x47/0x15: `SetClientData` e `GetClientData` — os
    // dados do cliente que o canal guarda.
    //
    // **O grupo `'G'` também é do canal.** O Eden (`nvhost_gpu.cpp`,
    // `Ioctl1`) tem três grupos no `nvhost_gpu`: 0x00, `'H'` e `'G'` — e
    // o `'G'` do canal (`SetClientData`/`GetClientData`) **não** é o `'G'`
    // do `nvhost-ctrl-gpu`. É por isso que o despacho é por device: o
    // mesmo grupo, comandos diferentes.
    void SetClientData(HLERequestContext& ctx) {
        IoctlClientData params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: SetClientData: 0x{:X}", params.data);
        if (auto* canal = CanalDoFd())
            canal->dados_cliente = params.data;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    void GetClientData(HLERequestContext& ctx) {
        IoctlClientData params{};
        LerEntrada(ctx, params);

        if (auto* canal = CanalDoFd())
            params.data = canal->dados_cliente;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // 0x48/0x1d: `ChannelSetTimeslice` — a fatia de tempo do canal.
    //
    // O Eden (`nvhost_gpu.cpp`, `ChannelSetTimeslice`): o valor tem de
    // estar entre 1000 e 5000; o resto é `BadParameter`.
    void ChannelSetTimeslice(HLERequestContext& ctx) {
        IoctlSetTimeslice params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV, "canal: ChannelSetTimeslice: {}", params.timeslice);
        if (params.timeslice < 1000 || params.timeslice > 5000) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }
        if (auto* canal = CanalDoFd())
            canal->fatia_de_tempo = params.timeslice;

        EscreverSaida(ctx, params);
        RespostaOk(ctx);
    }

    // Despeja `n` bytes de um endereço do guest, em hexadecimal.
    //
    // É o instrumento de medição do caminho da GPU: sem ver os bytes, o
    // formato é uma suposição — e a medição de `boot_p7s.log` provou que
    // a suposição estava errada.
    void Despejar(std::uint64_t endereco, std::size_t n, const char* etiqueta) {
        if (endereco == 0)
            return;
        std::vector<std::uint8_t> conteudo(n, 0);
        if (!LerDoGuest(endereco, conteudo.data(), conteudo.size()))
            return;
        char linha[512];
        std::size_t p = 0;
        p += static_cast<std::size_t>(
            std::snprintf(linha + p, sizeof(linha) - p, "[%s] 0x%llX:", etiqueta,
                          (unsigned long long)endereco));
        for (std::size_t i = 0; i < conteudo.size() && p + 4 < sizeof(linha); ++i)
            p += static_cast<std::size_t>(
                std::snprintf(linha + p, sizeof(linha) - p, " %02x", conteudo[i]));
        if (p + 2 < sizeof(linha))
            p += static_cast<std::size_t>(std::snprintf(linha + p, sizeof(linha) - p, "\n"));
        const ssize_t w = ::write(2, linha, p);
        (void)w;
    }

    // ─────────────────────────────────────────────────────────────────────
    // O `/dev/nvhost-ctrl` — o device da espera pela fence.
    //
    // **E' o device onde o guest espera que a GPU acabe**. O
    // `SubmitGPFIFO` devolve o par (syncpoint, valor) e o guest vem aqui
    // (`IocCtrlEventWait`) esperar que aquele syncpoint chegue a esse
    // valor.
    //
    // A tabela e' a do upstream (`nvhost_ctrl.cpp:41-57`) — o **grupo
    // 0x00**, que e' diferente do `'G'` do `/dev/nvhost-ctrl-gpu` e
    // do `0x00` do `/dev/nvhost-gpu` (o `GetWaitbase`).
    //
    // ─────────────────────────────────────────────────────────────────────

    // 0x00/0x1f: `IocCtrlEventRegister` — o guest regista um evento NV.
    //
    // O upstream (`nvhost_ctrl.cpp`) cria um `KEvent` do kernel e
    // guarda-o; o guest depois espera nele com o `WaitSynchronization`
    //.
    void CtrlEventRegister(HLERequestContext& ctx) {
        std::uint32_t event_id = 0;
        LerEntrada(ctx, event_id);

        LOG_DEBUG(Service_NVDRV, "CtrlEventRegister: event_id={}", event_id);

        if (event_id >= g610::gpu::kMaxNvEvents) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        // Se ja' estava registado, o upstream liberta-o primeiro
        // (`FreeEvent`) — e se estiver em uso, isso e' um erro.
        auto& evento = eventos_nv_[event_id];
        if (evento.registado) {
            if (evento.em_uso) {
                RespostaErro(ctx, NvResult::Busy, "ioctl");
                return;
            }
            LibertarEventoNv(event_id);
        }

        // O `KEvent` — como o do `nvdrv` (o `QueryEvent`).
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        evento.kevent = new Kernel::KEvent(kernel);
        evento.kevent->Initialize(kernel, &processo);
        evento.registado = true;
        evento.em_uso = false;
        evento.fd = fd_em_curso_;

        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    // 0x00/0x20: `IocCtrlEventUnregister` — o guest liberta um evento.
    void CtrlEventUnregister(HLERequestContext& ctx) {
        std::uint32_t event_id = 0;
        LerEntrada(ctx, event_id);
        event_id &= 0xFFu;

        LOG_DEBUG(Service_NVDRV, "CtrlEventUnregister: event_id={}", event_id);

        if (event_id >= g610::gpu::kMaxNvEvents) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        auto& evento = eventos_nv_[event_id];
        if (!evento.registado) {
            RespostaErro(ctx, NvResult::Success, "ioctl");
            return;
        }
        if (evento.em_uso) {
            RespostaErro(ctx, NvResult::Busy, "ioctl");
            return;
        }

        LibertarEventoNv(event_id);

        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    // 0x00/0x21: `IocCtrlEventUnregisterBatch` — liberta varios.
    void CtrlEventUnregisterBatch(HLERequestContext& ctx) {
        std::uint64_t mascara = 0;
        LerEntrada(ctx, mascara);

        LOG_DEBUG(Service_NVDRV, "CtrlEventUnregisterBatch: mascara=0x{:X}", mascara);

        NvResult resultado = NvResult::Success;
        while (mascara != 0) {
            const std::uint32_t event_id =
                static_cast<std::uint32_t>(__builtin_ctzll(mascara));
            mascara &= ~(1ULL << event_id);

            auto& evento = eventos_nv_[event_id];
            if (!evento.registado)
                continue;
            if (evento.em_uso) {
                resultado = NvResult::Busy;
                continue;
            }
            LibertarEventoNv(event_id);
        }

        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(resultado);
    }

    // 0x00/0x1c: `IocCtrlClearEventWait` — o guest larga a espera.
    void CtrlClearEventWait(HLERequestContext& ctx) {
        std::uint32_t event_id = 0;
        LerEntrada(ctx, event_id);

        LOG_DEBUG(Service_NVDRV, "CtrlClearEventWait: event_id=0x{:X}", event_id);

        const std::uint32_t slot = event_id & 0xFFu;
        if (slot < g610::gpu::kMaxNvEvents) {
            auto& evento = eventos_nv_[slot];
            if (evento.em_uso) {
                // O upstream (`IocCtrlClearEventWait`) cancela a accao
                // registada e limpa o evento.
                syncpoints_.Remover(evento.syncpoint, evento.valor);
                evento.em_uso = false;
                evento.syncpoint = 0;
                evento.valor = 0;
            }
        }

        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    // 0x00/0x1d e 0x00/0x1e: `IocCtrlEventWait` — a espera.
    //
    // **A alocacao** (0x1d) cria o evento e devolve o par
    // (syncpoint, slot); **a espera** (0x1e) regista a accao no
    // syncpoint.
    //
    // A semantica do upstream (`nvhost_ctrl.cpp`):
    //
    // - se o valor pedido **ja** foi alcancado, devolve sucesso e
    //     o valor actual;
    // - se nao, regista a accao e devolve `Timeout` — que
    //     significa "ainda nao, mas ja' estou a ver".
    void CtrlEventWait(HLERequestContext& ctx, bool alocacao) {
        struct IocCtrlEventWaitParams {
            std::int32_t fence_id;
            std::uint32_t fence_value;
            std::uint32_t timeout;
            std::uint32_t value;
        };
        static_assert(sizeof(IocCtrlEventWaitParams) == 16, "tamanho errado");

        IocCtrlEventWaitParams params{};
        LerEntrada(ctx, params);

        LOG_DEBUG(Service_NVDRV,
                  "CtrlEventWait: syncpt_id={} threshold={} timeout={} alocacao={}",
                  params.fence_id, params.fence_value, params.timeout, alocacao ? 1 : 0);

        // O upstream: `fence_id` fora do intervalo e' `BadParameter`.
        if (params.fence_id < 0 ||
            static_cast<std::uint32_t>(params.fence_id) >= g610::gpu::kMaxSyncpoints) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }
        const std::uint32_t fence_id = static_cast<std::uint32_t>(params.fence_id);

        // **Valor zero**: o guest so' quer saber o valor actual (o
        // upstream devolve o minimo do syncpoint).
        if (params.fence_value == 0) {
            params.value = syncpoints_.Valor(fence_id);
            EscreverSaida(ctx, params);
            RespostaErro(ctx, NvResult::Success, "ioctl");
            return;
        }

        // **Ja' cumprido** — o upstream devolve o valor actual.
        if (syncpoints_.EstaSinalizado(fence_id, params.fence_value)) {
            params.value = syncpoints_.Valor(fence_id);
            EscreverSaida(ctx, params);
            RespostaErro(ctx, NvResult::Success, "ioctl");
            return;
        }

        // O slot do evento.
        std::uint32_t slot = params.value & 0xFFu;
        if (alocacao) {
            slot = EventoNvLivre(fence_id);
            if (slot >= g610::gpu::kMaxNvEvents) {
                RespostaErro(ctx, NvResult::BadParameter, "ioctl");
                return;
            }
        } else if (slot >= g610::gpu::kMaxNvEvents) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        auto& evento = eventos_nv_[slot];
        if (!evento.registado) {
            // O upstream: um evento nao registado e' `BadParameter`.
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }
        if (evento.em_uso) {
            RespostaErro(ctx, NvResult::BadParameter, "ioctl");
            return;
        }

        // A accao: sinalizar o `KEvent` quando o syncpoint chegar ao
        // valor.
        Kernel::KEvent* kevent = evento.kevent;
        const std::uint32_t valor = params.fence_value;
        const bool registado = syncpoints_.Registar(
            fence_id, valor, [this, kevent, slot]() { SinalizarEventoNv(slot, kevent); });

        if (!registado) {
            // Chegou entretanto — sinaliza ja'.
            SinalizarEventoNv(slot, kevent);
            params.value = syncpoints_.Valor(fence_id);
            EscreverSaida(ctx, params);
            RespostaErro(ctx, NvResult::Success, "ioctl");
            return;
        }

        evento.syncpoint = fence_id;
        evento.valor = valor;
        evento.em_uso = true;

        // O valor de saida: o syncpoint e o slot.
        //
        // O upstream (`nvhost_ctrl.cpp`) na alocacao devolve o
        // syncpoint e o bit de "alocado"; na espera devolve o syncpoint
        // e o slot.
        params.value = fence_id;
        if (alocacao) {
            params.value |= (slot & 0xFu);
            params.value |= (1u << 12);  // o bit de "evento alocado"
        } else {
            params.value |= slot;
        }
        EscreverSaida(ctx, params);

        // `Timeout` — o upstream usa-o para dizer "ainda nao, mas ja'
        // estou a ver". **Nao e' um erro** — e' o que o guest
        // espera.
        RespostaErro(ctx, NvResult::Timeout, "ioctl");
    }

    // O slot de evento livre — o `FindFreeNvEvent` do upstream.
    std::uint32_t EventoNvLivre(std::uint32_t syncpoint) {
        std::uint32_t slot_livre = g610::gpu::kMaxNvEvents;
        std::uint32_t melhor = g610::gpu::kMaxNvEvents;
        for (std::uint32_t i = 0; i < g610::gpu::kMaxNvEvents; ++i) {
            auto& evento = eventos_nv_[i];
            if (!evento.registado) {
                if (slot_livre == g610::gpu::kMaxNvEvents)
                    slot_livre = i;
                continue;
            }
            if (evento.em_uso)
                continue;
            // O do mesmo syncpoint e' o melhor (o upstream).
            if (evento.syncpoint == syncpoint)
                melhor = i;
        }
        return melhor != g610::gpu::kMaxNvEvents ? melhor : slot_livre;
    }

    // Sinaliza o `KEvent` de um evento NV.
    void SinalizarEventoNv(std::uint32_t slot, Kernel::KEvent* kevent) {
        if (!kevent)
            return;
        auto& kernel = system.Kernel();
        kevent->Signal(kernel);
        auto& evento = eventos_nv_[slot];
        evento.em_uso = false;
        LOG_DEBUG(Service_NVDRV, "CtrlEventWait: evento {} sinalizado", slot);
    }

    // Liberta um evento NV — o `FreeNvEvent` do upstream.
    void LibertarEventoNv(std::uint32_t event_id) {
        auto& evento = eventos_nv_[event_id];
        if (evento.kevent) {
            auto& kernel = system.Kernel();
            evento.kevent->Finalize(kernel);
            delete evento.kevent;
            evento.kevent = nullptr;
        }
        evento.registado = false;
        evento.em_uso = false;
        evento.syncpoint = 0;
        evento.valor = 0;
    }

    // O canal do device de um ioctl (o `fd` do comando).
    Canal* CanalDoFd() {
        const DeviceFD fd = fd_em_curso_;
        const auto it = canais_.find(fd);
        return it == canais_.end() ? nullptr : &it->second;
    }

    // Ler memória do **guest**.
    //
    // O VA da GPU é o espaço do guest (ver a nota do `VmDaGpu`), por isso
    // ler o push buffer é ler a memória do guest nesse endereço. É a
    // `Memory` do processo — a mesma que o `WriteToOutgoingCommandBuffer`
    // do Eden usa para escrever a resposta.
    bool LerDoGuest(std::uint64_t endereco, void* destino, std::size_t tamanho) {
        auto& memoria = Kernel::GetCurrentMemory(system.Kernel());
        return memoria.ReadBlock(static_cast<Common::ProcessAddress>(endereco), destino, tamanho);
    }

    // O espelho de `LerDoGuest`: escrever na memoria do guest.
    //
    // E' o que o driver faz com o **payload do semaforo** (o upstream:
    // `puller.cpp`, `ProcessSemaphoreTriggerMethod` — o
    // `rasterizer->Query(..., Payload, ...)` escreve o valor na memoria
    // do guest). E' por ai que o jogo sabe que o trabalho da GPU
    // acabou.
    bool EscreverNoGuest(std::uint64_t endereco, const void* origem, std::size_t tamanho) {
        auto& memoria = Kernel::GetCurrentMemory(system.Kernel());
        return memoria.WriteBlock(static_cast<Common::ProcessAddress>(endereco), origem, tamanho);
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
            RespostaErro(ctx, NvResult::NotInitialized, "ioctl");
            return;
        }

        // O **tipo** do device de que o ioctl fala, e o canal (se for
        // um canal).
        const auto tipo_it = tipos_.find(fd);
        const TipoDeDevice tipo = tipo_it == tipos_.end() ? TipoDeDevice::Desconhecido
                                                         : tipo_it->second;
        fd_em_curso_ = fd;
        if (tipo == TipoDeDevice::Desconhecido) {
            LOG_WARNING(Service_NVDRV, "Ioctl: fd={} não é um device aberto", fd);
            RespostaErro(ctx, NvResult::BadValue, "ioctl");
            return;
        }

        // O despacho por **(device, grupo, comando)** — o do Eden.
        //
        // **O device importa, não só o grupo.** O grupo 0x00 tem comandos
        // nos dois devices: no `nvhost-ctrl` é o
        // `IocCtrlEventUnregisterBatch` (0x21) e no `nvhost_gpu` é o
        // `GetWaitbase` (0x03). Despachar só pelo grupo — como a versão
        // anterior fazia — dá o comando ao device errado.
        //
        // A tabela de cada device é a do upstream:
        // `nvhost_ctrl_gpu.cpp`, `nvhost_as_gpu.cpp`, `nvhost_gpu.cpp`,
        // `nvmap.cpp`.
        switch (tipo) {
        case TipoDeDevice::Controle:
            // O `/dev/nvhost-ctrl` — o device da **espera pela fence**.
            //
            // O **grupo 0x00** (o upstream, `nvhost_ctrl.cpp:41-57`) —
            // que e' diferente do `'G'` do `/dev/nvhost-ctrl-gpu`.
            if (comando.group() == 0x00) {
                switch (comando.cmd()) {
                case 0x1c:
                    CtrlClearEventWait(ctx);
                    return;
                case 0x1d:
                    CtrlEventWait(ctx, true);
                    return;
                case 0x1e:
                    CtrlEventWait(ctx, false);
                    return;
                case 0x1f:
                    CtrlEventRegister(ctx);
                    return;
                case 0x20:
                    CtrlEventUnregister(ctx);
                    return;
                case 0x21:
                    CtrlEventUnregisterBatch(ctx);
                    return;
                default:
                    break;
                }
            }
            break;
        case TipoDeDevice::ControleGpu:
            if (comando.group() == 'G') {
                switch (comando.cmd()) {
                case 0x1:
                    ZCullGetCtxSize(ctx);
                    return;
                case 0x2:
                    ZCullGetInfo(ctx);
                    return;
                case 0x3:
                    ZBCSetTable(ctx);
                    return;
                case 0x4:
                    ZBCQueryTable(ctx);
                    return;
                case 0x5:
                    GetCharacteristics1(ctx);
                    return;
                case 0x6:
                    GetTPCMasks1(ctx);
                    return;
                case 0x14:
                    GetActiveSlotMask(ctx);
                    return;
                case 0x1c:
                    GetGpuTime(ctx);
                    return;
                default:
                    break;
                }
            }
            break;
        case TipoDeDevice::AsGpu:
            if (comando.group() == 'A') {
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
            }
            break;
        case TipoDeDevice::Gpu:
            // O **canal**: o device por onde o trabalho chega à GPU.
            if (comando.group() == 0x00) {
                switch (comando.cmd()) {
                case 0x3:
                    GetWaitbase(ctx);
                    return;
                default:
                    break;
                }
            } else if (comando.group() == 'H') {
                switch (comando.cmd()) {
                case 0x1:
                    SetNVMAPfd(ctx);
                    return;
                case 0x3:
                    ChannelSetTimeout(ctx);
                    return;
                case 0x1d:
                    ChannelSetTimeslice(ctx);
                    return;
                case 0x8:
                    SubmitGPFIFOBase1(ctx);
                    return;
                case 0x9:
                    AllocateObjectContext(ctx);
                    return;
                case 0xb:
                    ZCullBind(ctx);
                    return;
                case 0xc:
                    SetErrorNotifier(ctx);
                    return;
                case 0xd:
                    SetChannelPriority(ctx);
                    return;
                case 0x18:
                    AllocGPFIFOEx(ctx);
                    return;
                case 0x1b:
                    SubmitGPFIFOKickoff(ctx);
                    return;
                case 0x1a:
                    AllocGPFIFOEx(ctx);
                    return;
                default:
                    break;
                }
            } else if (comando.group() == 'G') {
                // O `'G'` **do canal** (ver a nota do `SetClientData`).
                switch (comando.cmd()) {
                case 0x14:
                    SetClientData(ctx);
                    return;
                case 0x15:
                    GetClientData(ctx);
                    return;
                default:
                    break;
                }
            }
            break;
        case TipoDeDevice::Nvmap:
            if (comando.group() == 0x01) {
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
            break;
        default:
            break;
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

        RespostaErro(ctx, NvResult::NotImplemented, "ioctl");
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

        RespostaErro(ctx, NvResult::Success, "ioctl");
    }

    bool initialized_ = false;
    DeviceFD proximo_fd_ = 1;
    std::map<DeviceFD, std::string> dispositivos_;
    // O **tipo** de cada device (pelo nome) e o estado do **canal** de
    // cada `/dev/nvhost-gpu`.
    std::map<DeviceFD, TipoDeDevice> tipos_;
    std::map<DeviceFD, Canal> canais_;

    // ── O `/dev/nvhost-ctrl` e os syncpoints ───────────────────────────
    //
    // **Os syncpoints** — o objecto que o guest espera. A semantica e'
    // a do upstream (`video_core/host1x/syncpoint_manager.h`): cada
    // syncpoint tem um valor monotonico e um conjunto de accoes que
    // disparam quando o valor chega ao esperado.
    //
    // **A diferenca**: o upstream avanca o syncpoint numa thread de GPU
    //; aqui avanca **no `SubmitGPFIFO`** — que e' quando o trabalho
    // e' "executado". O guest ve o mesmo.
    g610::gpu::SyncpointManager syncpoints_;

    // **Os eventos NV** — o `nvhost_ctrl` tem 64 (o upstream:
    // `MaxNvEvents = 64`). Cada um e' um `KEvent` do kernel que o
    // guest pode esperar (`WaitSynchronization`).
    //
    // O `KEvent` e' criado pelo `IocCtrlEventRegister` (0x1f) e
    // destruido no `Unregister` (0x20) ou no `UnregisterBatch`
    // (0x21) — como no upstream.
    struct EventoNv {
        Kernel::KEvent* kevent = nullptr;
        // O syncpoint que este evento espera e o valor.
        std::uint32_t syncpoint = 0;
        std::uint32_t valor = 0;
        // O device a que pertence — o `QueryEvent` do upstream
        // (`nvdrv.cpp:211-229`) procura o device pelo fd.
        DeviceFD fd = -1;
        // Registado (o guest pediu-o) e a ser usado.
        bool registado = false;
        bool em_uso = false;
    };
    std::map<std::uint32_t, EventoNv> eventos_nv_;

    // O device do ioctl em curso (o `fd` do comando) — o canal que o
    // ioctl diz respeito.
    DeviceFD fd_em_curso_ = -1;
    // O **tradutor** Maxwell -> Vulkan da casa (`src/gpu/`). É ele que
    // consome o push buffer que o guest submete.
    g610::gpu::MaxwellTranslator tradutor_{g610::tel::Telemetria()};
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

    // A resposta de um ioctl que correu **mal** — e o registo de
    // qual foi.
    //
    // **Um `NvResult` diferente de `Success` mata o jogo**: o SDK
    // trata-o como motivo para abortar (e o `SetTerminateResult` que
    // aparece no log e' o pranto).
    static void RespostaErro(HLERequestContext& ctx, NvResult resultado,
                             const char* onde) {
        if (resultado != NvResult::Success && std::getenv("IA_IPC_TRACE") != nullptr) {
            std::fprintf(stderr, "[nvdrv] ERRO %s: NvResult=%u\n", onde,
                         static_cast<unsigned>(resultado));
        }
        ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.PushEnum(resultado);
    }

    // A resposta de um ioctl que correu bem: `Result` + `NvResult`.
    static void RespostaOk(HLERequestContext& ctx) {
        RespostaErro(ctx, NvResult::Success, "ioctl");
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
