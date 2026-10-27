// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Teste de desenho integrado: memoria sintetica do guest -> rasterizador -> Vulkan.
// DADOS SINTETICOS (nao de jogo). Shader de teste artificial.
#include "g610/gpu/rasterizer_g610.hpp"
#include "g610/gpu/vulkan_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

static int run_cmd(const std::string& cmd) {
    int r = std::system((cmd + " 2>/dev/null").c_str());
    return (r == -1) ? -1 : (r >> 8);
}
static bool write_file(const std::string& p, const std::string& d) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(d.data(), d.size());
    return (bool)f;
}
static bool read_file(const std::string& p, std::vector<char>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size_t n = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(n);
    f.read(out.data(), n);
    return (bool)f;
}

int main() {
    const char* vert = R"(#version 450
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
)";
    const char* frag = R"(#version 450
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(1.0, 0.0, 0.0, 1.0); }
)";
    write_file("/tmp/intdraw.vert", vert);
    write_file("/tmp/intdraw.frag", frag);
    if (run_cmd("glslangValidator -V --target-env vulkan1.1 -S vert -o /tmp/intdraw.vert.spv /tmp/intdraw.vert") != 0) {
        std::fprintf(stderr, "glslang vert fail\n"); return 1;
    }
    if (run_cmd("glslangValidator -V --target-env vulkan1.1 -S frag -o /tmp/intdraw.frag.spv /tmp/intdraw.frag") != 0) {
        std::fprintf(stderr, "glslang frag fail\n"); return 1;
    }
    std::vector<char> vs, fs;
    if (!read_file("/tmp/intdraw.vert.spv", vs) || !read_file("/tmp/intdraw.frag.spv", fs)) {
        std::fprintf(stderr, "read spv fail\n"); return 1;
    }

    // Dados sinteticos de vertices: triangulo parcial.
    // 3 vertices, stride 8 (2 floats), R32G32, attribute 0.
    struct V { float x, y; };
    V verts[3] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}};
    const std::uint64_t base_va = 0x200;
    const std::size_t total = sizeof(verts);

    auto mock_le = [&](std::uint64_t src, void* dst, std::size_t size) -> bool {
        if (src != base_va) return false;
        if (size != total) return false;
        std::memcpy(dst, verts, size);
        return true;
    };

    auto backend = std::make_shared<g610::gpu::VulkanBackend>();
    std::string err;
    if (!backend->init(err)) { std::fprintf(stderr, "init fail: %s\n", err.c_str()); return 1; }

    g610::gpu::RasterizerG610 r{nullptr, nullptr, mock_le, backend};

    // Simula um DrawCommand decodificado.
    g610::gpu::VertexAttributeDesc a0;
    a0.enabled = true;
    a0.buffer = 0;
    a0.offset = 0;
    a0.size_enum = 0x04; // R32G32
    a0.type_enum = 7;     // Float
    a0.component_count = 2;
    a0.size_bytes = 8;
    a0.stream_stride = 8;
    std::vector<g610::gpu::VertexAttributeDesc> attrs = {a0};

    r.AnotaDesenho(3, 0, false, base_va, 8, attrs, true, 4); // topology Triangles
    bool ok = r.desenhar_integrado(vs, fs);
    std::printf("desenhar_integrado: %s\n", ok ? "OK" : "FALHOU");

    backend->shutdown();
    return ok ? 0 : 1;
}
