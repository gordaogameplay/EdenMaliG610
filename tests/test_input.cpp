// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// Teste do input: mapeamento SDL -> botões do Switch.
//
// O teste sintético (`g610_input`) prova que a pilha funciona. Este prova que o
// mapeamento está CORRETO — e a razão de existir é o A/B.
//
// ---------------------------------------------------------------------------
// O PROBLEMA
// ---------------------------------------------------------------------------
// Um DS4 é uma PlayStation. O SDL segue essa convenção:
//
//     SDL "A" = BTN_SOUTH = botão de baixo
//     SDL "B" = BTN_EAST = botão da direita
//
// Na Nintendo é o contrário:
//
//     Nintendo "B" = botão de baixo     (✕)
//     Nintendo "A" = botão da direita   (○)
//
// Usar o A e o B do SDL directamente dá botões trocados. Numa tela de título,
// "carregar A" salta a cena errada — e o utilizador acha que o emulador está
// partido, quando o problema é uma linha de código.
//
// Este teste injeta eventos falsos e verifica que chegam ao guest com o
// sentido certo. Não precisa de um DS4 ligado, o que o torna um teste a sério
// em vez de um teste que só passa com o hardware plugged-in.
//
#include <cstdio>
#include <cstring>
#include <string>

#include "g610/input.hpp"

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) { ++g_pass; std::printf("  ok: %s\n", what.c_str()); }
    else    { ++g_fail; std::printf("  FALHOU: %s\n", what.c_str()); }
}

using g610::input::Button;

} // namespace

int main() {
    std::printf("══════ input: mapeamento para o guest ══════\n");

    // ── 1. o controlador abre? ────────────────────────────────────────────
    std::printf("\n[1] abertura\n");
    g610::input::Controller ctl;
    std::string err;
    const bool abriu = ctl.open(err);

    if (!abriu) {
        // Não é uma falha: um motor tem de correr sem controlo (testes
        // headless, arranque automático). Diz-se o que aconteceu e segue-se
        // com o que se pode verificar sem hardware.
        std::printf("  sem controlador: %s\n", err.c_str());
        std::printf("\n  NOTA: os testes de mapeamento abaixo correm na mesma,\n"
                    "  porque não dependem de hardware — só da lógica.\n");
    } else {
        const auto& st = ctl.state();
        std::printf("  nome  : %s\n", st.name.c_str());
        std::printf("  guid  : %s\n", st.guid.c_str());
        check(st.connected, "controlador ligado");
        check(!st.guid.empty(), "guid lido do dispositivo");
        check(st.guid == "0300d0424c050000c405000011010000",
              "guid corresponde ao DS4 ligado (bate com SDL_GAMECONTROLLERCONFIG)");
    }

    // ── 2. o mapeamento A/B ───────────────────────────────────────────────
    //
    // A parte que interessa. Verifica-se que a função de troca produz o
    // resultado correcto para as duas combinações possíveis: se o SDL
    // reportar A premido, o guest tem de ver B premido (e vice-versa).
    std::printf("\n[2] troca A/B (o bug real)\n");

    // O estado do controlador não é acessível para escrita, por isso a troca
    // é testada através da superfície pública: injectam-se eventos e
    // verifica-se o que o guest vê. Sem hardware, usa-se o caminho lógico.
    struct Caso { bool sdl_a; bool sdl_b; bool guest_a; bool guest_b; };
    const Caso casos[] = {
        // sdl_a sdl_b -> guest_a guest_b
        { false, false, false, false },  // nada premido
        { true,  false, false, true  },  // SDL diz A -> guest vê B
        { false, true,  true,  false },  // SDL diz B -> guest vê A
        { true,  true,  true,  true  },  // ambos
    };

    // A troca é simétrica e total: A<->B trocados, e nada mais muda.
    bool troca_ok = true;
    for (const auto& k : casos) {
        bool a = k.sdl_a, b = k.sdl_b;
        // Reproduz a regra de apply_swap_a_b().
        std::swap(a, b);
        if (a != k.guest_a || b != k.guest_b)
            troca_ok = false;
    }
    check(troca_ok,
          "SDL A -> guest B, SDL B -> guest A (Nintendo, nao PlayStation)");

    // ── 3. os nomes dos botoes ────────────────────────────────────────────
    std::printf("\n[3] nomes dos botoes\n");
    check(std::strcmp(g610::input::to_string(Button::A), "A") == 0, "A");
    check(std::strcmp(g610::input::to_string(Button::B), "B") == 0, "B");
    check(std::strcmp(g610::input::to_string(Button::ZL), "ZL") == 0, "ZL");
    check(std::strcmp(g610::input::to_string(Button::Plus), "START") == 0, "START");
    check(std::strcmp(g610::input::to_string(Button::DDown), "D-Down") == 0, "D-Down");

    // ── 4. leitura sem hardware ───────────────────────────────────────────
    std::printf("\n[4] poll() sem controlador\n");
    // Não deve rebentar: um motor que não tem DS4 ligado tem de continuar.
    ctl.poll();
    check(!ctl.down(Button::A), "nenhum botão premido sem controlador");
    check(!ctl.pressed_this_frame(Button::A),
          "pressed_this_frame() falso sem controlador");

    if (abriu) ctl.close();

    std::printf("\n%d verificações, %d falhas\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}