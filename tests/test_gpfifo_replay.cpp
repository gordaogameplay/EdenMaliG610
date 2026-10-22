// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o teste deterministico da causa dos 0 comandos de desenho.
//
// ## O que este teste reproduz
//
// O push buffer REAL de 8864 bytes que o jogo (Pokemon Shield) submete
// ao `SubmitGPFIFO` — capturado na laranja na data de 22/10/2026, com o
// instrumento `G610_GPFIFO_DUMP` (`src/services/nvdrv.cpp`), no
// descendente `a32f268` + comentarios (nenhum codigo executavel
// mudava: ver o diario). O ficheiro e' `tests/fixtures/gpfifo_submit0.bin`.
//
// ## A cadeia medida (o log vivo contava assim)
//
//   - 2216 palavras (8864 bytes), **624 chamadas** de metodo,
//     **6 descartadas** (os 6 modos-0 do arranque) e **376 ids unicos**;
//   - os 5 `BindObject` nas palavras 1/3/5/7/9:
//     sc0=0xB197 (Maxwell 3D), sc1=0xB1C0, sc4=0xB0B5, sc2=0xA140,
//     sc3=0x902D;
//   - **os metodos 1413 (`draw_end`) e 1414 (`draw_begin`) NAO aparecem
//     no fluxo** — o `(draw_begin=1414 draw_end=1413)` do log antigo
//     era literal do formato, nao assinatura medida;
//   - o que o jogo manda como "desenho" sao chamadas de **macro MME**:
//     o metodo >= 0xE00 (o upstream: `maxwell_3d.cpp`,
//     `MacroRegistersStart = 0xE00`, `CallMacroMethod`), e a estrutura
//     medida e' `3594:29 3595:29 3612:1 3616:2 3624:2 3625:2 3626:1
//     3627:1` — a macro `(3594-0xE00)>>1 = 5` chamada **29 vezes**;
//   - as 2 chamadas no subcanal 6 (m=0xB, args 0x80000000/0x70000000)
//     sao de canal (semáforo/fence), nunca receberam `BindObject`.
//
// ## A partes do teste
//
//   - Parte A ('a foto do caso real') **passa**: fixa a procura de
//     entrada a partir dos bytes reais, para o teste futuro barrar o
//     reinvencao de comandos artificial.
//   - Parte B (**a execução das macros**) **passa desde o stub da
//     chamada de firmware 4**: o `translate()` executa as 29 macros de
//     desenho até ao fim (a medida: 17 passos e 5 envios por chamada,
//     as parámetros 3/3 — o tracer mediu o mesmo). O que a casa não
//     pode fazer com honestidade é produzir o `DrawCommand`: o desenho
//     do jogo dispara dentro do blob da chamada de firmware 4
//     (`falcon[4]`), que o próprio upstream substitui por um stub de
//     registo que desenha zero (`maxwell_3d.cpp:486-492`). O déficit
//     de desenho é medido e impresso — o próximo passo é que o fecha.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "g610/gpu/maxwell_translator.hpp"
#include "g610/gpu/pushbuffer.hpp"
#include "g610/telemetry.hpp"

#ifndef GPFIFO_FIXTURE
#define GPFIFO_FIXTURE "tests/fixtures/gpfifo_submit0.bin"
#endif

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* nome, const std::string& detalhe = {}) {
    std::printf("  %s: %s%s\n", ok ? "ok" : "FALHOU", nome,
                detalhe.empty() ? "" : (" — " + detalhe).c_str());
    if (ok)
        ++g_pass;
    else
        ++g_fail;
}

} // namespace

int main() {
    using namespace g610::gpu;

    // O ficheiro com o caso real.
    std::FILE* f = std::fopen(GPFIFO_FIXTURE, "rb");
    if (f == nullptr) {
        std::printf("FALHOU: fixture ausente em %s\n", GPFIFO_FIXTURE);
        return 1;
    }
    std::vector<std::uint8_t> pushbuffer;
    std::uint8_t fragmento[4096];
    std::size_t leitura;
    while ((leitura = std::fread(fragmento, 1, sizeof fragmento, f)) > 0)
        pushbuffer.insert(pushbuffer.end(), fragmento, fragmento + leitura);
    std::fclose(f);

    // ------------------------------------------------------------------------
    // Parte A — a foto do caso real (os bytes do jogo, sem invencao).
    // ------------------------------------------------------------------------
    PushBufferReader leitor;
    std::size_t chamadas = 0;
    std::size_t binds = 0;
    std::uint32_t bind_sc0 = 0;   // o motor ligado ao subcanal 0.
    std::size_t m1413 = 0, m1414 = 0;
    std::size_t macro_3594 = 0, macro_3595 = 0;
    std::size_t sc6_chamadas = 0;
    const std::size_t palavras = pushbuffer.size() / 4;

    leitor.for_each(pushbuffer, [&](const MethodCall& m) {
        ++chamadas;
        if (m.method == 0) {
            ++binds;
            if (m.subchannel == 0)
                bind_sc0 = m.argument;
        }
        if (m.method == 1413)
            ++m1413;
        if (m.method == 1414)
            ++m1414;
        if (m.method == 3594)
            ++macro_3594;
        if (m.method == 3595)
            ++macro_3595;
        if (m.subchannel == 6)
            ++sc6_chamadas;
    });

    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%X", bind_sc0);

    check(palavras == 2216, "palavras do caso real", std::to_string(palavras));
    check(chamadas == 624, "chamadas no fluxo (o log vivo: 624)",
          std::to_string(chamadas));
    check(leitor.descartados() == 6,
          "descartados (os modos 0 do arranque; o log vivo: 6)",
          std::to_string(leitor.descartados()));
    check(binds == 5, "BindObject: 5", std::to_string(binds));
    check(bind_sc0 == 0xB197, "subcanal 0 ligado a Maxwell 3D (0xB197)", hex);
    check(m1413 == 0, "draw_end (1413) AUSENTE no fluxo real");
    check(m1414 == 0, "draw_begin (1414) AUSENTE no fluxo real");
    check(macro_3594 == 29, "a macro 0xE0A invocada 29 vezes", std::to_string(macro_3594));
    check(macro_3595 == 29, "o ARGS 0xE0B invocado 29 vezes", std::to_string(macro_3595));
    check(sc6_chamadas == 2, "2 chamadas de canal no subcanal 6",
          std::to_string(sc6_chamadas));

    // ------------------------------------------------------------------------
    // Parte B — a execução das macros de desenho pelo tradutor.
    //
    // O microcode (o balde 49, 25 palavras, o `9e3ca7054d12d63c`)
    // termina leaps: antes do stub da chamada de firmware 4 ele contava
    // passos infinitamente (o laco `52..68` lia o rascunho 0xD00, que
    // ficava a 0) e o teto de 1M passos abortava — 29 desconhecidos.
    // Com o stub (`regs[0xD00] = 1`, o mesmo do upstream) o laco sai
    // desde o primeiro decremento. O contrato fixa os números medidos.
    // ------------------------------------------------------------------------
    g610::tel::Telemetry tel;
    MaxwellTranslator tradutor{tel};
    std::vector<DrawCommand> comandos;
    std::string erro;
    const std::size_t n = tradutor.translate(pushbuffer, comandos, erro);
    std::printf("  [medicao] translate(): %zu comandos, desconhecidos %llu, "
                "descartados %llu, passos %llu, envios %llu, erro: %s\n",
                n, static_cast<unsigned long long>(tradutor.commands_unknown()),
                static_cast<unsigned long long>(tradutor.commands_dropped_feature()),
                static_cast<unsigned long long>(tradutor.macro_passos()),
                static_cast<unsigned long long>(tradutor.macro_envios()),
                erro.empty() ? "(vazio)" : erro.c_str());

    check(tradutor.commands_unknown() == 0,
          "as 29 macros de desenho terminam (desconhecidos: 0)",
          std::to_string(tradutor.commands_unknown()));
    check(erro.empty(), "nenhum sintoma do microcode (teto/pc/parámetros)");

    // Os totais medidos do microcode no mesmo fluxo (a macro do
    // desenho e' 29 x (17 passos, 5 envios) = 493/145; o resto sao as
    // outras macros chamadas — entry 14/16/20 — 1144 passos e 756
    // envios). Se o porte do interpretador divergir do upstream, os
    // dois números caem e o teste barre.
    check(tradutor.macro_passos() == 1637,
          "os passos do microcode (o medido: 1637 = 493 + 1144)",
          std::to_string(tradutor.macro_passos()));
    check(tradutor.macro_envios() == 901,
          "os envios do microcode (o medido: 901 = 145 + 756)",
          std::to_string(tradutor.macro_envios()));

    // O defeito em pé: comandos ainda a 0 — o desenho vive no blob da
    // chamada de firmware 4 que o upstream também não executa. Medido e
    // impresso, não varrido para o tapete.
    std::printf("  [medicao] comandos: %zu — o desenho é disparado pelo blob\n"
                "  do falcon[4] (a chamada de firmware 4); o upstream substitui-o\n"
                "  pelo stub e também não desenha estes bytes\n", n);

    std::printf("\ngpfifo_replay: %d passaram, %d falharam\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
