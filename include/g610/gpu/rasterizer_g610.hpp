// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o renderizador da casa: a implementação da
// `RasterizerInterface` do upstream.
//
// ## O que é
//
// O motor Maxwell do Eden (`video_core/engines/maxwell_3d.cpp`) fala com
// o renderizador por uma **interface abstracta**
// (`video_core/rasterizer_interface.h`) ✓. É por essa interface que o
// motor diz "desenha isto", "limpa isto", "espera pelo第五章".
//
// A casa implementa essa interface — é o `ValhallBackend` que o desenho
// do projecto já nomeia ✓ — em vez de vendorizar o `renderer_vulkan` do
// upstream (que traz shaders gerados, PanVK e forks de cabeçalhos que não
// existem em repositórios públicos).
//
// ## O que a interface NÃO é
//
// A interface **não** é onde o push buffer é lido (isso é o
// `dma_pusher`) nem onde a fence é incrementada (isso é o *puller*
//que escreve o *payload* do semáforo na memória do guest e sinaliza
// o syncpoint). O renderizador é a ponta de execução: os comandos de
// desenho, e as invalidações de cache.
//
// ## Porquê uma *interface* e não uma classe
//
// Porque é a costura que o motor do upstream já traz ✓ — usá-la é o que
// evita reescrever o motor.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "video_core/rasterizer_interface.h"

namespace g610::gpu {

class VulkanBackend;

// O renderizador da casa.
//
// ## Estado
//
// Hoje a casa **conta** os comandos em vez de os executar — o backend
// Vulkan (o `ValhallBackend`) entra por cima desta classe. A
// contagem não é decoração: é o que permite dizer, com medição, que o
// jogo chegou a desenhar ✓ — e o que a telemetria mostra ✓.
// **O namespace é o global `VideoCore`** ✓ — medido no upstream: o
// `rasterizer_interface.h` fecha `namespace Tegra` na linha 27 e abre
// `namespace VideoCore` na 29 ✓ (fora de `Tegra` ✓). E é assim que o
// próprio upstream a usa ✓ (`maxwell_3d.cpp:43` ✓,
// `kepler_memory.h:43` ✓, `puller.h:78` ✓): `VideoCore::RasterizerInterface`
// ✓.
//
// **Cuidado**: existe *outro* `VideoCore` ✓ — o `Tegra::VideoCore` de
// `video_core/video_core.h:16-27` ✓, que é onde vive o `RendererBase` ✓.
// São dois `VideoCore` diferentes ✓ e o upstream não os unifica.
// Escrever `Tegra::VideoCore::RasterizerInterface` não compila — e o
// erro que dá ("'Tegra::VideoCore' has not been declared") não diz
// nada sobre o namespace certo.
class RasterizerG610 final : public VideoCore::RasterizerInterface {
public:
    // O que a casa sabe fazer hoje, exposto para quem sabe escrever no
    // guest e sinalizar fences ✓.
    //
    // **Dois *hooks*** ✓: a escrita no guest e o sinal — porque quem
    // os faz (o `nvdrv` ✓, o motor ✓) sabe mais sobre o estado do que
    // o renderizador.
    using EscreveNoGuest =
        std::function<bool(std::uint64_t endereco, const void* dados, std::size_t tamanho)>;
    using SinalizaFence = std::function<void(std::uint32_t syncpoint)>;

    explicit RasterizerG610(EscreveNoGuest escreve = {}, SinalizaFence sinaliza = {},
                            std::shared_ptr<VulkanBackend> backend = nullptr);

    // ── A interface do upstream ───────────────────────────────────────────
    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                   u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void OnCacheInvalidation(PAddr addr, u64 size) override;
    bool OnCPUWrite(PAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;

    // ── O que a casa mede ─────────────────────────────────────────────────
    struct Contadores {
        std::uint64_t draws = 0;
        std::uint64_t draws_indirect = 0;
        std::uint64_t texturas = 0;
        std::uint64_t clears = 0;
        std::uint64_t computos = 0;
        std::uint64_t queries = 0;
        std::uint64_t syncpoints = 0;
        std::uint64_t fences = 0;
    };

    const Contadores& contadores() const noexcept { return contadores_; }

    // Os vertices/indices que o motor pediu no último desenho — é o que
    // permite confirmar que a traducao do estado está certa ✓ (a contagem
    // de vertices e' o que a GPU faria).
    void AnotaDesenho(std::uint64_t vertices, std::uint64_t indices, bool indexado);

    std::uint64_t vertices_ultimo = 0;
    std::uint64_t indices_ultimo = 0;
    bool indexado_ultimo = false;

private:
    EscreveNoGuest escreve_;
    SinalizaFence sinaliza_;
    std::shared_ptr<VulkanBackend> backend_;
    Contadores contadores_{};

public:
    // Acesso ao backend (para testes/diagnóstico).
    const std::shared_ptr<VulkanBackend>& backend() const noexcept { return backend_; }
};

}  // namespace g610::gpu
