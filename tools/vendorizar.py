#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Vendoriza um ficheiro do Eden, e fecha o fecho de headers transitivamente.

Porquê um script e não a mão: a cadeia de includes do Eden é profunda e cada
include é reescrito para `vendor/eden/...`. Fazer isso à mão erra, e uma
referência a um header não vendorizado só aparece como "fatal error: X: No
such file" — que não diz qual dos trinta e um ficheiros o pediu.

A regra: um include entre aspas que não comece por `<` e cujo alvo não
exista localmente é buscado no Eden e copiado. Includes de sistema (`<x>`)
e relativos (`../y`) ficam como estão, e os relativos são avisos — porque
rebasear um include relativo exige saber de onde ele vinha, e adivinhar
seria inventar.
"""
import os
import re
import subprocess
import sys

EDEN = os.path.expanduser("~/eden/src")
ROOT = os.path.expanduser("~/EdenMaliIA")
VENDOR = os.path.join(ROOT, "vendor", "eden")

INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def vendorizar(origem_rel):
    """Copia `origem_rel` (caminho relativo a ~/eden/src) e fecha o fecho."""
    n_copiados = 0
    fila = [origem_rel]
    vistos = set()
    avisos = []

    while fila:
        rel = fila.pop(0)
        if rel in vistos:
            continue
        vistos.add(rel)

        src = os.path.join(EDEN, rel)
        if not os.path.exists(src):
            avisos.append("NAO ENCONTRADO no Eden: %s" % rel)
            continue

        dst = os.path.join(VENDOR, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)

        with open(src, encoding="utf-8", errors="replace") as f:
            texto = f.read()

        # `core/hle/...` -> `vendor/eden/core/hle/...`. O prefixo relativo ao
        # src/ do Eden e' constante em todo o codigo, por isso e' seguro.
        novo = []
        for linha in texto.split("\n"):
            m = INC_RE.match(linha)
            if m:
                alvo = m.group(1)
                if alvo.startswith(".."):
                    avisos.append("INCLUDE RELATIVO (nao remapeado): %s -> %s" % (rel, alvo))
                    novo.append(linha)
                    continue
                novo.append(linha.replace('"%s"' % alvo, '"vendor/eden/%s"' % alvo))
                if not os.path.exists(os.path.join(VENDOR, alvo)):
                    fila.append(alvo)
            else:
                novo.append(linha)
        texto = "\n".join(novo)

        with open(dst, "w", encoding="utf-8") as f:
            f.write(texto)
        n_copiados += 1

    return n_copiados, vistos, avisos


def main():
    alvos = sys.argv[1:]
    if not alvos:
        print("uso: vendorizar.py <rel> [<rel> ...]   (rel relativo a ~/eden/src)")
        return 1

    total = 0
    todos = []
    todos_avisos = []
    for a in alvos:
        n, vistos, avisos = vendorizar(a)
        total += n
        todos += list(vistos)
        todos_avisos += avisos
        print("  %-46s -> %d ficheiros" % (a, n))

    print("\nTotal: %d ficheiros em vendor/eden" % total)
    if todos_avisos:
        print("\nAVISOS (%d):" % len(todos_avisos))
        for a in sorted(set(todos_avisos)):
            print("  ! " + a)

    # Sintaxe de cada .cpp vendorizado: e' o teste que diz a verdade.
    falhou = 0
    for rel in sorted(todos):
        if not rel.endswith(".cpp"):
            continue
        destino = os.path.join(VENDOR, rel)
        cmd = [
            "g++", "-std=c++20", "-I" + ROOT, "-I" + os.path.join(ROOT, "include"),
            "-I" + os.path.join(ROOT, "vendor", "third_party"),
            "-DANKERL_UNORDERED_DENSE_STD_MODULE=1", "-fsyntax-only", destino,
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        n = r.stderr.count("error:")
        if n:
            falhou += 1
            primeira = [l for l in r.stderr.split("\n") if "error:" in l]
            print("  %-42s ERROS=%-4d %s" % (rel.split("/")[-1], n,
                                            primeira[0].strip()[:90] if primeira else ""))
    if not falhou:
        print("\nTodos os .cpp vendorizados compilam.")
    return 0


if __name__ == "__main__":
    sys.exit(main())