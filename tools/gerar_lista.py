#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regera a lista `IA_EDEN_SOURCES` do `CMakeLists.txt` a partir do que existe.

Porquê uma ferramenta e não um comando escrito à mão: aconteceu três vezes nesta
costura a lista ficar desactualizada em relação ao que estava vendorizado — e
cada vez a falha só aparecia no `cmake -S .`, com um ficheiro em falta que
ninguém tinha pedido.

O caso que custou mais foi o inverso: o `podar_vendor.py` apanhou
`common/assert.cpp` como órfão e apagou-o, **estando ele na lista**. O
podador e a lista mediam coisas diferentes.

A lista passa a ser derivada. E `podar_vendor.py` passa a lê-la. São a mesma
fonte de verdade, e é a razão de este ficheiro existir.

Uso:
    gerar_lista.py            escreve a lista
    gerar_lista.py --excluir X ...   deixa de fora ficheiros à mão
"""
import glob
import os
import re
import sys

ROOT = os.path.expanduser("~/EdenMaliIA")
CMAKE = os.path.join(ROOT, "CMakeLists.txt")

# Ficheiros vendorizados que nao compilam contra a ponte. Ver
# PROGRESSO.md, "A parede da espera".
# `k_synchronization_object.cpp` do Eden nao entra: o nosso esta em
# `src/eden_synchronization_object.cpp` e faz as mesmas quatro funcoes sem a
# espera. Ver PROGRESSO.md, "A parede da espera".
#
# `k_server_session.cpp` ENTRA: o corte da page table ficou feito e ele
# compila. Estava aqui por ser o ultimo obstaculo, e um obstaculo que se
# resolve deixa de ser uma razao para excluir nada.
DEFAULT_EXCLUIDOS = (
    "vendor/eden/core/hle/kernel/k_synchronization_object.cpp",
)


def ficheiros(excluidos):
    todos = sorted(glob.glob(os.path.join(ROOT, "vendor/eden/**/*.cpp"), recursive=True))
    rel = [os.path.relpath(f, ROOT) for f in todos]
    return [f for f in rel
            if f not in excluidos and os.path.exists(os.path.join(ROOT, f))]


def escrever(lista):
    with open(CMAKE, encoding="utf-8") as f:
        s = f.read()
    m = re.search(r"set\(IA_EDEN_SOURCES\n.*?\n\)", s, re.S)
    if not m:
        sys.exit("IA_EDEN_SOURCES nao encontrada no CMakeLists.txt")
    corpo = "\n".join("  " + f for f in lista)
    novo = "set(IA_EDEN_SOURCES\n" + corpo + "\n)"
    with open(CMAKE, "w", encoding="utf-8") as f:
        f.write(s[:m.start()] + novo + s[m.end():])
    return len(lista)


def main():
    excluidos = set(DEFAULT_EXCLUIDOS)
    if "--excluir" in sys.argv:
        i = sys.argv.index("--excluir")
        excluidos |= set(sys.argv[i + 1:])
    lista = ficheiros(excluidos)
    n = escrever(lista)
    print("IA_EDEN_SOURCES: %d ficheiros" % n)
    for f in lista:
        print("  " + f)
    return 0


if __name__ == "__main__":
    sys.exit(main())