#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
#
# O tracer da máquina MME — a MESMA semântica do port da casa
# (src/gpu/macro_mme.cpp = macro_interpreter.cpp do upstream) — sobre
# o código real de um bucket e os params reais de uma chamada. Serve
# para ver o que o microcode faz passo a passo quando o interpretador
# C++ trava (a medição do loop).
#
# O ficheiro de registos do motor não é simulado (o Read devolve 0);
# o C++ real lê o estado acumulado da submissão.
#
# Uso: python3 tools/trace_macro.py <fixture.bin> <bucket> <p0,0x> <p1,0x> ...
import sys

MASK = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF


def sign_extend(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Opc:
    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = raw

    op = property(lambda s: s.raw & 7)
    result_op = property(lambda s: (s.raw >> 4) & 7)
    branch_cond = property(lambda s: (s.raw >> 4) & 1)
    branch_annul = property(lambda s: (s.raw >> 5) & 1)
    is_exit = property(lambda s: (s.raw >> 7) & 1)
    dst = property(lambda s: (s.raw >> 8) & 7)
    src_a = property(lambda s: (s.raw >> 11) & 7)
    src_b = property(lambda s: (s.raw >> 14) & 7)
    imm = property(lambda s: sign_extend(s.raw >> 14, 18))
    alu = property(lambda s: (s.raw >> 17) & 0x1F)
    bf_src_bit = property(lambda s: (s.raw >> 17) & 0x1F)
    bf_size = property(lambda s: (s.raw >> 22) & 0x1F)
    bf_dst_bit = property(lambda s: (s.raw >> 27) & 0x1F)
    mask = property(lambda s: (1 << s.bf_size) - 1)


def trace(code, params, teto=5000000, ler_engine=None, aplica_send=None):
    """A máquina recursiva do upstream — `Step(is_delay_slot)` fiel."""
    if ler_engine is None:
        ler_engine = lambda metodo: 0  # noqa: E731
    est = {
        "pc": 0,
        "delayed": None,
        "method_address": 0,
        "next_param": 1,
        "carry": False,
        "envios": [],
        "passos": [],
        "n": 0,
    }
    regs = [0] * 8
    num_params = len(params)
    if num_params:
        regs[1] = params[0]  # o 1º pré-carrega o r1 ✓

    def setear(reg, v):
        # r0 guarda zero: as escritas nele desaparecem (o upstream ✓).
        if reg != 0:
            regs[reg] = v & MASK

    def enviar(v):
        addr = est["method_address"] & 0xFFF
        if aplica_send is not None:
            aplica_send(addr, v)
        est["envios"].append((addr, v, est["method_address"]))
        nv = (addr + ((est["method_address"] >> 12) & 0x3F)) & 0xFFF
        est["method_address"] = (est["method_address"] & ~0xFFF) | nv

    def fetch():
        if est["next_param"] >= num_params:
            return 0  # o C++ marca `params esgotados`
        v = params[est["next_param"]]
        est["next_param"] += 1
        return v

    def resultado(r_op, reg, valor):
        if r_op == 0:      # IgnoreAndFetch
            setear(reg, fetch())
        elif r_op == 1:    # Move
            setear(reg, valor)
        elif r_op == 2:    # MoveAndSetMethod
            setear(reg, valor)
            est["method_address"] = valor
        elif r_op == 3:    # FetchAndSend
            setear(reg, fetch())
            enviar(valor)
        elif r_op == 4:    # MoveAndSend
            setear(reg, valor)
            enviar(valor)
        elif r_op == 5:    # FetchAndSetMethod
            setear(reg, fetch())
            est["method_address"] = valor
        elif r_op == 6:    # MoveAndSetMethodFetchAndSend
            setear(reg, valor)
            est["method_address"] = valor
            enviar(fetch())
        elif r_op == 7:    # MoveAndSetMethodSend
            setear(reg, valor)
            est["method_address"] = valor
            enviar((valor >> 12) & 0b111111)

    def linha(i, base, opc, texto):
        est["passos"].append(
            f"[{i:3d}] pc={base:3d} raw={opc.raw:#010x} ro={opc.result_op}"
            f"{f' exit' if opc.is_exit else ''}{f' slot' if False else ''} | {texto}")

    def passo(is_delay_slot):
        est["n"] += 1
        if est["n"] > teto:
            est["passos"].append(f"!!! TETO DE PASSOS ({teto}) — LOOP REPRODUZIDO")
            return False
        base = est["pc"]
        if base >= len(code) * 4:
            est["passos"].append(f"!!! PC FORA (pc={base}, teto={len(code) * 4})")
            return False
        opc = Opc(code[base // 4])
        est["pc"] += 4
        if est["delayed"] is not None:
            est["pc"] = est["delayed"]
            est["delayed"] = None
        i = est["n"] - 1

        antes = list(regs)
        if opc.op == 0:     # ALU
            a, b = regs[opc.src_a], regs[opc.src_b]
            al = opc.alu
            if al == 0:
                r64 = a + b
                est["carry"] = r64 > MASK
                res = r64 & MASK
            elif al == 1:
                r64 = a + b + (1 if est["carry"] else 0)
                est["carry"] = r64 > MASK
                res = r64 & MASK
            elif al == 2:
                r64 = (a - b) & MASK64
                est["carry"] = r64 < 0x100000000
                res = r64 & MASK
            elif al == 3:
                r64 = (a - b - (0 if est["carry"] else 1)) & MASK64
                est["carry"] = r64 < 0x100000000
                res = r64 & MASK
            elif al == 8:
                res = a ^ b
            elif al == 9:
                res = a | b
            elif al == 10:
                res = a & b
            elif al == 11:
                res = a & (~b & MASK)
            elif al == 12:
                res = ~(a & b) & MASK
            else:
                est["passos"].append(f"[{i:3d}] pc={base:3d} raw={opc.raw:#010x} !!! ALU {al} fora da tabela")
                return False
            resultado(opc.result_op, opc.dst, res)
            linha(i, base, opc,
                  f"ALU{al} a={a:#x} b={b:#x} r={res:#x} | {antes} -> {regs}")
        elif opc.op == 1:   # AddImmediate
            res = (regs[opc.src_a] + opc.imm) & MASK
            resultado(opc.result_op, opc.dst, res)
            linha(i, base, opc,
                  f"ADDI r{opc.src_a}({regs[opc.src_a]:#x})+{opc.imm} -> r{opc.dst}"
                  f" | {antes} -> {regs}")
        elif opc.op == 2:   # ExtractInsert
            campo = (regs[opc.src_b] >> opc.bf_src_bit) & opc.mask
            alvo = regs[opc.src_a]
            alvo &= ~(opc.mask << opc.bf_dst_bit) & MASK
            alvo |= (campo << opc.bf_dst_bit) & MASK
            resultado(opc.result_op, opc.dst, alvo)
            linha(i, base, opc,
                  f"EXTINS bf:{opc.bf_size}@{opc.bf_src_bit} de r{opc.src_b} -> "
                  f"bit{opc.bf_dst_bit} r{opc.dst}={alvo:#x} | {antes} -> {regs}")
        elif opc.op == 3:   # ExtractShiftLeftImmediate
            desl = regs[opc.src_a]
            campo = regs[opc.src_b]
            res = (((campo >> desl) & opc.mask) << opc.bf_dst_bit) & MASK
            resultado(opc.result_op, opc.dst, res)
            linha(i, base, opc,
                  f"ESLI desl=r{opc.src_a}({desl}) campo=r{opc.src_b} bit{opc.bf_dst_bit}"
                  f" r={res:#x} | {antes} -> {regs}")
        elif opc.op == 4:   # ExtractShiftLeftRegister
            desl = regs[opc.src_a]
            campo = regs[opc.src_b]
            res = (((campo >> opc.bf_src_bit) & opc.mask) << desl) & MASK
            resultado(opc.result_op, opc.dst, res)
            linha(i, base, opc,
                  f"ESLR desl=r{opc.src_a}({desl}) campo=r{opc.src_b}"
                  f" r={res:#x} | {antes} -> {regs}")
        elif opc.op == 5:   # Read
            metodo = (regs[opc.src_a] + opc.imm) & MASK
            res = ler_engine(metodo)
            resultado(opc.result_op, opc.dst, res)
            linha(i, base, opc,
                  f"READ m={metodo:#x} r={res:#x} | {antes} -> {regs}")
        elif opc.op == 7:   # Branch
            valor = regs[opc.src_a]
            tomado = (valor == 0 and opc.branch_cond == 0) or \
                     (valor != 0 and opc.branch_cond == 1)
            alvo = (base + opc.imm * 4) & MASK
            if tomado and not opc.branch_annul:
                linha(i, base, opc,
                      f"BRANCH r{opc.src_a}={valor:#x} TOMADO (slot) alvo={alvo}")
                est["delayed"] = alvo
                return passo(True)   # o slot executa primeiro
            if tomado:
                linha(i, base, opc,
                      f"BRANCH r{opc.src_a}={valor:#x} TOMADO (anulado) alvo={alvo}")
                est["pc"] = alvo
                return True
            linha(i, base, opc,
                  f"BRANCH r{opc.src_a}={valor:#x} NAO-TOMADO (alvo seria {alvo})")
        elif opc.op == 6:
            linha(i, base, opc, "!!! OP 6 (nao-usado; o C++ conta pc_fora)")
            return False
        else:
            linha(i, base, opc, "!!! OP DESCONHECIDO")
            return False

        if opc.is_exit and not is_delay_slot:
            passo(True)   # o slot do Exit executa (o resultado é ignorado)
            return False
        return True

    while passo(False):
        pass
    return est


def main():
    fixture = sys.argv[1]
    bucket_n = int(sys.argv[2])
    params = [int(x, 0) for x in sys.argv[3:]]
    bruto = open(fixture, "rb").read()
    n = len(bruto) // 4
    words = [int.from_bytes(bruto[i * 4:(i + 1) * 4], "little") for i in range(n)]
    import importlib.util
    spec = importlib.util.spec_from_file_location("sim", "tools/simular_mme.py")
    sim = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sim)
    buckets, positions, chamadas = sim.simular(words)
    code = buckets[bucket_n]
    print(f"bucket {bucket_n}: {len(code)} palavras; params={params}")
    est = trace(code, params)
    for p in est["passos"]:
        print(p)
    print(f"passos totais: {est['n']}; envios: {len(est['envios'])}")
    for a, v, raw in est["envios"][:60]:
        print(f"  Send addr={a:#x} (raw={raw:#x}) val={v:#x}")
    print(f"params: consumidos {est['next_param']} de {len(params)}")


if __name__ == "__main__":
    main()
