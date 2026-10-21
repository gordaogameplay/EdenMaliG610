// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
// O registo de handles do `nvmap` — partilhado entre o `nvdrv` e o compositor.
//
// ## Porque é partilhado
//
// O jogo entrega os **seus buffers** ao compositor pelo *binder* — e
// entrega-os pelo **handle do `nvmap`** (o `NvGraphicBuffer` do Eden
// traz `handle`, que é um handle do `nvmap`).
//
// Ou seja: o `nvdrv` é quem sabe que handle é que endereço; e o
// compositor é quem precisa desse endereço para apresentar o desenho.
//
// São dois serviços diferentes, e os serviços não falam uns com os
// outros — por isso o registo é **partilhado**, como o `Settings::values`
// e a telemetria.
//
// ## O que se regista
//
// Só o essencial: o handle e a região (endereço e tamanho) que ele
// representa. O `nvmap` da casa aloca a memória no espaço de VA da GPU
// (a mesma reserva que o `nvdrv` dá ao jogo) — por isso o endereço é
// memória do guest, e o compositor lê-a directamente.
//
// SPDX-FileCopyrightText: 2026 gordaogameplay
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace g610::services {

// O registo: handle do `nvmap` -> região.
class RegistoNvmap {
public:
    struct Regiao {
        std::uint64_t endereco = 0;
        std::uint64_t tamanho = 0;
    };

    // A instância única — a mesma razão do `Settings::values`: a ordem
    // de inicialização das translation units não é garantida.
    static RegistoNvmap& Instancia() {
        static RegistoNvmap r;
        return r;
    }

    void Registar(std::uint32_t handle, std::uint64_t endereco, std::uint64_t tamanho) {
        std::lock_guard<std::mutex> lk(mu_);
        regiao_[handle] = Regiao{endereco, tamanho};
    }

    void Remover(std::uint32_t handle) {
        std::lock_guard<std::mutex> lk(mu_);
        regiao_.erase(handle);
    }

    bool Procurar(std::uint32_t handle, Regiao& saida) const {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = regiao_.find(handle);
        if (it == regiao_.end())
            return false;
        saida = it->second;
        return true;
    }

private:
    mutable std::mutex mu_;
    std::map<std::uint32_t, Regiao> regiao_;
};

} // namespace g610::services
