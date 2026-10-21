// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later

// ── mostra ────────────────────────────────────────────────────────────────
//
// A PRIMEIRA IMAGEM NA TELA do Orange Pi 5 Pro: um triângulo laranja sobre
// fundo escuro, no swapchain do compositor sway (Wayland, HDMI-A-1), via
// SDL2 + Vulkan/panvk (Mali-G610).
//
// Decisão de apresentação, medida e não por gosto:
//   • a sessão do board É Wayland (sway, HDMI-A-1 — `ps`);
//   • não há sessão X (pgrep Xorg = 0) — X11 aqui seria o gateway XWayland
//     (um tradutor a mais por frame) ou trocar de sessão;
//   • Vulkan WSI directo no Wayland = o caminho do compositor, sem daemon.
// O SDL2 (já PUBLIC no g610_core) cria a janela e chama o WSI por nós:
// zero código de protocolo (xdg_shell etc.).
//
// A cadeia de shaders DO MOTOR continua no test_gpu_exec (Maxwell→IR→
// GLSL→SPIR-V→Mali, 20/20). Os dois shaders abaixo são o GLSL mínimo do
// *apresentador*; a imagem do JOGO fica atrás do drop fantasma do
// SendSyncRequest (PROGRESSO.md passo 6).
//
// Medida de verificação no board (feita por ssh, sem a tela na mão):
//   grim -o HDMI-A-1 /tmp/tela.ppm — e os pixels centrais/canto medidos
//   em python3: centro laranja (255/115/25 aprox), canto fundo escuro.
//
// Uso no board:
//   ./g610_mostra [segundos]      (30 por omissão; fecha com ESC/janela)

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define VK_CHECK(expr)                                                        \
    do {                                                                      \
        VkResult _r = (expr);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            std::printf("  FALHOU: %s -> %d\n", #expr, static_cast<int>(_r)); \
            return 2;                                                         \
        }                                                                     \
    } while (false)

static const char kVert[] = R"(
#version 450
void main() {
    // Triângulo CENTRADO (o fundo tem de ficar visível — se ele cobrir a
    // tela toda, não distinguiu shader de clear). Coordenadas em NDC.
    const vec2 pos[3] = vec2[3](vec2(-0.55, -0.50), vec2(0.55, -0.50),
                                vec2(0.00, 0.65));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
)";

static const char kFrag[] = R"(
#version 450
layout(location = 0) out vec4 cor;
void main() {
    cor = vec4(1.0, 0.45, 0.1, 1.0);  // laranja
}
)";

static int run_cmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

static bool compilar(const std::string& fonte, const char* estagio,
                     const std::string& spv) {
    const std::string caminho =
        "/tmp/g610_mostra." + std::string(estagio);
    FILE* f = std::fopen(caminho.c_str(), "w");
    if (f == nullptr)
        return false;
    std::fwrite(fonte.data(), 1, fonte.size(), f);
    std::fclose(f);
    return run_cmd("glslangValidator -V --target-env vulkan1.1 -S " +
                   std::string(estagio) + " " + caminho + " -o " + spv) == 0;
}

static std::vector<char> ler_spv(const std::string& caminho) {
    FILE* f = std::fopen(caminho.c_str(), "rb");
    if (f == nullptr)
        return {};
    std::vector<char> out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

int main(int argc, char** argv) {
    const int segundos = (argc > 1) ? std::atoi(argv[1]) : 30;
    if (segundos <= 0) {
        std::printf("uso: g610_mostra [segundos>0]\n");
        return 1;
    }

    // Porta medida do board. SOBRESCREVE SEMPRE: o ssh traz
    // XDG_RUNTIME_DIR=/run/user/1000 (sessao tty) e a sessao grafica
    // real (sway) usa /run/emuos + wayland-1 — os valores alvo sao os
    // mesmos dentro da sessao, portanto sobrescrever nao custa nada.
    // (Medido: com o env parcial do ssh, SDL_Init falha "wayland not
    // available" e grim falha "failed to create display"; com os dois
    // envs certos, 10/10 capturas bem-sucedidas.)
    ::setenv("XDG_RUNTIME_DIR", "/run/emuos", 1);
    ::setenv("WAYLAND_DISPLAY", "wayland-1", 1);
    // EXPLICITO: Wayland directo — sem XWayland a meio (medida do detalhe:
    // e Wayland que decidimos, e o SDL sem isso tenta X primeiro).
    ::setenv("SDL_VIDEODRIVER", "wayland", 1);

    // ── 1. janela (o SDL cria o wl_surface e o papel de top-level) ───────
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("  FALHOU: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    atexit(SDL_Quit);

    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_MODE_EMULATION, "0");
    SDL_Window* janel = SDL_CreateWindow(
        "EdenMaliG610 — primeira imagem",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
        SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN);
    if (janel == nullptr) {
        std::printf("  FALHOU: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(janel, &w, &h);
    std::printf("[1] janela SDL: %dx%d (driver %s)\n", w, h,
                SDL_GetCurrentVideoDriver());

    // ── 2. instance Vulkan com as extensions DO SDL ──────────────────────
    unsigned n_exts = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(janel, &n_exts, nullptr)) {
        std::printf("  FALHOU: SDL_Vulkan_GetInstanceExtensions: %s\n",
                    SDL_GetError());
        return 1;
    }
    std::vector<const char*> exts(n_exts);
    if (!SDL_Vulkan_GetInstanceExtensions(janel, &n_exts, exts.data())) {
        std::printf("  FALHOU: SDL_Vulkan_GetInstanceExtensions(2): %s\n",
                    SDL_GetError());
        return 1;
    }
    std::printf("[2] instance com %u extensions\n", n_exts);

    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "EdenMaliG610-mostra";
    app.apiVersion       = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = n_exts;
    ici.ppEnabledExtensionNames = exts.data();
    VkInstance inst{};
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(janel, inst, &surface)) {
        std::printf("  FALHOU: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return 1;
    }

    // ── 3. device (Mali), fila com present, swapchain ─────────────────────
    std::uint32_t n_phys = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n_phys, nullptr));
    std::vector<VkPhysicalDevice> phys(n_phys);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n_phys, phys.data()));

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    for (VkPhysicalDevice p : phys) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        std::printf("       device: %s\n", pr.deviceName);
        if (std::strstr(pr.deviceName, "Mali"))
            gpu = p;
    }
    if (gpu == VK_NULL_HANDLE) {
        std::printf("  FALHOU: nenhum Mali\n");
        return 1;
    }

    std::uint32_t n_fam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n_fam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n_fam);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n_fam, fams.data());

    std::uint32_t fam = UINT32_MAX;
    for (std::uint32_t i = 0; i < n_fam; ++i) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
            continue;
        VkBool32 pode = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &pode);
        if (pode) {
            fam = i;
            break;
        }
    }
    if (fam == UINT32_MAX) {
        std::printf("  FALHOU: nenhuma fila de graphics com present\n");
        return 1;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = fam;
    dqci.queueCount       = 1;
    dqci.pQueuePriorities = &prio;

    const char* exts_dev[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &dqci;
    dci.enabledExtensionCount   = 1;
    dci.ppEnabledExtensionNames = exts_dev;
    VkDevice dev{};
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &dev));
    VkQueue fila{};
    vkGetDeviceQueue(dev, fam, 0, &fila);

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
    std::printf("       surface: %ux%u  (imagens %u..%u)\n",
                caps.currentExtent.width, caps.currentExtent.height,
                caps.minImageCount, caps.maxImageCount);

    // Wayland: o surface ainda nao tem tamanho (spec: extent especial
    // 0xFFFFFFFF) — usa o tamanho da janela SDL, limitado ao range do
    // device. (Medido: panvk retorna 0xFF.. aqui no primeiro query.)
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu || ext.width == 0u) {
        ext.width  = static_cast<std::uint32_t>(w);
        ext.height = static_cast<std::uint32_t>(h);
        if (ext.width < caps.minImageExtent.width)
            ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height)
            ext.height = caps.minImageExtent.height;
        if (caps.maxImageExtent.width > 0 && ext.width > caps.maxImageExtent.width)
            ext.width = caps.maxImageExtent.width;
        if (caps.maxImageExtent.height > 0 && ext.height > caps.maxImageExtent.height)
            ext.height = caps.maxImageExtent.height;
    }
    std::printf("       extent escolhido: %ux%u\n", ext.width, ext.height);

    std::uint32_t n_fmt = 0;
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n_fmt, nullptr));
    std::vector<VkSurfaceFormatKHR> fmts(n_fmt);
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n_fmt, fmts.data()));
    VkFormat formato = fmts[0].format;
    for (const VkSurfaceFormatKHR& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM ||
            f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            formato = f.format;
            break;
        }
    }
    std::printf("       formato: %d\n", static_cast<int>(formato));

    // ── 4. shaders (o glslang do board — o mesmo compilador da casa) ──────
    if (!compilar(kVert, "vert", "/tmp/g610_mostra.vert.spv") ||
        !compilar(kFrag, "frag", "/tmp/g610_mostra.frag.spv")) {
        std::printf("  FALHOU: o glslang nao compilou os shaders\n");
        return 1;
    }
    const std::vector<char> vspv = ler_spv("/tmp/g610_mostra.vert.spv");
    const std::vector<char> fspv = ler_spv("/tmp/g610_mostra.frag.spv");
    if (vspv.empty() || fspv.empty()) {
        std::printf("  FALHOU: spv vazio\n");
        return 1;
    }

    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vspv.size();
    smci.pCode    = reinterpret_cast<const std::uint32_t*>(vspv.data());
    VkShaderModule vert{};
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &vert));
    smci.codeSize = fspv.size();
    smci.pCode    = reinterpret_cast<const std::uint32_t*>(fspv.data());
    VkShaderModule frag{};
    VK_CHECK(vkCreateShaderModule(dev, &smci, nullptr, &frag));

    // ── 5. render pass + framebuffers ────────────────────────────────────
    VkAttachmentDescription att{};
    att.format         = formato;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &att;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    VkRenderPass rp{};
    VK_CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));

    // ── 5b. swapchain + vistas + framebuffers, RENOVÁVEL ─────────────────
    // Medido: o primeiro acquire no panvk/Wayland chega SUBOPTIMAL
    // (1000001003) porque o swapchain nasce com a superfície ainda não
    // configurada pelo compositor. O dance padrão WSI: quando o
    // compositor reporta OUT_OF_DATE/SUBOPTIMAL, recria-se o swapchain
    // (o render pass não muda; o extent sim — por isso o viewport é
    // dinâmico no passo 6).
    VkSwapchainKHR swap = VK_NULL_HANDLE;
    std::vector<VkImage> imgs;
    std::vector<VkImageView> vistas;
    std::vector<VkFramebuffer> fbs;

    auto destruir_swap = [&]() {
        for (VkFramebuffer fb : fbs)
            vkDestroyFramebuffer(dev, fb, nullptr);
        for (VkImageView v : vistas)
            vkDestroyImageView(dev, v, nullptr);
        if (swap != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(dev, swap, nullptr);
        swap = VK_NULL_HANDLE;
        fbs.clear();
        vistas.clear();
        imgs.clear();
    };

    auto construir_swap = [&]() -> int {
        destruir_swap();

        VkSurfaceCapabilitiesKHR capsNovo{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &capsNovo) !=
            VK_SUCCESS)
            return 1;
        ext = capsNovo.currentExtent;
        if (ext.width == 0xFFFFFFFFu || ext.width == 0u) {
            ext.width  = static_cast<std::uint32_t>(w);
            ext.height = static_cast<std::uint32_t>(h);
            if (ext.width < capsNovo.minImageExtent.width)
                ext.width = capsNovo.minImageExtent.width;
            if (ext.height < capsNovo.minImageExtent.height)
                ext.height = capsNovo.minImageExtent.height;
        }
        std::printf("       (re)swapchain em %ux%u\n", ext.width, ext.height);

        VkSwapchainCreateInfoKHR sci{};
        sci.sType           = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface         = surface;
        sci.minImageCount   = capsNovo.minImageCount > 2 ? capsNovo.minImageCount : 2;
        sci.imageFormat     = formato;
        sci.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        sci.imageExtent     = ext;
        sci.imageArrayLayers = 1;
        sci.imageUsage      = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform    = capsNovo.currentTransform;
        sci.compositeAlpha  = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode     = VK_PRESENT_MODE_FIFO_KHR;  // vsync garantido
        sci.clipped         = VK_TRUE;
        if (vkCreateSwapchainKHR(dev, &sci, nullptr, &swap) != VK_SUCCESS)
            return 1;

        std::uint32_t n_img = 0;
        if (vkGetSwapchainImagesKHR(dev, swap, &n_img, nullptr) != VK_SUCCESS)
            return 1;
        std::printf("       swapchain: %u imagens\n", n_img);
        imgs.resize(n_img);
        vistas.resize(n_img);
        fbs.resize(n_img);
        if (vkGetSwapchainImagesKHR(dev, swap, &n_img, imgs.data()) != VK_SUCCESS)
            return 1;
        for (std::uint32_t i = 0; i < n_img; ++i) {
            VkImageViewCreateInfo ivci{};
            ivci.sType                        = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image                        = imgs[i];
            ivci.viewType                     = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format                       = formato;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.layerCount = 1;
            if (vkCreateImageView(dev, &ivci, nullptr, &vistas[i]) != VK_SUCCESS)
                return 1;

            VkFramebufferCreateInfo fbci{};
            fbci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass      = rp;
            fbci.attachmentCount = 1;
            fbci.pAttachments    = &vistas[i];
            fbci.width           = ext.width;
            fbci.height          = ext.height;
            fbci.layers          = 1;
            if (vkCreateFramebuffer(dev, &fbci, nullptr, &fbs[i]) != VK_SUCCESS)
                return 1;
        }
        return 0;
    };
    if (construir_swap() != 0) {
        std::printf("  FALHOU: vkCreateSwapchainKHR\n");
        return 2;
    }

    // ── 6. pipeline ──────────────────────────────────────────────────────
    VkPipelineShaderStageCreateInfo setas[2]{};
    setas[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    setas[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    setas[0].module = vert;
    setas[0].pName  = "main";
    setas[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    setas[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    setas[1].module = frag;
    setas[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &cba;

    // Viewport/scissor DINÂMICOS: com a renovação do swapchain o extent
    // pode variar — o pipeline não pode depender do tamanho fixo.
    const VkDynamicState dstate[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dstate;

    VkPipelineViewportStateCreateInfo vs{};
    vs.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount  = 1;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType               = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pl{};
    VK_CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    VkGraphicsPipelineCreateInfo ppci{};
    ppci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ppci.stageCount          = 2;
    ppci.pStages             = setas;
    ppci.pVertexInputState   = &vi;
    ppci.pInputAssemblyState = &ia;
    ppci.pViewportState      = &vs;
    ppci.pRasterizationState = &rs;
    ppci.pColorBlendState    = &cb;
    ppci.pMultisampleState   = &ms;
    ppci.pDynamicState       = &dyn;
    ppci.layout              = pl;
    ppci.renderPass          = rp;
    ppci.subpass             = 0;
    VkPipeline pipeline{};
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ppci, nullptr,
                                       &pipeline));

    // ── 7. comandos + o loop de apresentação ──────────────────────────────
    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = fam;
    VkCommandPool pool{};
    VK_CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    VK_CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkSemaphoreCreateInfo semci{};
    semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore semAcabou{}, semPronto{};
    VK_CHECK(vkCreateSemaphore(dev, &semci, nullptr, &semAcabou));
    VK_CHECK(vkCreateSemaphore(dev, &semci, nullptr, &semPronto));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence{};
    VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fence));

    VkClearValue clear{};
    // Fundo igual ao do board (o swaybg usa #101720 — medido no ps).
    clear.color.float32[0] = 0.063f;
    clear.color.float32[1] = 0.09f;
    clear.color.float32[2] = 0.125f;
    clear.color.float32[3] = 1.0f;

    const std::uint64_t t0 = SDL_GetTicks();
    std::uint64_t quadros = 0;
    bool renova = false;

    std::printf("[3] apresentando por %d s (FIFO/vsync) — ESC sai\n", segundos);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
                std::printf("    sair pedido por evento aos %llu ms\n",
                            static_cast<unsigned long long>(SDL_GetTicks() - t0));
                goto terminei;
            }
        }
        if (SDL_GetTicks() - t0 > static_cast<std::uint64_t>(segundos) * 1000u)
            break;

        VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(dev, 1, &fence));
        VK_CHECK(vkResetCommandBuffer(cmd, 0));

        std::uint32_t idx = 0;
        VkResult R = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, semAcabou,
                                           VK_NULL_HANDLE, &idx);
        if (R == VK_ERROR_OUT_OF_DATE_KHR) {
            if (construir_swap() != 0) {
                std::printf("  FALHOU: renovar swapchain apos OUT_OF_DATE\n");
                return 2;
            }
            continue;
        }
        if (R == VK_SUBOPTIMAL_KHR)
            renova = true;  // a imagem serve esta ronda; renova depois
        else if (R != VK_SUCCESS) {
            std::printf("  FALHOU: vkAcquireNextImageKHR -> %d\n",
                        static_cast<int>(R));
            return 2;
        }

        VkCommandBufferBeginInfo bbi{};
        bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bbi));

        VkRenderPassBeginInfo rpbi{};
        rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass        = rp;
        rpbi.framebuffer       = fbs[idx];
        rpbi.renderArea.extent = ext;
        rpbi.clearValueCount   = 1;
        rpbi.pClearValues      = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport vpF{0.0f, 0.0f,
                             static_cast<float>(ext.width),
                             static_cast<float>(ext.height), 0.0f, 1.0f};
        VkRect2D scF{};
        scF.offset.x = 0;
        scF.offset.y = 0;
        scF.extent   = ext;
        vkCmdSetViewport(cmd, 0, 1, &vpF);
        vkCmdSetScissor(cmd, 0, 1, &scF);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));

        const VkPipelineStageFlags espera =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &semAcabou;
        si.pWaitDstStageMask    = &espera;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &semPronto;
        VK_CHECK(vkQueueSubmit(fila, 1, &si, fence));

        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &semPronto;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swap;
        pi.pImageIndices      = &idx;
        VkResult P = vkQueuePresentKHR(fila, &pi);
        ++quadros;
        if (P == VK_ERROR_OUT_OF_DATE_KHR) {
            if (construir_swap() != 0) {
                std::printf("  FALHOU: renovar swapchain apos present\n");
                return 2;
            }
        } else if (P == VK_SUBOPTIMAL_KHR || renova) {
            renova = false;
            if (construir_swap() != 0) {
                std::printf("  FALHOU: renovar swapchain apos SUBOPTIMAL\n");
                return 2;
            }
        } else if (P != VK_SUCCESS) {
            std::printf("  FALHOU: vkQueuePresentKHR -> %d\n",
                        static_cast<int>(P));
            return 2;
        }
    }
terminei:

    vkDeviceWaitIdle(dev);
    std::printf("[4] %llu quadros apresentados em %llu ms\n",
                static_cast<unsigned long long>(quadros),
                static_cast<unsigned long long>(SDL_GetTicks() - t0));

    // ── 8. conclusão ──────────────────────────────────────────────────────
    vkDestroyFence(dev, fence, nullptr);
    vkDestroySemaphore(dev, semPronto, nullptr);
    vkDestroySemaphore(dev, semAcabou, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyPipelineLayout(dev, pl, nullptr);
    for (VkFramebuffer f : fbs)
        vkDestroyFramebuffer(dev, f, nullptr);
    for (VkImageView v : vistas)
        vkDestroyImageView(dev, v, nullptr);
    vkDestroyRenderPass(dev, rp, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroySwapchainKHR(dev, swap, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    SDL_DestroyWindow(janel);
    return 0;
}
