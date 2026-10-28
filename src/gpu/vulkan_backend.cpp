// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — implementação do backend Vulkan mínimo (Clear only).
#include "g610/gpu/vulkan_backend.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace g610::gpu {

namespace {
#define VK_CHECK_RET(expr, err, msg)                                           \
    do {                                                                      \
        VkResult _r = (expr);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            (err) = std::string(msg) + " (VkResult " + std::to_string(_r) +  \
                    ")";                                                      \
            return false;                                                     \
        }                                                                     \
    } while (0)
}  // namespace

VulkanBackend::VulkanBackend() = default;

VulkanBackend::~VulkanBackend() {
    shutdown();
}

bool VulkanBackend::create_instance(std::string& error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "EdenMaliG610";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        error = "vkCreateInstance falhou (VkResult " + std::to_string(r) + ")";
        return false;
    }

    std::uint32_t nphys = 0;
    r = vkEnumeratePhysicalDevices(instance_, &nphys, nullptr);
    if (r != VK_SUCCESS || nphys == 0) {
        error = "nenhum dispositivo físico Vulkan";
        return false;
    }
    std::vector<VkPhysicalDevice> phys(nphys);
    r = vkEnumeratePhysicalDevices(instance_, &nphys, phys.data());
    if (r != VK_SUCCESS) {
        error = "vkEnumeratePhysicalDevices falhou";
        return false;
    }

    // Preferir Mali se existir.
    physical_ = phys[0];
    for (auto p : phys) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(p, &pr);
        if (std::strstr(pr.deviceName, "Mali")) {
            physical_ = p;
            break;
        }
    }
    return true;
}

bool VulkanBackend::create_device(std::string& error) {
    std::uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, nullptr);
    if (nfam == 0) {
        error = "sem famílias de filas";
        return false;
    }
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nfam, fams.data());

    queue_family_ = UINT32_MAX;
    for (std::uint32_t i = 0; i < nfam; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queue_family_ = i;
            break;
        }
    }
    if (queue_family_ == UINT32_MAX) {
        for (std::uint32_t i = 0; i < nfam; ++i) {
            if (fams[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                queue_family_ = i;
                break;
            }
        }
    }
    if (queue_family_ == UINT32_MAX) {
        error = "sem fila gráfica/transfer";
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci{};
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = queue_family_;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;

    VkResult r = vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        error = "vkCreateDevice falhou (VkResult " + std::to_string(r) + ")";
        return false;
    }
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    // Propriedades de memória.
    // (armazenadas implicitamente via vkGetPhysicalDeviceMemoryProperties quando necessário)
    return true;
}

std::uint32_t VulkanBackend::find_memory_type(std::uint32_t type_bits,
                                             VkMemoryPropertyFlags wanted) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem);
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    // fallback: qualquer tipo compatível
    for (std::uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (type_bits & (1u << i))
            return i;
    }
    return UINT32_MAX;
}

bool VulkanBackend::create_image(std::string& error) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {kW, kH, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = vkCreateImage(device_, &ici, nullptr, &image_);
    if (r != VK_SUCCESS) {
        error = "vkCreateImage falhou";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image_, &req);
    std::uint32_t mt = find_memory_type(req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX)
        mt = find_memory_type(req.memoryTypeBits, 0);
    if (mt == UINT32_MAX) {
        error = "sem tipo de memória para a imagem";
        return false;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    r = vkAllocateMemory(device_, &mai, nullptr, &image_memory_);
    if (r != VK_SUCCESS) {
        error = "vkAllocateMemory (imagem) falhou";
        return false;
    }
    r = vkBindImageMemory(device_, image_, image_memory_, 0);
    if (r != VK_SUCCESS) {
        error = "vkBindImageMemory falhou";
        return false;
    }
    return true;
}

bool VulkanBackend::create_readback_buffer(std::string& error) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = kW * kH * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult r = vkCreateBuffer(device_, &bci, nullptr, &readback_);
    if (r != VK_SUCCESS) {
        error = "vkCreateBuffer falhou";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, readback_, &req);
    std::uint32_t mt = find_memory_type(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        error = "sem tipo de memória host-visible para o buffer";
        return false;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    r = vkAllocateMemory(device_, &mai, nullptr, &readback_memory_);
    if (r != VK_SUCCESS) {
        error = "vkAllocateMemory (buffer) falhou";
        return false;
    }
    r = vkBindBufferMemory(device_, readback_, readback_memory_, 0);
    if (r != VK_SUCCESS) {
        error = "vkBindBufferMemory falhou";
        return false;
    }
    return true;
}

bool VulkanBackend::create_command_pool(std::string& error) {
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = queue_family_;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkResult r = vkCreateCommandPool(device_, &cpci, nullptr, &command_pool_);
    if (r != VK_SUCCESS) {
        error = "vkCreateCommandPool falhou";
        return false;
    }
    return true;
}

bool VulkanBackend::init(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_)
        return true;

    if (!create_instance(error))
        return false;
    if (!create_device(error)) {
        shutdown();
        return false;
    }
    if (!create_image(error)) {
        shutdown();
        return false;
    }
    if (!create_readback_buffer(error)) {
        shutdown();
        return false;
    }
    if (!create_command_pool(error)) {
        shutdown();
        return false;
    }

    ready_ = true;
    return true;
}

void VulkanBackend::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    if (command_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (readback_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, readback_, nullptr);
        readback_ = VK_NULL_HANDLE;
    }
    if (readback_memory_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, readback_memory_, nullptr);
        readback_memory_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (image_memory_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image_memory_, nullptr);
        image_memory_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    ready_ = false;
}

ClearResult VulkanBackend::clear_image() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearResult result;
    if (!ready_) {
        result.error = "backend não inicializado";
        return result;
    }

    // Command buffer.
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    VkResult r = vkAllocateCommandBuffers(device_, &cbai, &cmd);
    if (r != VK_SUCCESS) {
        result.error = "vkAllocateCommandBuffers falhou";
        return result;
    }

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    r = vkBeginCommandBuffer(cmd, &cbbi);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        result.error = "vkBeginCommandBuffer falhou";
        return result;
    }

    // transition undefined -> transfer dst
    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = image_;
    b1.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b1.subresourceRange.levelCount = 1;
    b1.subresourceRange.layerCount = 1;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b1);

    VkClearColorValue clear{};
    clear.float32[0] = kR;
    clear.float32[1] = kG;
    clear.float32[2] = kB;
    clear.float32[3] = kA;

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    // transition transfer dst -> transfer src
    VkImageMemoryBarrier b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = image_;
    b2.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b2.subresourceRange.levelCount = 1;
    b2.subresourceRange.layerCount = 1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b2);

    VkBufferImageCopy cpy{};
    cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cpy.imageSubresource.layerCount = 1;
    cpy.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &cpy);

    r = vkEndCommandBuffer(cmd);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        result.error = "vkEndCommandBuffer falhou";
        return result;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence{};
    r = vkCreateFence(device_, &fci, nullptr, &fence);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        result.error = "vkCreateFence falhou";
        return result;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r = vkQueueSubmit(queue_, 1, &si, fence);
    if (r != VK_SUCCESS) {
        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        result.error = "vkQueueSubmit falhou";
        return result;
    }

    r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    if (r != VK_SUCCESS) {
        result.error = "vkWaitForFences falhou";
        return result;
    }

    // Readback + verificação numérica.
    void* p = nullptr;
    r = vkMapMemory(device_, readback_memory_, 0, kW * kH * 4, 0, &p);
    if (r != VK_SUCCESS) {
        result.error = "vkMapMemory falhou";
        return result;
    }

    const unsigned char* px = static_cast<const unsigned char*>(p);
    std::uint32_t checked = 0;
    std::uint32_t mismatched = 0;
    for (std::uint32_t i = 0; i < kW * kH; ++i) {
        unsigned char rr = px[i * 4 + 0];
        unsigned char gg = px[i * 4 + 1];
        unsigned char bb = px[i * 4 + 2];
        unsigned char aa = px[i * 4 + 3];
        ++checked;
        // 0.25*255=63.75->64, 0.5*255=127.5->128 (round), 0.75*255=191.25->191, 1.0->255
        if (!(rr >= 60 && rr <= 70 && gg >= 124 && gg <= 132 && bb >= 187 && bb <= 196 && aa == 255)) {
            ++mismatched;
        }
    }
    vkUnmapMemory(device_, readback_memory_);

    result.pixels_checked = checked;
    result.pixels_mismatched = mismatched;
    result.ok = (mismatched == 0);
    if (!result.ok) {
        result.error = "pixels não correspondem à cor de clear";
    }
    return result;
}

PipelineDrawResult VulkanBackend::draw_vertex_buffer(const std::vector<char>& vert_spv,
                                                   const std::vector<char>& frag_spv,
                                                   const void* vertex_data,
                                                   std::size_t vertex_count,
                                                   std::size_t stride_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineDrawResult result;
    if (!ready_) {
        result.error = "backend não inicializado";
        return result;
    }
    if (vert_spv.empty() || frag_spv.empty()) {
        result.error = "SPIR-V vazio";
        return result;
    }

    // Image view (cria se necessário — reutiliza a imagem existente).
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = image_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    VkResult r = vkCreateImageView(device_, &ivci, nullptr, &view);
    if (r != VK_SUCCESS) {
        result.error = "vkCreateImageView falhou";
        return result;
    }

    // Render pass: clear para preto, store.
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    VkRenderPass rp = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device_, &rpci, nullptr, &rp);
    if (r != VK_SUCCESS) {
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateRenderPass falhou";
        return result;
    }

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = kW;
    fbci.height = kH;
    fbci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    r = vkCreateFramebuffer(device_, &fbci, nullptr, &fb);
    if (r != VK_SUCCESS) {
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateFramebuffer falhou";
        return result;
    }

    // Shader modules.
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vert_spv.size();
    smci.pCode = reinterpret_cast<const std::uint32_t*>(vert_spv.data());
    VkShaderModule vs = VK_NULL_HANDLE;
    r = vkCreateShaderModule(device_, &smci, nullptr, &vs);
    if (r != VK_SUCCESS) {
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateShaderModule (vert) falhou";
        return result;
    }
    smci.codeSize = frag_spv.size();
    smci.pCode = reinterpret_cast<const std::uint32_t*>(frag_spv.data());
    VkShaderModule fs = VK_NULL_HANDLE;
    r = vkCreateShaderModule(device_, &smci, nullptr, &fs);
    if (r != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateShaderModule (frag) falhou";
        return result;
    }

    // Vertex buffer com 3 vértices (triângulo).
    if (vertex_data == nullptr || vertex_count == 0 || stride_bytes == 0) {
        result.error = "buffer de vertices invalido";
        // cleanup of already-created resources
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        return result;
    }
    const std::size_t total_bytes = vertex_count * stride_bytes;
    VkBuffer vbo = VK_NULL_HANDLE;
    VkBufferCreateInfo vbc{};
    vbc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbc.size = total_bytes;
    vbc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(device_, &vbc, nullptr, &vbo);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(device_, vbo, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateBuffer falhou";
        return result;
    }
    VkMemoryRequirements vreq{};
    vkGetBufferMemoryRequirements(device_, vbo, &vreq);
    std::uint32_t vmt = find_memory_type(vreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vmt == UINT32_MAX) vmt = find_memory_type(vreq.memoryTypeBits, 0);
    VkMemoryAllocateInfo vmai{};
    vmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    vmai.allocationSize = vreq.size;
    vmai.memoryTypeIndex = vmt;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device_, &vmai, nullptr, &vmem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkAllocateMemory (vbo) falhou";
        return result;
    }
    vkBindBufferMemory(device_, vbo, vmem, 0);
    void* vp = nullptr;
    r = vkMapMemory(device_, vmem, 0, total_bytes, 0, &vp);
    if (r != VK_SUCCESS || vp == nullptr) {
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkMapMemory (vbo) falhou";
        return result;
    }
    std::memcpy(vp, vertex_data, total_bytes);
    vkUnmapMemory(device_, vmem);

    // Pipeline layout (sem descriptors).
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device_, &plci, nullptr, &pl);
    if (r != VK_SUCCESS) {
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreatePipelineLayout falhou";
        return result;
    }

    // Graphics pipeline.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vib{};
    vib.binding = 0;
    vib.stride = 2 * sizeof(float);
    vib.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription via{};
    via.location = 0;
    via.binding = 0;
    via.format = VK_FORMAT_R32G32_SFLOAT;
    via.offset = 0;

    VkPipelineVertexInputStateCreateInfo vis{};
    vis.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vis.vertexBindingDescriptionCount = 1;
    vis.pVertexBindingDescriptions = &vib;
    vis.vertexAttributeDescriptionCount = 1;
    vis.pVertexAttributeDescriptions = &via;

    VkPipelineInputAssemblyStateCreateInfo ias{};
    ias.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp2{0.f, 0.f, float(kW), float(kH), 0.f, 1.f};
    VkRect2D scissor{{0, 0}, {kW, kH}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp2;
    vps.scissorCount = 1;
    vps.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo ras{};
    ras.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    ras.polygonMode = VK_POLYGON_MODE_FILL;
    ras.cullMode = VK_CULL_MODE_NONE;
    ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    ras.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msa{};
    msa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbs{};
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vis;
    gpci.pInputAssemblyState = &ias;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &ras;
    gpci.pMultisampleState = &msa;
    gpci.pColorBlendState = &cbs;
    gpci.layout = pl;
    gpci.renderPass = rp;
    gpci.subpass = 0;

    VkPipeline pipe = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe);
    if (r != VK_SUCCESS) {
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateGraphicsPipelines falhou";
        return result;
    }

    // Command buffer.
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device_, &cbai, &cmd);
    if (r != VK_SUCCESS) {
        // cleanup
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkAllocateCommandBuffers falhou";
        return result;
    }

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    r = vkBeginCommandBuffer(cmd, &cbbi);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkBeginCommandBuffer falhou";
        return result;
    }

    VkClearValue cv{};
    cv.color.float32[0] = 0.f;
    cv.color.float32[1] = 0.f;
    cv.color.float32[2] = 0.f;
    cv.color.float32[3] = 1.f;
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea = {{0, 0}, {kW, kH}};
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbo, &off);
    vkCmdDraw(cmd, static_cast<std::uint32_t>(vertex_count), 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // transition finalLayout (TRANSFER_SRC_OPTIMAL) already; copy.
    VkBufferImageCopy cpy{};
    cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cpy.imageSubresource.layerCount = 1;
    cpy.imageExtent = {kW, kH, 1};
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1, &cpy);

    r = vkEndCommandBuffer(cmd);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        // cleanup omitted for brevity but continue to release
        result.error = "vkEndCommandBuffer falhou";
        // fall through to cleanup below via early return after partial cleanup
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        return result;
    }

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    r = vkCreateFence(device_, &fci, nullptr, &fence);
    if (r != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkCreateFence falhou";
        return result;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r = vkQueueSubmit(queue_, 1, &si, fence);
    if (r != VK_SUCCESS) {
        vkDestroyFence(device_, fence, nullptr);
        vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkQueueSubmit falhou";
        return result;
    }
    r = vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    if (r != VK_SUCCESS) {
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkWaitForFences falhou";
        return result;
    }

    // Readback.
    void* p = nullptr;
    r = vkMapMemory(device_, readback_memory_, 0, kW * kH * 4, 0, &p);
    if (r != VK_SUCCESS) {
        vkDestroyPipeline(device_, pipe, nullptr);
        vkDestroyPipelineLayout(device_, pl, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
        vkDestroyBuffer(device_, vbo, nullptr);
        vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyFramebuffer(device_, fb, nullptr);
        vkDestroyRenderPass(device_, rp, nullptr);
        vkDestroyImageView(device_, view, nullptr);
        result.error = "vkMapMemory falhou";
        return result;
    }
    const unsigned char* px = static_cast<const unsigned char*>(p);
    std::uint32_t checked = 0, red = 0, bg = 0, other = 0;
    for (std::uint32_t i = 0; i < kW * kH; ++i) {
        unsigned char rr = px[i*4+0], gg = px[i*4+1], bb = px[i*4+2], aa = px[i*4+3];
        ++checked;
        if (rr > 200 && gg < 50 && bb < 50) ++red;
        else if (rr < 20 && gg < 20 && bb < 20) ++bg;
        else ++other;
    }
    vkUnmapMemory(device_, readback_memory_);

    // Cleanup.
    vkDestroyPipeline(device_, pipe, nullptr);
    vkDestroyPipelineLayout(device_, pl, nullptr);
    vkFreeMemory(device_, vmem, nullptr);
    vkDestroyBuffer(device_, vbo, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyFramebuffer(device_, fb, nullptr);
    vkDestroyRenderPass(device_, rp, nullptr);
    vkDestroyImageView(device_, view, nullptr);

    result.pixels_checked = checked;
    result.pixels_red = red;
    result.pixels_background = bg;
    result.pixels_other = other;
    result.ok = (red > 0 && bg > 0 && other == 0);
    if (!result.ok) {
        result.error = "pixels inesperados (red=" + std::to_string(red) +
                       " bg=" + std::to_string(bg) + " other=" + std::to_string(other) + ")";
    }
    return result;
}

PipelineDrawResult VulkanBackend::draw_triangle(const std::vector<char>& vert_spv,
                                               const std::vector<char>& frag_spv) {
    // Triângulo de teste (dados artificiais, apenas infraestrutura).
    const float verts[6] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
    return draw_vertex_buffer(vert_spv, frag_spv, verts, 3, 2 * sizeof(float));
}

std::vector<std::uint8_t> VulkanBackend::readback_pixels() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint8_t> pixels(kW * kH * 4, 0);
    if (!ready_)
        return pixels;
    void* p = nullptr;
    VkResult r = vkMapMemory(device_, readback_memory_, 0, kW * kH * 4, 0, &p);
    if (r != VK_SUCCESS || p == nullptr)
        return pixels;
    std::memcpy(pixels.data(), p, kW * kH * 4);
    vkUnmapMemory(device_, readback_memory_);
    return pixels;
}

}  // namespace g610::gpu
