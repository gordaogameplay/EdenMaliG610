// EdenMaliG610 — input do guest.
//
// O guest é um jogo de Nintendo. Os botões que ele vê não são os do teclado, nem
// os do(evdev), nem os que o SDL chama — são os do Switch: A, B, X, Y, L, R,
// ZL, ZR, L3, R3, START, SELECT, HOME, e o D-pad.
//
// ---------------------------------------------------------------------------
// A ARMADILHA DO A/B
// ---------------------------------------------------------------------------
// Um DS4 é um PlayStation. Os botões estão assim:
//
//              BTN_SOUTH (Cross, ✕)     BTN_EAST (Circle, ○)
//                     ▼                        ▼
//                  em baixo                a direita
//
// A PlayStation chama-lhe A e B. A Nintendo também — mas com o sentido
// TROCADO, porque em Nintendo:
//
//              botão esquerdo (✕) = B        botão direito (○) = A
//
// O SDL segue a convenção da PlayStation: o seu `A` é o `BTN_SOUTH`, que é o
// botão de BAIXO, que na Nintendo é o **B**. Medido no Orange Pi 5 Pro com
// `g610_input`:
//
//     A       <- BTN_SOUTH
//     B       <- BTN_EAST
//
// Ou seja: usar o A e o B do SDL directamente dá botões trocados. Numa tela de
// título, "carregar A" saltaria a cena errada. Este módulo é que desfaz a
// troca, num sítio só, para que o resto do motor nunca tenha de saber que
// existe uma PlayStation por baixo.
//
// ---------------------------------------------------------------------------
// PORQUÊ SDL E NÃO evdev DIRECTO
// ---------------------------------------------------------------------------
// evdev daria os botões crus (BTN_SOUTH, ABS_HAT0X...), que é o que o kernel
// entrega. O SDL acrescenta o mapeamento para uma API de controlador e trata
// de um bordo de casos: deadzones, axes de 0..65535 em vez de -32768..32767,
// e dispositivos que se apresenta de maneiras diferentes entre versões.
//
// O trade-off é que o SDL esconde os botões atrás da convenção PlayStation. É
// exactamente o que a secção de cima desfaz. Vale a pena porque a
// configuração de mapeamento por guid já existe no sistema
// (`SDL_GAMECONTROLLERCONFIG` em /etc/emuos/environment), e é o caminho que o
// resto do EmuOS já usa.
#pragma once

#include <cstdint>
#include <string>

namespace g610::input {

// Os botões do Switch, na nomenclatura que o guest espera.
enum class Button : std::uint8_t {
    A = 0, B, X, Y,
    L, R, ZL, ZR,
    L3, R3,
    Plus,     // START
    Minus,    // SELECT
    Home,
    DUp, DDown, DLeft, DRight,
    Count
};

const char* to_string(Button b) noexcept;

// Um eixo normalizado. O SDL dá 0..65535; o guest espera um valor com sinal,
// tipicamente -32768..32767 ou -1.0..1.0. Fica aqui o valor do SDL
// normalizado para ±1, com uma zona morta para não gerar ruído.
struct Axis {
    float x = 0.0f;  // analógico esquerdo
    float y = 0.0f;
    float rx = 0.0f; // analógico direito
    float ry = 0.0f;
    float lx = 0.0f; // triggers, 0..1
    float ly = 0.0f;
};

// O estado completo num instante.
struct State {
    bool     buttons[static_cast<int>(Button::Count)] = {};
    Axis     axis;
    bool     connected = false;
    int      instance_id = -1;
    std::string name;      // nome que o SDL devolve
    std::string guid;      // guid do dispositivo
};

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------
//
// Um controlador por thread. O motor é single-thread hoje, mas cada thread de
// guest vai ter o seu — e um SDL_GameController é ligado a uma thread que o
// eventa. Fazer isto partilhado entre threads daria eventos cruzados.
class Controller {
public:
    Controller() = default;
    ~Controller();

    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // Abre o SDL e o primeiro controlador encontrado.
    //
    // `erro` recebe uma descrição accionável quando falha: "não há
    // controlador", "o SDL não abriu", etc. Devolve false em vez de lancer —
    // um motor sem controlo tem de ser utilizável (para testes).
    bool open(std::string& erro);

    // Recolhe os eventos disponibles. Não bloqueia.
    //
    // Separado de `open()` porque o motor chama isto do loop, e um `open()`
    // que bloqueasse seria um deadlock no primeiro frame.
    void poll();

    const State& state() const noexcept { return st_; }

    // Verdadeiro se algum botão foi premido neste `poll()`. Serve para
    // detecção de "o utilizador carregou em A" sem guardar histórico.
    bool pressed_this_frame(Button b) const noexcept {
        return st_.buttons[static_cast<int>(b)] &&
               !prev_.buttons[static_cast<int>(b)];
    }
    bool released_this_frame(Button b) const noexcept {
        return !st_.buttons[static_cast<int>(b)] &&
               prev_.buttons[static_cast<int>(b)];
    }
    bool down(Button b) const noexcept {
        return st_.buttons[static_cast<int>(b)];
    }

    // Fecha o controlador e o SDL. Chamar duas vezes é seguro.
    void close();

private:
    void apply_swap_a_b();

    struct Impl;
    Impl*  impl_ = nullptr;
    State  st_{};
    State  prev_{};
};

} // namespace g610::input