// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — backend Vulkan mínimo para o RasterizerG610.
//
// Este backend é a ponta de execução real da GPU. Nesta etapa cobre
// APENAS `Clear()`: cria um dispositivo Vulkan, uma imagem offscreen,
// executa um clear real e lê os pixels de volta para verificação
// numérica. Não é o framebuffer do jogo.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace g610::gpu {

struct ClearResult {
    bool ok = false;
    std::uint32_t pixels_checked = 0;
    std::uint32_t pixels_mismatched = 0;
    std::string error;
};

struct PipelineDrawResult {
    bool ok = false;
    std::uint32_t pixels_checked = 0;
    std::uint32_t pixels_red = 0;
    std::uint32_t pixels_background = 0;
    std::uint32_t pixels_other = 0;
    std::string error;
};

class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Inicializa a instância, o dispositivo, a fila, a imagem e o
    // buffer de leitura. Devolve false e preenche `error` em caso de
    // falha; nunca mascara um erro como sucesso.
    bool init(std::string& error);

    // Liberta todos os recursos Vulkan.
    void shutdown();

    bool is_ready() const noexcept { return ready_; }

    // Executa um clear real (cor conhecida) e verifica os pixels por
    // readback. Thread-safe.
    ClearResult clear_image();

    // Executa um pipeline gráfico real: desenha um triângulo (SPIR-V
    // fornecido pelo chamador) sobre a imagem offscreen e verifica os
    // pixels por readback. Thread-safe.
    PipelineDrawResult draw_triangle(const std::vector<char>& vert_spv,
                                    const std::vector<char>& frag_spv);

    // Cor usada no clear (para diagnóstico).
    static constexpr float kR = 0.25f;
    static constexpr float kG = 0.5f;
    static constexpr float kB = 0.75f;
    static constexpr float kA = 1.0f;

private:
    bool create_instance(std::string& error);
    bool create_device(std::string& error);
    bool create_image(std::string& error);
    bool create_readback_buffer(std::string& error);
    bool create_command_pool(std::string& error);

    std::uint32_t find_memory_type(std::uint32_t type_bits,
                                   VkMemoryPropertyFlags wanted) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;

    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;

    static constexpr std::uint32_t kW = 64;
    static constexpr std::uint32_t kH = 64;

    bool ready_ = false;
    mutable std::mutex mutex_;
};

}  // namespace g610::gpu
