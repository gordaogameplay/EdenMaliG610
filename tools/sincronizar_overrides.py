#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sincroniza os overrides a partir do vendor.

## Porque este ficheiro existe

`tools/overrides/` guarda as nossas substituicoes aos ficheiros do Eden.
O `aplicar_overrides.py` copia-as **por cima** do vendor — e por isso, se o
vendor for editado e o override nao for sincronizado, **a edicao
perde-se**.

Foi o que aconteceu duas vezes nesta sessao: o log do
`CompleteSyncRequest` foi posto no `vendor/eden/core/hle/service/hle_ipc.cpp`,
o `aplicar_overrides.py` aplicou o override antigo por cima, e o log
nunca apareceu — o que se leu como "o codigo nao e' chamado", quando o
codigo era o outro.

**A regra:** depois de editar um ficheiro do vendor, correr este script.
E depois de sincronizar, **enviar o `vendor` E o `tools`** para o Pi — o
`aplicar_overrides.py` corre la.

Uso:
    python3 tools/sincronizar_overrides.py
"""
import os
import shutil
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVERRIDES = os.path.join(BASE, "tools", "overrides")
VENDOR = os.path.join(BASE, "vendor", "eden")


def main() -> int:
    if not os.path.isdir(OVERRIDES):
        print("nao ha overrides")
        return 0

    n = 0
    for raiz, _dirs, ficheiros in os.walk(OVERRIDES):
        for nome in sorted(ficheiros):
            if nome.endswith(".md") or nome == ".aplicados":
                continue
            origem = os.path.join(raiz, nome)
            relativo = os.path.relpath(origem, OVERRIDES)
            destino = os.path.join(VENDOR, relativo)
            if not os.path.exists(destino):
                print("AVISO: %s nao existe no vendor" % relativo)
                continue
            with open(origem, "rb") as f:
                a = f.read()
            with open(destino, "rb") as f:
                b = f.read()
            if a == b:
                continue
            # **A copia e' do vendor para o override.** O override e' a
            # FONTE — e' ele que o `aplicar_overrides.py` aplica por cima do
            # vendor. Se a copia fosse ao contrario, sincronizar apagaria a
            # edicao do vendor em vez de a preservar.
            shutil.copyfile(destino, origem)
            print("sincronizado: %s" % relativo)
            n += 1

    # **A lista de overrides e' a autoridade** — e e' o `aplicar_overrides.py`
    # que a usa. Sem ela, um override que desapareceu continuaria a ser
    # aplicado em silencio (e o `tar` nao apaga ficheiros).
    lista = []
    for raiz, _dirs, ficheiros in os.walk(OVERRIDES):
        for nome in sorted(ficheiros):
            if nome.endswith(".md") or nome == ".aplicados":
                continue
            lista.append(os.path.relpath(os.path.join(raiz, nome), OVERRIDES))
    with open(os.path.join(OVERRIDES, ".aplicados"), "w", encoding="utf-8") as f:
        for relativo in sorted(lista):
            f.write(relativo + "\n")

    print("%d ficheiro(s) sincronizado(s)" % n)
    if n:
        print("**Enviar o `vendor` E o `tools` para o Pi** — o "
              "`aplicar_overrides.py` corre la e o override antigo "
              "apagaria a edicao.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
