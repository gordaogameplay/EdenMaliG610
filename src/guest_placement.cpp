// EdenMaliG610 — onde o guest corre.
//
// A short version: **nos núcleos grandes, e não onde calhou cair.**
//
// ## O que o Switch faz
//
// Medido e documentado do exterior (frequências de engenharia do die, e o
// que a Nintendo deixa correr no produto):
//
//     Switch — Tegra X1
//         núcleo 0    A57 @ 1020 MHz   reservado ao sistema operativo
//         núcleos 1-3 A57 @ 1020 MHz   disponíveis para o jogo
//                                   (1785 MHz temporario, nas loadings)
//         núcleos 4-7 A53 @    0 MHz   DESACTIVADOS pela Nintendo
//
// Isto é o que importa: **o jogo corre em TRÊS núcleos A57 a 1.020 MHz**, e
// metade do die está morta.
//
// ## O que o Pi faz
//
// Medido em `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq`:
//
//     RK3588
//         núcleos 0-3  Cortex-A55 @ 1800 MHz
//         núcleos 4-7  Cortex-A76 @ 2400 MHz
//
// ## A conta
//
// Um Cortex-A76 é um núcleo fora-de-ordem muito mais largo que um A57, e
// corre a 2.4 GHz contra 1.02. A margem por núcleo é de várias vezes, e
// temos QUATRO A76 contra TRÊS A57.
//
// Ou seja: a CPU não é o gargalo. É o contrário de um gargalo.
//
// ## Porque isto ainda precisa de código
//
// **Medido: o guest corre na thread principal.** Não há `pthread_create` —
// o motor é um loop de traps síncrono em `NativeEngine::run()`, e portanto
// o jogo herda o núcleo onde o processo calhou. Se isso der um A55 a 1800
// MHz, estamos a jogar o jogo numa máquina mais lenta que o Switch.
//
// Duas formas de resolver, e as duas são feitas:
//
//   1. `IA_BIG_CORES=4-7` — a máscara de CPUs, ao vivo. Para medir o efeito
//      sem tocar no código.
//
//   2. O default — todas as acima de `IA_BIG_CORES`, e se não houver
//      big.LITTLE, fica como está.
//
// O mesmo número serve para o inverso: as threads de serviço — que é o que
// vai atender o IPC quando o guest estiver à espera — são melhor nos A55,
// e estão lá por omissão se o sistema as puser.
//
// A referência ao Switch está no comentário porque é dela que vem a
// comparação, e porque um dia alguém vai perguntar porque é que os
// agregados do jogo vão nos A76 e a resposta não é "porque são melhores":
// é porque o jogo corria em 1 GHz no hardware de origem e qualquer
// agendamento sensato é mais rápido que isso.
#include <sched.h>
#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <cstdlib>
#include <string>
#include <vector>

#include "g610/guest_placement.hpp"

namespace g610 {

namespace {

// Os núcleos grandes, lidos do sistema.
//
// Não há uma constante: o RK3588 é A55×4 + A76×4, mas um Pi 4 é
// A72×4, e um outro painel pode ser diferente. Descobrir a frequência
// máxima de cada núcleo e ficar com os que a têm maior é uma leitura do
// que a máquina é, não uma suposição sobre qual é a máquina.
std::vector<int> nucleos_grandes() {
    const char* forcado = std::getenv("IA_BIG_CORES");
    if (forcado != nullptr && *forcado != '\0') {
        std::vector<int> v;
        const std::string s(forcado);
        std::size_t i = 0;
        while (i < s.size()) {
            std::size_t j = s.find(',', i);
            if (j == std::string::npos)
                j = s.size();
            const std::size_t d = s.find('-', i);
            if (d != std::string::npos && d < j) {
                const int a = std::atoi(s.substr(i, d - i).c_str());
                const int b = std::atoi(s.substr(d + 1, j - d - 1).c_str());
                for (int c = a; c <= b; ++c)
                    v.push_back(c);
            } else if (j > i) {
                v.push_back(std::atoi(s.substr(i, j - i).c_str()));
            }
            i = j + 1;
        }
        return v;
    }

    // Sem lista: pergunta ao sistema. Os ficheiros de frequência dizem o
    // máximo que cada núcleo aguenta, e no big.LITTLE isso é uma leitura
    // limpa de "quais são os grandes".
    std::vector<std::pair<int, long>> f;
    for (int c = 0; c < 16; ++c) {
        char caminho[128];
        std::snprintf(caminho, sizeof caminho,
                      "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        std::FILE* fp = std::fopen(caminho, "r");
        if (fp == nullptr)
            continue;
        long khz = 0;
        if (std::fscanf(fp, "%ld", &khz) != 1)
            khz = 0;
        fclose(fp);
        f.emplace_back(c, khz);
    }
    if (f.size() < 2)
        return {};

    long maximo = 0;
    for (const auto& x : f)
        if (x.second > maximo)
            maximo = x.second;

    std::vector<int> grandes;
    for (const auto& x : f)
        if (x.second == maximo && maximo > 0)
            grandes.push_back(x.first);
    return grandes;
}

} // namespace

// Põe o guest nos núcleos grandes. Devolve quantos fixou.
std::size_t fixar_guest_nos_nucleos_grandes() {
    const std::vector<int> cpus = nucleos_grandes();
    if (cpus.empty())
        return 0;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (int c : cpus)
        CPU_SET(c, &mask);

    // pthread_setaffinity_np e nao sched_setaffinity: a thread principal e' a
    // que corre o guest, e `sched_setaffinity(0, ...)` aplica-se ao que chamar
    // aquilo. Sao a mesma mascara no nosso caso — uma so thread — mas a
    // segunda forma nao deixa duvida sobre o que esta a ser limitado.
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof mask, &mask);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[cpu] nao consegui fixar a thread nos nucleos grandes: %s\n",
                     std::strerror(rc));
        return 0;
    }

    std::fprintf(stderr, "[cpu] guest fixado nos nucleos grandes:");
    for (int c : cpus)
        std::fprintf(stderr, " %d", c);
    std::fprintf(stderr, "  (%zu)\n", cpus.size());
    return cpus.size();
}

} // namespace g610