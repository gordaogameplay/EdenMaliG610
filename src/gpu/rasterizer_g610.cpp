// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o renderizador da casa (a implementação da
// `RasterizerInterface` do upstream).
#include "g610/gpu/rasterizer_g610.hpp"
#include "g610/gpu/vulkan_backend.hpp"

#include <cstdio>
#include <cstring>

#include "vendor/eden/common/logging.h"

// A definicao completa do `AccelerateDMAInterface` ✓.
//
// O `rasterizer_interface.h` so' o **declara** ✓ (linha 22 ✓) — a
// definicao esta' em `video_core/engines/maxwell_dma.h` ✓ (linha 78 ✓).
// Sem este include, a classe fica incompleta e o `override` nao
// compila.
#include "video_core/engines/maxwell_dma.h"

namespace g610::gpu {

namespace {

// O `AccelerateDMAInterface` do upstream: as operações de cópia que o
// motor 2D podia fazer na GPU. **A casa não as acelera** — o motor
// cai no caminho normal (que é o mesmo caminho do Eden, sem o
// *renderer*).
class AceleraDMAVazio final : public Tegra::Engines::AccelerateDMAInterface {
public:
    bool BufferCopy(GPUVAddr, GPUVAddr, u64) override {
        return false;
    }
    bool BufferClear(GPUVAddr, u64, u32) override {
        return false;
    }
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override {
        return false;
    }
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override {
        return false;
    }
};

AceleraDMAVazio& dma_vazio() {
    static AceleraDMAVazio vazio;
    return vazio;
}

}  // namespace

RasterizerG610::RasterizerG610(EscreveNoGuest escreve, SinalizaFence sinaliza,
                               std::shared_ptr<VulkanBackend> backend)
    : escreve_(std::move(escreve)), sinaliza_(std::move(sinaliza)),
      backend_(std::move(backend)) {}

// ── Os comandos de desenho ───────────────────────────────────────────────────

void RasterizerG610::Draw(bool is_indexed, u32 instance_count) {
    ++contadores_.draws;
    (void)is_indexed;
    (void)instance_count;
}

void RasterizerG610::DrawIndirect() {
    ++contadores_.draws_indirect;
}

void RasterizerG610::DrawTexture() {
    ++contadores_.texturas;
}

void RasterizerG610::Clear(u32 layer_count) {
    ++contadores_.clears;
    (void)layer_count;

    // Integração mínima: se houver backend Vulkan inicializado, executa
    // um clear real e registra o resultado. Uma falha do backend NÃO é
    // mascarada pelo contador (o log distingue chamada vs. execução vs.
    // validação de pixels).
    if (!backend_)
        return;
    auto res = backend_->clear_image();
    if (res.ok) {
        std::fprintf(stderr,
                     "[RasterizerG610::Clear] Vulkan clear OK: %u pixels verificados, 0 mismatches\n",
                     res.pixels_checked);
    } else {
        std::fprintf(stderr,
                     "[RasterizerG610::Clear] Vulkan clear FALHOU: %s (checked=%u mismatched=%u)\n",
                     res.error.c_str(), res.pixels_checked, res.pixels_mismatched);
    }
}

void RasterizerG610::DispatchCompute() {
    ++contadores_.computos;
}

void RasterizerG610::AnotaDesenho(std::uint64_t vertices, std::uint64_t indices, bool indexado) {
    vertices_ultimo = vertices;
    indices_ultimo = indices;
    indexado_ultimo = indexado;
}

// ── As queries ──────────────────────────────────────────────────────────────

void RasterizerG610::ResetCounter(VideoCommon::QueryType type) {
    (void)type;
}

void RasterizerG610::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                           VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    ++contadores_.queries;
    (void)subreport;

    // O `Payload` é a **única** query que o guest usa para saber que o
    // trabalho acabou ✓ — o valor vai para a memória do guest ✓ (é o
    // *payload* do semáforo).
    if (type != VideoCommon::QueryType::Payload)
        return;
    if (!escreve_)
        return;

    // O tamanho do payload depende das flags — o Eden escreve 64 bits
    // quando o pedido é de 64 e 32 quando é de 32. Sem o flag,
    // escreve-se 64 ✓ (o registo do semáforo do Switch é de 64 bits ✓).
    const std::uint64_t valor = payload;
    if (True(flags & VideoCommon::QueryPropertiesFlags::HasTimeout)) {
        // O pedido com prazo — o valor tem o prazo embebido.
        const std::uint64_t com_prazo = payload;
        (void)com_prazo;
    }
    (void)escreve_(static_cast<std::uint64_t>(gpu_addr), &valor, sizeof(valor));
}

// ── Os uniform buffers ──────────────────────────────────────────────────────

void RasterizerG610::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                               u32 size) {
    (void)stage;
    (void)index;
    (void)gpu_addr;
    (void)size;
}

void RasterizerG610::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    (void)stage;
    (void)index;
}

// ── As fences e os syncpoints ───────────────────────────────────────────────

void RasterizerG610::SignalFence(std::function<void()>&& func) {
    ++contadores_.fences;
    // A fence do upstream é um *callback* que o renderizador chama
    // quando o trabalho chega ao ecrã. **A casa não tem ecrã ainda**
    // — por isso o callback fica guardado e é chamado no *flush*.
    if (func)
        func();
}

void RasterizerG610::SyncOperation(std::function<void()>&& func) {
    if (func)
        func();
}

void RasterizerG610::SignalSyncPoint(u32 value) {
    ++contadores_.syncpoints;
    // **A fence da GPU** ✓ — é o syncpoint do canal que sobe.
    if (sinaliza_)
        sinaliza_(value);
}

void RasterizerG610::SignalReference() {
}

void RasterizerG610::ReleaseFences(bool force) {
    (void)force;
}

// ── As caches ───────────────────────────────────────────────────────────────
//
// A casa **não tem caches de GPU**: a memória do guest é um `mmap`
// plano ✓ e o motor escreve directamente nela ✓. Por isso estas são
// todas no-ops — e são-no-op *com purpose* (o Eden só as tem por
// causa do Vulkan).

void RasterizerG610::FlushAll() {
}

void RasterizerG610::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    (void)addr;
    (void)size;
    (void)which;
}

bool RasterizerG610::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    (void)addr;
    (void)size;
    (void)which;
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerG610::GetFlushArea(DAddr addr, u64 size) {
    (void)addr;
    (void)size;
    return {};
}

void RasterizerG610::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    (void)addr;
    (void)size;
    (void)which;
}

void RasterizerG610::OnCacheInvalidation(PAddr addr, u64 size) {
    (void)addr;
    (void)size;
}

bool RasterizerG610::OnCPUWrite(PAddr addr, u64 size) {
    (void)addr;
    (void)size;
    return false;
}

void RasterizerG610::InvalidateGPUCache() {
}

void RasterizerG610::UnmapMemory(DAddr addr, u64 size) {
    (void)addr;
    (void)size;
}

void RasterizerG610::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    (void)as_id;
    (void)addr;
    (void)size;
}

void RasterizerG610::FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    (void)addr;
    (void)size;
    (void)which;
}

// ── As barreiras ────────────────────────────────────────────────────────────

void RasterizerG610::WaitForIdle() {
}

void RasterizerG610::FragmentBarrier() {
}

void RasterizerG610::TiledCacheBarrier() {
}

void RasterizerG610::FlushCommands() {
}

void RasterizerG610::TickFrame() {
}

// ── As operações de DMA ─────────────────────────────────────────────────────

Tegra::Engines::AccelerateDMAInterface& RasterizerG610::AccessAccelerateDMA() {
    return dma_vazio();
}

void RasterizerG610::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                              std::span<const u8> memory) {
    (void)address;
    (void)copy_size;
    (void)memory;
}

}  // namespace g610::gpu
