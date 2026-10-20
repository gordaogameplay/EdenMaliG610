// EdenMaliIA — implementação da telemetria.
#include "ia/telemetry.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ia::tel {

namespace {
constexpr const char* kBottleneckNames[] = {
    "unknown", "cpu", "gpu", "shader", "present",
};

Nanos now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}
} // namespace

const char* to_string(Bottleneck b) {
    const auto i = static_cast<std::size_t>(b);
    return i < static_cast<std::size_t>(Bottleneck::Count) ? kBottleneckNames[i]
                                                           : "unknown";
}

// ---------------------------------------------------------------------------
// ServiceProfiler
// ---------------------------------------------------------------------------

void ServiceProfiler::record(std::uint32_t number, Nanos ns) noexcept {
    if (number >= kSlots)
        return;
    Slot& s = slots_[number];
    s.calls.fetch_add(1, std::memory_order_relaxed);
    s.total_ns.fetch_add(ns, std::memory_order_relaxed);

    // max é uma redução; com várias threads/services-per-thread pode perder a
    // corrida, mas o valor continua a ser o maior observado em cada leitura —
    // para diagnóstico de "qual serviço é o mais lento" isso é suficiente.
    Nanos cur = s.max_ns.load(std::memory_order_relaxed);
    while (cur < ns &&
           !s.max_ns.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
    }
}

void ServiceProfiler::reset() noexcept {
    for (auto& s : slots_) {
        s.calls.store(0, std::memory_order_relaxed);
        s.total_ns.store(0, std::memory_order_relaxed);
        s.max_ns.store(0, std::memory_order_relaxed);
    }
}

ServiceProfiler::Entry ServiceProfiler::entry(std::uint32_t number) const noexcept {
    Entry e{};
    if (number < kSlots) {
        e.calls    = slots_[number].calls.load(std::memory_order_relaxed);
        e.total_ns = slots_[number].total_ns.load(std::memory_order_relaxed);
        e.max_ns   = slots_[number].max_ns.load(std::memory_order_relaxed);
    }
    return e;
}

int ServiceProfiler::hottest() const noexcept {
    int    best = -1;
    Nanos  best_ns = 0;
    for (std::size_t i = 0; i < kSlots; ++i) {
        const Nanos t = slots_[i].total_ns.load(std::memory_order_relaxed);
        if (t > best_ns) {
            best_ns = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

Nanos ServiceProfiler::total_ns() const noexcept {
    Nanos sum = 0;
    for (std::size_t i = 0; i < kSlots; ++i)
        sum += slots_[i].total_ns.load(std::memory_order_relaxed);
    return sum;
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------

Telemetry::Telemetry() { history_.reserve(history_cap_); }

void Telemetry::set_output_dir(const std::string& dir) {
    std::lock_guard lock(mtx_);
    out_dir_ = dir;
}

void Telemetry::set_live_console(bool on) { live_ = on; }

void Telemetry::set_frame_history(std::size_t frames) {
    std::lock_guard lock(mtx_);
    history_cap_ = frames ? frames : 1;
    history_.clear();
    history_.reserve(history_cap_);
}

void Telemetry::start(std::string title_id, std::string title_name) {
    std::lock_guard lock(mtx_);
    title_id_   = std::move(title_id);
    title_name_ = std::move(title_name);
    std::error_code ec;
    std::filesystem::create_directories(out_dir_, ec);
}

Nanos Telemetry::time_now() noexcept { return now_ns(); }

Nanos Telemetry::elapsed(Nanos start) noexcept { return now_ns() - start; }

// --- ciclo de frame ---------------------------------------------------------

std::uint64_t Telemetry::frame_begin() {
    current_ = FrameRecord{};
    current_.index = frame_seq_.fetch_add(1, std::memory_order_relaxed);
    frame_start_ = now_ns();
    return current_.index;
}


void Telemetry::frame_draw(std::uint32_t triangles) {
    current_.draw_calls += 1;
    current_.triangles += triangles;
    gpu_.draw_calls.fetch_add(1, std::memory_order_relaxed);
    gpu_.triangles.fetch_add(triangles, std::memory_order_relaxed);
}

void Telemetry::frame_gpu(Nanos ns)        { current_.gpu_ns += ns; }
void Telemetry::frame_translate(Nanos ns)   { current_.translate_ns += ns; }
void Telemetry::frame_shader(Nanos ns)      { current_.shader_ns += ns; }
void Telemetry::frame_present(Nanos ns)     { current_.present_ns += ns; }

Bottleneck Telemetry::classify(const FrameRecord& r) noexcept {
    const Nanos cpu_total = r.cpu_ns + r.translate_ns + r.shader_ns;
    if (r.total_ns <= 0)
        return Bottleneck::Unknown;

    // A ordem dos testes importa: cada condição é o sintoma mais específico
    // antes do mais genérico.
    //
    //   shader   — compilação domina o frame: o jogo está a criar shaders
    //              novos, não a desenhar os já existentes.
    //   cpu      — o guest passa o tempo a pensar (sem draws) ou a enviar
    //              comandos; a GPU não é o travão.
    //   gpu      — a Mali ocupa ~70% ou mais do frame.
    //   present  — o frame acaba muito antes do vsync; o display é o travão.
    if (r.shader_ns > 0 && r.shader_ns * 2 > r.total_ns)
        return Bottleneck::ShaderBound;
    if (r.draw_calls == 0 && r.gpu_ns * 10 < r.total_ns)
        return Bottleneck::CpuBound;
    if (r.gpu_ns > 0 && r.gpu_ns * 10 >= r.total_ns * 7)
        return Bottleneck::GpuBound;
    if (r.present_ns > 0 && r.present_ns * 2 > r.total_ns)
        return Bottleneck::PresentBound;
    if (cpu_total > r.gpu_ns)
        return Bottleneck::CpuBound;
    return Bottleneck::GpuBound;
}

void Telemetry::frame_end() {
    FrameRecord r = current_;
    r.total_ns   = now_ns() - frame_start_;
    r.svc_calls   = svc_in_frame_.load(std::memory_order_relaxed);
    r.page_faults = page_faults_in_frame_.load(std::memory_order_relaxed);
    r.bottleneck  = classify(r);
    // Zera os contadores por-frame só depois de os ter lido.
    svc_in_frame_.store(0, std::memory_order_relaxed);
    page_faults_in_frame_.store(0, std::memory_order_relaxed);

    last_ = r;

    {
        std::lock_guard lock(mtx_);
        if (history_.size() == history_cap_)
            history_.erase(history_.begin());
        history_.push_back(r);

        // média móvel simples do tempo de frame
        const std::size_t n = history_.size();
        Nanos sum = 0;
        for (const auto& f : history_)
            sum += f.total_ns;
        avg_frame_ns_.store(sum / static_cast<Nanos>(n), std::memory_order_relaxed);

        // gargalo dominante = o mais frequente no histórico recente
        std::array<int, static_cast<std::size_t>(Bottleneck::Count)> tally{};
        for (const auto& f : history_)
            ++tally[static_cast<std::size_t>(f.bottleneck)];
        int best = 0;
        for (std::size_t i = 1; i < tally.size(); ++i)
            if (tally[i] > tally[best])
                best = static_cast<int>(i);
        dominant_ = static_cast<Bottleneck>(best);
    }

    maybe_print_live();
}

FrameRecord Telemetry::history_at(std::size_t i) const {
    std::lock_guard lock(mtx_);
    if (i >= history_.size())
        return last_;
    return history_[i];
}

double Telemetry::average_fps() const noexcept {
    const Nanos avg = avg_frame_ns_.load(std::memory_order_relaxed);
    if (avg <= 0)
        return 0.0;
    return 1e9 / static_cast<double>(avg);
}

// --- relatórios -------------------------------------------------------------

bool Telemetry::flush(const std::string& tag) {
    std::lock_guard lock(mtx_);
    std::error_code ec;
    std::filesystem::create_directories(out_dir_, ec);
    if (ec)
        return false;

    const auto stamp =
        std::to_string(frame_seq_.load(std::memory_order_relaxed));
    const std::string base = out_dir_ + "/" + (tag.empty() ? "session" : tag);

    // --- JSON de instantâneo -------------------------------------------
    {
        std::ofstream j(base + "_" + stamp + ".json", std::ios::trunc);
        if (!j)
            return false;
        j << std::fixed;
        j << "{\n";
        j << "  \"title_id\": \"" << title_id_ << "\",\n";
        j << "  \"title\": \"" << title_name_ << "\",\n";
        j << "  \"frames\": " << history_.size() << ",\n";
        j << std::setprecision(2);
        j << "  \"avg_fps\": " << average_fps() << ",\n";
        j << std::setprecision(3);
        j << "  \"avg_frame_ms\": "
          << (avg_frame_ns_.load() / 1e6) << ",\n";
        j << "  \"dominant_bottleneck\": \"" << to_string(dominant_) << "\",\n";

        j << "  \"exec\": {\n";
        j << "    \"svc_calls\": " << exec_.svc_calls.load() << ",\n";
        j << "    \"page_faults\": " << exec_.page_faults.load() << ",\n";
        j << "    \"alignment_faults\": " << exec_.alignment_faults.load() << ",\n";
        j << "    \"illegal_instructions\": " << exec_.illegal_insts.load() << ",\n";
        j << "    \"guest_instructions\": " << exec_.guest_instructions.load() << "\n";
        j << "  },\n";

        j << "  \"gpu\": {\n";
        j << "    \"draw_calls\": " << gpu_.draw_calls.load() << ",\n";
        j << "    \"triangles\": " << gpu_.triangles.load() << ",\n";
        j << "    \"shaders_compiled\": " << gpu_.shaders_compiled.load() << ",\n";
        j << "    \"shader_cache_hits\": " << gpu_.cache_hits.load() << ",\n";
        j << "    \"shader_cache_misses\": " << gpu_.cache_misses.load() << ",\n";
        j << "    \"submits\": " << gpu_.submits.load() << ",\n";
        j << "    \"gpu_faults\": " << gpu_.gpu_faults.load() << ",\n";
        j << "    \"device_lost\": " << gpu_.device_lost.load() << ",\n";
        j << "    \"texture_bytes\": " << gpu_.texture_bytes.load() << "\n";
        j << "  },\n";

        j << "  \"services_top\": [\n";
        struct Row { int num; std::uint64_t calls; Nanos total; Nanos max; };
        std::vector<Row> rows;
        for (std::size_t i = 0; i < ServiceProfiler::kSlots; ++i) {
            const auto e = services_.entry(static_cast<std::uint32_t>(i));
            if (e.calls)
                rows.push_back({ static_cast<int>(i), e.calls, e.total_ns, e.max_ns });
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.total > b.total; });
        for (std::size_t i = 0; i < rows.size() && i < 16; ++i) {
            j << "    {\"svc\": " << rows[i].num
              << ", \"calls\": " << rows[i].calls
              << ", \"total_ms\": " << std::setprecision(4) << (rows[i].total / 1e6)
              << ", \"max_us\": " << (rows[i].max / 1e3) << "}";
            if (i + 1 < rows.size() && i < 15)
                j << ",";
            j << "\n";
        }
        j << "  ]\n";
        j << "}\n";
    }

    // --- CSV de frames (anexado) ---------------------------------------
    {
        const std::string csv = base + ".csv";
        const bool need_header =
            !std::filesystem::exists(csv) || std::filesystem::file_size(csv, ec) == 0;
        std::ofstream c(csv, std::ios::app);
        if (!c)
            return false;
        if (need_header)
            c << "frame,total_ms,cpu_ms,gpu_ms,translate_ms,shader_ms,"
                 "present_ms,draws,tris,bottleneck\n";
        for (const auto& f : history_)
            c << f.index << ',' << (f.total_ns / 1e6) << ',' << (f.cpu_ns / 1e6)
              << ',' << (f.gpu_ns / 1e6) << ',' << (f.translate_ns / 1e6) << ','
              << (f.shader_ns / 1e6) << ',' << (f.present_ns / 1e6) << ','
              << f.draw_calls << ',' << f.triangles << ','
              << to_string(f.bottleneck) << '\n';
    }

    return true;
}

std::string Telemetry::summary() const {
    std::ostringstream o;
    o << "══════════ EdenMaliIA — telemetria ══════════\n";
    if (!title_name_.empty())
        o << "  título      : " << title_name_ << "  [" << title_id_ << "]\n";
    o << std::fixed;
    o << std::setprecision(2);
    o << "  FPS médio   : " << average_fps() << "   ("
      << (avg_frame_ns_.load() / 1e6) << " ms/frame)\n";
    o << "  gargalo     : " << to_string(dominant_) << "\n";
    o << "\n  ── execução nativa ──\n";
    o << "  SVC calls   : " << exec_.svc_calls.load() << "\n";
    o << "  page faults : " << exec_.page_faults.load() << "\n";
    o << "  align faults: " << exec_.alignment_faults.load() << "\n";
    o << "\n  ── GPU ──\n";
    o << "  draw calls  : " << gpu_.draw_calls.load() << "\n";
    o << "  triangles   : " << gpu_.triangles.load() << "\n";
    o << "  shaders     : " << gpu_.shaders_compiled.load()
      << "  (cache " << gpu_.cache_hits.load() << "/"
      << gpu_.cache_misses.load() << ")\n";
    o << "  submits     : " << gpu_.submits.load() << "\n";
    o << "  faults      : " << gpu_.gpu_faults.load()
      << "   device_lost: " << gpu_.device_lost.load() << "\n";

    const int h = services_.hottest();
    if (h >= 0) {
        const auto e = services_.entry(static_cast<std::uint32_t>(h));
        o << "\n  ── serviço mais caro ──\n";
        o << "  svc " << h << "  calls=" << e.calls
          << "  total=" << std::setprecision(2) << (e.total_ns / 1e6) << " ms"
          << "  max=" << (e.max_ns / 1e3) << " µs\n";
    }
    o << "════════════════════════════════════════════\n";
    return o.str();
}

void Telemetry::maybe_print_live() {
    if (!live_)
        return;
    // Uma linha por segundo, não por frame — a 60 fps enche a tela e esconde
    // a informação.
    if (++live_counter_ % 60 != 0)
        return;
    const FrameRecord& f = last_;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "[ia] #%-6" PRIu64 " %6.2f fps  %6.2f ms  "
                  "cpu=%5.2f gpu=%5.2f tr=%5.2f sh=%5.2f pf=%5.2f  draws=%-5u  %s\n",
                  f.index, average_fps(), f.total_ns / 1e6, f.cpu_ns / 1e6,
                  f.gpu_ns / 1e6, f.translate_ns / 1e6, f.shader_ns / 1e6,
                  f.present_ns / 1e6, f.draw_calls, to_string(f.bottleneck));
    std::fputs(line, stdout);
    std::fflush(stdout);
}

} // namespace ia::tel
