// EdenMaliIA — input do guest.
#include "ia/input.hpp"

#include <SDL2/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace ia::input {

const char* to_string(Button b) noexcept {
    switch (b) {
    case Button::A:  return "A";
    case Button::B:  return "B";
    case Button::X:  return "X";
    case Button::Y:  return "Y";
    case Button::L:  return "L";
    case Button::R:  return "R";
    case Button::ZL: return "ZL";
    case Button::ZR: return "ZR";
    case Button::L3: return "L3";
    case Button::R3: return "R3";
    case Button::Plus:  return "START";
    case Button::Minus: return "SELECT";
    case Button::Home:  return "HOME";
    case Button::DUp:    return "D-Up";
    case Button::DDown:  return "D-Down";
    case Button::DLeft:  return "D-Left";
    case Button::DRight: return "D-Right";
    default: return "?";
    }
}

struct Controller::Impl {
    SDL_GameController* gc = nullptr;
    bool                sdl_open = false;
};

// Converte o valor bruto do SDL (0..65535, ou -32768..32767 nos eixos) para
// ±1.0, com zona morta.
//
// A zona morta não é um detalhe: um manípulo analógico parado produz ruído de
// ±200 que, se passar pelo guest, faz a personagem andar sozinha. O guest tem
// a sua própria zona morta, mas chegar já filtrado evita o pior.
static float normalise(int raw, int deadzone = 8000) noexcept {
    float v = (raw < 0) ? static_cast<float>(raw) / 32768.0f
                         : static_cast<float>(raw) / 32767.0f;
    if (std::fabs(v) < static_cast<float>(deadzone) / 32768.0f)
        return 0.0f;
    return v;
}

Controller::~Controller() { close(); }

bool Controller::open(std::string& erro) {
    if (impl_ != nullptr) {
        erro = "controlador ja' aberto";
        return true;
    }

    impl_ = new Impl();

    // Só input: sem vídeo e sem eventos. Uma janela aqui roubaria o foco ao
    // menu da sessão — que é exactamente o problema que se quer evitar.
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        erro = std::string("SDL_Init: ") + SDL_GetError();
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    impl_->sdl_open = true;

    // Em SDL2 o número de game controllers é o número de joysticks que têm
    // mapeamento. Não existe SDL_GameControllerNumControllers (ao contrário
    // do SDL3), e por isso itera-se pelos joysticks.
    int indice = -1;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) {
            indice = i;
            break;
        }

    if (indice < 0) {
        char buf[256];
        const int n = SDL_NumJoysticks();
        std::snprintf(buf, sizeof buf,
                      "nenhum game controller reconhecido (%d joystick(s) "
                      "anexado(s)); falta o mapeamento por guid em "
                      "SDL_GAMECONTROLLERCONFIG",
                      n);
        erro = buf;
        close();
        return false;
    }

    impl_->gc = SDL_GameControllerOpen(indice);
    if (impl_->gc == nullptr) {
        erro = std::string("SDL_GameControllerOpen: ") + SDL_GetError();
        close();
        return false;
    }

    SDL_Joystick* js = SDL_GameControllerGetJoystick(impl_->gc);
    st_.instance_id = SDL_JoystickInstanceID(js);

    const char* nome = SDL_GameControllerName(impl_->gc);
    st_.name = nome ? nome : "(sem nome)";

    char guid[33] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof guid);
    st_.guid = guid;

    st_.connected = true;
    prev_ = st_;
    erro.clear();
    return true;
}

void Controller::close() {
    if (impl_ == nullptr)
        return;
    if (impl_->gc != nullptr)
        SDL_GameControllerClose(impl_->gc);
    if (impl_->sdl_open)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    delete impl_;
    impl_ = nullptr;
    st_.connected = false;
    st_.instance_id = -1;
}

void Controller::apply_swap_a_b() {
    // O SDL diz que A é BTN_SOUTH (o botão de baixo, que na PlayStation é A).
    // Na Nintendo o botão de baixo é B. Trocar aqui, num sítio só, é o que
    // impede que o resto do motor tenha de saber disso.
    std::swap(st_.buttons[static_cast<int>(Button::A)],
              st_.buttons[static_cast<int>(Button::B)]);
}

void Controller::poll() {
    if (impl_ == nullptr || impl_->gc == nullptr)
        return;

    prev_ = st_;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            // Um controlador ligado a meio da sessão. Reabrir é mais simples
            // e mais robusto do que gerir hot-plug por evento.
            const int idx = ev.cdevice.which;
            if (idx >= 0 && SDL_IsGameController(idx)) {
                if (impl_->gc == nullptr)
                    impl_->gc = SDL_GameControllerOpen(idx);
                st_.connected = true;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            if (st_.instance_id == ev.cdevice.which) {
                st_.connected = false;
                st_ = State{};
                st_.name.clear();
                st_.guid.clear();
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool down = (ev.type == SDL_CONTROLLERBUTTONDOWN);
            switch (ev.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A:
                st_.buttons[static_cast<int>(Button::A)] = down; break;
            case SDL_CONTROLLER_BUTTON_B:
                st_.buttons[static_cast<int>(Button::B)] = down; break;
            case SDL_CONTROLLER_BUTTON_X:
                st_.buttons[static_cast<int>(Button::X)] = down; break;
            case SDL_CONTROLLER_BUTTON_Y:
                st_.buttons[static_cast<int>(Button::Y)] = down; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                st_.buttons[static_cast<int>(Button::L)] = down; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                st_.buttons[static_cast<int>(Button::R)] = down; break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                st_.buttons[static_cast<int>(Button::L3)] = down;
                st_.buttons[static_cast<int>(Button::ZL)] = down; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
                st_.buttons[static_cast<int>(Button::R3)] = down;
                st_.buttons[static_cast<int>(Button::ZR)] = down; break;
            case SDL_CONTROLLER_BUTTON_START:
                st_.buttons[static_cast<int>(Button::Plus)] = down; break;
            case SDL_CONTROLLER_BUTTON_BACK:
                st_.buttons[static_cast<int>(Button::Minus)] = down; break;
            case SDL_CONTROLLER_BUTTON_GUIDE:
                st_.buttons[static_cast<int>(Button::Home)] = down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                st_.buttons[static_cast<int>(Button::DUp)] = down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                st_.buttons[static_cast<int>(Button::DDown)] = down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                st_.buttons[static_cast<int>(Button::DLeft)] = down; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                st_.buttons[static_cast<int>(Button::DRight)] = down; break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
            switch (ev.caxis.axis) {
            case SDL_CONTROLLER_AXIS_LEFTX:
                st_.axis.x = normalise(ev.caxis.value); break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                // O eixo Y cresce para baixo no SDL e o guest espera para
                // cima. Sem esta inversão, "cima" no guest é "baixo".
                st_.axis.y = -normalise(ev.caxis.value); break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
                st_.axis.rx = normalise(ev.caxis.value); break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                st_.axis.ry = -normalise(ev.caxis.value); break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                // Os triggers só vão de 0 a 65535: não têm centro.
                st_.axis.lx = ev.caxis.value / 32767.0f; break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                st_.axis.ly = ev.caxis.value / 32767.0f; break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    apply_swap_a_b();
}

} // namespace ia::input