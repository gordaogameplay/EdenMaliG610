// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o interpretador das macros MME (o caminho LE do upstream).
//
// ## De onde vem isto
//
// O jogo desenha por **macros MME**: métodos >= 0xE00 (`0xE0A` = 3594,
// medido) que disparam microcode carregado na RAM das macros (os
// métodos `load_mme`, 69..72). A máquina do upstream tem dois caminhos
// (`macro.cpp`, `MacroEngine::Execute`):
//
//   HLE — o hash do código bate com um builder conhecido
//         (`macro_hle.cpp`: 15 hashes);
//   LE  — senão, executa o microcode num interpretador
//         (`macro_interpreter.cpp`).
//
// **Medido** (`tools/simular_mme.py` sobre os 3 caps reais do jogo): os
// 14 buckets que o jogo carrega coincidem com os builders HLE — mas a
// macro do **desenho** não: `entry` 5, bucket 49, 25 instruções, hash
// `9e3ca7054d12d63c`, fora da tabela. É por isso que o jogo desenha
// por aqui e o LE é obrigatório. Este ficheiro é o porte fiel daquela
// máquina.
//
// ## A máquina (`Macro::Opcode`, `macro.h` do upstream ✓)
//
// O microcode é um programa com uma instrução de 32 bits por palavra;
// o `pc` conta **bytes** (`GetOpcode`: `code[pc / sizeof(u32)]` ✓) e o
// alvo de um ramo é `imediato * 4` (`GetBranchTarget` ✓). Os registos
// de macro são 8 (`NUM_MACRO_REGISTERS` ✓) e o registo 0 é fio a zero
// (o `SetRegister` ignora escritas nele ✓). A 1ª chamada fica no
// registo 1 (`registers[1] = params[0]` ✓) e o `FetchParameter` começa
// no índice 1.
//
// O `Send` do microcode devolve métodos ao motor (o
// `maxwell3d.CallMethod(endereco, valor, true)` ✓) — é por aí que a
// macro manda desenhar: escreve o estado (contagens, bases) e dispara
// os métodos de desenho; o tradutor da casa emite o `DrawCommand` ✓.
//
// ## As únicas diferenças da casa (todas honestas)
//
// O upstream usa `ASSERT` que aborta o processo. A casa não aborta a
// submissão por um microcode estranho: conta o sintoma
// (`pc_fora`, `params_esgotados`, teto de passos) e para a execução
// daquela macro — o jogo continua. O teto de passos (1M) é a proteção
// contra loop infinito no motor vivo (o GPU real travaria à mesma).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace g610::gpu::mme {

// Os registos de macro (`Macro::NUM_MACRO_REGISTERS` do upstream ✓).
inline constexpr std::size_t kNumRegistos = 8;

// `Macro::Operation` (o `macro.h` do upstream ✓).
enum class Operacao : std::uint32_t {
    ALU = 0,
    SomaImediata = 1,           // AddImmediate
    ExtrairInserir = 2,         // ExtractInsert
    ExtrairDeslocarImediato = 3, // ExtractShiftLeftImmediate
    ExtrairDeslocarRegisto = 4,  // ExtractShiftLeftRegister
    Leitura = 5,                // Read
    NaoUsado = 6,               // Unused (codificação inválida)
    Ramo = 7,                   // Branch
};

// `Macro::ALUOperation` ✓ — as operações 4..7 não são codificações
// válidas (o comentário do upstream ✓).
enum class OperacaoALU : std::uint32_t {
    Soma = 0,                    // Add
    SomaComTransporte = 1,       // AddWithCarry
    Subtracao = 2,               // Subtract
    SubtracaoComEmprestimo = 3,  // SubtractWithBorrow
    Xor = 8,
    Ou = 9,                      // Or
    E = 10,                      // And
    ENao = 11,                   // AndNot
    NaoE = 12,                   // Nand
};

// `Macro::ResultOperation` ✓.
enum class OperacaoResultado : std::uint32_t {
    IgnorarEBuscar = 0,          // IgnoreAndFetch
    Mover = 1,                   // Move
    MoverEMetodo = 2,            // MoveAndSetMethod
    BuscarEEnviar = 3,           // FetchAndSend
    MoverEEnviar = 4,            // MoveAndSend
    BuscarEMetodo = 5,           // FetchAndSetMethod
    MetodoBuscarEEnviar = 6,     // MoveAndSetMethodFetchAndSend
    MetodoEEnviar = 7,           // MoveAndSetMethodSend
};

// `Macro::BranchCondition` ✓.
enum class CondicaoRamo : std::uint32_t { Zero = 0, NaoZero = 1 };

// `Macro::Opcode` — o `union` com os `BitField` do upstream ✓, aqui
// com deslocamentos (a casa não usa a lib `BitField`).
struct Opcode {
    std::uint32_t raw = 0;

    constexpr Operacao operacao() const {
        return static_cast<Operacao>(raw & 0x7u);
    }
    constexpr OperacaoResultado operacao_resultado() const {
        return static_cast<OperacaoResultado>((raw >> 4) & 0x7u);
    }
    constexpr CondicaoRamo condicao_ramo() const {
        return static_cast<CondicaoRamo>((raw >> 4) & 0x1u);
    }
    // Se armado num ramo, este não tem slot de atraso (`branch_annul` ✓).
    constexpr bool ramo_anulado() const { return ((raw >> 5) & 0x1u) != 0; }
    constexpr bool is_saida() const { return ((raw >> 7) & 0x1u) != 0; }
    constexpr std::uint32_t dst() const { return (raw >> 8) & 0x7u; }
    constexpr std::uint32_t src_a() const { return (raw >> 11) & 0x7u; }
    constexpr std::uint32_t src_b() const { return (raw >> 14) & 0x7u; }
    // O imediato s18 sobrepõe-se ao src_b e ao campo da ALU ✓.
    constexpr std::int32_t imediato() const {
        const std::int32_t campo = static_cast<std::int32_t>((raw >> 14) & 0x3FFFFu);
        return (campo & 0x20000) ? (campo - 0x40000) : campo;
    }
    constexpr OperacaoALU op_alu() const {
        return static_cast<OperacaoALU>((raw >> 17) & 0x1Fu);
    }
    constexpr std::uint32_t bf_src_bit() const { return (raw >> 17) & 0x1Fu; }
    constexpr std::uint32_t bf_size() const { return (raw >> 22) & 0x1Fu; }
    constexpr std::uint32_t bf_dst_bit() const { return (raw >> 27) & 0x1Fu; }
    // O `GetBitfieldMask` ✓.
    constexpr std::uint32_t mascara_bitfield() const {
        return (1u << (bf_size() & 31u)) - 1u;
    }
    // O `GetBranchTarget` ✓ — em BYTES porque o pc conta bytes ✓.
    constexpr std::int32_t alvo_ramo() const {
        return static_cast<std::int32_t>(imediato() * 4);
    }
};

// `Macro::MethodAddress` ✓ — o endereço do próximo `Send`: o endereço
// (bits 0..11) e o incremento (bits 12..17).
struct EnderecoMetodo {
    std::uint32_t raw = 0;
    constexpr std::uint32_t endereco() const { return raw & 0xFFFu; }
    constexpr std::uint32_t incremento() const { return (raw >> 12) & 0x3Fu; }
};

// Os callbacks que ligam a máquina ao motor:
//   - `enviar`: o `maxwell3d.CallMethod(endereco, valor, true)` do `Send` ✓;
//   - `ler`:    o `maxwell3d.GetRegisterValue(metodo)` do `Read` ✓.
using EnviarFn = std::function<void(std::uint32_t metodo, std::uint32_t valor)>;
using LerFn = std::function<std::uint32_t(std::uint32_t metodo)>;

// O interpretador — o porte de `MacroInterpreterImpl` ✓.
class Interpretador {
public:
    Interpretador(EnviarFn enviar, LerFn ler)
        : enviar_externo_{std::move(enviar)}, ler_externo_{std::move(ler)} {}

    // Executa o código da macro (o *bucket* do `uploaded_macro_code`,
    // desde a instrução 0) com os parámetros da chamada. Cada
    // `executar` é um "reset + executa até sair" (o upstream ✓).
    void executar(const std::vector<std::uint32_t>& codigo,
                  const std::vector<std::uint32_t>& params);

    // As medições do passo — o sintoma real de qualquer anomalia.
    std::uint64_t passos() const { return passos_; }
    std::uint32_t envios() const { return envios_; }
    bool saiu_por_teto() const { return saiu_por_teto_; }
    bool pc_fora() const { return pc_fora_; }
    bool params_esgotados() const { return params_esgotados_; }

    // A única adição da casa: o teto de passos (proteção contra
    // microcode em loop; o upstream não tem — confia no ASSERT).
    static constexpr std::uint64_t kTetoDePassos = 1000000;

private:
    // Um passo: devolve se a execução continua.
    bool passo(bool is_delay_slot);
    // A aplicação do resultado ao registo (o `ProcessResult` ✓).
    void resultado(OperacaoResultado op, std::uint32_t reg, std::uint32_t valor);
    // O `GetALUResult` ✓.
    std::uint32_t resultado_alu(OperacaoALU op, std::uint32_t src_a, std::uint32_t src_b);
    bool condicao(CondicaoRamo c, std::uint32_t valor) const;
    // O registo id (0 é fio a zero ✓).
    std::uint32_t reg(std::uint32_t id) const { return id == 0 ? 0u : registos_[id]; }
    void definir_reg(std::uint32_t id, std::uint32_t valor);
    void definir_endereco(std::uint32_t e);
    void enviar(std::uint32_t valor);
    std::uint32_t ler(std::uint32_t metodo) const;
    std::uint32_t buscar_param();

    EnviarFn enviar_externo_;
    LerFn ler_externo_;
    // Cópias — o código e os parámetros vivem durante toda a execução
    // (o upstream copia os parámetros para o seu buffer ✓; subs no
    // código entre microcode e motor: uma macro pode mandar métodos
    // que tocam no mesmo estado).
    std::vector<std::uint32_t> codigo_;
    std::vector<std::uint32_t> params_;
    std::uint32_t pc_ = 0;                // em BYTES ✓
    std::int32_t pc_atrasado_ = -1;       // o `delayed_pc` ✓ (-1 = vazio)
    std::array<std::uint32_t, kNumRegistos> registos_{};
    EnderecoMetodo endereco_{};
    std::uint32_t proximo_param_ = 1;     // o r1 já tem o 1º param ✓
    std::uint32_t num_params_ = 0;
    bool carry_ = false;
    // As medições.
    std::uint64_t passos_ = 0;
    std::uint32_t envios_ = 0;
    bool saiu_por_teto_ = false;
    bool pc_fora_ = false;
    bool params_esgotados_ = false;
};

}  // namespace g610::gpu::mme
