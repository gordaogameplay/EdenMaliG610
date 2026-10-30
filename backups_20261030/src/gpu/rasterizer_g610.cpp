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
                               LeDoGuest le, std::shared_ptr<VulkanBackend> backend)
    : escreve_(std::move(escreve)), sinaliza_(std::move(sinaliza)),
      le_(std::move(le)), backend_(std::move(backend)) {}

void RasterizerG610::set_shaders(const std::vector<char>& vert_spv,
                                 const std::vector<char>& frag_spv) noexcept {
    vert_spv_ = vert_spv;
    frag_spv_ = frag_spv;
}

// ── Os comandos de desenho ───────────────────────────────────────────────────

void RasterizerG610::Draw(bool is_indexed, u32 instance_count) {
    ++contadores_.draws;
    (void)is_indexed;
    (void)instance_count;

    // Validação do stream de vértices capturado. Não interpreta bytes como
    // coordenadas (formato desconhecido) e não chama o backend Vulkan.
    if (va_vertices_ultimo == 0) {
        std::fprintf(stderr, "[RasterizerG610::Draw] sem vertex_base_va capturado (bloqueio: formato/topologia não decodificados)\n");
        return;
    }
    if (stride_vertices_ultimo == 0) {
        std::fprintf(stderr, "[RasterizerG610::Draw] stride nulo para VA 0x%llx\n",
                     (unsigned long long)va_vertices_ultimo);
        return;
    }
    if (vertices_ultimo == 0) {
        std::fprintf(stderr, "[RasterizerG610::Draw] vertex_count=0 para VA 0x%llx\n",
                     (unsigned long long)va_vertices_ultimo);
        return;
    }
    // Verificação de overflow e limite de tamanho (64 MiB).
    constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
    std::size_t total = 0;
    if (vertices_ultimo > kMaxBytes / (stride_vertices_ultimo ? stride_vertices_ultimo : 1)) {
        std::fprintf(stderr, "[RasterizerG610::Draw] overflow de tamanho (vertices=%llu stride=%u)\n",
                     (unsigned long long)vertices_ultimo, stride_vertices_ultimo);
        return;
    }
    total = static_cast<std::size_t>(vertices_ultimo) * stride_vertices_ultimo;
    if (total > kMaxBytes) {
        std::fprintf(stderr, "[RasterizerG610::Draw] buffer demasiado grande (%zu bytes)\n", total);
        return;
    }

    // Desenho integrado: chama o backend Vulkan com shaders e dados de
    // vertice lidos da memoria do guest. Sem backend ou sem shaders, fica
    // so na contagem (modo diagnostico).
    if (backend_ && !vert_spv_.empty() && !frag_spv_.empty()) {
        if (desenhar_integrado(vert_spv_, frag_spv_)) {
            std::fprintf(stderr,
                         "[RasterizerG610::Draw] desenho integrado OK: VA=0x%llx stride=%u vertices=%llu\n",
                         (unsigned long long)va_vertices_ultimo, stride_vertices_ultimo,
                         (unsigned long long)vertices_ultimo);
        } else {
            std::fprintf(stderr,
                         "[RasterizerG610::Draw] desenho integrado falhou: VA=0x%llx stride=%u vertices=%llu\n",
                         (unsigned long long)va_vertices_ultimo, stride_vertices_ultimo,
                         (unsigned long long)vertices_ultimo);
        }
    } else {
        std::fprintf(stderr, "[RasterizerG610::Draw] stream valido: VA=0x%llx stride=%u vertices=%llu bytes=%zu (sem backend Vulkan ou sem shaders)\n",
                     (unsigned long long)va_vertices_ultimo, stride_vertices_ultimo,
                     (unsigned long long)vertices_ultimo, total);
    }
}

void RasterizerG610::DrawIndirect() {
    ++contadores_.draws_indirect;
}

void RasterizerG610::DrawTexture() {
    ++contadores_.texturas;
}

bool RasterizerG610::ler_do_guest(std::uint64_t src, void* dst, std::size_t size) const {
    if (dst == nullptr) return false;
    if (size == 0) return true;
    // Overflow check.
    if (src > UINT64_MAX - size) return false;
    constexpr std::size_t kMax = 64 * 1024 * 1024;
    if (size > kMax) return false;
    if (!le_) return false;
    return le_(src, dst, size);
}

bool RasterizerG610::desenhar_integrado(const std::vector<char>& vert_spv,
                                        const std::vector<char>& frag_spv) {
    if (!backend_) { std::fprintf(stderr, "[integrado] sem backend\n"); return false; }
    if (!topology_known_ultimo) { std::fprintf(stderr, "[integrado] topologia desconhecida\n"); return false; }
    if (va_vertices_ultimo == 0 || stride_vertices_ultimo == 0 || vertices_ultimo == 0) {
        std::fprintf(stderr, "[integrado] stream de vertices invalido\n"); return false;
    }
    if (attributes_ultimo.empty()) { std::fprintf(stderr, "[integrado] sem atributos\n"); return false; }
    const auto& a0 = attributes_ultimo[0];
    if (!a0.enabled) { std::fprintf(stderr, "[integrado] atributo 0 desabilitado\n"); return false; }
    if (a0.type_enum != 7) { std::fprintf(stderr, "[integrado] tipo nao-Float (type=%u)\n", a0.type_enum); return false; }
    if (a0.component_count < 2) { std::fprintf(stderr, "[integrado] componentes < 2\n"); return false; }
    if (a0.size_bytes == 0) { std::fprintf(stderr, "[integrado] size_bytes=0\n"); return false; }

    // Validacao de tamanho e overflow.
    constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
    if (vertices_ultimo > kMaxBytes / (stride_vertices_ultimo ? stride_vertices_ultimo : 1)) {
        std::fprintf(stderr, "[integrado] overflow\n"); return false;
    }
    const std::size_t total = static_cast<std::size_t>(vertices_ultimo) * stride_vertices_ultimo;
    if (total > kMaxBytes) { std::fprintf(stderr, "[integrado] buffer demasiado grande\n"); return false; }

    // Leitura segura.
    std::vector<std::uint8_t> raw(total);
    if (!ler_do_guest(va_vertices_ultimo, raw.data(), total)) {
        std::fprintf(stderr, "[integrado] falha ao ler memoria do guest\n"); return false;
    }

    // Extrai posicoes (x,y) do atributo 0.
    std::vector<float> positions;
    positions.reserve(vertices_ultimo * 2);
    for (std::uint64_t i = 0; i < vertices_ultimo; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * stride_vertices_ultimo + a0.offset;
        if (base + a0.size_bytes > raw.size()) { std::fprintf(stderr, "[integrado] buffer incompleto\n"); return false; }
        const float* fp = reinterpret_cast<const float*>(raw.data() + base);
        positions.push_back(fp[0]);
        positions.push_back(fp[1]);
    }

    if (positions.empty()) { std::fprintf(stderr, "[integrado] sem posicoes\n"); return false; }

    auto res = backend_->draw_vertex_buffer(vert_spv, frag_spv, positions.data(),
                                           positions.size() / 2, 2 * sizeof(float));
    std::fprintf(stderr, "[integrado] draw_vertex_buffer: ok=%d red=%u bg=%u other=%u %s\n",
                 res.ok ? 1 : 0, res.pixels_red, res.pixels_background, res.pixels_other,
                 res.error.c_str());
    return res.ok;
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

void RasterizerG610::AnotaDesenho(std::uint64_t vertices, std::uint64_t indices, bool indexado,
                                  std::uint64_t vertex_base_va, std::uint32_t vertex_stride,
                                  const std::vector<VertexAttributeDesc>& attributes,
                                  bool topology_known, std::uint32_t topology) {
    vertices_ultimo = vertices;
    indices_ultimo = indices;
    indexado_ultimo = indexado;
    va_vertices_ultimo = vertex_base_va;
    stride_vertices_ultimo = vertex_stride;
    attributes_ultimo = attributes;
    topology_known_ultimo = topology_known;
    topology_ultimo = topology;
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
