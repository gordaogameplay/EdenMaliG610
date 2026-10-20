// Teste do controlador: prova que a pilha SDL2 + evdev + mapeamento funciona.
//
// Não é um teste do motor — é um teste do INPUT. A pergunta é se um programa
// SDL consegue abrir o DS4,recebe eventos e dizer que botão é que foi premido.
// Se isto funcionar, o input do motor tem uma base; se não, o problema está
// antes do motor e é preciso resolverse aqui.
//
// imprime o que vê e sai ao fim dealguns segundos. Se o utilizador premir um
// botão, o nome sai pelo mesmo caminho que um teste automático leria.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>

namespace {

// Os botões do Switch, na ordem em que o menu os lê. O teste mapeia cada um
// para o que o DS4 chama de outra coisa, e imprime os dois nomes — que é a
// forma de confirmar que o mapeamento não está trocado (A com B, etc).
struct Botao {
    const char* nome_switch;
    SDL_GameControllerButton btn;
};

constexpr Botao kBotoes[] = {
    {"A",     SDL_CONTROLLER_BUTTON_A},
    {"B",     SDL_CONTROLLER_BUTTON_B},
    {"X",     SDL_CONTROLLER_BUTTON_X},
    {"Y",     SDL_CONTROLLER_BUTTON_Y},
    {"L",     SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"R",     SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"ZL",    SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"ZR",    SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"L3",    SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"R3",    SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"START", SDL_CONTROLLER_BUTTON_START},
    {"SELECT",SDL_CONTROLLER_BUTTON_BACK},
    {"HOME",  SDL_CONTROLLER_BUTTON_GUIDE},
};

const char* botao_raw(SDL_GameControllerButton b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "BTN_SOUTH";
    case SDL_CONTROLLER_BUTTON_B: return "BTN_EAST";
    case SDL_CONTROLLER_BUTTON_X: return "BTN_WEST";
    case SDL_CONTROLLER_BUTTON_Y: return "BTN_NORTH";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "BTN_L1/BTN_TL";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "BTN_R1/BTN_TR";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:  return "BTN_L3/BTN_THUMBL";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "BTN_R3/BTN_THUMBR";
    case SDL_CONTROLLER_BUTTON_START: return "BTN_START";
    case SDL_CONTROLLER_BUTTON_BACK:  return "BTN_SELECT";
    case SDL_CONTROLLER_BUTTON_GUIDE:  return "BTN_MODE";
    default: return "?";
    }
}

} // namespace

int main(int argc, char** argv) {
    // Segundos a correr. 0 = infinito (até Ctrl-C).
    const int segundos = (argc > 1) ? std::atoi(argv[1]) : 10;

    // Não abrir janela: este teste é só input, e uma janela roubar o foco ao
    // menu da sessão seria exactamente o problema que estamos a evitar.
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::printf("SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    std::printf("joysticks anexados : %d\n", SDL_NumJoysticks());
    std::printf("mapeamentos SDL   : %d\n", SDL_GameControllerNumMappings());

    // Em SDL2 o numero de game controllers e' o numero de joysticks que têm
    // mapeamento — não existe um "NumGameControllers" (ao contrário do SDL3).
    int indice = -1;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) {
            indice = i;
            break;
        }

    if (indice < 0) {
        std::printf("\n  NENHUM game controller reconhecido.\n");
        std::printf("  SDL conhece %d joysticks, mas nenhum tem mapeamento.\n",
                    SDL_NumJoysticks());
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            std::printf("    [%d] %s\n", i, SDL_JoystickNameForIndex(i));
        }
        std::printf("\n  ->SDL_GAMECONTROLLERCONFIG precisa de incluir o guid deste.\n");
        SDL_Quit();
        return 2;
    }

    SDL_GameController* gc = SDL_GameControllerOpen(indice);
    if (gc == nullptr) {
        std::printf("SDL_GameControllerOpen(%d) falhou: %s\n", indice,
                    SDL_GetError());
        SDL_Quit();
        return 2;
    }

    SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
    const char* nome = SDL_GameControllerName(gc);
    char guid[33] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof guid);

    std::printf("\n  aberto    : [%d] %s\n", indice, nome ? nome : "?");
    std::printf("  guid      : %s\n", guid);
    std::printf("  joystik id: %d\n", SDL_JoystickInstanceID(js));
    std::printf("  eixos     : %d\n", SDL_JoystickNumAxes(js));
    std::printf("  balls     : %d\n", SDL_JoystickNumBalls(js));

    std::printf("\n  mapeamento SDL -> evdev:\n");
    for (const auto& b : kBotoes)
        std::printf("    %-7s <- %s\n", b.nome_switch, botao_raw(b.btn));

    std::printf("\n  a premir botoes durante %ds...\n", segundos);
    std::fflush(stdout);

    const Uint32 fim = SDL_GetTicks() + static_cast<Uint32>(segundos) * 1000;
    int eventos = 0;
    bool SDL_CALL = false;

    while (SDL_GetTicks() < fim || segundos == 0) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                const auto* b = SDL_GameControllerGetStringForButton(
                    static_cast<SDL_GameControllerButton>(ev.cbutton.button));
                std::printf("  [botao] %-24s (raw %d)\n",
                            b ? b : "?", ev.cbutton.button);
                ++eventos;
                SDL_CALL = true;
            } else if (ev.type == SDL_CONTROLLERBUTTONUP) {
                const auto* b = SDL_GameControllerGetStringForButton(
                    static_cast<SDL_GameControllerButton>(ev.cbutton.button));
                std::printf("  [larga ] %s\n", b ? b : "?");
                ++eventos;
            } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
                // Só mostra se mexeu o suficiente para importar.
                if (std::abs(ev.caxis.value) > 12000) {
                    std::printf("  [eixo  ] %-10s = %d\n",
                                SDL_GameControllerGetStringForAxis(
                                    static_cast<SDL_GameControllerAxis>(
                                        ev.caxis.axis)),
                                ev.caxis.value);
                    ++eventos;
                    SDL_CALL = true;
                }
            } else if (ev.type == SDL_QUIT) {
                SDL_CALL = true;
            }
        }
        SDL_Delay(10);
    }

    std::printf("\n  eventos vistos: %d\n", eventos);
    if (eventos == 0)
        std::printf("  AVISO: nada. Ou o DS4 esta' sem energia, ou o\n"
                    "         SDL nao recebeu eventos do evdev.\n");

    SDL_GameControllerClose(gc);
    SDL_Quit();
    return SDL_CALL ? 0 : 3;
}