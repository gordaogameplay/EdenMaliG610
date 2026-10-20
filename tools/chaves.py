#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-
"""Conta as chaves de verdade: ignora comentários e literais.

Existe porque **contei-as mal durante horas**. O contador de uma linha
`{` menos `}` includes as chaves que estão dentro de comentários, e um ficheiro
cheio de notas em português desbalanceia logo.

A primeira vez que isto custou tempo, um comentário meu dizia
«um `namespace g610 {` e declarou `g610::Kernel`» — e o contador dizia que faltava
uma chave. Duas tentativas de consertar o ficheiro, e o ficheiro estava certo
desde o início.

Uma medição errada é pior do que nenhuma: ela manda-me mexer em código que
está bom.

Uso:
    chaves.py <ficheiro> [...]     diz o saldo de cada um
    chaves.py --todos               todos os ficheiros do projecto
"""
import glob
import os
import sys


def saldo(texto):
    """Balanço de {}, () e [] fora de comentários e de literais."""
    i = 0
    n = len(texto)
    profundidade = {"{": 0, "(": 0, "[": 0}
    pares = {"}": "{", ")": "(", "]": "["}
    linha = 1
    saldo_por_linha = {}
    estado = "codigo"

    while i < n:
        c = texto[i]
        if c == "\n":
            linha += 1
            if estado == "linha":
                estado = "codigo"
            i += 1
            continue

        if estado == "codigo":
            if texto.startswith("//", i):
                estado = "linha"
                i += 2
                continue
            if texto.startswith("/*", i):
                estado = "bloco"
                i += 2
                continue
            if c == '"':
                estado = "string"
                i += 1
                continue
            if c == "'":
                estado = "char"
                i += 1
                continue
            if c in profundidade:
                profundidade[c] += 1
                saldo_por_linha[linha] = dict(profundidade)
            elif c in pares:
                profundidade[pares[c]] -= 1
                saldo_por_linha[linha] = dict(profundidade)
        elif estado == "linha":
            if texto.startswith("*/", i) and False:
                pass
        elif estado == "bloco":
            if texto.startswith("*/", i):
                estado = "codigo"
                i += 2
                continue
        elif estado == "string":
            if c == "\\":
                i += 2
                continue
            if c == '"':
                estado = "codigo"
        elif estado == "char":
            if c == "\\":
                i += 2
                continue
            if c == "'":
                estado = "codigo"
        i += 1

    return profundidade, saldo_por_linha


def main():
    ficheiros = []
    if "--todos" in sys.argv:
        padroes = ["src/*.cpp", "src/*.hpp", "include/g610/*.hpp", "include/g610/*.h",
                   "tests/*.cpp", "vendor/eden/core/*.h", "vendor/eden/common/*.h",
                   "vendor/eden/core/hle/kernel/k_*bridge*.h"]
        for pad in padroes:
            ficheiros += glob.glob(pad)
    else:
        ficheiros = [a for a in sys.argv[1:] if not a.startswith("--")]

    mau = 0
    for f in sorted(ficheiros):
        if not os.path.exists(f):
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            texto = fh.read()
        prof, por_linha = saldo(texto)
        mau += sum(1 for v in prof.values() if v != 0)
        marca = "" if all(v == 0 for v in prof.values()) else "   <-- DESBALANCEADO"
        print("%-62s {}=%d ()=%d []=%d%s" % (f, prof["{"], prof["("], prof["["], marca))
        if any(v != 0 for v in prof.values()):
            for ln in sorted(por_linha)[-1:]:
                p = por_linha[ln]
                if any(v != 0 for v in p.values()):
                    print("      ultimo ponto medido: linha %d -> {}=%d ()=%d []=%d"
                          % (ln, p["{"], p["("], p["["]))
    return 1 if mau else 0


if __name__ == "__main__":
    sys.exit(main())