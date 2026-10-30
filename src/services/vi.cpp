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

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

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
// do thread de vsync para leitura de pixels (atomic para visibilidade
// entre threads).
std::atomic<std::uint64_t> g_janela_vi{0};

// ─────────────────────────────────────────────────────────────────────────────
// Apresentação na tela (Wayland + Vulkan + panvk / Mali-G610).
//
// Criamos um SDL window fullscreen + Vulkan swapchain e apresentamos os
// pixels do framebuffer do guest (g_janela_vi) a cada vsync.
// ─────────────────────────────────────────────────────────────────────────────
struct PresentContext {
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore sem_acquire = VK_NULL_HANDLE;
    VkSemaphore sem_present = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool ready = false;
};

static PresentContext g_pres;

static bool present_init() {
    std::fprintf(stderr, "[vi] present_init: iniciando...\n");
    // Forçar Wayland antes do SDL_Init.
    ::setenv("XDG_RUNTIME_DIR", "/run/emuos", 1);
    ::setenv("WAYLAND_DISPLAY", "wayland-1", 1);
    ::setenv("SDL_VIDEODRIVER", "wayland", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[vi] SDL_Init falhou: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "[vi] SDL_Init OK\n");

    SDL_Window* win = SDL_CreateWindow(
        "EdenMaliG610 — Pokemon Shield",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (!win) {
        std::fprintf(stderr, "[vi] SDL_CreateWindow falhou: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "[vi] SDL_CreateWindow OK\n");
    SDL_ShowWindow(win);
    std::fprintf(stderr, "[vi] SDL_ShowWindow OK\n");
    g_pres.window = win;

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);

    unsigned n_exts = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(win, &n_exts, nullptr)) {
        std::fprintf(stderr, "[vi] SDL_Vulkan_GetInstanceExtensions falhou\n");
        return false;
    }
    std::vector<const char*> exts(n_exts);
    if (!SDL_Vulkan_GetInstanceExtensions(win, &n_exts, exts.data())) {
        std::fprintf(stderr, "[vi] SDL_Vulkan_GetInstanceExtensions(2) falhou\n");
        return false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "EdenMaliG610";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = n_exts;
    ici.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ici, nullptr, &g_pres.instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[vi] vkCreateInstance falhou\n");
        return false;
    }

    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(win, g_pres.instance, &surf)) {
        std::fprintf(stderr, "[vi] SDL_Vulkan_CreateSurface falhou\n");
        return false;
    }
    g_pres.surface = surf;

    std::uint32_t n_phys = 0;
    vkEnumeratePhysicalDevices(g_pres.instance, &n_phys, nullptr);
    std::vector<VkPhysicalDevice> phys(n_phys);
    vkEnumeratePhysicalDevices(g_pres.instance, &n_phys, phys.data());
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    for (auto p : phys) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        if (std::strstr(pr.deviceName, "Mali")) {
            gpu = p;
            break;
        }
    }
    if (gpu == VK_NULL_HANDLE) gpu = phys[0];
    g_pres.physical = gpu;

    std::uint32_t n_fam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n_fam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n_fam);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n_fam, fams.data());
    std::uint32_t fam = UINT32_MAX;
    for (std::uint32_t i = 0; i < n_fam; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
        VkBool32 pode = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, g_pres.surface, &pode);
        if (pode) { fam = i; break; }
    }
    if (fam == UINT32_MAX) {
        std::fprintf(stderr, "[vi] nenhuma fila graphics+present\n");
        return false;
    }
    g_pres.queue_family = fam;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = fam;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &prio;
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    if (vkCreateDevice(gpu, &dci, nullptr, &g_pres.device) != VK_SUCCESS) {
        std::fprintf(stderr, "[vi] vkCreateDevice falhou\n");
        return false;
    }
    vkGetDeviceQueue(g_pres.device, fam, 0, &g_pres.queue);

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, g_pres.surface, &caps);
    g_pres.extent = caps.currentExtent;
    if (g_pres.extent.width == 0xFFFFFFFFu || g_pres.extent.width == 0u) {
        g_pres.extent.width = static_cast<std::uint32_t>(w);
        g_pres.extent.height = static_cast<std::uint32_t>(h);
    }

    std::uint32_t n_fmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, g_pres.surface, &n_fmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n_fmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, g_pres.surface, &n_fmt, fmts.data());
    g_pres.format = fmts[0].format;
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            g_pres.format = f.format;
            break;
        }
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = g_pres.surface;
    sci.minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2;
    sci.imageFormat = g_pres.format;
    sci.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = g_pres.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_pres.device, &sci, nullptr, &g_pres.swapchain) != VK_SUCCESS) {
        std::fprintf(stderr, "[vi] vkCreateSwapchainKHR falhou\n");
        return false;
    }

    std::uint32_t n_img = 0;
    vkGetSwapchainImagesKHR(g_pres.device, g_pres.swapchain, &n_img, nullptr);
    g_pres.images.resize(n_img);
    g_pres.views.resize(n_img);
    g_pres.framebuffers.resize(n_img);
    vkGetSwapchainImagesKHR(g_pres.device, g_pres.swapchain, &n_img, g_pres.images.data());

    // Render pass
    VkAttachmentDescription att{};
    att.format = g_pres.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    vkCreateRenderPass(g_pres.device, &rpci, nullptr, &g_pres.render_pass);

    // Pipeline (passthrough: vertex shader só passa posição, fragment shader usa textura)
    // Usamos um pipeline simples de transferência: copiamos os pixels do guest
    // para a imagem do swapchain via cmdBlitImage (sem shaders necessários).
    // Não precisamos de pipeline gráfico para blit.

    // Command pool + buffer
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = fam;
    vkCreateCommandPool(g_pres.device, &cpci, nullptr, &g_pres.command_pool);
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_pres.command_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_pres.device, &cbai, &g_pres.command_buffer);

    // Semaphores + fence
    VkSemaphoreCreateInfo semci{};
    semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(g_pres.device, &semci, nullptr, &g_pres.sem_acquire);
    vkCreateSemaphore(g_pres.device, &semci, nullptr, &g_pres.sem_present);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(g_pres.device, &fci, nullptr, &g_pres.fence);

    // Image views
    for (std::uint32_t i = 0; i < n_img; ++i) {
        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = g_pres.images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = g_pres.format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        vkCreateImageView(g_pres.device, &ivci, nullptr, &g_pres.views[i]);
    }

    g_pres.ready = true;
    std::fprintf(stderr,
        "[vi] apresentacao pronta: %ux%u formato=%d\n",
        g_pres.extent.width, g_pres.extent.height,
        static_cast<int>(g_pres.format));
    return true;
}

static void present_guest_framebuffer() {
    static std::uint64_t call_count = 0;
    ++call_count;
    std::fprintf(stderr, "[vi] present_guest_framebuffer: chamada %llu\n",
        (unsigned long long)call_count);
    std::fflush(stderr);
    static bool first_call = true;
    if (first_call) {
        first_call = false;
        std::fprintf(stderr, "[vi] present_guest_framebuffer: primeira chamada\n");
    }
    if (!g_pres.ready && !present_init())
        return;
    if (g_janela_vi.load() == 0)
        return;

    auto* proc = Kernel::Bridge::processo();
    if (!proc) return;

    // Ler o framebuffer do guest (RGBA8, 1280x720)
    const std::uint32_t fw = 1280;
    const std::uint32_t fh = 720;
    const std::uint64_t fsize = static_cast<std::uint64_t>(fw) * fh * 4u;
    std::vector<std::uint8_t> pixels(fsize);
    if (!proc->GetMemory().ReadBlock(
            static_cast<Common::ProcessAddress>(g_janela_vi.load()),
            pixels.data(), fsize))
        return;

    // Adquirir próxima imagem do swapchain
    VkResult r = vkWaitForFences(g_pres.device, 1, &g_pres.fence, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) return;
    vkResetFences(g_pres.device, 1, &g_pres.fence);

    std::uint32_t idx = 0;
    r = vkAcquireNextImageKHR(g_pres.device, g_pres.swapchain, UINT64_MAX,
                              g_pres.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // Não recriamos swapchain aqui para evitar complexidade;
        // apenas saltamos este quadro.
        return;
    }
    if (r != VK_SUCCESS) return;

    // Fazer upload dos pixels para a imagem do swapchain via staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = fsize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    vkCreateBuffer(g_pres.device, &bci, nullptr, &staging);

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(g_pres.device, staging, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = 0;  // simplificar: usar tipo 0 (geralmente host-visible)
    vkAllocateMemory(g_pres.device, &mai, nullptr, &staging_mem);
    vkBindBufferMemory(g_pres.device, staging, staging_mem, 0);

    void* mapped = nullptr;
    vkMapMemory(g_pres.device, staging_mem, 0, fsize, 0, &mapped);
    std::memcpy(mapped, pixels.data(), fsize);
    vkUnmapMemory(g_pres.device, staging_mem);

    // Command buffer: copy buffer → image
    vkResetCommandBuffer(g_pres.command_buffer, 0);
    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_pres.command_buffer, &cbbi);

    // Transição da imagem para TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier bar1{};
    bar1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar1.image = g_pres.images[idx];
    bar1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar1.subresourceRange.levelCount = 1;
    bar1.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(g_pres.command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar1);

    // Copiar buffer → image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = fw;
    region.bufferImageHeight = fh;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {fw, fh, 1};
    vkCmdCopyBufferToImage(g_pres.command_buffer, staging, g_pres.images[idx],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transição para PRESENT_SRC_KHR
    VkImageMemoryBarrier bar2{};
    bar2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar2.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar2.image = g_pres.images[idx];
    bar2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar2.subresourceRange.levelCount = 1;
    bar2.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(g_pres.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar2);

    vkEndCommandBuffer(g_pres.command_buffer);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_pres.command_buffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_pres.sem_present;
    vkQueueSubmit(g_pres.queue, 1, &si, g_pres.fence);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_pres.sem_present;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_pres.swapchain;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(g_pres.queue, &pi);

    // Limpeza
    vkDestroyBuffer(g_pres.device, staging, nullptr);
    vkFreeMemory(g_pres.device, staging_mem, nullptr);
}


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

        // Apresentação periódica: ler o framebuffer real do jogo de
        // g_janela_vi e apresentar via Vulkan swapchain (Wayland/HDMI).
        // O desenho real vem dos comandos Maxwell do guest via NVDRV;
        // aqui apenas espelhamos o que o guest escreveu para o ecrã.
        // Executado SEM SER AFETADO pelo gate natural_boot, para garantir
        // que os frames do jogo apareçam na tela mesmo no boot natural.
        static std::uint64_t frame_counter = 0;
        ++frame_counter;
        present_guest_framebuffer();

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
        g_janela_vi.store(endereco);

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
