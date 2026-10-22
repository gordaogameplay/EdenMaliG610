// SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
// SPDX-License-Identifier: GPL-3.0-or-later
// EdenMaliG610 — o interpretador das macros MME (o caminho LE do
// upstream, `vendor/eden/src/video_core/macro/macro_interpreter.cpp`).
//
// Cada função corresponde ao upstream de mesmo nome; as dicas `✓`
// marcam os pontos copiados tal cual (a máquina é pequena e o
// microcode real do jogo depende do detalhe: o slot de atraso do
// `Exit`, o transporte da ALU, o incremento do endereço de 12 bits).
#include "g610/gpu/macro_mme.hpp"

namespace g610::gpu::mme {

namespace {
// O tamanho máximo do código compilável no campo (a RAM MME esta' em
// 0x800 palavras no hardware ✓; os 623 medidos do jogo cabem de sobra).
constexpr std::size_t kCodigoTeto = 0x10000;
}  // namespace

void Interpretador::executar(const std::vector<std::uint32_t>& codigo,
                             const std::vector<std::uint32_t>& params) {
    // O Reset ✓ (o upstream zeroa tudo por chamada).
    registos_ = {};
    pc_ = 0;
    pc_atrasado_ = -1;
    endereco_ = {};
    proximo_param_ = 1;
    carry_ = false;
    passos_ = 0;
    envios_ = 0;
    saiu_por_teto_ = false;
    pc_fora_ = false;
    params_esgotados_ = false;

    // O código (o `code` do upstream ✓) — cópia local.
    codigo_ = codigo;
    if (codigo_.size() > kCodigoTeto) {
        // Um bucket maior do que qualquer upload medido: o jogo nunca
        // manda isto; corta honestamente.
        codigo_.resize(kCodigoTeto);
        pc_fora_ = true;
    }

    // Os parámetros (o buffer do upstream ✓ — a cópia evita que o
    // motor mude o vector durante a execução).
    params_ = params;
    num_params_ = static_cast<std::uint32_t>(params_.size());
    // O 1º param pré-carrega o registo 1 (o upstream `Execute` ✓). Com
    // params vazios o upstream é UB; a casa conta o sintoma e segue.
    if (num_params_ > 0) {
        registos_[1] = params_[0];
    } else {
        params_esgotados_ = true;
    }

    // A execução até ao Exit (o upstream ✓).
    bool continua = true;
    while (continua && !saiu_por_teto_ && !pc_fora_ && !params_esgotados_) {
        continua = passo(false);
    }
}

bool Interpretador::passo(bool is_delay_slot) {
    const std::uint32_t endereco_base = pc_;

    // O `GetOpcode` ✓ — o pc conta bytes; a instrução é `code[pc / 4]`.
    const std::uint64_t bytes = static_cast<std::uint64_t>(codigo_.size()) * 4u;
    if (pc_ >= bytes) {
        pc_fora_ = true;
        return false;
    }
    Opcode opc{codigo_[pc_ / 4u]};
    pc_ += 4;

    // Se veio de um slot de atraso, o pc salta para o alvo pendente ✓.
    if (pc_atrasado_ >= 0) {
        pc_ = static_cast<std::uint32_t>(pc_atrasado_);
        pc_atrasado_ = -1;
    }

    ++passos_;
    if (passos_ > kTetoDePassos) {
        saiu_por_teto_ = true;
        return false;
    }

    switch (opc.operacao()) {
    case Operacao::ALU: {
        const std::uint32_t resultado_pre = resultado_alu(
            opc.op_alu(), reg(opc.src_a()), reg(opc.src_b()));
        resultado(opc.operacao_resultado(), opc.dst(), resultado_pre);
        break;
    }
    case Operacao::SomaImediata: {
        const std::uint32_t resultado_pre =
            reg(opc.src_a()) + static_cast<std::uint32_t>(opc.imediato());
        resultado(opc.operacao_resultado(), opc.dst(), resultado_pre);
        break;
    }
    case Operacao::ExtrairInserir: {
        // Os nomes do upstream ✓: o valor de src_a é o registo destino
        // e o de src_b é o conteúdo do campo a inserir.
        std::uint32_t campo = reg(opc.src_b());
        campo = (campo >> (opc.bf_src_bit() & 31u)) & opc.mascara_bitfield();
        std::uint32_t alvo = reg(opc.src_a());
        const std::uint32_t bit = opc.bf_dst_bit() & 31u;
        alvo &= ~(opc.mascara_bitfield() << bit);
        alvo |= campo << bit;
        resultado(opc.operacao_resultado(), opc.dst(), alvo);
        break;
    }
    case Operacao::ExtrairDeslocarImediato: {
        const std::uint32_t deslocamento = reg(opc.src_a());
        const std::uint32_t campo = reg(opc.src_b());
        const std::uint32_t pre =
            ((campo >> (deslocamento & 31u)) & opc.mascara_bitfield())
            << (opc.bf_dst_bit() & 31u);
        resultado(opc.operacao_resultado(), opc.dst(), pre);
        break;
    }
    case Operacao::ExtrairDeslocarRegisto: {
        const std::uint32_t deslocamento = reg(opc.src_a());
        const std::uint32_t campo = reg(opc.src_b());
        const std::uint32_t pre =
            ((campo >> (opc.bf_src_bit() & 31u)) & opc.mascara_bitfield())
            << (deslocamento & 31u);
        resultado(opc.operacao_resultado(), opc.dst(), pre);
        break;
    }
    case Operacao::Leitura: {
        const std::uint32_t metodo = reg(opc.src_a()) +
                                     static_cast<std::uint32_t>(opc.imediato());
        const std::uint32_t pre = ler(metodo);
        resultado(opc.operacao_resultado(), opc.dst(), pre);
        break;
    }
    case Operacao::Ramo: {
        // A máquina real não valida ramo em slot de atraso (o upstream
        // ASSERT); a casa conta e trata como não tomado — sintoma real,
        // nunca acontece num microcode são.
        if (is_delay_slot) {
            return true;
        }
        const std::uint32_t valor = reg(opc.src_a());
        if (condicao(opc.condicao_ramo(), valor)) {
            if (opc.ramo_anulado()) {
                // O salto imediato (o slot anulado ✓).
                pc_ = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(endereco_base) + opc.alvo_ramo());
                return true;
            }
            // O slot de atraso executa primeiro ✓; o alvo fica pendente.
            pc_atrasado_ = static_cast<std::int32_t>(
                static_cast<std::int64_t>(endereco_base) + opc.alvo_ramo());
            return passo(true);
        }
        break;
    }
    case Operacao::NaoUsado:
    default:
        // Codificação inválida: o upstream loga e continua (o que
        // leria lixo até ao infinito); a casa para e conta — sintoma
        // real, honesto e travado.
        pc_fora_ = true;
        return false;
    }

    // O Exit tem um slot de atraso: a próxima instrução executa, e é
    // depois disso que o programa acaba ✓.
    if (opc.is_saida() && !is_delay_slot) {
        passo(true);
        return false;
    }
    return true;
}

std::uint32_t Interpretador::resultado_alu(OperacaoALU op, std::uint32_t src_a,
                                           std::uint32_t src_b) {
    switch (op) {
    case OperacaoALU::Soma: {
        const std::uint64_t r = static_cast<std::uint64_t>(src_a) + src_b;
        carry_ = r > 0xFFFFFFFFu;
        return static_cast<std::uint32_t>(r);
    }
    case OperacaoALU::SomaComTransporte: {
        const std::uint64_t r = static_cast<std::uint64_t>(src_a) + src_b +
                                (carry_ ? 1u : 0u);
        carry_ = r > 0xFFFFFFFFu;
        return static_cast<std::uint32_t>(r);
    }
    case OperacaoALU::Subtracao: {
        const std::uint64_t r = static_cast<std::uint64_t>(src_a) - src_b;
        carry_ = r < 0x100000000ull;  // sem empréstimo ✓
        return static_cast<std::uint32_t>(r);
    }
    case OperacaoALU::SubtracaoComEmprestimo: {
        const std::uint64_t r = static_cast<std::uint64_t>(src_a) - src_b -
                                (carry_ ? 0u : 1u);
        carry_ = r < 0x100000000ull;  // sem empréstimo ✓
        return static_cast<std::uint32_t>(r);
    }
    case OperacaoALU::Xor:
        return src_a ^ src_b;
    case OperacaoALU::Ou:
        return src_a | src_b;
    case OperacaoALU::E:
        return src_a & src_b;
    case OperacaoALU::ENao:
        return src_a & ~src_b;
    case OperacaoALU::NaoE:
        return ~(src_a & src_b);
    default:
        // Codificação inválida (as operações 4..7 ✓) — o upstream
        // loga e devolve 0; a casa faz o mesmo (a ALU é pura, sem
        // estado a corromper).
        return 0;
    }
}

void Interpretador::resultado(OperacaoResultado op, std::uint32_t reg_id,
                              std::uint32_t valor) {
    switch (op) {
    case OperacaoResultado::IgnorarEBuscar:
        // Busca um parámetro e ignora o resultado ✓.
        definir_reg(reg_id, buscar_param());
        break;
    case OperacaoResultado::Mover:
        definir_reg(reg_id, valor);
        break;
    case OperacaoResultado::MoverEMetodo:
        // Move e usa o valor como endereço de método ✓.
        definir_reg(reg_id, valor);
        definir_endereco(valor);
        break;
    case OperacaoResultado::BuscarEEnviar:
        // Busca um parámetro e envia o resultado ✓.
        definir_reg(reg_id, buscar_param());
        enviar(valor);
        break;
    case OperacaoResultado::MoverEEnviar:
        definir_reg(reg_id, valor);
        enviar(valor);
        break;
    case OperacaoResultado::BuscarEMetodo:
        // Busca um parámetro e usa-o como endereço de método ✓.
        definir_reg(reg_id, buscar_param());
        definir_endereco(valor);
        break;
    case OperacaoResultado::MetodoBuscarEEnviar:
        definir_reg(reg_id, valor);
        definir_endereco(valor);
        enviar(buscar_param());
        break;
    case OperacaoResultado::MetodoEEnviar:
        // Manda bits 12:17 do resultado ✓ (o incremento, em 6 bits).
        definir_reg(reg_id, valor);
        definir_endereco(valor);
        enviar((valor >> 12) & 0x3Fu);
        break;
    }
}

bool Interpretador::condicao(CondicaoRamo c, std::uint32_t valor) const {
    switch (c) {
    case CondicaoRamo::Zero:
        return valor == 0;
    case CondicaoRamo::NaoZero:
        return valor != 0;
    }
    return false;
}

void Interpretador::definir_reg(std::uint32_t id, std::uint32_t valor) {
    // O registo 0 é fio a zero: as escritas nele não acontecem ✓.
    if (id == 0) return;
    if (id < kNumRegistos) registos_[id] = valor;
}

void Interpretador::definir_endereco(std::uint32_t e) {
    // O `SetMethodAddress` do upstream ✓ (o `raw` inteiro: os campos
    // address/increment são lidos por máscara no `send`).
    endereco_.raw = e;
}

void Interpretador::enviar(std::uint32_t valor) {
    ++envios_;
    // O `maxwell3d.CallMethod(endereco, valor, true)` ✓.
    enviar_externo_(endereco_.endereco(), valor);
    // O endereço avança pelo incremento — **só no campo address**
    // (o `BitField<0,12>::Assign` do upstream ✓ envolve em 4096).
    const std::uint32_t proximo = ((endereco_.raw & 0xFFFu) + endereco_.incremento()) & 0xFFFu;
    endereco_.raw = (endereco_.raw & ~0xFFFu) | proximo;
}

std::uint32_t Interpretador::ler(std::uint32_t metodo) const {
    // O `GetRegisterValue` ✓ — o reg_array do motor; fora da tabela é
    // sintoma (o upstream ASSERT; a casa devolve 0 e deixa contar).
    return ler_externo_(metodo);
}

std::uint32_t Interpretador::buscar_param() {
    // O `FetchParameter` ✓ — o ASSERT do upstream (`next < num`) torna-
    // se aqui uma contagem honesta: microcode a pedir mais parámetros
    // do que a chamada trouxe.
    if (proximo_param_ >= num_params_) {
        params_esgotados_ = true;
        return 0;
    }
    return params_[proximo_param_++];
}

}  // namespace g610::gpu::mme
