// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — executor nativo de ROMs de Switch para RK3588 / Mali-G610.
//
// Ponto de entrada. Faz três coisas, por ordem:
//
//   1. Lê o NPDM e decide se o programa pode correr nativamente (39-bit +
//      64-bit). Esta decisão é a fronteira com o piDroid: é o único campo
//      do ficheiro do programa que o motor controla.
//   2. Carrega o NSO para o espaço de endereçamento do guest, sem traduzir
//      uma única instrução.
//   3. Arranca a telemetria e mantém a instrumentação ligada durante toda a
//      sessão.
//
// A instrumentação não é opcional neste motor: sem ela não há forma de saber
// se um travão é da CPU, da GPU, da tradução ou do display.
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <filesystem>

#include "g610/gpu/maxwell_translator.hpp"
#include "g610/patcher.hpp"
#include "g610/guest_program.hpp"
#include "g610/nso_loader.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"
#include "g610/eden_bridge.hpp"
#include "g610/native_engine.hpp"

namespace {

void print_usage(const char* argv0) {
    std::cout
        << "EdenMaliG610 — executor nativo Switch -> RK3588\n\n"
        << "uso:\n"
        << "  " << argv0 << " doctor <main.nso> [--npdm <main.npdm>]\n"
        << "  " << argv0 << " exefs <exefs_dir>      programa multi-módulo\n"
        << "      Lê o programa, valida 64-bit/39-bit e imprime o plano de\n"
        << "      execução sem arrancar nada.\n\n"
        << "  " << argv0 << " run <main.nso> [--npdm <main.npdm>]\n"
        << "      Executa com telemetria ligada. Ver docs/ARQUITETURA.md para\n"
        << "      o estado de implementação das fases.\n\n"
        << "  " << argv0 << " probe\n"
        << "      Só telemetria: imprime o ambiente medido (CPU, GPU, clock).\n";
}

std::string arg_value(int argc, char** argv, const char* flag, std::string def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return def;
}

// ---------------------------------------------------------------------------
// probe — o inventário do que a máquina consegue, medido.
// ---------------------------------------------------------------------------
void cmd_probe(g610::tel::Telemetry& tel) {
    std::cout << "\n══════ ambiente medido ══════\n";

    std::cout << "  CPU      : ";
    {
        char buf[256];
        std::FILE* f = std::fopen("/proc/cpuinfo", "r");
        if (f) {
            bool got = false;
            while (std::fgets(buf, sizeof(buf), f)) {
                if (std::strncmp(buf, "model name", 10) == 0) {
                    char* c = std::strchr(buf, ':');
                    if (c) { std::cout << c + 2; got = true; }
                    break;
                }
            }
            std::fclose(f);
            if (!got)
                std::cout << "(não identificado)";
        }
    }
    std::cout << "  núcleos  : " << std::thread::hardware_concurrency() << "\n";

    std::cout << "  GPU clock: ";
    if (std::FILE* f = std::fopen(
            "/sys/class/devfreq/fb000000.gpu/cur_freq", "r")) {
        char buf[64] = {};
        if (std::fgets(buf, sizeof(buf), f))
            std::cout << (std::strtoll(buf, nullptr, 10) / 1'000'000) << " MHz (pedido)"
                      << "\n";
        std::fclose(f);
    } else {
        std::cout << "fb000000.gpu ausente\n";
    }

    std::cout << "  telemetria: " << (tel.exec().svc_calls.load()
                                             ? "ativa"
                                             : "pronta")
              << "\n";
    std::cout << "══════════════════════════════\n\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string cmd = argv[1];

    // --- telemetria: ligada antes de tudo, para capturar também o arranque ---
    g610::tel::Telemetry telemetry;
    telemetry.set_output_dir("g610-telemetry");
    telemetry.set_frame_history(1800); // ~30 s a 60 fps
    telemetry.start("0000000000000000", cmd);
    telemetry.set_live_console(true);

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        print_usage(argv[0]);
        return 0;
    }

    if (cmd == "probe") {
        cmd_probe(telemetry);
        telemetry.flush("probe");
        return 0;
    }

    if (cmd != "doctor" && cmd != "run" && cmd != "exefs") {
        std::cerr << "comando desconhecido: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // --- exefs: programa multi-módulo --------------------------------------
    //
    // Carrega rtld + main + sdk, planeia o espaço de endereçamento, mapeia e
    // patcha. Não executa: o objectivo é responder "está tudo no sítio certo?"
    // antes de correr um programa de 39 bits.
    if (cmd == "exefs") {
        if (argc < 3) {
            std::cerr << "uso: " << argv[0] << " exefs <exefs_dir>\n";
            return 2;
        }
        g610::GuestProgram prog(telemetry);
        std::string err;
        if (!prog.load(argv[2], err)) {
            std::cerr << "\ncarregamento: " << err << "\n";
            return 3;
        }
        if (!prog.map_into(128ull * 1024 * 1024, err)) {
            std::cerr << "\nmapeamento: " << err << "\n";
            return 3;
        }
        if (!prog.patch_all(err)) {
            std::cerr << "\npatch: " << err << "\n";
            return 3;
        }
        std::cout << "\n══════ programa multi-módulo ══════\n" << prog.summary();
        std::cout << "  heap base : 0x" << std::hex << g610::layout::kHeapBase << std::dec
                  << " (SetHeapSize devolve este endereço)\n";
        std::cout << "\n  NOTA: o entry point é o do rtld, não o do main. O main\n"
                  << "  só corre quando o rtld o chama, depois de mapear a memória\n"
                  << "  e resolver os imports a partir da sdk.\n";
        telemetry.flush("exefs");
        return 0;
    }

    if (argc < 3) {
        std::cerr << "falta o caminho do NSO\n";
        return 1;
    }

    namespace fs = std::filesystem;
    const std::string nso_path = [&]() -> std::string {
        // Se o utilizador passou um directório ExeFS, resolve para o NSO principal.
        if (fs::is_directory(argv[2]))
            return std::string(argv[2]) + "/main";
        return argv[2];
    }();

    // --- 1. NPDM: a decisão nativo vs JIT --------------------------------
    std::string npdm_path = arg_value(argc, argv, "--npdm", "");
    if (npdm_path.empty())
        npdm_path = g610::NsoLoader::find_npdm(nso_path);

    g610::LoadedModule mod;
    std::string error;

    if (!npdm_path.empty()) {
        if (!g610::NsoLoader{}.load_npdm(npdm_path, mod, error)) {
            std::cerr << "NPDM: " << error << "\n";
        }
    } else {
        std::cout << "[aviso] NPDM não encontrado; assumindo 39-bit.\n"
                  << "        use --npdm <ficheiro> para ser explícito.\n";
        mod.address_space_type = 3;
        mod.has_64_bit         = true;
    }

    std::cout << "\n══════ programa ══════\n";
    std::cout << "  npdm      : " << (npdm_path.empty() ? "(nenhum)" : npdm_path)
              << "\n";
    std::cout << "  64-bit    : " << (mod.has_64_bit ? "sim" : "NÃO") << "\n";
    std::cout << "  addr space: " << mod.address_space_type << " ("
              << (mod.address_space_type == 3 ? "Is39Bit" : "outro")
              << ")\n";

    const bool native_ok = mod.has_64_bit && mod.address_space_type == 3;
    std::cout << "  execução  : "
              << (native_ok ? "NATIVA (sem JIT)"
                            : "JIT — falta 64-bit ou address_space_type=3")
              << "\n";

    // --- 2. carga do NSO ---------------------------------------------------
    if (!g610::NsoLoader{}.load(nso_path, mod, error)) {
        std::cerr << "\nNSO: " << error << "\n";
        return 2;
    }

    std::cout << "\n══════ módulo ══════\n";
    std::cout << "  nome      : "
              << (mod.module_name.empty() ? "(sem nome)" : mod.module_name) << "\n";
    std::cout << "  versão    : 0x" << std::hex << mod.version << std::dec << "\n";
    std::cout << "  base      : 0x" << std::hex << mod.base << std::dec << "\n";
    std::cout << "  entry     : 0x" << std::hex << mod.entry_point << std::dec << "\n";
    std::cout << "  text      : " << mod.text_size()   << " bytes"
              << (mod.segments[g610::kNsoText].compressed ? " (LZ4)" : "") << "\n";
    std::cout << "  rodata    : " << mod.rodata_size() << " bytes"
              << (mod.segments[g610::kNsoRodata].compressed ? " (LZ4)" : "") << "\n";
    std::cout << "  data      : " << mod.data_size()   << " bytes"
              << (mod.segments[g610::kNsoData].compressed ? " (LZ4)" : "") << "\n";
    std::cout << "  bss       : " << mod.bss_size << " bytes\n";
    std::cout << "  imagem    : " << mod.image.size() << " bytes"
              << " (espaço virtual, com gaps)\n";
    // Os gaps entre segmentos não são um detalhe: é a diferença entre a
    // imagem estar correcta e a rodata estar 3328 bytes deslocada.
    for (int i = 0; i < 2; ++i) {
        const std::uint64_t end =
            mod.segments[i].vaddr + mod.segments[i].size;
        const std::uint64_t gap = mod.segments[i + 1].vaddr - end;
        if (gap > 0)
            std::cout << "  gap       : " << std::dec << gap
                      << " bytes antes de " << (i == 0 ? "rodata" : "data")
                      << std::hex << "\n";
    }

    // O entry point tem de estar dentro do segmento de texto: é o início do
    // código executável. Fora dele, o primeiro salto corre em dados.
    const bool entry_ok =
        mod.entry_point >= mod.base + mod.segments[g610::kNsoText].vaddr &&
        mod.entry_point < mod.base + mod.segments[g610::kNsoText].vaddr + mod.text_size();
    std::cout << "  entry ok  : " << (entry_ok ? "sim" : "NÃO — fora de .text")
              << "\n";

    // --- 3. patcher estático ------------------------------------------------
    // Corre uma vez, aqui. É o passo que converte o programa em código que o
    // A76 pode correr nativamente: os SVC passam a BRK (para o despachante
    // apanhar a trap) e os hints a NOP (um WFI bloquearia o guest à espera de
    // um evento que só o host pode tratar).
    g610::StaticPatcher patcher(telemetry);
    g610::PatchReport pr{};
    // O patcher varre o `.text` no SEU offset dentro da imagem — que é o
    // vaddr do segmento, não 0. Nos módulos medidos o text começa em 0, mas
    // isso é propriedade deste jogo, não da formato: varrer de 0 arrastaria os
    // gaps e o começo da rodata para dentro da zona de código.
    if (!mod.segment_inside(g610::kNsoText)) {
        std::cerr << "\npatch: o .text está fora da imagem do módulo\n";
        return 3;
    }
    if (!patcher.patch(mod.image, mod.segments[g610::kNsoText].vaddr,
                       mod.text_size(), pr, error)) {
        std::cerr << "\npatch: " << error << "\n";
        return 3;
    }

    std::cout << "\n══════ patch estático ══════\n";
    std::cout << pr.summary();

    // Um programa sem SVC não tem sistema de serviços: não vai chamar nada ao
    // host, ou o NPDM está mal lido, ou o jogo é um dado corrompido. Vale a
    // pena dizer em vez de deixar o guest parar na primeira trap sem explicação.
    if (!pr.saw_any_svc) {
        std::cout << "\n  AVISO: nenhum SVC encontrado no texto.\n"
                  << "          O guest não vai conseguir chamar o sistema.\n";
    }

    // --- 4. motor ----------------------------------------------------------
    g610::SvcDispatcher svc(telemetry);
    g610::gpu::MaxwellTranslator gpu(telemetry);

    std::cout << "\n══════ motor ══════\n";
    std::cout << "  SVC dispatcher    : activo ("
              << sizeof(g610::SvcDispatcher) << " bytes de estado)\n";
    std::cout << "  Maxwell->Valhall  : activo\n";
    std::cout << "  telemetria        : " << (native_ok ? "ligada" : "ligada (modo JIT not supported)")
              << "\n";

    if (cmd == "doctor") {
        std::cout << "\n  estado: diagnóstico completo. use 'run' para executar.\n";
        telemetry.flush("doctor");
        return 0;
    }

    // --- run ---------------------------------------------------------------
    // Usa GuestProgram (rtld + main + sdk), liga a memoria ao despachante,
    // conecta o Eden e arranca o motor nativo.
    const fs::path exefs_arg = argv[2];
    fs::path exefs_dir;
    if (fs::is_directory(exefs_arg)) {
        exefs_dir = exefs_arg;
    } else {
        exefs_dir = exefs_arg.parent_path();
        if (exefs_dir.empty())
            exefs_dir = ".";
    }

    g610::GuestProgram prog(telemetry);
    std::string err;
    if (!prog.load(exefs_dir.string(), err)) {
        std::cerr << "\ncarregamento: " << err << "\n";
        return 3;
    }
    if (!prog.map_into(128ull * 1024 * 1024, err)) {
        std::cerr << "\nmapeamento: " << err << "\n";
        return 3;
    }
    if (!prog.patch_all(err)) {
        std::cerr << "\npatch: " << err << "\n";
        return 3;
    }

    svc.bind_memory(&prog.memory());
    svc.set_heap_base(g610::layout::kHeapBase);
    svc.set_heap_size(g610::layout::kHeapSize);
    svc.set_alias_region(g610::layout::kAliasBase, g610::layout::kAliasSize);
    svc.set_stack_region(g610::layout::kStackBase, g610::layout::kStackSize);
    if (!svc.attach_eden(1, 1)) {
        std::cerr << "\nedem: nao foi possivel ligar o Eden ao despachante\n";
        return 4;
    }
    svc.set_eden_tls(g610::layout::kTlsBase);

    g610::NativeEngine engine(telemetry, svc);
    std::string engine_err;
    if (!engine.install_handlers(engine_err)) {
        std::cerr << "\nmotor: " << engine_err << "\n";
        return 5;
    }
    engine.set_guest_tls(g610::layout::kTlsBase);
    engine.set_guest_x1(svc.main_thread_handle());

    std::cout << "\n══════ motor ══════\n";
    std::cout << "  SVC dispatcher    : activo (memoria ligada, Eden activo)\n";
    std::cout << "  vi servicos       : registados (vi:m, vi:s, vi:u)\n";
    std::cout << "  vsync thread      : arrancada (60 Hz)\n";
    std::cout << "  Maxwell->Valhall  : activo\n";
    std::cout << "  telemetria        : ligada\n";

    auto result = engine.run(
        reinterpret_cast<void(*)()>(prog.entry_point()), prog.stack_top());

    std::cout << "\n══════ resultado da execucao ══════\n";
    std::cout << "  SVCs    : " << result.svc_calls << "\n";
    std::cout << "  traps   : " << result.traps << "\n";
    std::cout << "  faults  : " << result.faults << "\n";
    std::cout << "  host_f  : " << result.host_faults << "\n";
    std::cout << "  saiu    : " << (result.exited ? "sim" : "nao") << "\n";
    if (!result.error.empty())
        std::cout << "  erro    : " << result.error << "\n";
    telemetry.flush("run");
    return 0;
}
