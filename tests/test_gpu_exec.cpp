// Executar o SPIR-V gerado NA MALI-G610 de verdade.
//
// Onde acaba a cadeia do marco 1: SASS -> IR -> GLSL -> SPIR-V, e o
// `spirv-val` a dizer que o binário é válido. Isso prova que o binário é
// válido. NÃO prova que a Mali o execute, nem que dê o resultado certo.
//
// Este ficheiro fecha o ciclo. Ele:
//
//   1. compila o GLSL com o glslang (o mesmo compilador de sempre)
//   2. abre um device Vulkan a sério — sem surface, sem janela, sem Wayland
//   3. cria uma imagem de 64x64 offscreen e um render pass
//   4. escreve as constantes no banco (c[0][0x10]=1.0f, c[0][0x14]=0.5f)
//   5. desenha um triangulo que cobre tudo
//   6. copia a imagem para a memoria do host e lê o pixel
//   7. compara com o valor que o shader DEVE dar
//
// ## O controlo, e porque é que ele é obrigatório
//
// A primeira versão media 0.000000 quando esperava 1.500000, e eu escrevi um
// diagnóstico que dizia "o fragmento correu mas as constantes chegaram
// vazias". Esse diagnóstico estava CERTO e era uma MENTIRA ao mesmo tempo:
// 0.000000 é também o que se lê de um buffer que nunca foi preenchido, porque
// um buffer novo é zero.
//
// Um teste que só tem um caminho não tem diagnóstico nenhum. Por isso este
// desenha DOIS shaders:
//
//   - um CONTROLÓ, escrito à mão, que não toca no banco de constantes e
//     escreve 3.0. Se der 3.0, o pipeline, a geometria, a rasterização e o
//     readback funcionam, e o problema está onde o tradutor toca.
//   - o shader DO TRADUTOR, que tem de dar 1.5.
//
// Se ambos derem 0, o tradutor está inocente e é este ficheiro que está
// errado — que foi exactamente o que aconteceu.
//
// A regra do projecto: não excluir hipótese sem a testar. Um teste que não
// distingue entre "a GPU falhou" e "o meu teste falhou" está a testar uma
// coisa só, e não sabe qual.
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// O mesmo número que o backend usou. Declarado aqui, e não repetido, para
// que o teste e o emissor não possam divergir em silêncio.
std::uint32_t glsl_cbuf_stride_bytes();

namespace {

// ── telemetria ────────────────────────────────────────────────────────────

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    ++g_pass;
    std::printf("  %s: %s\n", ok ? "ok" : "FALHOU", what.c_str());
    if (!ok)
        ++g_fail;
}

// ── Vulkan, com erros nomeados ─────────────────────────────────────────────

// O Vulkan não tem excepções: devolve códigos. Um `VK_CHECK` que imprime a
// linha e o ficheiro é a diferença entre "não funciona" e "falhou no passo 6".
#define VK_CHECK(x)                                                        \
    do {                                                                   \
        const VkResult vk_r_ = (x);                                       \
        if (vk_r_ != VK_SUCCESS) {                                        \
            std::printf("  FALHOU: %s -> VkResult %d  (%s:%d)\n", #x,     \
                        static_cast<int>(vk_r_), __FILE__, __LINE__);      \
            return 1;                                                      \
        }                                                                  \
    } while (0)

constexpr std::uint32_t kLargura = 64;
constexpr std::uint32_t kAltura  = 64;

// O valor de origem do render pass. Escolhido para não colidir com nenhum
// resultado possível do shader, e MUITO diferente de 0 para que "não
// desenhou nada" não se pareça com "calculou zero".
constexpr float kValorClear = -7.0f;

// O que o shader do tradutor tem de dar: R4 = 1.0f * 0.5f + 1.0f.
constexpr float kEsperado = 1.5f;

// O que o shader de controlo tem de dar.
constexpr float kEsperadoControlo = 3.0f;

// `uint u_c[1][256]` em std140: 1 * 4096 bytes. O passo é 16, não 4 — ver
// `cbuf_stride_bytes()` no backend.
constexpr VkDeviceSize kTamUbo = 4096;

// O shader de controlo. Não toca em nada: escreve um literal.
const char* const kGlslControlo = R"(#version 310 es
precision highp float;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(3.0);
}
)";

// O segundo controlo: o MESMO banco de constantes, lido e reescrito sem
// qualquer aritmética. Se der 1.0, o UBO está ligado e a indexação está
// certa, e o problema está na tradução. Se der lixo, o problema é a ligação.
//
// Um teste tem de isolar. Com um só controlo só se sabe que "alguma coisa" está
// mal; com dois sabe-se o quê.
const char* const kGlslControloCbuf = R"(#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 0) uniform Ubo {
    uint u_c[1][256];
};
layout(location = 0) out vec4 o_color;
void main() {
    // uintBitsToFloat, e nao float(): ver `converter` no backend. O valor no
    // banco e' um padrao de bits, e reinterpreta-lo e' o que o guest faz.
    o_color = vec4(uintBitsToFloat(u_c[0][4]));
}
)";

// O vertex shader é o mínimo possível: passa a posição do buffer para o clip
// space. Os três vértices formam um triângulo que cobre o viewport inteiro,
// portanto cada pixel recebe o fragment shader — não há cantos por preencher.
//
// vertices: (-1,-1) (3,-1) (-1,3)
const char* const kGlslVertice = R"(#version 310 es
layout(location = 0) in vec2 a_pos;
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

// Corre um comando e devolve o SEU codigo de saida.
//
// Chamar isto com um `grep -c` e tratar o resultado como contagem é um erro
// que já foi cometido aqui: `grep -c` devolve 0 no codigo de saida quando
// encontra e 1 quando não encontra. Para "o texto aparece no binário" o
// comando certo é `grep -q` com `== 0`.
int run(const std::string& cmd) {
    const int r = std::system((cmd + " 2>/dev/null").c_str());
    return (r == -1) ? -1 : (r >> 8);
}

bool escrever_ficheiro(const std::string& caminho, const std::string& dados) {
    std::FILE* f = std::fopen(caminho.c_str(), "wb");
    if (f == nullptr)
        return false;
    std::fwrite(dados.data(), 1, dados.size(), f);
    fclose(f);
    return true;
}

bool ler_ficheiro(const std::string& caminho, std::vector<char>& out) {
    std::FILE* f = std::fopen(caminho.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<std::size_t>(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Compila um GLSL com o glslang — o mesmo compilador que o marco 1 usou.
bool compilar(const std::string& fonte, const char* estagio,
              const std::string& spv) {
    const std::string in = spv + ".src";
    if (!escrever_ficheiro(in, fonte))
        return false;
    return run("glslangValidator -V --target-env vulkan1.1 -S " +
               std::string(estagio) + " -o " + spv + " " + in) == 0;
}

// Escolhe um tipo de memória que serve para ESTE objecto.
//
// A primeira versão copiava `memoryTypeIndex` da imagem para os buffers:
// `VkMemoryAllocateInfo mai_vbo = mai;`. Não é o mesmo tipo — a imagem e o
// buffer têm requisitos diferentes, e um `memoryTypeIndex` que não serve ao
// objecto não dá erro, dá memória que não é a que se pediu.
std::uint32_t escolher_tipo_memoria(std::uint32_t bits, bool precisa_host,
                                    const VkPhysicalDeviceMemoryProperties& mem,
                                    std::uint32_t preferido) {
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (!(bits & (1u << i)))
            continue;
        const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
        if (precisa_host == ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0))
            return i;
    }
    return preferido;
}

// ── o device, montado uma vez e usado por dois shaders ─────────────────────

struct Ctx {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice         dev = VK_NULL_HANDLE;
    VkQueue          fila = VK_NULL_HANDLE;
    VkCommandPool    pool = VK_NULL_HANDLE;
    std::uint32_t    fam = 0;
    VkPhysicalDeviceMemoryProperties mem{};
    std::uint32_t    tipo_host = UINT32_MAX;

    VkImage        img     = VK_NULL_HANDLE;
    VkDeviceMemory mem_img = VK_NULL_HANDLE;
    VkImageView    vista   = VK_NULL_HANDLE;
    VkRenderPass   rp      = VK_NULL_HANDLE;
    VkFramebuffer  fb      = VK_NULL_HANDLE;
    VkPipelineLayout pl    = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkShaderModule mod_vert = VK_NULL_HANDLE;
    VkBuffer       vbo     = VK_NULL_HANDLE;
    VkDeviceMemory mem_vbo = VK_NULL_HANDLE;
    VkDeviceMemory mem_leitura = VK_NULL_HANDLE;
    VkBuffer       leitura = VK_NULL_HANDLE;
};

constexpr VkDeviceSize kTamLeitura = VkDeviceSize(kLargura) * kAltura * 16;

} // namespace

int main(int argc, char** argv) {
    std::string glsl_tradutor = (argc > 1) ? argv[1] : "/tmp/ia_min.frag";

    // ── 1. device ─────────────────────────────────────────────────────────
    std::printf("[1] abrir o device Vulkan\n");

    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "EdenMaliIA";
    app.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    // SEM extensions e SEM layers. Não há surface: desenha-se para uma
    // imagem offscreen e lê-se a imagem. É o caminho mais curto entre "tenho
    // um shader" e "sei o que a GPU desenhou", e é o mesmo caminho que o NVN
    // vai usar — render para textura, não para janela.
    VkInstance inst{};
    VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));

    std::uint32_t n_phys = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n_phys, nullptr));
    if (n_phys == 0) {
        std::printf("  FALHOU: nenhum device Vulkan\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> phys(n_phys);
    VK_CHECK(vkEnumeratePhysicalDevices(inst, &n_phys, phys.data()));

    Ctx ctx;
    for (VkPhysicalDevice p : phys) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        std::printf("       device %u: %s\n",
                    static_cast<unsigned>(&p - phys.data()), pr.deviceName);
        if (ctx.gpu == VK_NULL_HANDLE || std::strstr(pr.deviceName, "Mali")) {
            ctx.gpu = p;
            if (std::strstr(pr.deviceName, "Mali"))
                break;
        }
    }
    check(ctx.gpu != VK_NULL_HANDLE, "ha um device Vulkan");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.gpu, &props);
    std::printf("       escolhido: %s  (api %u.%u.%u)\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion));

    std::uint32_t n_fam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.gpu, &n_fam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n_fam);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.gpu, &n_fam, fams.data());
    for (std::uint32_t i = 0; i < n_fam; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            ctx.fam = i;
            break;
        }
    }
    check(ctx.fam != UINT32_MAX, "ha uma fila de graficos");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = ctx.fam;
    dqci.queueCount       = 1;
    dqci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &dqci;

    VK_CHECK(vkCreateDevice(ctx.gpu, &dci, nullptr, &ctx.dev));
    vkGetDeviceQueue(ctx.dev, ctx.fam, 0, &ctx.fila);

    vkGetPhysicalDeviceMemoryProperties(ctx.gpu, &ctx.mem);
    const VkMemoryPropertyFlags quer = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t i = 0; i < ctx.mem.memoryTypeCount; ++i) {
        if ((ctx.mem.memoryTypes[i].propertyFlags & quer) == quer) {
            ctx.tipo_host = i;
            break;
        }
    }
    check(ctx.tipo_host != UINT32_MAX, "ha memoria do host coerente");

    VkCommandPoolCreateInfo cpci{};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = ctx.fam;
    cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.dev, &cpci, nullptr, &ctx.pool));

    // ── 2. a imagem onde se desenha ───────────────────────────────────────
    std::printf("\n[2] montar a cena\n");

    VkImageCreateInfo ici_img{};
    ici_img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici_img.imageType     = VK_IMAGE_TYPE_2D;
    ici_img.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
    ici_img.extent        = {kLargura, kAltura, 1};
    ici_img.mipLevels     = 1;
    ici_img.arrayLayers   = 1;
    ici_img.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici_img.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici_img.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici_img.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici_img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.dev, &ici_img, nullptr, &ctx.img));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.dev, ctx.img, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = req.size;
    // A imagem só quer DEVICE_LOCAL. Passar a prioridade do host aqui seria
    // um desperdício.
    mai.memoryTypeIndex = escolher_tipo_memoria(req.memoryTypeBits, false, ctx.mem, 0);
    VK_CHECK(vkAllocateMemory(ctx.dev, &mai, nullptr, &ctx.mem_img));
    VK_CHECK(vkBindImageMemory(ctx.dev, ctx.img, ctx.mem_img, 0));

    VkImageViewCreateInfo ivci{};
    ivci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image                       = ctx.img;
    ivci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format                      = VK_FORMAT_R32G32B32A32_SFLOAT;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.dev, &ivci, nullptr, &ctx.vista));

    VkAttachmentDescription att{};
    att.format         = VK_FORMAT_R32G32B32A32_SFLOAT;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    // TRANSFER_SRC e não PRESENT: o passo seguinte é copiar para o host, e
    // nada aqui vai para um ecrã. Um PRESENT aqui seria uma mentira.
    att.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    // Dependências de subpass EXPLÍCITAS.
    //
    // Sem elas, o Vulkan 1.0 cria dependências implícitas cujo
    // `dstStageMask` é TOP_OF_PIPE/BOTTOM_OF_PIPE — o suficiente para a
    // transição de layout, e NÃO suficiente para a cópia de imagem ficar
    // à espera da escrita na cor. O sintoma medido aqui foi dos mais
    // enganadores que há: cada desenho lia o resultado do ANTERIOR. O
    // primeiro desenho lia 0 (a imagem ainda nunca tinha sido escrita) e o
    // segundo lia o valor do primeiro.
    //
    // Dois desenhos que se leem trocados, com cada um deles individualmente
    // plausível. Nada avisa disto — nem validação, nem `vulkaninfo`, nem
    // nenhum erro. Se não houvesse um shader de controlo a dar 3.0, a
    // conclusão teria sido "o tradutor está errado", e o tradutor estava
    // inocente.
    VkSubpassDependency deps[2]{};

    deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass      = 0;
    deps[0].srcStageMask    = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    deps[0].srcAccessMask   = 0;
    deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // A segunda é a que importa, e a DIREÇÃO está invertida na primeira
    // tentativa: para a escrita do subpass aparecer aos comandos que vêm
    // depois, o `srcSubpass` é o próprio subpass (0) e o `dstSubpass` é o
    // EXTERNAL. Com os dois trocados, a barreira descreve a entrada em vez
    // da saída, e a cópia lê a imagem de antes.
    deps[1].srcSubpass      = 0;
    deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments    = &att;
    rpci.subpassCount    = 1;
    rpci.pSubpasses      = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies   = deps;
    VK_CHECK(vkCreateRenderPass(ctx.dev, &rpci, nullptr, &ctx.rp));

    VkFramebufferCreateInfo fbci{};
    fbci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass      = ctx.rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments    = &ctx.vista;
    fbci.width           = kLargura;
    fbci.height          = kAltura;
    fbci.layers           = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.dev, &fbci, nullptr, &ctx.fb));

    std::vector<char> cod_vert;
    check(compilar(kGlslVertice, "vert", "/tmp/ia_exec.vert.spv") &&
              ler_ficheiro("/tmp/ia_exec.vert.spv", cod_vert),
          "o vertex shader compilou");

    VkShaderModuleCreateInfo smci{};
    smci.sType   = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.pCode   = reinterpret_cast<const std::uint32_t*>(cod_vert.data());
    smci.codeSize = cod_vert.size();
    VK_CHECK(vkCreateShaderModule(ctx.dev, &smci, nullptr, &ctx.mod_vert));

    // ── 3. desenhar um shader e ler o pixel ───────────────────────────────

    // Fazer um desenho e devolver o valor do pixel do meio. `descricao` é o que
    // se imprime para o diagnóstico; `spv` fica de fora para o teste poder
    // inspecionar o binário.
    auto desenhar = [&](const std::string& glsl, const std::string& spv,
                        const std::string& descricao) -> float {
        if (!compilar(glsl, "frag", spv)) {
            std::printf("  FALHOU: %s nao compilou\n", descricao.c_str());
            ++g_pass; ++g_fail;
            return 0.f;
        }
        std::vector<char> cod;
        if (!ler_ficheiro(spv, cod) || cod.empty()) {
            std::printf("  FALHOU: %s nao produziu SPIR-V\n", descricao.c_str());
            ++g_pass; ++g_fail;
            return 0.f;
        }

        VkShaderModule mod_frag{};
        smci.pCode    = reinterpret_cast<const std::uint32_t*>(cod.data());
        smci.codeSize = cod.size();
        if (vkCreateShaderModule(ctx.dev, &smci, nullptr, &mod_frag) != VK_SUCCESS) {
            std::printf("  FALHOU: a Mali recusou o modulo de %s\n",
                        descricao.c_str());
            ++g_pass; ++g_fail;
            return 0.f;
        }

        // O banco de constantes: UMA binding, uniform buffer.
        VkDescriptorSetLayoutBinding bind{};
        bind.binding         = 0;
        bind.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bind.descriptorCount = 1;
        bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 1;
        dslci.pBindings    = &bind;
        VkDescriptorSetLayout dsl{};
        vkCreateDescriptorSetLayout(ctx.dev, &dslci, nullptr, &dsl);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts    = &dsl;
        VkPipelineLayout pl{};
        vkCreatePipelineLayout(ctx.dev, &plci, nullptr, &pl);

        VkPipelineShaderStageCreateInfo est[2]{};
        est[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        est[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        est[0].module = ctx.mod_vert;
        est[0].pName  = "main";
        est[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        est[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        est[1].module = mod_frag;
        est[1].pName  = "main";

        // Os três vértices do triângulo que cobre tudo.
        const float triangulo[6] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};

        VkVertexInputBindingDescription vib{};
        vib.binding   = 0;
        vib.stride    = 2 * sizeof(float);
        vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription via{};
        via.location = 0;
        via.binding  = 0;
        via.format   = VK_FORMAT_R32G32_SFLOAT;

        VkPipelineVertexInputStateCreateInfo vis{};
        vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vis.vertexBindingDescriptionCount = 1;
        vis.pVertexBindingDescriptions = &vib;
        vis.vertexAttributeDescriptionCount = 1;
        vis.pVertexAttributeDescriptions = &via;

        VkPipelineInputAssemblyStateCreateInfo ias{};
        ias.sType                 = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ias.topology              = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ias.primitiveRestartEnable = VK_FALSE;

        VkViewport viewport{0.f, 0.f, float(kLargura), float(kAltura), 0.f, 1.f};
        VkRect2D scissor{{0, 0}, {kLargura, kAltura}};

        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.pViewports    = &viewport;
        vps.scissorCount  = 1;
        vps.pScissors     = &scissor;

        VkPipelineRasterizationStateCreateInfo ras{};
        ras.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        ras.polygonMode = VK_POLYGON_MODE_FILL;
        ras.cullMode    = VK_CULL_MODE_NONE;
        ras.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        ras.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo msa{};
        msa.sType               = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        msa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments    = &cba;

        VkGraphicsPipelineCreateInfo gpci{};
        gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount          = 2;
        gpci.pStages             = est;
        gpci.pVertexInputState   = &vis;
        gpci.pInputAssemblyState = &ias;
        gpci.pViewportState      = &vps;
        gpci.pRasterizationState  = &ras;
        gpci.pMultisampleState    = &msa;
        gpci.pColorBlendState    = &cbs;
        gpci.layout              = pl;
        gpci.renderPass          = ctx.rp;
        gpci.subpass             = 0;

        VkPipeline pipe{};
        const VkResult r_pipe = vkCreateGraphicsPipelines(ctx.dev, VK_NULL_HANDLE, 1,
                                                          &gpci, nullptr, &pipe);
        if (r_pipe != VK_SUCCESS) {
            std::printf("  FALHOU: a Mali recusou o pipeline de %s (VkResult %d)\n",
                        descricao.c_str(), static_cast<int>(r_pipe));
            ++g_pass; ++g_fail;
            vkDestroyShaderModule(ctx.dev, mod_frag, nullptr);
            return 0.f;
        }

        // ── o banco de constantes ─────────────────────────────────────────
        // c[0][0x10] = 1.0f, c[0][0x14] = 0.5f.
        //
        // Byte = palavra * 4, e só. A palavra do Maxwell (`c[0][0x10]`) vira
        // byte no IR (×4, no `v_cbuf` do frontend), e o byte vira índice no
        // GLSL (÷16, o passo do std140). São duas conversões em direcções
        // opostas, e encadá-las erradas dá o resultado que dá: um banco
        // escrito no sítio errado, lido no sítio certo, e um pixel preto sem
        // nenhum erro pelo caminho. Já aconteceu.
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = kTamUbo;
        bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer ubo{};
        vkCreateBuffer(ctx.dev, &bci, nullptr, &ubo);

        VkMemoryRequirements req_ubo{};
        vkGetBufferMemoryRequirements(ctx.dev, ubo, &req_ubo);
        VkMemoryAllocateInfo mai_ubo{};
        mai_ubo.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai_ubo.allocationSize = req_ubo.size;
        mai_ubo.memoryTypeIndex = escolher_tipo_memoria(req_ubo.memoryTypeBits, true,
                                                        ctx.mem, ctx.tipo_host);
        // O tipo escolhido TEM de servir ao objecto. Se `escolher_tipo_memoria`
        // não encontrou nenhum e devolveu o preferido, isso é um bug — e um
        // `vkAllocateMemory` com um tipo inválido não dá erro, dá lixo.
        check((req_ubo.memoryTypeBits & (1u << mai_ubo.memoryTypeIndex)) != 0,
              "o tipo de memoria do UBO serve para o UBO");

        VkDeviceMemory mem_ubo{};
        VK_CHECK(vkAllocateMemory(ctx.dev, &mai_ubo, nullptr, &mem_ubo));
        VK_CHECK(vkBindBufferMemory(ctx.dev, ubo, mem_ubo, 0));

        void* p_ubo = nullptr;
        VK_CHECK(vkMapMemory(ctx.dev, mem_ubo, 0, kTamUbo, 0, &p_ubo));
        auto* base = static_cast<char*>(p_ubo);
        std::memset(base, 0, kTamUbo);

        constexpr unsigned kBytesPorPalavraMaxwell = 4;
        auto escrever_constante = [&](unsigned palavra, float valor) {
            const std::size_t byte = std::size_t(palavra) * kBytesPorPalavraMaxwell;
            if (byte + sizeof(float) > kTamUbo)
                return;
            std::memcpy(base + byte, &valor, sizeof valor);
        };
        escrever_constante(0x10, 1.0f);
        escrever_constante(0x14, 0.5f);
        vkUnmapMemory(ctx.dev, mem_ubo);

        // Confirma que a escrita ficou lá. Um `memcpy` para memória de host
        // que não é a que se pensou não dá erro — e o shader lê zero.
        void* p_ubo2 = nullptr;
        VK_CHECK(vkMapMemory(ctx.dev, mem_ubo, 0, kTamUbo, 0, &p_ubo2));
        // `p_ubo2` é `void*` vindo do mapa; a soma com `+` faz pointer
        // arithmetic de 1 byte, e o cast tem de ser para `char*` ANTES do
        // offset. Somar a um `void*` não compila — e o compilador a dizer
        // isso é uma cortesia, não um pormenor.
        float leu = 0.f;
        std::memcpy(&leu,
                    static_cast<const char*>(p_ubo2) + 0x10 * kBytesPorPalavraMaxwell,
                    sizeof leu);
        check(leu == 1.0f, "c[0][0x10] esta mesmo a 1.0f na memoria do host");
        vkUnmapMemory(ctx.dev, mem_ubo);

        VkDescriptorPoolSize ps{};
        ps.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ps.descriptorCount = 1;

        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = 1;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &ps;
        VkDescriptorPool dp{};
        vkCreateDescriptorPool(ctx.dev, &dpci, nullptr, &dp);

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool    = dp;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &dsl;
        VkDescriptorSet dset{};
        VK_CHECK(vkAllocateDescriptorSets(ctx.dev, &dsai, &dset));

        VkDescriptorBufferInfo dbi{};
        dbi.buffer = ubo;
        dbi.range  = kTamUbo;

        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = dset;
        w.dstBinding      = 0;
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo     = &dbi;
        vkUpdateDescriptorSets(ctx.dev, 1, &w, 0, nullptr);

        // ── o vertex buffer ───────────────────────────────────────────────
        if (ctx.vbo == VK_NULL_HANDLE) {
            VkBufferCreateInfo vbc = bci;
            vbc.size  = sizeof(triangulo);
            vbc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            vkCreateBuffer(ctx.dev, &vbc, nullptr, &ctx.vbo);

            VkMemoryRequirements req_vbo{};
            vkGetBufferMemoryRequirements(ctx.dev, ctx.vbo, &req_vbo);
            VkMemoryAllocateInfo mai_vbo{};
            mai_vbo.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai_vbo.allocationSize = req_vbo.size;
            mai_vbo.memoryTypeIndex = escolher_tipo_memoria(req_vbo.memoryTypeBits,
                                                            true, ctx.mem,
                                                            ctx.tipo_host);
            check((req_vbo.memoryTypeBits & (1u << mai_vbo.memoryTypeIndex)) != 0,
                  "o tipo de memoria do vertex buffer serve para ele");
            VK_CHECK(vkAllocateMemory(ctx.dev, &mai_vbo, nullptr, &ctx.mem_vbo));
            VK_CHECK(vkBindBufferMemory(ctx.dev, ctx.vbo, ctx.mem_vbo, 0));

            void* pv = nullptr;
            VK_CHECK(vkMapMemory(ctx.dev, ctx.mem_vbo, 0, sizeof(triangulo), 0, &pv));
            std::memcpy(pv, triangulo, sizeof(triangulo));
            vkUnmapMemory(ctx.dev, ctx.mem_vbo);
        }

        // ── o buffer de leitura ───────────────────────────────────────────
        if (ctx.leitura == VK_NULL_HANDLE) {
            VkBufferCreateInfo rbci = bci;
            rbci.size  = kTamLeitura;
            rbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(ctx.dev, &rbci, nullptr, &ctx.leitura);

            VkMemoryRequirements req_rb{};
            vkGetBufferMemoryRequirements(ctx.dev, ctx.leitura, &req_rb);
            VkMemoryAllocateInfo mai_rb{};
            mai_rb.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai_rb.allocationSize = req_rb.size;
            mai_rb.memoryTypeIndex = escolher_tipo_memoria(req_rb.memoryTypeBits,
                                                            true, ctx.mem,
                                                            ctx.tipo_host);
            check((req_rb.memoryTypeBits & (1u << mai_rb.memoryTypeIndex)) != 0,
                  "o tipo de memoria do buffer de leitura serve para ele");
            VK_CHECK(vkAllocateMemory(ctx.dev, &mai_rb, nullptr, &ctx.mem_leitura));
            VK_CHECK(vkBindBufferMemory(ctx.dev, ctx.leitura, ctx.mem_leitura, 0));
        }

        // ── gravar ────────────────────────────────────────────────────────
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = ctx.pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        VK_CHECK(vkAllocateCommandBuffers(ctx.dev, &cbai, &cmd));

        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));

        VkClearValue clear{};
        clear.color.float32[0] = kValorClear;
        clear.color.float32[1] = kValorClear;
        clear.color.float32[2] = kValorClear;
        clear.color.float32[3] = kValorClear;

        VkRenderPassBeginInfo rpbi{};
        rpbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass      = ctx.rp;
        rpbi.framebuffer     = ctx.fb;
        rpbi.renderArea      = {{0, 0}, {kLargura, kAltura}};
        rpbi.clearValueCount = 1;
        rpbi.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
                                &dset, 0, nullptr);
        const VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ctx.vbo, &off);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        VkBufferImageCopy cpy{};
        cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.imageSubresource.layerCount = 1;
        cpy.imageExtent = {kLargura, kAltura, 1};
        vkCmdCopyImageToBuffer(cmd, ctx.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               ctx.leitura, 1, &cpy);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence{};
        VK_CHECK(vkCreateFence(ctx.dev, &fci, nullptr, &fence));

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;
        if (vkQueueSubmit(ctx.fila, 1, &si, fence) != VK_SUCCESS) {
            std::printf("  FALHOU: %s — a fila recusou o comando\n",
                        descricao.c_str());
            ++g_pass; ++g_fail;
        }
        VK_CHECK(vkWaitForFences(ctx.dev, 1, &fence, VK_TRUE, UINT64_MAX));

        void* p_rb = nullptr;
        VK_CHECK(vkMapMemory(ctx.dev, ctx.mem_leitura, 0, kTamLeitura, 0, &p_rb));
        const float* px = static_cast<const float*>(p_rb);
        const std::size_t meio = (kAltura / 2) * kLargura + (kLargura / 2);

        std::printf("       %-22s centro = %.6f  cantos =", descricao.c_str(),
                    px[meio * 4]);
        const int cantos[4][2] = {{1, 1}, {kLargura - 2, 1}, {1, kAltura - 2},
                                  {kLargura - 2, kAltura - 2}};
        bool cantos_ok = true;
        for (auto& c : cantos) {
            const std::size_t cp = std::size_t(c[1]) * kLargura + c[0];
            std::printf(" %.4f", px[cp * 4]);
            if (std::fabs(px[cp * 4] - px[meio * 4]) > 1e-5f)
                cantos_ok = false;
        }
        std::printf("\n");
        check(cantos_ok, "  o triangulo cobre os quatro cantos");

        const float r = px[meio * 4];
        vkUnmapMemory(ctx.dev, ctx.mem_leitura);

        vkDestroyFence(ctx.dev, fence, nullptr);
        vkFreeCommandBuffers(ctx.dev, ctx.pool, 1, &cmd);
        vkDestroyDescriptorPool(ctx.dev, dp, nullptr);
        vkDestroyBuffer(ctx.dev, ubo, nullptr);
        vkFreeMemory(ctx.dev, mem_ubo, nullptr);
        vkDestroyPipeline(ctx.dev, pipe, nullptr);
        vkDestroyPipelineLayout(ctx.dev, pl, nullptr);
        vkDestroyDescriptorSetLayout(ctx.dev, dsl, nullptr);
        vkDestroyShaderModule(ctx.dev, mod_frag, nullptr);
        return r;
    };

    // ── 4. o shader DE CONTROLO ───────────────────────────────────────────
    // Escreve um literal e não toca em nada. Se der 3.0, então o pipeline, a
    // geometria, a rasterização e o readback estão bons — e o problema está
    // onde o tradutor toca. Se der 0, é este ficheiro que está errado.
    std::printf("\n[3] shader de CONTROLO (escrito a mao, sem o banco)\n");
    const float controlo = desenhar(kGlslControlo, "/tmp/ia_exec.controlo.spv",
                                    "controlo");
    check(std::fabs(controlo - kEsperadoControlo) < 1e-5f,
          "o shader de controlo devolveu 3.0 — a cena inteira funciona");

    // ── 4b. o CONTROLO DO BANCO ───────────────────────────────────────────
    // A mesma declaração de banco, o mesmo UBO, a mesma escrita — mas sem
    // aritmética nenhuma. Se isto não der 1.0, o tradutor está inocente.
    std::printf("\n[3b] shader de CONTROLO do banco (le c[0][0x10], nada mais)\n");
    const float controlo_cbuf = desenhar(kGlslControloCbuf,
                                         "/tmp/ia_exec.controlo_cbuf.spv",
                                         "controlo do banco");
    check(std::fabs(controlo_cbuf - 1.0f) < 1e-5f,
          "o banco de constantes chega ao shader: c[0][0x10] leu 1.0");
    if (std::fabs(controlo_cbuf - 1.0f) >= 1e-5f) {
        std::printf("       (leu %.6f)\n", controlo_cbuf);
    }

    // ── 5. o shader DO TRADUTOR ───────────────────────────────────────────
    std::string glsl;
    {
        std::vector<char> lido;
        if (!ler_ficheiro(glsl_tradutor, lido)) {
            std::printf("  FALHOU: nao li o GLSL de %s\n"
                        "           corre primeiro o ia_translate_test.\n",
                        glsl_tradutor.c_str());
            ++g_pass; ++g_fail;
            return 1;
        }
        glsl.assign(lido.begin(), lido.end());
    }

    std::printf("\n[4] shader do TRADUTOR (%s)\n", glsl_tradutor.c_str());
    std::printf("       passo do array de constantes: %u bytes\n",
                glsl_cbuf_stride_bytes());
    const float obtido = desenhar(glsl, "/tmp/ia_exec.frag.spv", "tradutor");

    std::printf("       previsto = %.6f\n", kEsperado);

    // O passo do array, MEDIDO no binário que o glslang acabou de produzir.
    // A regra está no backend e o teste não a repete: confirma-a. Se o layout
    // mudar um dia, isto falha com um número em vez de a imagem ficar preta e
    // ninguém perceber porquê.
    check(run("spirv-dis /tmp/ia_exec.frag.spv | grep -q 'ArrayStride 16'") == 0,
          "o SPIR-V medido tem ArrayStride 16 (o passo que o backend usou)");

    check(std::fabs(obtido - kEsperado) < 1e-5f,
          "R = 1.0 * 0.5 + 1.0 = 1.5  (a Mali calculou o que o shader diz)");
    check(std::fabs(obtido - kValorClear) > 1e-5f,
          "o fragmento chegou a correr (o pixel nao e' o valor do clear)");

    // O diagnóstico certo, agora que o controlo diz qual e a parte culpada.
    if (std::fabs(obtido - kEsperado) >= 1e-5f) {
        if (std::fabs(obtido) < 1e-5f && std::fabs(controlo - kEsperadoControlo) < 1e-5f) {
            std::printf("\n  DIAGNOSTICO: o controlo deu %.3f (bem) e o tradutor deu 0.\n"
                        "       A cena funciona. Logo o banco de constantes nao\n"
                        "       chega ao shader. O problema e' do layout ou da\n"
                        "       ligacao do UBO, nao da traducao.\n", controlo);
        } else if (std::fabs(obtido) < 1e-5f) {
            std::printf("\n  DIAGNOSTICO: o tradutor deu 0 e o controlo deu %.3f.\n"
                        "       A cena funciona; o shader do tradutor corre mas\n"
                        "       leu zeros do banco.\n", controlo);
        } else {
            std::printf("\n  DIAGNOSTICO: o tradutor deu %.6f, o controlo deu %.6f.\n"
                        "       A cena funciona e o valor está errado mas não zero:\n"
                        "       o shader corre com o banco ligado. Olhar para o\n"
                        "       indice do banco, no GLSL.\n", obtido, controlo);
        }
    }

    vkDeviceWaitIdle(ctx.dev);
    vkDestroyBuffer(ctx.dev, ctx.leitura, nullptr);
    vkFreeMemory(ctx.dev, ctx.mem_leitura, nullptr);
    vkDestroyBuffer(ctx.dev, ctx.vbo, nullptr);
    vkFreeMemory(ctx.dev, ctx.mem_vbo, nullptr);
    vkDestroyShaderModule(ctx.dev, ctx.mod_vert, nullptr);
    vkDestroyFramebuffer(ctx.dev, ctx.fb, nullptr);
    vkDestroyRenderPass(ctx.dev, ctx.rp, nullptr);
    vkDestroyImageView(ctx.dev, ctx.vista, nullptr);
    vkDestroyImage(ctx.dev, ctx.img, nullptr);
    vkFreeMemory(ctx.dev, ctx.mem_img, nullptr);
    vkDestroyCommandPool(ctx.dev, ctx.pool, nullptr);
    vkDestroyDevice(ctx.dev, nullptr);
    vkDestroyInstance(inst, nullptr);

    std::printf("\n%d verificacoes, %d falhas\n", g_pass, g_fail);
    if (g_fail == 0) {
        std::printf("\nA cadeia Maxwell -> IR -> GLSL -> SPIR-V -> Mali-G610 esta\n"
                    "medida de ponta a ponta em %s.\n", props.deviceName);
    }
    return g_fail == 0 ? 0 : 1;
}