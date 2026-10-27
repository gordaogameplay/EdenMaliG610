// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Teste de decodificacao de atributos de vertices Maxwell (ETAPA 6).
#include <cstdio>
#include <cstring>
#include <vector>

#include "g610/gpu/maxwell_comandos.hpp"
#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"
#include "g610/telemetry.hpp"

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* what) {
    ++g_pass;
    std::printf("  %s: %s\n", ok ? "ok" : "FALHOU", what);
    if (!ok) ++g_fail;
}
std::uint32_t cabecalho(std::uint32_t metodo, std::uint32_t n) {
    return (metodo & 0x1FFFu) | ((n & 0x1FFFu) << 16) | (1u << 29); // modo Increasing
}
void push(std::vector<std::uint8_t>& b, std::uint32_t w) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&w);
    b.insert(b.end(), p, p + 4);
}
}

int main() {
    using namespace g610::gpu;
    // Atributo 0: R32G32, Float, buffer 0, offset 0, constant 0.
    // raw = size(0x04)<<21 | type(7)<<27 | buffer 0
    std::uint32_t attr0 = (0x04u << 21) | (7u << 27);
    // vertex stream 0: stride 8, enable bit 12 => 0x00001008
    std::uint32_t vs0_stride_enable = 8 | (1u << 12);
    std::uint32_t vs0_addr_hi = 0x000000AB;
    std::uint32_t vs0_addr_lo = 0xCD000000;
    // topology_override raw (lower 16 bits)
    std::uint32_t topo = 0x0004; // Triangles
    // primitive_topology_control = 1 (UseSeparateState)
    std::uint32_t ptc = 1;

    std::vector<std::uint8_t> buf;
    // Bind subcanal 0 ao motor Maxwell3D.
    push(buf, cabecalho(0, 1)); push(buf, 0xB197u);
    // vertex_attrib_format[0] @ 0x458 (byte 0x1160)
    push(buf, cabecalho(0x458, 1)); push(buf, attr0);
    // primitive_topology_control @ 0x652 (byte 0x1948)
    push(buf, cabecalho(0x652, 1)); push(buf, ptc);
    // topology_override @ 0x65C (byte 0x1970)
    push(buf, cabecalho(0x65C, 1)); push(buf, topo);
    // vertex_streams[0] @ 0x700 (byte 0x1C00)
    push(buf, cabecalho(0x700, 1)); push(buf, vs0_stride_enable);
    push(buf, cabecalho(0x701, 1)); push(buf, vs0_addr_hi);
    push(buf, cabecalho(0x702, 1)); push(buf, vs0_addr_lo);
    // vertex_buffer_count @ 862
    push(buf, cabecalho(maxwell3d::vertex_buffer_count, 1)); push(buf, 3);
    // draw_begin @ 1414
    push(buf, cabecalho(maxwell3d::draw_begin, 1)); push(buf, 0);
    // draw_end @ 1413
    push(buf, cabecalho(maxwell3d::draw_end, 1)); push(buf, 0);

    g610::tel::Telemetry tel;
    MaxwellTranslator tr(tel);
    std::vector<DrawCommand> cmds;
    std::string err;
    std::size_t n = tr.translate(buf, cmds, err);
    check(err.empty(), "sem erro de traducao");
    check(cmds.size() == 1, "um DrawCommand");
    if (cmds.size() == 1) {
        const auto& c = cmds[0];
        check(c.vertex_count == 3, "vertex_count=3");
        check(c.attributes.size() == 1, "um atributo decodificado");
        if (!c.attributes.empty()) {
            const auto& a = c.attributes[0];
            check(a.enabled, "atributo habilitado");
            check(a.component_count == 2, "component_count=2");
            check(a.size_bytes == 8, "size_bytes=8");
            check(a.type_enum == 7, "type Float");
            check(a.stream_stride == 8, "stream_stride=8");
        }
        check(c.vertex_base_va == ((std::uint64_t)vs0_addr_hi << 32 | vs0_addr_lo), "vertex_base_va correto");
        check(c.vertex_stride == 8, "vertex_stride=8");
        check(c.primitive_topology_control_raw == 1, "ptc=1");
        check(c.topology_raw == topo, "topology_raw capturado");
    }
    std::printf("\n%d verificacoes, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
