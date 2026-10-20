// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — a tabela de objectos do kernel.
//
// Copiada do desenho do Eden (`k_handle_table.h`, `k_handle_table.cpp`,
// `svc_port.cpp`), não do código. O Eden é GPL e o nosso projecto não é; o
// que se copia é a ARQUITETURA, e cada linha aqui diz de onde veio.
//
// ## O que um handle é
//
// `k_handle_table.h:262-278`:
//
//     union HandlePack {
//         u32 raw{};
//         BitField<0, 15, u32> index;
//         BitField<15, 15, u32> linear_id;
//         BitField<30, 2, u32> reserved;
//     };
//
//     static constexpr Handle EncodeHandle(u16 index, u16 linear_id) {
//         HandlePack handle{};
//         handle.index.Assign(index);
//         handle.linear_id.Assign(linear_id);
//         handle.reserved.Assign(0);
//         return handle.raw;
//     }
//
// E a validação (`IsValidHandle`, linha 201-217):
//
//     if (raw_value == 0)    return false;
//     if (linear_id == 0)    return false;
//     if (index >= m_table_size) return false;
//
// **O `linear_id` é uma geração.** É o que faz um handle fechado ser
// detectado como fechado quando o índice já foi reatribuído — sem ele, um
// handle antigo continuaria a parecer válido e a apontar para o objecto
// errado. É a razão de ser do campo inteiro.
//
// Os handles que a versão anterior fabricava (`0x1000`, `0x1001`, ...)
// tinham `linear_id = 0`, que é INVÁLIDO pela regra do Eden. Nenhum deles
// passaria um `IsValidHandle`. Funcionavam porque o nosso dispatcher nunca
// os validava — e uma tabela que valida o que valida sozinha não é uma
// tabela, é um `switch` grande.
//
// ## Reserva e registo são dois passos
//
// `svc_port.cpp:37-50`:
//
//     R_TRY(handle_table.Reserve(system.Kernel(), out));
//     ...
//     handle_table.Register(system.Kernel(), *out, session);
//
// São dois passos e não um porque o Eden reserva o índice ANTES de criar o
// objecto — se a criação falhar, fica um índice reservado e é devolvido
// logo a seguir por `Unreserve`. É o que impede que dois objectos fiquem
// com o mesmo índice se a criação do segundo falhar a meio.
//
// ## O que não está aqui
//
// Contagem de referências, fechos, e a árvore de objectos do kernel
// (KProcess, KThread, KPort, KServerPort, ...). Aqui só há o que o IPC
// precisa: um handle, o índice que aponta, e a geração que o valida. Um
// emulador que implementa referências e arvore completa tem 2000 linhas
// antes de responder à primeira mensagem; este responde.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace g610 {

// ── o handle ──────────────────────────────────────────────────────────────
//
// Um u32 com três campos, e não um inteiro opaco. A distinção não é
// cosmética: `linear_id` é a geração, e é o que distingue "este handle"
// de "o handle que este índice teve antes".

struct Handle {
    std::uint32_t raw = 0;

    static constexpr Handle encode(std::uint16_t index, std::uint16_t linear_id) noexcept {
        Handle h;
        h.raw = (static_cast<std::uint32_t>(linear_id & 0x7FFF) << 15) |
                (static_cast<std::uint32_t>(index) & 0x7FFF);
        return h;
    }

    constexpr std::uint16_t index() const noexcept {
        return static_cast<std::uint16_t>(raw & 0x7FFF);
    }
    constexpr std::uint16_t linear_id() const noexcept {
        return static_cast<std::uint16_t>((raw >> 15) & 0x7FFF);
    }
    constexpr std::uint32_t reserved() const noexcept {
        return (raw >> 30) & 0x3;
    }

    // `IsValidHandle`, do Eden: raw != 0, linear_id != 0, reserved == 0.
    // O índice fora da tabela é verificado em `HandleTable::get()`, que é
    // onde a tabela existe — aqui só o que se pode ver sem a tabela.
    constexpr bool shape_valido() const noexcept {
        return raw != 0 && linear_id() != 0 && reserved() == 0;
    }

    bool operator==(const Handle& o) const noexcept { return raw == o.raw; }
    bool operator==(std::uint32_t o) const noexcept { return raw == o; }
    bool operator<(const Handle& o) const noexcept { return raw < o.raw; }
};

// Handle inválido. Existe para que "sem sessão" não seja `0`, que é um
// handle que o Eden rejeita.
inline constexpr Handle kInvalidHandle{};

// ── os objectos ───────────────────────────────────────────────────────────
//
// Dois tipos, e só. O Eden tem uma árvore de objectos com subclasses para
// cada serviço; aqui o que interessa ao IPC é: que porta é, e que serviço
// atende.
//
// A distinção `Port` vs `Session` não é preciosismo. `ConnectToNamedPort`
// cria uma sessão por dentro (`svc_port.cpp:44`) e devolve o handle DELA.
// Se os dois fossem a mesma coisa, o `SendSyncRequest` seguinte não teria
// Distinguished nada e um handle de porta acabaria a ser usado como sessão
// sem ninguém dar por isso — que é exactamente o que acontecia antes deste
// ficheiro, com um contador que fabricava os dois ao mesmo tempo.

struct KObject {
    // `Thread`: uma thread de guest que o motor criou mas **nao** corre
    // (ver `svc_create_thread`). O objecto existe para o handle ser valido.
    enum class Kind : std::uint8_t { Port, Session, Thread };

    Kind kind = Kind::Port;

    // A porta a que pertence. Vazio para uma porta.
    std::string name;

    // Para uma sessão: o handle da porta mãe. É assim que um `SendSyncRequest`
    // sabe a que serviço tem de atender.
    Handle parent;

    // Contador de referências, no sentido mínimo: quantos handles apontam
    // para aqui. Não é a contagem do Eden (`KAutoObject::Open/Close` com
    // spinlock e dispatch), é o suficiente para não haver um dangling, e
    // está escrito como tal para não se confundir com aquilo.
    unsigned refs = 0;
};

// ── a tabela ───────────────────────────────────────────────────────────────
//
// Índice directo, lista de livres, e uma geração por índice.
//
// A geração é o que faz a lista de livres ser correcta: quando o índice 7
// é reatribuído, o `linear_id` guardado passa a ser o novo, e um handle
// antigo com o índice 7 e a geração velha falha a validação.

class HandleTable {
public:
    explicit HandleTable(std::size_t capacidade = 0x4000)
        : m_capacidade(capacidade), m_objs(capacidade), m_linear(capacidade, 0) {
        // A lista de livres começa cheia, do fim para o início, para que os
        // primeiros handles saiam com índices baixos e o dump seja legível.
        m_livres.reserve(capacidade);
        for (std::size_t i = capacidade; i > 0; --i)
            m_livres.push_back(static_cast<std::uint16_t>(i - 1));
    }

    std::size_t capacidade() const noexcept { return m_capacidade; }

    // Reserva um índice e devolve um handle ainda sem objecto.
    //
    // É o passo `Reserve` do Eden: dá um handle que não aponta para nada,
    // e que só passa a valer depois de `registar`. A separação existe
    // porque o objecto se cria DEPOIS — se a criação falhasse e o índice já
    // estivesse registado, ficava um handle válido a apontar para meio
    // objecto.
    Handle reservar() {
        if (m_livres.empty())
            return kInvalidHandle;
        const std::uint16_t idx = m_livres.back();
        m_livres.pop_back();
        ++m_next_linear;
        if (m_next_linear > kLinearMax)
            m_next_linear = kLinearMin;
        return Handle::encode(idx, m_next_linear);
    }

    // O passo `Register`: o handle passa a apontar para o objecto.
    bool registar(Handle h, KObject* obj) {
        if (!h.shape_valido() || obj == nullptr)
            return false;
        const std::size_t idx = h.index();
        if (idx >= m_capacidade || m_objs[idx] != nullptr)
            return false;
        m_objs[idx] = obj;
        m_linear[idx] = h.linear_id();
        ++obj->refs;
        return true;
    }

    // `GetObject`: valida a forma, o índice e a geração.
    KObject* obter(Handle h) const {
        if (!h.shape_valido())
            return nullptr;
        const std::size_t idx = h.index();
        if (idx >= m_capacidade)
            return nullptr;
        if (m_linear[idx] != h.linear_id())
            return nullptr;      // geração velha: o handle já foi fechado
        return m_objs[idx];
    }

    // ── contabilidade ────────────────────────────────────────────────────
    std::size_t vivos() const noexcept { return m_vivos; }
    std::uint32_t reservas_rejeitadas() const noexcept { return m_cheias; }

    // Quantos handles falharam a validação ao serem usados. Deve ser 0 em
    // funcionamento normal, e é o número que diz se há um handle flutterado
    // algures.
    std::uint32_t invalidos_usados() const noexcept { return m_invalidos; }
    void contar_invalido() noexcept { ++m_invalidos; }

private:
    static constexpr std::uint16_t kLinearMin = 1;
    static constexpr std::uint16_t kLinearMax = 0x7FFF;

    std::size_t m_capacidade;
    std::vector<KObject*> m_objs;
    std::vector<std::uint16_t> m_livres;
    std::vector<std::uint16_t> m_linear;   // a geração com que cada índice foi dado
    std::uint16_t m_next_linear = 0;
    std::size_t m_vivos = 0;
    std::uint32_t m_cheias = 0;
    std::uint32_t m_invalidos = 0;
};

} // namespace g610