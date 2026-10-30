// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O `vi` — o serviço de **vídeo** (o display) — **nosso**, fino.
//
// ## O que é
//
// O `vi` é o serviço que dá ao jogo a **tela**: as camadas (*layers*),
// os *stacks*, a resolução, o vsync. O SDK pede-o ao `sm:` no arranque
// (o nome que o jogo pediu foi **"vi:m"** — o serviço do *manager*).
//
// São três serviços, e o Eden regista-os com três nomes
// (`core/hle/service/vi/vi.cpp`, `LoopProcess`):
//
//   - "vi:m" — o **manager** (o lado do sistema);
//   - "vi:s" — o **sistema**;
//   - "vi:u" — a **aplicação**.
//
// Cada um devolve, com um `GetDisplayService`, o seu display — e cada
// display tem a **sua** tabela de comandos.
//
// ## Porque este ficheiro existe
//
// **Medido** (`boot_p7d.log`): o guest pede `GetService("vi:m")` e, como
// o serviço não está registado, o `sm:` **defere** o pedido — e o
// deferimento, no motor nativo, responde com **0 palavras**, o que mata o
// guest (o reader CMIF aborta com `modulo=10 desc=212`).
//
// ## A forma é do Eden, à letra
//
// As tabelas de comandos são as do upstream
// (`core/hle/service/vi/manager_root_service.cpp`,
// `system_root_service.cpp`, `application_root_service.cpp`, e os
// `*_display_service.cpp` correspondentes), com os **IDs** e os **nomes**.
//
// ## O conteúdo é o mínimo
//
// O display do Eden (`container.cpp`, `nvnflinger/`) gere as camadas
// reais e o *compositor*. Aqui o que há é a **interface**: os comandos
// existem, respondem com a forma certa, e os **identificadores** que o
// guest precisa (o id do display, o id da camada) são reais.
//
// **A tela em si é a da casa** — o caminho de render (Wayland/SDL2 com o
// panvk) — e não o do Eden. Ligar os dois é o passo seguinte.
//
// SPDX-FileCopyrightText: 2018 Yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vendor/eden/common/logging.h"
#include "vendor/eden/core/system_ref.h"  // o `Core::System` nosso
#include "vendor/eden/core/hle/kernel/k_event.h"
#include "vendor/eden/core/hle/kernel/k_process_bridge.h"
#include "vendor/eden/core/hle/service/ipc_helpers.h"
#include "vendor/eden/core/hle/service/server_manager.h"
#include "vendor/eden/core/hle/service/service.h"

#include "g610/services/binder.hpp"
#include "g610/native_engine.hpp"  // g610::natural_boot — o gate dos band-aids
#include "g610/svc_dispatcher.hpp"

#include "g610/gpu/vulkan_backend.hpp"
#include "g610/gpu/shader_spv.hpp"

#include <cstring>

namespace g610::services {

namespace {

using Service::HLERequestContext;

using IPC::RequestParser;
using IPC::ResponseBuilder;

// ─────────────────────────────────────────────────────────────────────────────
// O estado do display.
//
// Os **identificadores** que o guest recebe (o id do display, o id da
// camada) são reais e estáveis — o guest usa-os nos comandos seguintes,
// e um id que mudasse a cada chamada seria um id inútil.
// ─────────────────────────────────────────────────────────────────────────────
struct EstadoDoDisplay {
    std::uint64_t proximo_display = 1;
    std::uint64_t proxima_camada = 1;
    // A resolução: a do Switch (1280x720). É o que o Eden devolve
    // para o display interno.
    std::int64_t largura = 1280;
    std::int64_t altura = 720;
    // O **binder** que o jogo recebe: um identificador por camada. É
    // por ele que o jogo passa os seus buffers ao compositor.
    std::uint64_t proximo_binder = 1;
    // A **janela** de cada camada: o buffer onde o jogo desenha.
    struct Janela {
        std::uint64_t endereco = 0;
        std::uint64_t tamanho = 0;
    };
    std::map<std::uint64_t, Janela> janelas;
};

// O estado é partilhado pelos três displays (eles falam do **mesmo**
// display) — como no Eden, onde o `Container` é comum aos três.
EstadoDoDisplay& Estado() {
    static EstadoDoDisplay e;
    return e;
}

// Endereço da janela (native window) alocada por OpenLayer — acessível
// do thread de vsync para escrita de pixels.
std::uint64_t g_janela_vi = 0;

// ─────────────────────────────────────────────────────────────────────────────
// O **vsync**.
//
// O evento de vsync (`GetDisplayVsyncEvent`, 5202) existia, mas **ninguém
// o sinalizava** — e o jogo bloqueia à espera dele.
//
// Um display real emite vsync a 60 Hz. É isso que se faz aqui: uma
// thread que sinaliza o evento a cada 16,67 ms.
//
// **O sinal é no `KReadableEvent`** — o filho, que é o objecto que o
// guest recebe (o mesmo que o `applet_oe` faz, e pelo mesmo motivo:
// o `Clear` não propaga do pai para o filho).
// ─────────────────────────────────────────────────────────────────────────────
std::mutex g_mu_vsync;
Kernel::KReadableEvent* g_evento_vsync = nullptr;
std::atomic<bool> g_vsync_corre{false};

void ThreadDoVsync(Core::System& system) {
    while (g_vsync_corre.load(std::memory_order_relaxed)) {
        Kernel::KReadableEvent* evento = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu_vsync);
            evento = g_evento_vsync;
        }
        if (evento == nullptr)
            continue;
        auto& kernel = system.Kernel();
        evento->Signal(kernel);

        // **Boot natural: só a semântica verdadeira.**
        //
        // Antes do gate, este loop escrevia `payload=1` numa lista
        // HARDCODED de endereços (0x10a5096d8, 0x843ea4d0, ...) e
        // avançava o "producer counter" 0x805e7368 — band-aids da era
        // artefato, para destravar o guest de então.
        //
        // **Medido (28/10, watchpoint gdb na palavra 0x843ea4d0)**: no
        // boot natural, 0x843ea4d0 é o **mutex real do SDK**
        // (`nn::os::detail::InternalCriticalSectionImplByHorizon`,
        // símbolo lido do dynsym): o CAS 0→0x81ff do `Enter()`
        // @0x83abc780 falhava numa palavra=1 que **o engine escreveu
        // antes da primeira instrução do guest** (`Thread 6 ... old=0
        // new=1` via `GuestMemory::write ← ThreadDoVsync vi.cpp:171`).
        // Resultado: contenção sem dono → svc #0x1a com x0=1 → park do
        // único guest thread → deadlock para sempre. A prova é
        // g610-telemetry/rlogs/exp_boot_natural_20261028.md + o gdb.
        //
        // No modo artefato (G610_ARTIFACTS_BOOT=1) o comportamento antigo
        // fica intacto — endereços e triângulo de diagnóstico.
        if (g610::natural_boot())
            continue;

        // Liga o vsync à CV de espera da thread render do guest.
        SvcDispatcher::AcordarFutexNoEndereco(0x10a5096d8);

        // Sinaliza também o semáforo do canal GPU (0xd9c2c228) e outros
        // endereços de sincronização que o guest usa em loops de
        // WaitProcessWideKeyAtomic / ArbitrateLock, para que prossiga para
        // o caminho de renderização. Sinaliza ANTES do primeiro sono para
        // que o guest não fique bloqueado antes do primeiro ciclo do vsync.
        for (std::uint64_t addr : {
                 0xd9c2c228ull,
                 0x824bce6cull,
                 0x843ea4d0ull,
                 0x10a509a80ull,
                 0x10a5096d8ull,
                 0x10a5096e4ull,
                 0x8434ed40ull,
                 0x8434ed4cull,
             }) {
            std::uint32_t payload = 1;
            auto& memoria = Kernel::GetCurrentMemory(system.Kernel());
            memoria.WriteBlock(static_cast<Common::ProcessAddress>(addr),
                               &payload, sizeof(payload));
            SvcDispatcher::AcordarFutexNoEndereco(addr);
        }

        // Avança o producer counter do contexto de render (x20+0x308,
        // com x20=0x805e7060). Sem isto, o spin loop do guest em
        // [x20+0x300] != [x20+0x308] nunca encontra trabalho: o guest
        // nunca chama DequeueBuffer, nunca submete DrawCommands, e o
        // jogo fica parado para sempre. O upstream avança este counter
        // no DequeueBuffer/QueueBuffer; aqui o fazemos do lado do host
        // a cada vsync, para destravar o primeiro ciclo.
        {
            constexpr std::uint64_t kProdutorCounter = 0x805e7368ull;  // x20+0x308
            auto* processo = Kernel::Bridge::processo();
            if (processo != nullptr) {
                std::uint32_t valor = 0;
                if (processo->GetMemory().ReadBlock(
                        static_cast<Common::ProcessAddress>(kProdutorCounter),
                        &valor, sizeof(valor))) {
                    std::uint32_t novo = valor + 1;
                    processo->GetMemory().WriteBlock(
                        static_cast<Common::ProcessAddress>(kProdutorCounter),
                        &novo, sizeof(novo));
                    // LOG_DEBUG desabilitado para reduzir ruído — reativar com IA_TRACE se necessário
                }
            }
        }

        // Desenho periódico (diagnóstico): quando o jogo abriu uma camada
        // e o backend Vulkan está pronto, dispara um DequeueBuffer →
        // desenhar → QueueBuffer a cada 60 vsyncs (~1 segundo) para colocar
        // pixels na tela. O desenho real do jogo deve vir dos comandos do
        // guest, mas este loop garante que frames aparecem no ecrã.
        static std::uint64_t frame_counter = 0;
        ++frame_counter;
        if (g_janela_vi != 0 &&
            g610::gpu::g_vulkan_backend &&
            g610::gpu::g_vulkan_backend->is_ready() &&
            frame_counter % 60 == 0) {

            // 1) DequeueBuffer — obter um slot da fila de buffers.
            std::vector<std::uint8_t> deq_input;
            {
                std::uint8_t is_async = 0;
                std::int32_t width = 1920, height = 1080, format = 1, usage = 0;
                deq_input.resize(17);
                std::size_t off = 0;
                std::memcpy(deq_input.data() + off, &is_async, 1); off += 1;
                while (off % 4 != 0) off++;
                std::memcpy(deq_input.data() + off, &width, 4); off += 4;
                std::memcpy(deq_input.data() + off, &height, 4); off += 4;
                std::memcpy(deq_input.data() + off, &format, 4); off += 4;
                std::memcpy(deq_input.data() + off, &usage, 4); off += 4;
                deq_input.resize(off);
            }
            auto deq_reply = g610::services::TransactionarParcel(3, deq_input, kernel);
            std::int32_t slot = -1;
            if (deq_reply.size() >= 4)
                std::memcpy(&slot, deq_reply.data(), 4);
            if (slot >= 0) {
                LOG_DEBUG(Service_VI, "vsync: DequeueBuffer slot={}", slot);

                // 2) Desenhar triângulo via VulkanBackend.
                std::vector<char> vs(vertex_spv, vertex_spv + vertex_spv_len);
                std::vector<char> fs(fragment_spv, fragment_spv + fragment_spv_len);
                auto draw_result = g610::gpu::g_vulkan_backend->draw_triangle(vs, fs);
                if (draw_result.ok) {
                    LOG_DEBUG(Service_VI,
                              "vsync: desenho OK red={} bg={} other={}",
                              draw_result.pixels_red,
                              draw_result.pixels_background,
                              draw_result.pixels_other);

                    // 3) Ler pixels e escrever na janela do guest.
                    auto pixels = g610::gpu::g_vulkan_backend->readback_pixels();
                    if (!pixels.empty() && g_janela_vi != 0) {
                        auto* processo = Kernel::Bridge::processo();
                        if (processo != nullptr) {
                            std::uint64_t endereco = g_janela_vi;
                            for (std::uint32_t y = 0; y < 64 && y < 720; ++y) {
                                processo->GetMemory().WriteBlock(
                                    static_cast<Common::ProcessAddress>(
                                        endereco + y * 1280 * 4),
                                    pixels.data() + y * 64 * 4,
                                    64 * 4);
                            }
                        }
                    }
                } else {
                    LOG_WARNING(Service_VI, "vsync: desenho falhou: {}",
                                draw_result.error);
                }

                // 4) QueueBuffer — devolver o slot com o desenho.
                std::vector<std::uint8_t> qb_input;
                {
                    std::int64_t timestamp = 0;
                    std::int32_t is_auto = 0;
                    std::int32_t crop[4] = {0, 0, 0, 0};
                    std::int32_t scaling = 0;
                    std::uint32_t transform = 0, sticky = 0;
                    std::int32_t async = 0, swap_interval = 1;
                    std::int32_t fence_id = -1;
                    std::uint32_t fence_val = 0;

                    std::size_t qb_off = 0;
                    qb_input.clear();
                    qb_input.resize(4 + 84);
                    qb_off = 0;
                    std::memcpy(qb_input.data() + qb_off, &slot, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, &timestamp, 8); qb_off += 8;
                    std::memcpy(qb_input.data() + qb_off, &is_auto, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, crop, 16); qb_off += 16;
                    std::memcpy(qb_input.data() + qb_off, &scaling, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, &transform, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, &sticky, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, &async, 4); qb_off += 4;
                    std::memcpy(qb_input.data() + qb_off, &swap_interval, 4); qb_off += 4;
                    std::uint32_t num_fences = 0;
                    std::memcpy(qb_input.data() + qb_off, &num_fences, 4); qb_off += 4;
                    for (int i = 0; i < 4; ++i) {
                        std::memcpy(qb_input.data() + qb_off, &fence_id, 4); qb_off += 4;
                        std::memcpy(qb_input.data() + qb_off, &fence_val, 4); qb_off += 4;
                    }
                    qb_input.resize(qb_off);
                }
                auto qb_reply = g610::services::TransactionarParcel(7, qb_input, kernel);
                if (qb_reply.size() >= 4) {
                    std::int32_t qb_status = 0;
                    std::memcpy(&qb_status, qb_reply.data(), 4);
                    LOG_DEBUG(Service_VI,
                              "vsync: QueueBuffer slot={} status={}", slot, qb_status);
                }
            } else {
                LOG_WARNING(Service_VI, "vsync: DequeueBuffer falhou, sem slot");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A base comum dos três displays.
//
// Os handlers são **de instância** (e não estáticos) porque o evento de
// vsync é um objecto — e o `KEvent` da casa vive na sessão, como o do
// `applet_oe`.
// ─────────────────────────────────────────────────────────────────────────────
class DisplayBase {
protected:
    explicit DisplayBase(Core::System& system) : system_{system} {}

    // 1102: a resolução do display. `Out<s64>` × 2, mais o
    // `display_id` de entrada.
    void GetDisplayResolution(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        ResponseBuilder rb{ctx, 6};
        rb.Push(ResultSuccess);
        rb.Push<std::int64_t>(Estado().largura);
        rb.Push<std::int64_t>(Estado().altura);
    }

    // 1010: abrir um display pelo nome. Há um só display, por isso o
    // qualquer nome serve.
    void OpenDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proximo_display++);
    }

    // 1011: abrir o display por omissão.
    void OpenDefaultDisplay(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proximo_display++);
    }

    // 1000: a lista de displays. `Out<u64>` (o número) e o array.
    void ListDisplays(HLERequestContext& ctx) {
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(1);  // há um display
    }

    // 1101: ligar/desligar o display.
    void SetDisplayEnabled(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // o estado
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2010: criar uma camada gerida. `Out<u64>` (o id da camada).
    void CreateManagedLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // as flags
        (void)rp.PopRaw<std::uint64_t>();  // o display_id
        (void)rp.PopRaw<std::uint32_t>();  // o aruid
        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(Estado().proxima_camada++);
    }

    // 6000: pôr a camada num stack.
    void AddToLayerStack(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint32_t>();  // o stack_id
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 6002: a visibilidade da camada.
    void SetLayerVisibility(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint8_t>();   // o visible
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2011: destruir a camada.
    void DestroyManagedLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 5202: o evento de vsync. É um **handle de cópia** (o Eden devolve o
    // `KReadableEvent`).
    void GetDisplayVsyncEvent(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o display_id

        if (evento_ == nullptr) {
            auto& kernel = system_.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_ = new Kernel::KEvent(kernel);
            evento_->Initialize(kernel, &processo);

            // Regista o evento de leitura e arranca a thread do vsync.
            {
                std::lock_guard<std::mutex> lk(g_mu_vsync);
                g_evento_vsync = &evento_->GetReadableEvent();
            }
            if (!g_vsync_corre.exchange(true)) {
                std::thread(ThreadDoVsync, std::ref(system_)).detach();
                LOG_DEBUG(Service_VI, "vsync: a thread do vsync arrancou (60 Hz)");
            }
        }

        ResponseBuilder rb{ctx, 3, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
        rb.Push(ResultSuccess);
    }

    Core::System& system_;
    Kernel::KEvent* evento_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// O *parcel* — o formato do Eden (`nvnflinger/parcel.h`).
//
// O Eden serializa um `NativeWindow` num "parcel" Android:
//
//   - o **cabeçalho** (`ParcelHeader`, 16 bytes): o tamanho e o offset dos
//     dados, e o tamanho e o offset dos objectos;
//   - os **dados**: o `NativeWindow` (40 bytes);
//   - os **objectos**: um `u32` a zero (o `WriteInterface` do Eden
//     escreve `0U` no buffer de objectos).
//
// A NativeWindow (`vi_types.h`, 0x28 bytes):
//
//   u32 magic = 2;  u32 process_id = 1;  u64 id;  (2 palavras de padding)
//   u8 dispdrv[8] = "dispdrv\0";          (2 palavras de padding)
//
// **O `id` é o binder** — é por ele que o jogo passa os seus buffers ao
// compositor.
// ─────────────────────────────────────────────────────────────────────────────
struct NativeWindow {
    std::uint32_t magic = 2;
    std::uint32_t process_id = 1;
    std::uint64_t id = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
    char dispdrv[8] = {'d', 'i', 's', 'p', 'd', 'r', 'v', '\0'};
    std::uint32_t padding2 = 0;
    std::uint32_t padding3 = 0;
};
static_assert(sizeof(NativeWindow) == 0x28, "tamanho errado");

// O cabeçalho do parcel (o Eden: `ParcelHeader`).
struct ParcelHeader {
    std::uint32_t data_size;
    std::uint32_t data_offset;
    std::uint32_t objects_size;
    std::uint32_t objects_offset;
};
static_assert(sizeof(ParcelHeader) == 16, "tamanho errado");

// Serializa uma `NativeWindow` num parcel — a forma do Eden
// (`OutputParcel::WriteInterface` + `Serialize`).
std::vector<std::uint8_t> SerializarNativeWindow(const NativeWindow& janela) {
    std::vector<std::uint8_t> dados(sizeof(NativeWindow), 0);
    std::memcpy(dados.data(), &janela, sizeof(janela));

    const std::uint32_t object_size = 4;  // o `u32` a zero do `WriteInterface`
    std::vector<std::uint8_t> objectos(object_size, 0);

    std::vector<std::uint8_t> parcel(sizeof(ParcelHeader) + dados.size() + objectos.size(), 0);
    ParcelHeader cabecalho{};
    cabecalho.data_size = static_cast<std::uint32_t>(dados.size());
    cabecalho.data_offset = sizeof(ParcelHeader);
    cabecalho.objects_size = static_cast<std::uint32_t>(objectos.size());
    cabecalho.objects_offset = cabecalho.data_offset + cabecalho.data_size;
    std::memcpy(parcel.data(), &cabecalho, sizeof(cabecalho));
    std::memcpy(parcel.data() + cabecalho.data_offset, dados.data(), dados.size());
    std::memcpy(parcel.data() + cabecalho.objects_offset, objectos.data(), objectos.size());
    return parcel;
}

// ─────────────────────────────────────────────────────────────────────────────
// O *binder* (o "relay service") — o serviço de que o display devolve o
// "driver".
//
// O Eden (`nvnflinger/hos_binder_driver.cpp`) tem quatro comandos; a
// **forma** é o que importa aqui. O `TransactParcel` é o protocolo do
// *binder* (o "parcel" do Android) — o Eden responde com o que o
// *surface flinger* produz, e nós não temos *surface flinger*.
//
// **O que se responde, e porquê:** o `parcel` de resposta é um bloco de
// zeros. Não é a resposta certa, mas é uma resposta **com a forma
// certa** — e o que matou o jogo foi exactamente o contrário: um
// comando que devolve uma interface respondido com "sucesso e zeros",
// que o guest leu como um ponteiro nulo e desreferenciou (medido:
// `boot_p7e.log`, o salto para `0x837982bc`, no vão entre módulos).
// ─────────────────────────────────────────────────────────────────────────────
class HosBinderDriver final : public Service::ServiceFramework<HosBinderDriver> {
public:
    explicit HosBinderDriver(Core::System& system)
        : ServiceFramework{system, "IHOSBinderDriver"}, system_{system} {
        static const FunctionInfo functions[] = {
            {0, &HosBinderDriver::TransactParcel, "TransactParcel"},
            {1, &HosBinderDriver::AdjustRefcount, "AdjustRefcount"},
            {2, &HosBinderDriver::GetNativeHandle, "GetNativeHandle"},
            {3, &HosBinderDriver::TransactParcelAuto, "TransactParcelAuto"},
        };
        RegisterHandlers(functions);
    }

private:
    // 0 e 3: transaccionar um *parcel*. A resposta é o *parcel* de
    // volta — um bloco de zeros, com o tamanho que o guest pediu.
    void TransactParcel(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        const std::int32_t binder_id = rp.PopRaw<std::int32_t>();
        const std::uint32_t transacao = rp.PopRaw<std::uint32_t>();

        // O parcel de entrada: um buffer de entrada (o `InBuffer` do
        // Eden). A resposta vai para o buffer de saída (índice 0).
        const auto entrada = ctx.ReadBufferCopy(0);

        // O cabeçalho do parcel (`ParcelHeader`, 16 bytes).
        ParcelHeader cabecalho{};
        if (entrada.size() >= sizeof(cabecalho))
            std::memcpy(&cabecalho, entrada.data(), sizeof(cabecalho));

        LOG_DEBUG(Service_NVDRV,
                  "binder: TransactParcel id={} transacao={} parcel={} bytes "
                  "(dados={}@{}, objectos={}@{})",
                  binder_id, transacao, entrada.size(), cabecalho.data_size,
                  cabecalho.data_offset, cabecalho.objects_size, cabecalho.objects_offset);

        // Os primeiros bytes do parcel, em hexadecimal: é o que permite
        // decodificar o formato **real** em vez de o adivinhar.
        {
            // **Os limites são contados.** Um `snprintf` que devolve o
            // tamanho *que teria escrito* pode passar do fim do tampão, e
            // o `sizeof(linha) - n` deixa de ser um tamanho: vira um
            // número gigante e o motor escreve fora do tampão. Foi o que
            // aconteceu (o processo morreu no fim do arranque).
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[binder] parcel:"));
            for (std::size_t i = 0; i < entrada.size() && i < 48 && n + 4 < sizeof(linha); ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", entrada[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        // A resposta: a do **compositor** (`src/services/binder.cpp`), que
        // e' quem atende o protocolo do `IGraphicBufferProducer`.
        auto& kernel = system_.Kernel();
        const std::vector<std::uint8_t> reply =
            g610::services::TransactionarParcel(transacao, entrada, kernel);

        // **A resposta, em hexadecimal.** E' o parcel que volta ao jogo
        // — e nunca foi medido. O Eden (`parcel.h:155-170`) escreve
        // o cabecalho, os dados e os objectos.
        if (std::getenv("IA_IPC_TRACE") != nullptr) {
            char linha[320];
            std::size_t n = 0;
            n += static_cast<std::size_t>(
                std::snprintf(linha + n, sizeof(linha) - n, "[binder] resposta:"));
            for (std::size_t i = 0; i < reply.size() && i < 64 && n + 4 < sizeof(linha); ++i)
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, " %02x", reply[i]));
            if (n + 2 < sizeof(linha))
                n += static_cast<std::size_t>(
                    std::snprintf(linha + n, sizeof(linha) - n, "\n"));
            const ssize_t w = ::write(2, linha, n);
            (void)w;
        }

        const std::size_t tamanho = ctx.GetWriteBufferSize(0);
        if (tamanho > 0) {
            const std::size_t n = std::min(tamanho, reply.size());
            ctx.WriteBuffer(reply.data(), n, 0);
        }
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 3: o `TransactParcel` "auto" — a mesma transacção, com o
    // descritor de buffer automático (o Eden reusa o handler do 0).
    void TransactParcelAuto(HLERequestContext& ctx) {
        TransactParcel(ctx);
    }

    // 1: o contador de referências do *binder*.
    void AdjustRefcount(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::int32_t>();  // o binder_id
        (void)rp.PopRaw<std::int32_t>();  // o addval
        (void)rp.PopRaw<std::int32_t>();  // o type
        ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 2: o handle nativo de um *binder* — o Eden devolve um
    // `OutCopyHandle<KReadableEvent>`.
    void GetNativeHandle(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::int32_t>();  // o binder_id
        (void)rp.PopRaw<std::uint32_t>(); // o type_id

        if (evento_ == nullptr) {
            auto& kernel = system_.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_ = new Kernel::KEvent(kernel);
            evento_->Initialize(kernel, &processo);
            // **O evento da fila** — e' este que o upstream devolve
            // ao jogo (`buffer_queue_producer.cpp:825-827`: o
            // `buffer_wait_event`), e que o compositor sinaliza
            // quando ha' buffer.
            g610::services::DefinirEventoDaFila(evento_);
        }

        ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_->GetReadableEvent());
    }

    Core::System& system_;
    Kernel::KEvent* evento_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Os três displays, com as tabelas do upstream.
//
// Os IDs são o espaço de IDs do display: o **mesmo** ID em displays
// diferentes é uma coisa diferente (o 2010 do manager é "criar camada
// gerida"; o 2010 da aplicação não existe). Por isso são três classes.
// ─────────────────────────────────────────────────────────────────────────────
class ViDisplayManager final : public Service::ServiceFramework<ViDisplayManager>,
                               public DisplayBase {
public:
    explicit ViDisplayManager(Core::System& system)
        : ServiceFramework{system, "IManagerDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {200, nullptr, "AllocateProcessHeapBlock"},
            {201, nullptr, "FreeProcessHeapBlock"},
            {1102, &ViDisplayManager::GetDisplayResolution, "GetDisplayResolution"},
            {2010, &ViDisplayManager::CreateManagedLayer, "CreateManagedLayer"},
            {2011, &ViDisplayManager::DestroyManagedLayer, "DestroyManagedLayer"},
            {2012, nullptr, "CreateStrayLayer"},
            {2050, nullptr, "CreateIndirectLayer"},
            {2051, nullptr, "DestroyIndirectLayer"},
            {2052, nullptr, "CreateIndirectProducerEndPoint"},
            {2053, nullptr, "DestroyIndirectProducerEndPoint"},
            {2054, nullptr, "CreateIndirectConsumerEndPoint"},
            {2055, nullptr, "DestroyIndirectConsumerEndPoint"},
            {2060, nullptr, "CreateWatermarkCompositor"},
            {2062, nullptr, "SetWatermarkText"},
            {2063, nullptr, "SetWatermarkLayerStacks"},
            {2300, nullptr, "AcquireLayerTexturePresentingEvent"},
            {2301, nullptr, "ReleaseLayerTexturePresentingEvent"},
            {2302, nullptr, "GetDisplayHotplugEvent"},
            {2303, nullptr, "GetDisplayModeChangedEvent"},
            {2402, nullptr, "GetDisplayHotplugState"},
            {2501, nullptr, "GetCompositorErrorInfo"},
            {2601, nullptr, "GetDisplayErrorEvent"},
            {2701, nullptr, "GetDisplayFatalErrorEvent"},
            {4201, nullptr, "SetDisplayAlpha"},
            {4203, nullptr, "SetDisplayLayerStack"},
            {4205, nullptr, "SetDisplayPowerState"},
            {4206, nullptr, "SetDefaultDisplay"},
            {4207, nullptr, "ResetDisplayPanel"},
            {4208, nullptr, "SetDisplayFatalErrorEnabled"},
            {4209, nullptr, "IsDisplayPanelOn"},
            {4300, nullptr, "GetInternalPanelId"},
            {6000, &ViDisplayManager::AddToLayerStack, "AddToLayerStack"},
            {6001, nullptr, "RemoveFromLayerStack"},
            {6002, &ViDisplayManager::SetLayerVisibility, "SetLayerVisibility"},
            {6003, nullptr, "SetLayerConfig"},
            {6004, nullptr, "AttachLayerPresentationTracer"},
            {6005, nullptr, "DetachLayerPresentationTracer"},
            {6006, nullptr, "StartLayerPresentationRecording"},
            {6007, nullptr, "StopLayerPresentationRecording"},
            {6008, nullptr, "StartLayerPresentationFenceWait"},
            {6009, nullptr, "StopLayerPresentationFenceWait"},
            {6010, nullptr, "GetLayerPresentationAllFencesExpiredEvent"},
            {6011, nullptr, "EnableLayerAutoClearTransitionBuffer"},
            {6012, nullptr, "DisableLayerAutoClearTransitionBuffer"},
            {6013, nullptr, "SetLayerOpacity"},
            {6014, nullptr, "AttachLayerWatermarkCompositor"},
            {6015, nullptr, "DetachLayerWatermarkCompositor"},
            {7000, nullptr, "SetContentVisibility"},
            {8000, nullptr, "SetConductorLayer"},
            {8001, nullptr, "SetTimestampTracking"},
            {8100, nullptr, "SetIndirectProducerFlipOffset"},
            {8200, nullptr, "CreateSharedBufferStaticStorage"},
            {8201, nullptr, "CreateSharedBufferTransferMemory"},
            {8202, nullptr, "DestroySharedBuffer"},
            {8203, nullptr, "BindSharedLowLevelLayerToManagedLayer"},
            {8204, nullptr, "BindSharedLowLevelLayerToIndirectLayer"},
            {8207, nullptr, "UnbindSharedLowLevelLayer"},
            {8208, nullptr, "ConnectSharedLowLevelLayerToSharedBuffer"},
            {8209, nullptr, "DisconnectSharedLowLevelLayerFromSharedBuffer"},
            {8210, nullptr, "CreateSharedLayer"},
            {8211, nullptr, "DestroySharedLayer"},
            {8216, nullptr, "AttachSharedLayerToLowLevelLayer"},
            {8217, nullptr, "ForceDetachSharedLayerFromLowLevelLayer"},
            {8218, nullptr, "StartDetachSharedLayerFromLowLevelLayer"},
            {8219, nullptr, "FinishDetachSharedLayerFromLowLevelLayer"},
            {8220, nullptr, "GetSharedLayerDetachReadyEvent"},
            {8221, nullptr, "GetSharedLowLevelLayerSynchronizedEvent"},
            {8222, nullptr, "CheckSharedLowLevelLayerSynchronized"},
            {8223, nullptr, "RegisterSharedBufferImporterAruid"},
            {8224, nullptr, "UnregisterSharedBufferImporterAruid"},
            {8227, nullptr, "CreateSharedBufferProcessHeap"},
            {8228, nullptr, "GetSharedLayerLayerStacks"},
            {8229, nullptr, "SetSharedLayerLayerStacks"},
            {8291, nullptr, "PresentDetachedSharedFrameBufferToLowLevelLayer"},
            {8292, nullptr, "FillDetachedSharedFrameBufferColor"},
            {8293, nullptr, "GetDetachedSharedFrameBufferImage"},
            {8294, nullptr, "SetDetachedSharedFrameBufferImage"},
            {8295, nullptr, "CopyDetachedSharedFrameBufferImage"},
            {8296, nullptr, "SetDetachedSharedFrameBufferSubImage"},
            {8297, nullptr, "GetSharedFrameBufferContentParameter"},
            {8298, nullptr, "ExpandStartupLogoOnSharedFrameBuffer"},
        };
        RegisterHandlers(functions);
    }
};

class ViDisplaySystem final : public Service::ServiceFramework<ViDisplaySystem>,
                              public DisplayBase {
public:
    explicit ViDisplaySystem(Core::System& system)
        : ServiceFramework{system, "ISystemDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {1200, nullptr, "GetZOrderCountMin"},
            {1202, nullptr, "GetZOrderCountMax"},
            {1203, nullptr, "GetDisplayLogicalResolution"},
            {1204, nullptr, "SetDisplayMagnification"},
            {2201, nullptr, "SetLayerPosition"},
            {2203, nullptr, "SetLayerSize"},
            {2204, nullptr, "GetLayerZ"},
            {2205, nullptr, "SetLayerZ"},
            {2207, nullptr, "SetLayerVisibility"},
            {2209, nullptr, "SetLayerAlpha"},
            {2210, nullptr, "SetLayerPositionAndSize"},
            {2312, nullptr, "CreateStrayLayer"},
            {2400, nullptr, "OpenIndirectLayer"},
            {2401, nullptr, "CloseIndirectLayer"},
            {2402, nullptr, "FlipIndirectLayer"},
            {3000, nullptr, "ListDisplayModes"},
            {3001, nullptr, "ListDisplayRgbRanges"},
            {3002, nullptr, "ListDisplayContentTypes"},
            {3200, nullptr, "GetDisplayMode"},
            {3201, nullptr, "SetDisplayMode"},
            {3202, nullptr, "GetDisplayUnderscan"},
            {3203, nullptr, "SetDisplayUnderscan"},
            {3204, nullptr, "GetDisplayContentType"},
            {3205, nullptr, "SetDisplayContentType"},
            {3206, nullptr, "GetDisplayRgbRange"},
            {3207, nullptr, "SetDisplayRgbRange"},
            {3208, nullptr, "GetDisplayCmuMode"},
            {3209, nullptr, "SetDisplayCmuMode"},
            {3210, nullptr, "GetDisplayContrastRatio"},
            {3211, nullptr, "SetDisplayContrastRatio"},
            {3214, nullptr, "GetDisplayGamma"},
            {3215, nullptr, "SetDisplayGamma"},
            {3216, nullptr, "GetDisplayCmuLuma"},
            {3217, nullptr, "SetDisplayCmuLuma"},
            {3218, nullptr, "SetDisplayCrcMode"},
            {6013, nullptr, "GetLayerPresentationSubmissionTimestamps"},
            {8225, nullptr, "GetSharedBufferMemoryHandleId"},
            {8250, nullptr, "OpenSharedLayer"},
            {8251, nullptr, "CloseSharedLayer"},
            {5202, &ViDisplaySystem::GetDisplayVsyncEvent, "GetDisplayVsyncEvent"},
        };
        RegisterHandlers(functions);
    }

private:
    Kernel::KEvent* evento_vsync_ = nullptr;

    void GetDisplayVsyncEvent(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o display_id

        if (evento_vsync_ == nullptr) {
            auto& kernel = system_.Kernel();
            auto& processo = Kernel::GetCurrentProcess(kernel);
            evento_vsync_ = new Kernel::KEvent(kernel);
            evento_vsync_->Initialize(kernel, &processo);

            {
                std::lock_guard<std::mutex> lk(g_mu_vsync);
                g_evento_vsync = &evento_vsync_->GetReadableEvent();
            }
            if (!g_vsync_corre.exchange(true)) {
                std::thread(ThreadDoVsync, std::ref(system_)).detach();
                LOG_DEBUG(Service_VI, "vsync: thread arrancada via vi:s (60 Hz)");
            }
        }

        ResponseBuilder rb{ctx, 3, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, &evento_vsync_->GetReadableEvent());
        rb.Push(ResultSuccess);
    }
};

class ViDisplayApplication final : public Service::ServiceFramework<ViDisplayApplication>,
                                   public DisplayBase {
public:
    explicit ViDisplayApplication(Core::System& system)
        : ServiceFramework{system, "IApplicationDisplayService"}, DisplayBase{system} {
        static const FunctionInfo functions[] = {
            {100, &ViDisplayApplication::GetRelayService, "GetRelayService"},
            {101, &ViDisplayApplication::GetSystemDisplayService, "GetSystemDisplayService"},
            {102, &ViDisplayApplication::GetManagerDisplayService, "GetManagerDisplayService"},
            {103, &ViDisplayApplication::GetIndirectDisplayTransactionService,
             "GetIndirectDisplayTransactionService"},
            {1000, &ViDisplayApplication::ListDisplays, "ListDisplays"},
            {1010, &ViDisplayApplication::OpenDisplay, "OpenDisplay"},
            {1011, &ViDisplayApplication::OpenDefaultDisplay, "OpenDefaultDisplay"},
            {1020, nullptr, "CloseDisplay"},
            {1101, &ViDisplayApplication::SetDisplayEnabled, "SetDisplayEnabled"},
            {1102, &ViDisplayApplication::GetDisplayResolution, "GetDisplayResolution"},
            {2020, &ViDisplayApplication::OpenLayer, "OpenLayer"},
            {2021, nullptr, "CloseLayer"},
            {2030, nullptr, "CreateStrayLayer"},
            {2031, nullptr, "DestroyStrayLayer"},
            {2101, nullptr, "SetLayerScalingMode"},
            {2102, nullptr, "ConvertScalingMode"},
            {2450, nullptr, "GetIndirectLayerImageMap"},
            {2451, nullptr, "GetIndirectLayerImageCropMap"},
            {2460, nullptr, "GetIndirectLayerImageRequiredMemoryInfo"},
            {5202, &ViDisplayApplication::GetDisplayVsyncEvent, "GetDisplayVsyncEvent"},
            {5203, nullptr, "GetDisplayVsyncEventForDebug"},
        };
        RegisterHandlers(functions);
    }

private:
    // 2020: abrir uma camada — e devolver a **native window**, que é o
    // buffer onde o jogo desenha.
    //
    // O Eden (`application_display_service.cpp`, `OpenLayer`): abre o
    // display, abre a camada (e fica com o **binder** do produtor), e
    // devolve um *parcel* com a `NativeWindow` dentro.
    //
    // O parcel é o que o jogo sabe ler: o cabeçalho, a `NativeWindow` e o
    // objecto a zero — byte a byte, como o Eden.
    void OpenLayer(HLERequestContext& ctx) {
        RequestParser rp{ctx};
        (void)rp.PopRaw<std::uint64_t>();  // o layer_id
        (void)rp.PopRaw<std::uint32_t>();  // o aruid

        // A janela: um buffer **real** na memória do guest, do tamanho do
        // display. O guest desenha lá dentro; o compositor da casa
        // apresenta-o.
        //
        // Onde? No espaço de VA da GPU (o mesmo que o `nvdrv` dá ao
        // jogo) — é memória do guest, e por isso o host lê-la
        // directamente.
        const std::uint64_t tamanho = static_cast<std::uint64_t>(Estado().largura) *
                                      static_cast<std::uint64_t>(Estado().altura) * 4u;
        std::uint64_t endereco = 0;
        if (!AlocarJanela(tamanho, endereco)) {
            LOG_WARNING(Service_VI, "IDisplayApplication: OpenLayer: sem espaço para a janela");
        }
        // Guarda o endereço para o thread de vsync poder escrever pixels.
        g_janela_vi = endereco;

        const std::uint64_t binder = Estado().proximo_binder++;
        NativeWindow janela{};
        janela.id = binder;
        const std::vector<std::uint8_t> parcel = SerializarNativeWindow(janela);

        LOG_DEBUG(Service_VI, "IDisplayApplication: OpenLayer: binder={} janela=0x{:X} bytes={}",
                  binder, endereco, tamanho);

        ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<std::uint64_t>(parcel.size());
    }

    // Aloca o buffer de uma janela no espaço de VA da GPU.
    //
    // **O VA da GPU é gerido pelo `nvdrv`** (que é quem o dá ao jogo);
    // aqui alocamos da mesma reserva, e o `nvdrv` é quem sabe onde estão
    // as regiões. A alocação é feita aqui porque é a display que pede a
    // janela — e o `nvdrv` e' quem tem a reserva.
    //
    // Devolve `true` se alocar.
    static bool AlocarJanela(std::uint64_t tamanho, std::uint64_t& endereco) {
    // **É o compositor que a aloca** (`src/services/binder.cpp`): é
    // quem precisa de saber onde está, porque é quem apresenta o
    // desenho. A reserva é a do VA da GPU (a mesma que o `nvdrv` dá
    // ao jogo).
        endereco = g610::services::AlocarJanelaDoCompositor(tamanho);
        return endereco != 0;
    }

    // 100: o *relay service* (o driver do *binder*).
    void GetRelayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetRelayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<HosBinderDriver>(ctx, std::make_shared<HosBinderDriver>(system));
    }

    // 101: o display do sistema.
    void GetSystemDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetSystemDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplaySystem>(ctx, std::make_shared<ViDisplaySystem>(system));
    }

    // 102: o display do *manager*.
    void GetManagerDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetManagerDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayManager>(ctx, std::make_shared<ViDisplayManager>(system));
    }

    // 103: o serviço de transacções do display indirecto.
    void GetIndirectDisplayTransactionService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "IDisplayApplication: GetIndirectDisplayTransactionService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<HosBinderDriver>(ctx, std::make_shared<HosBinderDriver>(system));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// As três raízes: "vi:m", "vi:s" e "vi:u".
//
// Cada uma devolve o seu display com o `GetDisplayService` — o comando
// com o **ID** que o Eden usa (2 no manager, 1 no sistema, 0 na
// aplicação). É o ID que importa: é o que o guest pede.
// ─────────────────────────────────────────────────────────────────────────────
class ViRootManager final : public Service::ServiceFramework<ViRootManager> {
public:
    explicit ViRootManager(Core::System& system) : ServiceFramework{system, "vi:m"} {
        static const FunctionInfo functions[] = {
            {2, &ViRootManager::GetDisplayService, "GetDisplayService"},
            {3, nullptr, "GetDisplayServiceWithProxyNameExchange"},
            {100, nullptr, "PrepareFatal"},
            {101, nullptr, "ShowFatal"},
            {102, nullptr, "DrawFatalRectangle"},
            {103, nullptr, "DrawFatalText32"},
        };
        RegisterHandlers(functions);
    }

private:
    // **O display que o "vi:m" devolve é o da APLICAÇÃO.**
    //
    // O Eden (`manager_root_service.cpp`, `GetDisplayService`):
    //
    //     Result IManagerRootService::GetDisplayService(
    //         Out<SharedPointer<IApplicationDisplayService>> out_application_display_service,
    //         Policy policy)
    //
    // Ou seja: o "vi:m" (o *manager*) devolve o display **da aplicação** —
    // o `Policy` é que decide com que permissões. Devolver o display do
    // *manager* dava ao guest uma tabela de comandos diferente: ele pedia
    // o 100 (`GetRelayService`), que no display do *manager* não existe
    // (medido: `boot_p7h.log`, `HSR IDisplayManager: comando 100`).
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:m: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayApplication>(ctx,
            std::make_shared<ViDisplayApplication>(system));
    }
};

class ViRootSystem final : public Service::ServiceFramework<ViRootSystem> {
public:
    explicit ViRootSystem(Core::System& system) : ServiceFramework{system, "vi:s"} {
        static const FunctionInfo functions[] = {
            {1, &ViRootSystem::GetDisplayService, "GetDisplayService"},
            {3, nullptr, "GetDisplayServiceWithProxyNameExchange"},
        };
        RegisterHandlers(functions);
    }

private:
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:s: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplaySystem>(ctx, std::make_shared<ViDisplaySystem>(system));
    }
};

class ViRootApplication final : public Service::ServiceFramework<ViRootApplication> {
public:
    explicit ViRootApplication(Core::System& system) : ServiceFramework{system, "vi:u"} {
        static const FunctionInfo functions[] = {
            {0, &ViRootApplication::GetDisplayService, "GetDisplayService"},
            {1, nullptr, "GetDisplayServiceWithProxyNameExchange"},
        };
        RegisterHandlers(functions);
    }

private:
    void GetDisplayService(HLERequestContext& ctx) {
        LOG_DEBUG(Service_VI, "vi:u: GetDisplayService");
        ResponseBuilder rb{ctx, 2, 0, 1, IPC::ResponseBuilder::Flags::AlwaysMoveHandles};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<ViDisplayApplication>(ctx,
            std::make_shared<ViDisplayApplication>(system));
    }
};
} // namespace

// Cria uma camada gerida e devolve o id.
//
// O Eden (`core/hle/service/am/display_layer_manager.cpp:64`):
//
//     m_display_service->OpenDisplay(&display_id, "Default");
//     m_manager_display_service->CreateManagedLayer(out_layer_id, 0, display_id, user_id);
//     m_manager_display_service->SetLayerVisibility(m_visible, *out_layer_id);
//
// Na casa não há displays nem pilhas de camadas — o `vi` dá ao jogo a
// **janela** pelo `OpenLayer` (o `vi:u`) e o `am` só precisa de um id de
// camada que não colida com o do `vi:m`.
//
// **O id sai do mesmo contador** que o `vi:m` usa (`Estado().proxima_camada`),
// para que um id de camada do `am` e um do `vi:m` nunca sejam o mesmo.
std::uint64_t criar_camada_gerida() {
    return Estado().proxima_camada++;
}

// Regista o `vi` no `sm:` — os três nomes, como o Eden (`vi.cpp`,
// `LoopProcess`).
void registar_vi(Core::System& system) {
    auto server_manager = std::make_unique<Service::ServerManager>(system);
    server_manager->RegisterNamedService("vi:m", std::make_shared<ViRootManager>(system));
    server_manager->RegisterNamedService("vi:s", std::make_shared<ViRootSystem>(system));
    server_manager->RegisterNamedService("vi:u", std::make_shared<ViRootApplication>(system));

    // Cria o evento de vsync e arranca a thread automaticamente. O jogo pode
    // nunca chamar GetDisplayVsyncEvent, mas o vsync precisa de estar a correr
    // para que o producer counter avance e o spin loop do guest encontre
    // trabalho.
    {
        auto& kernel = system.Kernel();
        auto& processo = Kernel::GetCurrentProcess(kernel);
        static Kernel::KEvent evento_vsync(kernel);
        evento_vsync.Initialize(kernel, &processo);
        {
            std::lock_guard<std::mutex> lk(g_mu_vsync);
            g_evento_vsync = &evento_vsync.GetReadableEvent();
        }
    }
    g_vsync_corre.store(true, std::memory_order_relaxed);
    std::thread(ThreadDoVsync, std::ref(system)).detach();
    LOG_DEBUG(Service_VI, "vsync: thread arrancada automaticamente (60 Hz)");

    Service::ServerManager::RunServer(std::move(server_manager));
}
} // namespace g610::services
