#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Mede o fecho de headers vendorizado e diz o que é peso morto.

Porquê: o `tools/vendorizar.py` copia um `.cpp` e segue os `#include` dele.
Isso garante que nada falta, mas nada garante que não **sobe**. Na primeira
vez que o corremos, ele brought `k_page_table_base.h`, `k_scheduler.h` e
afins — o emulador, em forma de cabeçalho, que nunca chega a ser compilado
mas fica no repositório e no `README.md` a dizer que está lá.

A regra que este script impõe é a mesma do projecto: **não excluir uma
hipótese sem a testar**. Um header é removível se, e só se, nenhuma das fontes
que compilamos o alcança. Isso é uma medida, não uma opinião.

Uso:
    vendorizar.py --alcancavel     # imprime a lista do que compila
    vendorizar.py --podar          # apaga o que nao e' alcancavel
"""
import os
import re
import subprocess
import sys

ROOT = os.path.expanduser("~/EdenMaliIA")
VENDOR = os.path.join(ROOT, "vendor", "eden")

INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def incluir_de(ficheiro, visitados, raizes_extra):
    """Segue os includes de `ficheiro`, em profundidade, e devolve o alcançado."""
    if ficheiro in visitados or not os.path.exists(ficheiro):
        return visitados
    visitados.add(ficheiro)
    with open(ficheiro, encoding="utf-8", errors="replace") as f:
        for linha in f:
            m = INC_RE.match(linha)
            if not m:
                continue
            alvo = m.group(1)
            if alvo.startswith("vendor/eden/"):
                incluir_de(os.path.join(ROOT, alvo), visitados, raizes_extra)
            elif alvo.startswith(".."):
                seguir = os.path.normpath(os.path.join(os.path.dirname(ficheiro), alvo))
                incluir_de(seguir, visitados, raizes_extra)
            else:
                for r in raizes_extra:
                    cand = os.path.join(r, alvo)
                    if os.path.exists(cand):
                        incluir_de(cand, visitados, raizes_extra)
                        break
    return visitados


def raizes_de_include():
    return [ROOT, os.path.join(ROOT, "include"), os.path.join(ROOT, "vendor", "third_party")]


def alcancaveis():
    """As fontes que realmente compilam, e tudo o que elas alcançam."""
    raizes = raizes_de_include()
    visitados = set()

    # As fontes do nosso motor que tocam no Eden.
    for rel in ("src/eden_bridge.cpp",
                "vendor/eden/core/hle/kernel/k_kernel_core_bridge.h",
                "vendor/eden/core/hle/kernel/k_process_bridge.h",
                "vendor/eden/core/system_ref.h"):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            incluir_de(p, visitados, raizes)

    for p in sorted(os.listdir(os.path.join(VENDOR, "core/hle/kernel"))):
        if p.endswith(".cpp"):
            incluir_de(os.path.join(VENDOR, "core/hle/kernel", p), visitados, raizes)
    for sub in ("core/hle/service", "core/hle/service/sm"):
        d = os.path.join(VENDOR, sub)
        if os.path.isdir(d):
            for p in sorted(os.listdir(d)):
                if p.endswith(".cpp"):
                    incluir_de(os.path.join(d, p), visitados, raizes)
    return visitados


def main():
    alc = alcancaveis()
    todos = set()
    for dirpath, _, files in os.walk(VENDOR):
        for p in files:
            if p.endswith((".h", ".cpp")):
                todos.add(os.path.join(dirpath, p))

    orfaos = sorted(todos - alc)
    rel = lambda p: os.path.relpath(p, ROOT)

    if "--podar" in sys.argv:
        for p in orfaos:
            os.remove(p)
        # directorios vazios, de baixo para cima
        for dirpath, _, _ in sorted(os.walk(VENDOR), reverse=True):
            if not os.listdir(dirpath) and dirpath != VENDOR:
                os.rmdir(dirpath)
        print("Podados: %d ficheiros" % len(orfaos))
        for p in orfaos:
            print("  - %s" % rel(p))
        return 0

    print("Alcancados: %d" % len(alc))
    print("Orfaos:     %d\n" % len(orfaos))
    linhas = 0
    for p in orfaos:
        with open(p, encoding="utf-8", errors="replace") as f:
            n = f.read().count("\n")
        linhas += n
        print("  %-62s %5d" % (rel(p), n))
    print("\nTotal em orfaos: %d linhas" % linhas)
    return 0


if __name__ == "__main__":
    sys.exit(main())