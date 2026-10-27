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
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

}  // namespace g610::gpu
