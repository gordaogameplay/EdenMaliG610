// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// Teste do leitor do push buffer e do tradutor Maxwell.
//
// O que este teste fixa
// ---------------------
// 1. **O formato do push buffer**: um fluxo de métodos — cada método
//    começa por um `CommandHeader` (o método nos bits 0..12, o subcanal
//    em 13..15, o número de argumentos em 16..28, o modo em 29..31) e
//    é seguido pelos argumentos. É a máquina de estados do upstream
//    (`dma_pusher.cpp`, `ProcessCommands`).
//
// 2. **Os IDs dos comandos**: `draw.begin` = 1414, `draw.end` = 1413,
//    `vertex_buffer.count` = 862, `index_buffer.count` = 1528. Estes
//    números **não são suposições**: são os índices dos registos no
//    motor do upstream, extraídos compilando o cabeçalho dele
//    (`tools/extrair_comandos_maxwell.py`).
//
// Sem este teste, um push buffer mal lido aparece como "o jogo não
// desenha" — e não se sabe se o formato está errado ou se o shader é.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "g610/gpu/maxwell_comandos.hpp"
#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"
#include "g610/telemetry.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    ++g_pass;
    std::printf("  %s: %s\n", ok ? "ok" : "FALHOU", what.c_str());
    if (!ok)
        ++g_fail;
}

// O cabeçalho de um método, no formato do upstream
// (`Tegra::CommandHeader`, `dma_pusher.h`).
std::uint32_t cabecalho(std::uint32_t metodo, std::uint32_t subcanal,
                         std::uint32_t n_argumentos, std::uint32_t modo) {
    return (metodo & 0x1FFFu) | ((subcanal & 0x7u) << 13) |
           ((n_argumentos & 0x1FFFu) << 16) | ((modo & 0x7u) << 29);
}

void empurrar(std::vector<std::uint8_t>& buffer, std::uint32_t palavra) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&palavra);
    buffer.insert(buffer.end(), p, p + 4);
}

} // namespace

int main() {
    using namespace g610::gpu;

    // ── 1. os IDs reais ─────────────────────────────────────────────────
    std::printf("[1] os IDs dos comandos (do upstream)\n");
    check(maxwell3d::draw_begin == 1414, "draw.begin = 1414");
    check(maxwell3d::draw_end == 1413, "draw.end = 1413");
    check(maxwell3d::vertex_buffer_count == 862, "vertex_buffer.count = 862");
    check(maxwell3d::index_buffer_count == 1528, "index_buffer.count = 1528");
    check(maxwell3d::clear_surface == 1652, "clear_surface = 1652");

    // ── 2. o leitor: um método com argumentos ───────────────────────────
    std::printf("\n[2] o leitor do fluxo de metodos\n");
    {
        std::vector<std::uint8_t> buffer;
        empurrar(buffer, cabecalho(maxwell3d::vertex_buffer_count, 0, 1, modo::Increasing));
        empurrar(buffer, 6);  // o argumento: 6 vertices
        empurrar(buffer, cabecalho(maxwell3d::draw_begin, 0, 1, modo::Increasing));
        empurrar(buffer, 0);  // o `draw.begin` nao tem argumentos uteis
        empurrar(buffer, cabecalho(maxwell3d::draw_end, 0, 1, modo::Increasing));
        empurrar(buffer, 0);

        PushBufferReader leitor;
        std::vector<MethodCall> chamadas;
        const std::size_t n = leitor.for_each(buffer, [&](const MethodCall& m) {
            chamadas.push_back(m);
        });

        check(n == 3, "tres metodos lidos");
        check(chamadas.size() == 3, "tres chamadas");
        if (chamadas.size() == 3) {
            check(chamadas[0].method == maxwell3d::vertex_buffer_count,
                  "o primeiro metodo e' o vertex_buffer.count");
            check(chamadas[0].argument == 6, "o argumento do primeiro e' 6");
            check(chamadas[1].method == maxwell3d::draw_begin, "o segundo e' o draw.begin");
            check(chamadas[2].method == maxwell3d::draw_end, "o terceiro e' o draw.end");
        }
        check(leitor.descartados() == 0, "nada descartado");
    }

    // ── 3. o leitor: NonIncreasing (um bloco de argumentos) ─────────────
    std::printf("\n[3] o modo NonIncreasing (um bloco de argumentos)\n");
    {
        std::vector<std::uint8_t> buffer;
        empurrar(buffer, cabecalho(maxwell3d::vertex_buffer_count, 0, 3, modo::NonIncreasing));
        empurrar(buffer, 10);
        empurrar(buffer, 20);
        empurrar(buffer, 30);

        PushBufferReader leitor;
        std::vector<MethodCall> chamadas;
        leitor.for_each(buffer, [&](const MethodCall& m) { chamadas.push_back(m); });

        check(chamadas.size() == 1, "o bloco e' uma chamada so");
        if (!chamadas.empty())
            check(chamadas[0].method == maxwell3d::vertex_buffer_count,
                  "a chamada e' do metodo certo");
    }

    // ── 4. o tradutor: um desenho ───────────────────────────────────────
    std::printf("\n[4] o tradutor produz o desenho\n");
    {
        std::vector<std::uint8_t> buffer;
        // **O bind** — o metodo 0x00 do canal (`BindObject`) liga o
        // subcanal 0 ao motor 3D (o upstream,
        // `src/video_core/engines/puller.cpp:29-58`). Sem isto o
        // tradutor nao sabe que o subcanal 0 e' o motor 3D.
        empurrar(buffer, cabecalho(canal::BindObject, 0, 1, modo::Increasing));
        empurrar(buffer, motor::Maxwell3D);
        empurrar(buffer, cabecalho(maxwell3d::vertex_buffer_count, 0, 1, modo::Increasing));
        empurrar(buffer, 6);
        empurrar(buffer, cabecalho(maxwell3d::draw_begin, 0, 1, modo::Increasing));
        empurrar(buffer, 0);  // o `draw.begin` nao tem argumentos uteis
        empurrar(buffer, cabecalho(maxwell3d::draw_end, 0, 1, modo::Increasing));
        empurrar(buffer, 0);

        g610::tel::Telemetry tel;
        MaxwellTranslator tradutor(tel);
        std::vector<DrawCommand> desenhos;
        std::string erro;
        tradutor.translate(buffer, desenhos, erro);

        check(erro.empty(), "sem erro");
        check(desenhos.size() == 1, "um desenho");
        if (desenhos.size() == 1) {
            check(desenhos[0].vertex_count == 6, "o desenho tem 6 vertices");
            check(!desenhos[0].indexado, "o desenho nao e' indexado");
            check(desenhos[0].instance_count == 1, "uma instancia");
            check(desenhos[0].est_triangles == 2, "2 triangulos estimados");
        }
    }

    // ── 5. o tradutor: um desenho indexado ──────────────────────────────
    std::printf("\n[5] o tradutor: um desenho indexado\n");
    {
        std::vector<std::uint8_t> buffer;
        // O bind — ver o teste [4].
        empurrar(buffer, cabecalho(canal::BindObject, 0, 1, modo::Increasing));
        empurrar(buffer, motor::Maxwell3D);
        empurrar(buffer, cabecalho(maxwell3d::index_buffer_count, 0, 1, modo::Increasing));
        empurrar(buffer, 12);
        empurrar(buffer, cabecalho(maxwell3d::draw_begin, 0, 1, modo::Increasing));
        empurrar(buffer, 0);  // o `draw.begin` nao tem argumentos uteis
        empurrar(buffer, cabecalho(maxwell3d::draw_end, 0, 1, modo::Increasing));
        empurrar(buffer, 0);

        g610::tel::Telemetry tel;
        MaxwellTranslator tradutor(tel);
        std::vector<DrawCommand> desenhos;
        std::string erro;
        tradutor.translate(buffer, desenhos, erro);

        check(desenhos.size() == 1, "um desenho");
        if (desenhos.size() == 1) {
            check(desenhos[0].indexado, "o desenho e' indexado");
            check(desenhos[0].index_count == 12, "12 indices");
            check(desenhos[0].vertex_count == 0, "sem vertices (e' indexado)");
        }
    }

    std::printf("\n%d ok, %d falhados\n", g_pass - g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
