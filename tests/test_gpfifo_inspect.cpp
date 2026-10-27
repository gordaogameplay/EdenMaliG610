// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Ferramenta de inspecao de capturas GPFIFO (ETAPA 9).
// Uso: g610_gpfifo_inspect <arquivo.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "g610/gpu/maxwell_comandos.hpp"
#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"
#include "g610/telemetry.hpp"

namespace {
bool read_file(const std::string& p, std::vector<std::uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size_t n = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(n);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return (bool)f;
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <arquivo.bin>\n", argv[0]);
        return 2;
    }
    std::string path = argv[1];
    std::vector<std::uint8_t> data;
    if (!read_file(path, data)) {
        std::fprintf(stderr, "erro: nao foi possivel ler %s\n", path.c_str());
        return 2;
    }
    std::printf("arquivo: %s\n", path.c_str());
    std::printf("tamanho: %zu bytes (%zu palavras)\n", data.size(), data.size() / 4);
    if (data.empty()) {
        std::printf("resultado: arquivo vazio\n");
        return 0;
    }
    if (data.size() % 4 != 0) {
        std::printf("aviso: tamanho nao multiplo de 4 (truncado?)\n");
    }

    // Identificar metodos relevantes via PushBufferReader.
    g610::gpu::PushBufferReader reader;
    std::uint32_t draw_begin = 0, draw_end = 0, vertex_count = 0, index_count = 0;
    std::size_t total_methods = 0;
    reader.for_each(data, [&](const g610::gpu::MethodCall& m) {
        ++total_methods;
        if (m.method == g610::gpu::maxwell3d::draw_begin) ++draw_begin;
        if (m.method == g610::gpu::maxwell3d::draw_end) ++draw_end;
        if (m.method == g610::gpu::maxwell3d::vertex_buffer_count) vertex_count = m.argument;
        if (m.method == g610::gpu::maxwell3d::index_buffer_count) index_count = m.argument;
    });
    std::printf("metodos lidos: %zu\n", total_methods);
    std::printf("descartados: %zu\n", reader.descartados());
    std::printf("draw_begin (1414): %u\n", draw_begin);
    std::printf("draw_end (1413): %u\n", draw_end);
    std::printf("vertex_buffer_count (ultimo): %u\n", vertex_count);
    std::printf("index_buffer_count (ultimo): %u\n", index_count);

    // Traduzir.
    g610::tel::Telemetry tel;
    g610::gpu::MaxwellTranslator tr(tel);
    std::vector<g610::gpu::DrawCommand> cmds;
    std::string err;
    std::size_t n = tr.translate(data, cmds, err);
    std::printf("comandos traduzidos: %zu\n", n);
    if (!err.empty()) std::printf("erro de traducao: %s\n", err.c_str());
    std::printf("DrawCommands produzidos: %zu\n", cmds.size());

    if (cmds.empty()) {
        std::printf("resultado: NENHUM DrawCommand (sem draw_end real ou formato invalido)\n");
    } else {
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            const auto& c = cmds[i];
            std::printf("  [%zu] method=%u indexed=%d vertices=%u indices=%u "
                        "va=0x%llx stride=%u attrs=%zu topo_known=%d topology=%u "
                        "ptc=%u topo_raw=0x%x\n",
                        i, c.method, c.indexado ? 1 : 0, c.vertex_count, c.index_count,
                        (unsigned long long)c.vertex_base_va, c.vertex_stride,
                        c.attributes.size(), c.topology_known ? 1 : 0, c.topology,
                        c.primitive_topology_control_raw, c.topology_raw);
        }
    }
    return 0;
}
