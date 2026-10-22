// SPDX-FileCopyrightText: 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o device `/dev/nvhost-ctrl` e os syncpoints.
//
// ## O que e'
//
// O upstream (`core/hle/service/nvdrv/devices/nvhost_ctrl.cpp`) tem um
// device **separado** do `/dev/nvhost-ctrl-gpu`: e' o `nvhost_ctrl`, e e'
// ele que tem o grupo 0x00 —
//
//     case 0x1b: NvOsGetConfigU32
//     case 0x1c: IocCtrlClearEventWait
//     case 0x1d: IocCtrlEventWait (alocacao)
//     case 0x1e: IocCtrlEventWait (espera)
//     case 0x1f: IocCtrlEventRegister
//     case 0x20: IocCtrlEventUnregister
//     case 0x21: IocCtrlEventUnregisterBatch
//
// **Este e' o mecanismo pelo qual o guest espera pela fence da GPU.**
// O `SubmitGPFIFO` devolve o par (syncpoint, valor) e o guest vem aqui
// esperar que aquele syncpoint chegue a esse valor.
//
// A casa nao tinha este device: `TipoDeDevice::Controle` existia na enum
// mas nao tinha caso no despacho (`src/services/nvdrv.cpp`), e por isso o
// ioctl caia em "nao implementado".
//
// ## O syncpoint manager
//
// O upstream (`video_core/host1x/syncpoint_manager.h`) tem um array de 192
// syncpoints, cada um com um valor monotonico e uma lista de accoes
// registadas. A acao e' disparada quando o valor do syncpoint chega ao
// esperado.
//
// **A casa nao tem thread de GPU** — o trabalho e' feito de seguida, no
// `SubmitGPFIFO` — por isso o syncpoint avanca **na submissao** e a
// accao dispara **imediatamente** se o valor ja foi alcancado.
//
// Isto nao e' uma diferenca de arquitectura que se note: no Eden o
// syncpoint avanca quando o trabalho **e' executado**, e o guest so
// repara depois. Aqui o trabalho e' "executado" no `SubmitGPFIFO`, e
// o syncpoint avanca ai. O guest ve o mesmo: o valor ja estar ta quando
// ele vem perguntar.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace g610::gpu {

// O numero maximo de syncpoints — o do upstream
// (`nvdata.h`: `MaxSyncPoints = 192`).
inline constexpr std::uint32_t kMaxSyncpoints = 192;

// O numero maximo de eventos NV — o do upstream (`MaxNvEvents = 64`).
inline constexpr std::uint32_t kMaxNvEvents = 64;

// O geridor de syncpoints.
//
// A semantica e' a do upstream (`SyncpointManager`):
//
//   - cada syncpoint tem um **valor** monotonico;
// - `Increment(id)` sobe o valor e dispara as accoes cujo valor
//     esperado foi alcancado;
// - `RegisterAction(id, valor_esperado, accao)` regista uma accao que
//     dispara quando o syncpoint chegar ao valor.
//
// **A diferenca para o upstream**: la' as accoes correm numa thread de
// GPU; aqui correm no ponto em que o syncpoint avanca — que e' o
// `SubmitGPFIFO`. O resultado para o guest e' o mesmo.
class SyncpointManager {
public:
    // O valor actual do syncpoint.
    [[nodiscard]] std::uint32_t Valor(std::uint32_t id) const {
        std::scoped_lock lock(garde);
        return valores_[id];
    }

    // O syncpoint esta' no valor esperado (ou ja passou).
    [[nodiscard]] bool EstaSinalizado(std::uint32_t id, std::uint32_t valor) const {
        std::scoped_lock lock(garde);
        return valores_[id] >= valor;
    }

    // Um syncpoint novo — o proximo id livre.
    //
    // O upstream aloca o syncpoint do canal na criacao do canal
    // (`nvhost_gpu.cpp`, `channel_syncpoint`). Aqui e' a mesma coisa:
    // o primeiro `AllocGPFIFOEx` de um canal.
    std::uint32_t Alocar() {
        std::scoped_lock lock(garde);
        for (std::uint32_t i = 0; i < kMaxSyncpoints; ++i) {
            if (!alocados_[i]) {
                alocados_[i] = true;
                valores_[i] = 0;
                return i;
            }
        }
        return kMaxSyncpoints;  // sem sitio
    }

    // Sobe o syncpoint — e dispara as accoes.
    void Incrementar(std::uint32_t id, std::uint32_t incremento) {
        std::vector<std::function<void()>> a_disparar;
        {
            std::scoped_lock lock(garde);
            valores_[id] += incremento;
            const std::uint32_t valor = valores_[id];

            // As accoes cujo valor esperado foi alcancado — as que
            // ficaram para tras sao removidas.
            auto& lista = acoes_[id];
            for (auto it = lista.begin(); it != lista.end();) {
                if (valor >= it->second) {
                    a_disparar.push_back(it->first);
                    it = lista.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // **Fora do trinco** — a accao sinaliza um `KEvent`, e isso
        // pode acabar a mexer no estado do kernel.
        for (auto& accao : a_disparar) {
            accao();
        }
    }

    // Regista uma accao para quando o syncpoint chegar ao valor.
    //
    // Devolve `true` se a accao foi registada — e `false` se o valor
    // **ja** foi alcancado (nesse caso a accao dispara logo e o
    // chamador nao a regista).
    bool Registar(std::uint32_t id, std::uint32_t valor_esperado,
                  std::function<void()> accao) {
        {
            std::scoped_lock lock(garde);
            if (valores_[id] >= valor_esperado) {
                return false;  // ja esta' cumprido
            }
            acoes_[id].emplace_back(std::move(accao), valor_esperado);
        }
        return true;
    }

    // Cancela uma accao — o `DeregisterGuestAction` do upstream.
    void Remover(std::uint32_t id, std::uint32_t valor_esperado) {
        std::scoped_lock lock(garde);
        auto& lista = acoes_[id];
        for (auto it = lista.begin(); it != lista.end();) {
            if (it->second == valor_esperado) {
                it = lista.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    mutable std::mutex garde;
    std::uint32_t valores_[kMaxSyncpoints]{};
    bool alocados_[kMaxSyncpoints]{};
    // As accoes registadas: (accao, valor esperado).
    std::map<std::uint32_t, std::vector<std::pair<std::function<void()>, std::uint32_t>>> acoes_;
};

}  // namespace g610::gpu
