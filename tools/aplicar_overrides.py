#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Aplica as nossas substituicoes sobre a arvore vendorizada.

## Porque

A arvore `vendor/` e' uma copia do Eden, e uma copia volta a ser a copia do
Eden sempre que se sincroniza. As nossas substituicoes — os ficheiros que
escrevemos em vez dos do Eden — precisam de sobreviver a isso.

Por isso vivem em `tools/overrides/`, **fora** do vendor, e este script
copia-as para o sitio. E' o unico ponto do projecto onde se decide que um
ficheiro do Eden e' substituido por um nosso, e porquê.

## Como se usa

    python3 tools/aplicar_overrides.py

Corre depois de qualquer `vendorizar.py` ou `podar_vendor.py`, e antes de
compilar.
"""
import os
import shutil
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVERRIDES = os.path.join(RAIZ, "tools", "overrides")


def main() -> int:
    if not os.path.isdir(OVERRIDES):
        print("nao ha overrides")
        return 0
    n = 0
    for raiz, _dirs, ficheiros in os.walk(OVERRIDES):
        for nome in sorted(ficheiros):
            if nome.endswith(".md"):
                continue
            # O caminho dentro de `tools/overrides/` e' o caminho dentro de
            # `vendor/eden/`. E' assim que o override sabe onde vai.
            relativo = os.path.relpath(os.path.join(raiz, nome), OVERRIDES)
            destino = os.path.join(RAIZ, "vendor", "eden", relativo)
            os.makedirs(os.path.dirname(destino), exist_ok=True)
            shutil.copyfile(os.path.join(raiz, nome), destino)
            print("  override: %s" % relativo)
            n += 1
    print("%d override(s) aplicado(s)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
