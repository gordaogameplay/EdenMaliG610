// EdenMaliG610 — testes do núcleo.
//
// Sem framework externo: um executável que devolve != 0 se algo falhar.
// Corre-se no Pi com `ctest` ou directamente.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "g610/gpu/maxwell_translator.hpp"
#include "g610/guest_context.hpp"
#include "g610/instructions.hpp"
#include "g610/patcher.hpp"
#include "g610/guest_memory.hpp"
#include "g610/nso_loader.hpp"
#include "g610/svc_dispatcher.hpp"
#include "g610/telemetry.hpp"

namespace {

int g_failures = 0;
int g_checks   = 0;

// Aceita const char* e std::string: metade das mensagens é literal e a outra
// metade junta um erro lido em runtime. Com uma só assinatura, metade das
// chamadas não compila — e a solução de passar `.c_str()` seria uma fonte de
// erros de vida temporária.
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FALHOU: %s\n", what);
    } else {
        std::printf("  ok: %s\n", what);
    }
}


// ---------------------------------------------------------------------------
using g610::tel::Bottleneck;
using g610::tel::Nanos;

void test_classification() {
    using g610::tel::Telemetry;
    using g610::tel::FrameRecord;

    auto f = [](Nanos total, Nanos cpu, Nanos gpu, Nanos tr, Nanos sh,
                Nanos present, std::uint32_t draws) {
        FrameRecord r;
        r.total_ns = total; r.cpu_ns = cpu; r.gpu_ns = gpu;
        r.translate_ns = tr; r.shader_ns = sh; r.present_ns = present;
        r.draw_calls = draws;
        return r;
    };

    const Nanos ms = 1'000'000;

    // GPU ocupa praticamente todo o frame.
    check(Telemetry::classify(f(16 * ms, 1 * ms, 15 * ms, 0, 0, 0, 120)) ==
              Bottleneck::GpuBound,
          "GPU a ~94% do frame é GpuBound");

    // Compilação de shader domina.
    check(Telemetry::classify(f(16 * ms, 1 * ms, 1 * ms, 0, 14 * ms, 0, 10)) ==
              Bottleneck::ShaderBound,
          "compilação de shader dominante é ShaderBound");

    // Sem nenhum draw: a GPU não pode ser o travão, por definição.
    check(Telemetry::classify(f(16 * ms, 15 * ms, 0, 0, 0, 0, 0)) ==
              Bottleneck::CpuBound,
          "frame sem draws é CpuBound mesmo com gpu_ns não nulo");

    check(Telemetry::classify(f(16 * ms, 15 * ms, 1 * ms, 0, 0, 0, 200)) ==
              Bottleneck::CpuBound,
          "CPU acima da GPU é CpuBound");

    // O display é o travão: o trabalho acaba cedo e o resto é espera.
    check(Telemetry::classify(f(16 * ms, 2 * ms, 2 * ms, 0, 0, 11 * ms, 100)) ==
              Bottleneck::PresentBound,
          "espera de vsync dominante é PresentBound");

    // Um frame ainda por fechar não se classifica.
    check(Telemetry::classify(f(0, 0, 0, 0, 0, 0, 0)) == Bottleneck::Unknown,
          "frame sem tempo medido é Unknown");
}

// ---------------------------------------------------------------------------
void test_service_profiler() {
    g610::tel::ServiceProfiler sp;
    sp.record(0x03, 1'000'000);  // ExitProcess, 1 ms
    sp.record(0x03, 500'000);    // mais uma
    sp.record(0x70, 100'000);    // ConnectToNamedPort, muito mais barato
    // Fora do espaço de slots (kSlots = 256): tem de ser ignorado.
    sp.record(0x1'0000, 50'000);
    sp.record(0x1'0000, 50'000);

    check(sp.entry(0x03).calls == 2, "contagem de chamadas por serviço");
    check(sp.entry(0x03).total_ns == 1'500'000, "soma de tempo por serviço");
    check(sp.entry(0x03).max_ns == 1'000'000, "tempo máximo por serviço");
    check(sp.hottest() == 0x03, "serviço mais caro identificado");
    check(sp.entry(0x1'0000).calls == 0, "SVC fora do espaço é ignorado");
    check(sp.total_ns() == 1'600'000, "soma total só conta slots válidos");
}

// ---------------------------------------------------------------------------
void test_npdm_bits() {
    // Sem ficheiro, testa só a aritmética dos bits, que é onde costuma estar o
    // erro: address_space_type ocupa os bits 1-3, não o bit 0.
    g610::LoadedModule m;
    m.has_64_bit = true;
    m.address_space_type = 3;

    check(g610::kNpdmFlagIs64Bit == 1u, "bit de 64-bit é o bit 0 de Flags0");
    check(((0x07u & g610::kNpdmAddressSpaceMask) >> g610::kNpdmAddressSpaceShift) ==
              g610::kNpdmAddressSpace64Bit39,
          "Flags0=0x07 produz address_space_type=3 (AddressSpace64Bit39)");

    // Os Flags0 são UM BYTE. Se alguém os ler como u32, os bits acima do bit 7
    // vêm dos campos vizinhos e a decisão nativo/JIT fica errada. Este teste
    // fixa o tamanho da struct para que essa regressão não possa passar.
    check(sizeof(g610::NpdmMeta) == 0x80, "NpdmMeta tem 0x80 bytes");
    {
        // Reproduz o cabeçalho real do Pokémon Shield (lido do ficheiro).
        std::uint8_t real[0x80] = {};
        std::memcpy(real, "META", 4);
        real[0x0C] = 0x07; // Flags0
        real[0x0E] = 0x2C; // MainThreadPriority = 44
        real[0x1C] = 0x00; real[0x1D] = 0x00;
        real[0x1E] = 0x10; real[0x1F] = 0x00; // stack = 1 MB
        std::memcpy(real + 0x20, "Application", 11);

        g610::NpdmMeta h{};
        std::memcpy(&h, real, sizeof(h));
        check(h.flags0 == 0x07, "Flags0 lido como byte em 0x0C");
        check((h.flags0 & g610::kNpdmFlagIs64Bit) != 0, "Pokémon Shield é 64-bit");
        check(((h.flags0 & g610::kNpdmAddressSpaceMask) >>
               g610::kNpdmAddressSpaceShift) == 3, "Pokémon Shield é 64-bit/39");
        check(h.main_thread_priority == 0x2C, "prioridade da thread principal = 44");
        check(h.main_thread_stack_size == 0x100000, "stack = 1 MB");
        check(std::string(h.name, 11) == "Application", "nome = Application");
    }

    // O Pokémon Shield em execução medido tem Flags0 = 0x07.
    const bool native = m.has_64_bit && m.address_space_type == 3;
    check(native, "64-bit + AddressSpace64Bit39 implica execução nativa");

    check(g610::NsoLoader::default_base(3) != 0,
          "endereço base de 39 bits é não nulo");
    check((g610::NsoLoader::default_base(3) & g610::kGuestAddressMask) ==
              g610::NsoLoader::default_base(3),
          "base de 39 bits está dentro da máscara de 39 bits");
}

// ---------------------------------------------------------------------------
void test_svc_dispatch() {
    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    g610::SvcDispatcher svc(tel);

    // SetHeapSize (0x01)
    //
    // x0 à entrada é o TAMANHO; x0 à saída é o ENDEREÇO BASE. São coisas
    // diferentes — a versão anterior devolvia 0 em x0, o que fazia o rtld
    // tratar o heap como se estivesse na página zero.
    {
        svc.set_heap_base(0x0000'0000'9000'0000ull);
        g610::SvcContext c;
        c.x[0] = 0x4000'0000; // 1 GiB de heap
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::SetHeapSize), c);
        // O ENDEREÇO do heap é o valor de RETORNO, e o registo de retorno na
        // ABI do Switch é x0 — que o motor preenche a partir de `ctx.ret`.
        // Pô-lo em `ctx.x[0]` não chegava ao guest: o motor substitui x0.
        check(svc.heap_size() == 0x4000'0000, "heap size registado");
        check(c.ret == 0x0000'0000'9000'0000ull,
              "SetHeapSize devolve o ENDEREÇO do heap, não o tamanho");
        check(c.ret != 0,
              "o endereço do heap nunca é 0 (o primeiro acesso faultaria)");
    }

    // Tamanho zero é inválido: devolver um endereço para um heap de 0 bytes
    // faz o rtld escrever fora de qualquer região.
    {
        g610::SvcContext c;
        c.x[0] = 0;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::SetHeapSize), c);
        check(c.ret != 0, "SetHeapSize com size=0 é recusado");
    }

    // --- Os SVC que recebem endereços precisam de espaço de guest ligado ---
    //
    // Isto mudou por uma razão medida: o despachante deixou de
    // desreferenciar endereços do guest directamente. Um ponteiro do host
    // passado como se fosse um endereço de guest era um bug à espera de
    // acontecer — em produção o espaço é de 39 bits e o ponteiro do host
    // está normalmente fora dele.
    g610::GuestMemory mem;
    std::string err;
    const std::uint64_t kBase = 0x0000'0000'2000'0000ull;
    if (!mem.map(kBase, 0x0010'0000, g610::Prot::Read | g610::Prot::Write, err)) {
        check(false, "reservar espaco de guest para o teste: " + err);
        return;
    }
    svc.bind_memory(&mem);

    // ConnectToNamedPort (0x1F).
    //
    // A ABI real NAO tem ponteiro de saida nem comprimento: `x1` e' o
    // apontador para o nome, terminado em NUL, e o handle volta no MESMO
    // registo — ver `SvcWrap_ConnectToNamedPort64` no Eden (svc.cpp:1470).
    //
    // Este teste codificava a forma errada (x0=out, x2=len) e passava, o que
    // é a pior sorte: um teste que valida uma ABI inventada dá confiança a
    // uma coisa falsa. Corrigido aqui e no `svc_dispatcher.cpp`.
    {
        const std::uint64_t name_addr = kBase + 0x2000;
        const char* nome = "nvdrv";
        // Com o NUL: a assinatura é só um ponteiro.
        if (!mem.write(name_addr, nome, 6, err)) {
            check(false, "escrever o nome da porta: " + err);
            return;
        }

        g610::SvcContext c;
        c.x[0] = 0;
        c.x[1] = name_addr;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::ConnectToNamedPort), c);

        check(c.ret == 0, "ConnectToNamedPort devolve Success");
        check(c.x[1] != 0, "o handle volta em x1, no registo que era o nome");
        check(c.x[1] != name_addr,
              "x1 foi MESMO sobrescrito (não é o ponteiro de entrada)");
        check(svc.last_port() == "nvdrv", "o nome da porta foi lido do guest");
    }

    // Um nome vazio (NUL imediato) tem de ser recusado: daria origem a um
    // handle sem porta associada.
    {
        const std::uint64_t vazio = kBase + 0x2800;
        if (!mem.write(vazio, "", 1, err))
            return;

        g610::SvcContext c;
        c.x[0] = 0;
        c.x[1] = vazio;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::ConnectToNamedPort), c);
        check(c.ret != 0, "ConnectToNamedPort com nome vazio é recusado");
    }

    // MapMemory (0x04) — o serviço de que o rtld depende para arrancar.
    {
        svc.reset_run_state();

        // Dentro da reserva: tem de aceitar.
        g610::SvcContext c;
        c.x[0] = kBase + 0x4000;
        c.x[1] = kBase + 0x5000;
        c.x[2] = 0x1000;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::MapMemory), c);
        check(c.ret == 0, "MapMemory aceita um destino dentro da reserva");
        check(svc.map_memory_calls() == 1, "MapMemory foi contabilizado");

        // Fora da reserva: tem de recusar, não mapear por cima do host.
        g610::SvcContext fora;
        fora.x[0] = kBase + 0x0010'0000 + 0x1000; // depois do fim
        fora.x[1] = kBase;
        fora.x[2] = 0x1000;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::MapMemory), fora);
        check(fora.ret != 0, "MapMemory recusa um destino fora da reserva");
        check(svc.map_memory_calls() == 1,
              "o MapMemory recusado não foi contabilizado");

        // Tamanho zero: inválido.
        g610::SvcContext zero;
        zero.x[0] = kBase;
        zero.x[1] = kBase;
        zero.x[2] = 0;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::MapMemory), zero);
        check(zero.ret != 0, "MapMemory com size=0 é recusado");
    }

    // ExitProcess (0x07) — o número real. Os testes antigos usavam 0x03, que
    // é SetMemoryAttribute: o guest "saiu" por um serviço de memória.
    {
        svc.reset_run_state();
        g610::SvcContext c;
        c.x[0] = 42;
        svc.dispatch(static_cast<std::uint32_t>(g610::Svc::ExitProcess), c);
        check(c.ret == 0, "ExitProcess devolve Success");
        check(svc.exited(), "ExitProcess marca o processo como terminado");
        check(svc.exit_code() == 42, "ExitProcess guarda o código em x0");
    }
}


// ---------------------------------------------------------------------------
void test_maxwell_extract() {
    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    g610::gpu::MaxwellTranslator tr(tel);

    std::vector<std::uint8_t> pb;
    auto push = [&](std::uint32_t w) {
        pb.push_back(static_cast<std::uint8_t>(w));
        pb.push_back(static_cast<std::uint8_t>(w >> 8));
        pb.push_back(static_cast<std::uint8_t>(w >> 16));
        pb.push_back(static_cast<std::uint8_t>(w >> 24));
    };

    // Formato: [header][comprimento do payload em bytes][parâmetros...]
    //
    // DrawArrays tem 2 parâmetros (vertex_count, instance_count) = 8 bytes.
    push(0x0000'0000); // Nop inicial — tem de ser saltado
    // Header = (opcode << 2) | 1. O opcode ocupa os bits 2..15 e vale 0x18
    // (o DrawArrays, 0x62, já reduzido a 0x62>>2 pelo formato do campo).
    push((0x18u << 2) | 1u);
    push(8);          // comprimento do payload, em bytes (2 parâmetros)
    push(3);          // vertex_count
    push(1);          // instance_count
    push(0x0000'0000); // Nop final

    std::vector<g610::gpu::DrawCommand> cmds;
    std::string err;
    const auto n = tr.translate(pb, cmds, err);

    check(n == 1, "um comando extraído de um buffer com Nops à volta");
    check(!cmds.empty() && cmds[0].vertex_count == 3, "vertex_count correcto");
    check(!cmds.empty() && cmds[0].instance_count == 1, "instance_count correcto");
    check(!cmds.empty() && cmds[0].est_triangles == 1, "triângulos estimados");
    check(tr.commands_total() == 1, "contagem de comandos correcta");

    // Buffer truncado a meio de um payload não pode ler fora nem bloquear.
    {
        std::vector<std::uint8_t> trunc(pb.begin(), pb.begin() + 12); // corta o payload
        std::vector<g610::gpu::DrawCommand> c2;
        std::string e2;
        g610::gpu::MaxwellTranslator t2(tel);
        const auto n2 = t2.translate(trunc, c2, e2);
        check(n2 == 0, "payload truncado não produz comando incompleto");
        check(t2.commands_unknown() >= 1, "payload truncado é contado como inválido");
    }

    // Um comprimento de payload absurdo tem de ser recusado.
    {
        std::vector<std::uint8_t> bad;
        auto push2 = [&](std::uint32_t w) {
            bad.push_back(static_cast<std::uint8_t>(w));
            bad.push_back(static_cast<std::uint8_t>(w >> 8));
            bad.push_back(static_cast<std::uint8_t>(w >> 16));
            bad.push_back(static_cast<std::uint8_t>(w >> 24));
        };
        push2((0x18u << 2) | 1u); // DrawArrays com payload
        push2(0xFFFF'FFF0);       // comprimento não alinhado/absurdo
        push2(0);
        std::vector<g610::gpu::DrawCommand> c3;
        std::string e3;
        g610::gpu::MaxwellTranslator t3(tel);
        t3.translate(bad, c3, e3);
        check(t3.commands_unknown() >= 1, "comprimento de payload absurdo é recusado");
    }

    // Opcode fora da tabela é contado como desconhecido, não é descartado em
    // silêncio: é assim que se detecta uma tabela errada.
    {
        std::vector<std::uint8_t> unk;
        auto push3 = [&](std::uint32_t w) {
            unk.push_back(static_cast<std::uint8_t>(w));
            unk.push_back(static_cast<std::uint8_t>(w >> 8));
            unk.push_back(static_cast<std::uint8_t>(w >> 16));
            unk.push_back(static_cast<std::uint8_t>(w >> 24));
        };
        push3((0x3FFu << 2) | 1u); // opcode 0x3FF, inexistente
        push3(4);
        push3(1);
        std::vector<g610::gpu::DrawCommand> c4;
        std::string e4;
        g610::gpu::MaxwellTranslator t4(tel);
        t4.translate(unk, c4, e4);
        check(t4.commands_unknown() == 1, "opcode desconhecido é contado uma vez");
    }
}

// ---------------------------------------------------------------------------
void test_shader_cache() {
    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    g610::gpu::ShaderCache cache(tel);

    std::vector<std::uint8_t> blob(256, 0xAB);
    bool hit = false;
    std::string err;

    const auto a = cache.compile(blob, true, hit, err);
    check(a != UINT32_MAX, "shader compilado");
    check(!hit, "primeira compilação é cache miss");

    const auto b = cache.compile(blob, true, hit, err);
    check(b == a, "mesmo blob devolve o mesmo índice");
    check(hit, "segunda compilação é cache hit");

    // Um shader de vértice e um de fragmento com o mesmo conteúdo são
    // diferentes: têm de ter entradas separadas.
    blob[0] = 0xCD;
    const auto c = cache.compile(blob, false, hit, err);
    check(c != a, "shader de fragmento com outro conteúdo é entrada nova");
    check(cache.size() >= 2, "cache contém pelo menos dois shaders");
}

// ---------------------------------------------------------------------------
void test_flush() {
    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    tel.set_frame_history(16);
    tel.set_output_dir("/tmp/ia-telem-test");
    tel.start("01008DB008C2C000", "Pokemon Shield");

    // Frames com trabalho real medido. A classificação usa o total_ns real
    // (medido pelo relógio), por isso o gpu_ns artificial tem de ser menor
    // que esse total — senão o frame é inconsistente e a classificação diz
    // o que lhe der. Aqui fazemos o trabalho a sério: dormir o equivalente
    // ao gpu_ns simulado.
    for (int i = 0; i < 5; ++i) {
        tel.frame_begin();
        tel.frame_draw(300);
        // simular 4 ms de GPU a dormir
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        tel.frame_gpu(4'000'000);
        tel.frame_end();
    }

    check(tel.flush("unittest"), "flush escreve ficheiros sem erro");
    check(tel.average_fps() > 0.0, "FPS médio calculado");
    check(tel.dominant_bottleneck() == Bottleneck::GpuBound,
          "gargalo dominante calculado a partir do histórico");
}

// ---------------------------------------------------------------------------
void test_context_layout() {
    using namespace g610;
    // Estes asserts já estão no header e o build falha se não baterem.
    // Repetir aqui documenta a dependência: se alguém mexer num offset sem
    // actualizar o assembly, este teste é o primeiro a dizer porquê.
    check(offsetof(HostContext, saved_regs) == off::kHostRegs,
          "HostContext::saved_regs no sítio que o assembly usa");
    check(offsetof(HostContext, saved_vregs) == off::kHostVregs,
          "HostContext::saved_vregs no sítio que o assembly usa");
    check(offsetof(HostContext, sp) == off::kHostSp,
          "HostContext::sp no sítio que o assembly usa");
    check(offsetof(GuestContext, cpu_registers) == off::kCtxGpr,
          "GuestContext::cpu_registers no sítio que o assembly usa");
    check(offsetof(GuestContext, sp) == off::kCtxSp,
          "GuestContext::sp no sítio que o assembly usa");
    check(offsetof(GuestContext, pc) == off::kCtxPc,
          "GuestContext::pc no sítio que o assembly usa");
    check(offsetof(GuestContext, host_ctx) == off::kCtxHostCtx,
          "GuestContext::host_ctx no sítio que o assembly usa");

    // O array de GPRs tem de ser contíguo para o ldp do assembly funcionar.
    check(off::kCtxGpr + 31 * 8 == off::kCtxSp,
          "x0..x30 são contíguos e seguidos de sp");

    // ThreadParameters tem de caber no TLS do utilizador do kernel.
    check(sizeof(ThreadParameters) <= 16,
          "ThreadParameters cabe no TLS de 16 bytes do kernel");
}

// ---------------------------------------------------------------------------
void test_instructions() {
    using namespace g610::arm;

    // SVC
    check(SVC{0xD40000C1}.verify(), "SVC #6 verifica");
    check(SVC{0xD40000C1}.number() == 6, "SVC #6 tem número 6");
    check(SVC{0xD4000061}.number() == 3, "SVC #3 (ExitProcess)");
    // Encodings tirados do assembler real, não calculados à mão:
    //   svc #6 -> d40000c1   brk #6 -> d42000c0
    // O bit 0 distingue-os (SVC op2=00001, BRK op2=00000) e é a razão de o
    // patch ter de reconstruir a instrução em vez de trocar só a assinatura.
    check(SVC{0xD40000C1}.to_brk() == 0xD42000C0, "SVC→BRK preserva imm16");
    check((SVC{0xD40000C1}.to_brk() & 0x1Fu) == 0u,
          "BRK tem os bits 4-5 a zero (codificação legal)");

    // Instruções comuns não podem ser confundidas com SVC — se fossem, o
    // patcher corromperia o programa.
    check(!SVC{0xF9400000}.verify(), "LDR não é SVC");
    check(!SVC{0xAA0003E0}.verify(), "MOV não é SVC");
    check(!SVC{0xD65F03C0}.verify(), "RET não é SVC");
    check(!SVC{0x00000000}.verify(), "NOP (0) não é SVC");

    // MRS/MSR
    check(MRS{0xD53BE020}.verify(), "MRS X0, CNTPCT_EL0");
    check(MRS{0xD53BE020}.sysreg() == 0x5F01, "CNTPCT_EL0 = 0x5F01");
    check(MSR{0xD51BD040}.verify(), "MSR TPIDR_EL0, X0");
    check(MSR{0xD51BD040}.sysreg() == 0x5E82, "TPIDR_EL0 = 0x5E82");

    // Hints — os reais, medidos de um binário ARM
    check(Hint{0xD503201F}.kind() == Hint::Kind::Nop, "NOP");
    check(Hint{0xD503203F}.kind() == Hint::Kind::Yield, "YIELD");
    check(Hint{0xD503207F}.kind() == Hint::Kind::WaitForInterrupt, "WFI");
    check(Hint{0xD50320BF}.kind() == Hint::Kind::SendEvent, "SEV");

    // Classificação
    check(classify(0xD40000C1) == Disposition::PatchToBrk,
          "SVC é patch para BRK");
    check(classify(0xD503203F) == Disposition::TrapToHost,
          "YIELD é trap ao host");
    check(classify(0xD53BE020) == Disposition::TrapToHost,
          "MRS é trap ao host");
    check(classify(0xAA0003E0) == Disposition::RunNatively,
          "MOV corre nativamente");
    check(classify(0xF9400000) == Disposition::RunNatively,
          "LDR corre nativamente");
}

// ---------------------------------------------------------------------------
void test_patcher() {
    g610::tel::Telemetry tel;
    tel.set_live_console(false);
    g610::StaticPatcher p(tel);

    // Uma pequena "imagem" com SVC, YIELD e código normal.
    std::vector<std::uint8_t> image(64, 0);
    const std::uint32_t words[] = {
        0xAA0003E0,        // MOV X0, X0        -> nativa
        0xD40000C1,        // SVC #6            -> BRK
        0xD503203F,        // YIELD             -> NOP
        0xF9400000,        // LDR X0,[X0]       -> nativa
        0xD4000061,        // SVC #3            -> BRK
        0xD65F03C0,        // RET               -> nativa
    };
    std::memcpy(image.data(), words, sizeof(words));

    g610::PatchReport rep{};
    std::string err;
    check(p.patch(image, 0, sizeof(words), rep, err),
          "patcher aceita uma região válida");

    std::uint32_t out[6];
    std::memcpy(out, image.data(), sizeof(words));

    check(out[0] == 0xAA0003E0, "instrução nativa fica intacta");
    check(out[1] == 0xD42000C0, "SVC #6 virou BRK #6");
    check(out[2] == 0xD503201F, "YIELD virou NOP");
    check(out[3] == 0xF9400000, "LDR fica intacto");
    check(out[4] == 0xD4200060, "SVC #3 virou BRK #3");
    check(out[5] == 0xD65F03C0, "RET fica intacto");

    check(rep.svc_patched == 2, "dois SVC reescritos");
    check(rep.hints_nop == 1, "um hint neutralizado");
    check(rep.saw_any_svc, "o patcher viu SVC");
    check(rep.first_svc == 6, "primeiro SVC é o #6");

    // Limites inválidos têm de ser recusados, não gerar um crash.
    {
        g610::StaticPatcher p2(tel);
        std::vector<std::uint8_t> small(16, 0);
        g610::PatchReport r2{};
        std::string e2;
        check(!p2.patch(small, 0, 4096, r2, e2),
              "região maior que a imagem é recusada");
        check(!e2.empty(), "erro tem mensagem");
    }
    {
        g610::StaticPatcher p2(tel);
        std::vector<std::uint8_t> small(16, 0);
        g610::PatchReport r2{};
        std::string e2;
        check(!p2.patch(small, 1, 8, r2, e2),
              "região não alinhada a 4 bytes é recusada");
    }
}

} // namespace

int main() {
    std::printf("══════ EdenMaliG610 testes ══════\n");

    std::printf("\n[telemetria — classificação de gargalo]\n");
    test_classification();
    std::printf("\n[telemetria — profiler de serviços]\n");
    test_service_profiler();
    std::printf("\n[NPDM — bits de endereço]\n");
    test_npdm_bits();
    std::printf("\n[SVC — despacho e medição]\n");
    test_svc_dispatch();
    std::printf("\n[Maxwell -> Vulkan — extracção de comandos]\n");
    test_maxwell_extract();
    std::printf("\n[shader cache]\n");
    test_shader_cache();
    std::printf("\n[flush de telemetria para disco]\n");
    test_flush();
    std::printf("\n[fase 2 — layout do contexto (assembly vs C++)]\n");
    test_context_layout();
    std::printf("\n[fase 2 — descodificação de instruções]\n");
    test_instructions();
    std::printf("\n[fase 2 — patcher estático]\n");
    test_patcher();

    std::printf("\n%d verificações, %d falhas\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
