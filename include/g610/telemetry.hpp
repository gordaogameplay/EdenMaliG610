// EdenMaliG610 — telemetria embutida.
//
// Este header existe por uma razão: um emulador pronto não instrumenta o
// caminho crítico, e sem números não há diagnóstico. Aqui cada ponto onde
// o Switch guest pode gastar tempo é medido no local, sem amostragem
// externa e sem parar o jogo.
//
// Três famílias de métrica:
//   1. CPU   — custo dos SVC (serviços do Horizon), faults de página, traps.
//   2. GPU   — custo da tradução Maxwell -> Valhall e do submit Vulkan.
//   3. Frame — a decomposição do tempo total, que é a que diz onde está o
//               gargalo (CPU, GPU, compilação de shader ou espera de vsync).
//
// Tudo é lock-free: a telemetria nunca pode alterar o que mede.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace g610::tel {

using Clock = std::chrono::steady_clock;
using Nanos = std::int64_t;

// ---------------------------------------------------------------------------
// Classificação de gargalo por frame.
//
// O jogo é limitado por um destes quatro regimes. O que interessa não é só o
// FPS: é saber qual deles está activo, porque o conserto é diferente para cada.
// ---------------------------------------------------------------------------
enum class Bottleneck : std::uint8_t {
    Unknown = 0,
    CpuBound,       // a CPU não submits o frame a tempo
    GpuBound,       // a GPU ocupa quase todo o frame
    ShaderBound,    // compilação/recompilação de shader domina
    PresentBound,   // frame termina muito antes do vsync — espera de display
    Count
};

const char* to_string(Bottleneck b);

// ---------------------------------------------------------------------------
// Registro de um frame.
// ---------------------------------------------------------------------------
struct FrameRecord {
    std::uint64_t index      = 0;
    Nanos         cpu_ns     = 0;  // tempo em código do guest + wrappers
    Nanos         gpu_ns     = 0;  // tempo ocupado pela GPU
    Nanos         translate_ns = 0; // tradução Maxwell -> Vulkan (CPU side)
    Nanos         shader_ns  = 0;  // compilação SPIR-V
    Nanos         present_ns = 0;  // espera do frame no ecrã / vsync
    Nanos         total_ns   = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t triangles  = 0;
    std::uint32_t svc_calls  = 0;
    std::uint32_t page_faults = 0;
    Bottleneck    bottleneck = Bottleneck::Unknown;
};

// ---------------------------------------------------------------------------
// Custos de SVC.
//
// O guest chama serviços do Horizon com frequência altíssima. Saber quais
// dominam é o que permite decidir entre "acelerar o serviço" e "acelerar o
// desenho". Um array de 256 entradas cobre o espaço de serviços do Switch.
// ---------------------------------------------------------------------------
class ServiceProfiler {
public:
    static constexpr std::size_t kSlots = 256;

    void record(std::uint32_t number, Nanos ns) noexcept;
    void reset() noexcept;

    struct Entry {
        std::uint64_t calls = 0;
        Nanos         total_ns = 0;
        Nanos         max_ns = 0;
    };
    // Devolve por valor: o Entry é pequeno (três campos) e devolver por
    // referência exigiria um acumulador com estado, que custaria mais do que a
    // cópia num caminho de leitura.
    Entry entry(std::uint32_t number) const noexcept;
    // Índice do serviço com maior tempo total. -1 se nenhum foi chamado.
    int hottest() const noexcept;
    Nanos total_ns() const noexcept;

private:
    struct alignas(64) Slot {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::int64_t>  total_ns{0};
        std::atomic<std::int64_t>  max_ns{0};
    };
    // Um array de atomics contíguos seria false-sharing puro; cada slot ganha
    // a sua linha de cache.
    std::array<Slot, kSlots> slots_{};
};

// ---------------------------------------------------------------------------
// Contadores globais de execução nativa.
// ---------------------------------------------------------------------------
struct ExecCounters {
    std::atomic<std::uint64_t> svc_calls{0};
    std::atomic<std::uint64_t> page_faults{0};
    std::atomic<std::uint64_t> alignment_faults{0};
    std::atomic<std::uint64_t> illegal_insts{0};
    std::atomic<std::uint64_t> translated_blocks{0};
    std::atomic<std::uint64_t> guest_instructions{0};
};

// ---------------------------------------------------------------------------
// Contadores da camada GPU.
// ---------------------------------------------------------------------------
struct GpuCounters {
    std::atomic<std::uint64_t> draw_calls{0};
    std::atomic<std::uint64_t> triangles{0};
    std::atomic<std::uint64_t> shaders_compiled{0};
    std::atomic<std::uint64_t> cache_hits{0};
    std::atomic<std::uint64_t> cache_misses{0};
    std::atomic<std::uint64_t> submits{0};
    std::atomic<std::uint64_t> gpu_faults{0};
    std::atomic<std::uint64_t> device_lost{0};
    std::atomic<std::uint64_t> textures_allocated{0};
    std::atomic<std::uint64_t> texture_bytes{0};
};

// ---------------------------------------------------------------------------
// Telemetria principal.
//
// Thread-safe. Os caminhos quentes usam apenas atomics; a escrita em disco
// acontece só no flush, fora do caminho crítico.
// ---------------------------------------------------------------------------
class Telemetry {
public:
    Telemetry();

    // Configuração ---------------------------------------------------------
    void set_output_dir(const std::string& dir);
    void set_live_console(bool on);
    void set_frame_history(std::size_t frames);
    void start(std::string title_id, std::string title_name);

    // Caminhos quentes -----------------------------------------------------
    ExecCounters& exec() noexcept { return exec_; }
    GpuCounters&  gpu()  noexcept { return gpu_; }
    ServiceProfiler& services() noexcept { return services_; }

    // Chamados pelo despachante de SVC e pelos tratadores de fault, para que o
    // custo do frame refleita o que aconteceu noutras threads e não só no
    // caminho de render.
    void note_svc() noexcept {
        svc_in_frame_.fetch_add(1, std::memory_order_relaxed);
    }
    void note_page_fault() noexcept {
        page_faults_in_frame_.fetch_add(1, std::memory_order_relaxed);
        exec_.page_faults.fetch_add(1, std::memory_order_relaxed);
    }

    // Mede um bloco de código e devolve o custo (para escopo manual).
    Nanos time_now() noexcept;
    Nanos elapsed(Nanos start) noexcept;

    // Anuncia o início de um frame. Devolve o token do frame.
    std::uint64_t frame_begin();
    void frame_draw(std::uint32_t triangles);
    void frame_gpu(Nanos ns);
    void frame_translate(Nanos ns);
    void frame_shader(Nanos ns);
    void frame_present(Nanos ns);
    // Fecha o frame, classifica o gargalo e arquiva.
    void frame_end();

    // Leitura ---------------------------------------------------------------
    const FrameRecord& last_frame() const noexcept { return last_; }
    std::size_t history_size() const noexcept { return history_.size(); }
    FrameRecord history_at(std::size_t i) const;
    Nanos average_frame_ns() const noexcept { return avg_frame_ns_.load(std::memory_order_relaxed); }
    double   average_fps() const noexcept;
    Bottleneck dominant_bottleneck() const noexcept { return dominant_; }
    const ExecCounters& exec_snapshot() const noexcept { return exec_; }
    const GpuCounters&  gpu_snapshot()  const noexcept { return gpu_; }

    // Relatórios ------------------------------------------------------------
    // Escreve um snapshot JSON + CSV do frame. Cria ficheiros se preciso.
    bool flush(const std::string& tag);
    // Resumo textual para consola.
    std::string summary() const;

    // Decide em que regime o frame está, a partir da decomposição do tempo.
    //
    // É uma função pura e determinística, por isso é pública: os testes
    // classificam frames sintéticos sem precisar de dormir à espera de
    // milissegundos reais, e assim o teste é exacto em vez de aproximativo.
    static Bottleneck classify(const FrameRecord& r) noexcept;

private:
    void maybe_print_live();

    ExecCounters    exec_{};
    GpuCounters     gpu_{};
    ServiceProfiler services_{};

    mutable std::mutex mtx_;                 // protege histórico e ficheiros
    std::vector<FrameRecord> history_;
    std::size_t history_cap_ = 600;

    // --- estado do frame em curso -------------------------------------------
    // Só a thread de render escreve em current_; os contadores por-frame são
    // atómicos porque os SVC podem acontecer noutras threads.
    FrameRecord current_{};
    Nanos       frame_start_{0};
    std::atomic<std::uint32_t> svc_in_frame_{0};
    std::atomic<std::uint32_t> page_faults_in_frame_{0};
    std::atomic<std::uint64_t> frame_seq_{0};

    FrameRecord last_{};
    std::atomic<Nanos> avg_frame_ns_{0};
    Bottleneck  dominant_ = Bottleneck::Unknown;

    std::string out_dir_ = "g610-telemetry";
    std::string title_id_, title_name_;
    bool        live_ = true;
    std::uint64_t live_counter_ = 0;
};

// ---------------------------------------------------------------------------
// Temporizador de âmbito.
//
//   IA_SCOPE_TIMER(tel.translate_ns, tel.time_now());
//
// Devolve o tempo gasto no âmbito para o acumulador indicado, mesmo que o
// código lance uma excepção.
// ---------------------------------------------------------------------------
class ScopedTimer {
public:
    ScopedTimer(Clock::time_point start, std::atomic<Nanos>* sink) noexcept
        : start_(start), sink_(sink) {}
    ~ScopedTimer() noexcept {
        const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           Clock::now() - start_)
                           .count();
        sink_->fetch_add(d, std::memory_order_relaxed);
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Clock::time_point        start_;
    std::atomic<Nanos>*      sink_;
};

} // namespace g610::tel

#define IA_SCOPE_TIMER(atomic_sink)                                             \
    ::g610::tel::ScopedTimer g610_timer__{::g610::tel::Clock::now(), &(atomic_sink)}
