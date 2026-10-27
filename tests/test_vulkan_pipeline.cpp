// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// Teste de pipeline gráfico Vulkan real: triângulo offscreen + readback.
#include "g610/gpu/vulkan_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
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
void main() {
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";
    const char* frag = R"(#version 450
layout(location = 0) out vec4 o_color;
void main() {
    o_color = vec4(1.0, 0.0, 0.0, 1.0);
}
)";

    if (!write_file("/tmp/vkpipe.vert", vert)) { std::fprintf(stderr, "write vert fail\n"); return 1; }
    if (!write_file("/tmp/vkpipe.frag", frag)) { std::fprintf(stderr, "write frag fail\n"); return 1; }

    if (run_cmd("glslangValidator -V --target-env vulkan1.1 -S vert -o /tmp/vkpipe.vert.spv /tmp/vkpipe.vert") != 0) {
        std::fprintf(stderr, "glslang vert fail\n"); return 1;
    }
    if (run_cmd("glslangValidator -V --target-env vulkan1.1 -S frag -o /tmp/vkpipe.frag.spv /tmp/vkpipe.frag") != 0) {
        std::fprintf(stderr, "glslang frag fail\n"); return 1;
    }

    std::vector<char> vs, fs;
    if (!read_file("/tmp/vkpipe.vert.spv", vs) || vs.empty()) { std::fprintf(stderr, "read vert spv fail\n"); return 1; }
    if (!read_file("/tmp/vkpipe.frag.spv", fs) || fs.empty()) { std::fprintf(stderr, "read frag spv fail\n"); return 1; }

    g610::gpu::VulkanBackend backend;
    std::string err;
    if (!backend.init(err)) { std::fprintf(stderr, "init fail: %s\n", err.c_str()); return 1; }

    auto res = backend.draw_triangle(vs, fs);
    std::printf("PipelineDrawResult: ok=%d checked=%u red=%u bg=%u other=%u\n",
                res.ok ? 1 : 0, res.pixels_checked, res.pixels_red, res.pixels_background, res.pixels_other);
    if (!res.error.empty()) std::printf("erro: %s\n", res.error.c_str());

    backend.shutdown();
    return res.ok ? 0 : 1;
}
