#!/usr/bin/env python3
"""Aplica as nossas substituicoes sobre a arvore vendorizada.

## Porque

A arvore `vendor/` e' uma copia do Eden, e uma copia volta a ser a copia do
Eden sempre que se sincroniza. As nossas substituicoes — os ficheiros que
escrevemos em vez dos do Eden — precisam de sobreviver a isso.

Por isso vivem em `tools/overrides/`, **fora** do vendor, e este script
copia-as para o sitio. E' o unico ponto do projecto onde se decide que um
ficheiro do Eden e' substituido por um nosso, e porquê.

## O ficheiro `.aplicados`

**Um override que desaparece tem de deixar de ser aplicado.**

Foi o que aconteceu duas vezes nesta sessao: o `ipc_helpers.h` e o
`hle_ipc.h` foram removidos do projecto (porque eram redundantes — eram
copias do vendor), mas ficaram no `tools/overrides/` do Pi. O `tar` **nao
apaga** ficheiros, e o `aplicar_overrides.py` continuava a aplica-los — o que
significava que as nossas edicoes ao vendor eram **revertidas em silencio**.

O sintoma era desconcertante: uma mudanca feito e correcta no
`ipc_helpers.h` nao aparecia no binario, e o compilador dizia que o metodo
nao existia.

Por isso este script guarda a lista do que aplicou, em
`tools/overrides/.aplicados`, e **apaga do vendor** o que ja nao esta na
lista. Um override que se va tem de levar as suas substituicoes com ele.

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
MARCADOR = os.path.join(OVERRIDES, ".aplicados")


def destinos():
    """Os caminhos (relativos ao vendor) que tem override."""
    lista = []
    for raiz, _dirs, ficheiros in os.walk(OVERRIDES):
        for nome in sorted(ficheiros):
            if nome.endswith(".md") or nome == ".aplicados":
                continue
            lista.append(os.path.relpath(os.path.join(raiz, nome), OVERRIDES))
    return sorted(lista)


def main() -> int:
    if not os.path.isdir(OVERRIDES):
        print("nao ha overrides")
        return 0

    VENDOR = os.path.join(RAIZ, "vendor", "eden")

    # **A lista do que tem override e' o `.aplicados`** — e nao o que
    # estiver em disco. A razao: um override que desapareceu pode ficar
    # no disco de quem tem o projecto (o `tar` nao apaga ficheiros), e
    # continuar a ser aplicado em silencio, para sempre.
    #
    # A lista e' **versionada no repositorio**, e e' o
    # `sincronizar_overrides.py` que a regenera.
    if not os.path.exists(MARCADOR):
        print("nao ha lista de overrides (%s)" % os.path.relpath(MARCADOR, RAIZ))
        return 1
    with open(MARCADOR, encoding="utf-8") as f:
        actuais = [l.strip() for l in f if l.strip()]

    n = 0
    for relativo in actuais:
        origem = os.path.join(OVERRIDES, relativo)
        destino = os.path.join(VENDOR, relativo)
        if not os.path.exists(origem):
            print("  AVISO: %s esta na lista mas nao existe" % relativo)
            continue
        os.makedirs(os.path.dirname(destino), exist_ok=True)
        shutil.copyfile(origem, destino)
        print("  override: %s" % relativo)
        n += 1

    # **Os overrides que desapareceram sao apagados do `tools/overrides/`.**
    # Sem isto, ficam la — o `tar` nao apaga ficheiros — e a proxima
    # execucao volta a aplica-los, e a repor no vendor uma versao antiga.
    for raiz, _dirs, ficheiros in os.walk(OVERRIDES):
        for nome in ficheiros:
            if nome.endswith(".md") or nome == ".aplicados":
                continue
            caminho = os.path.join(raiz, nome)
            relativo = os.path.relpath(caminho, OVERRIDES)
            if relativo not in actuais:
                os.remove(caminho)
                print("  override obsoleto removido: %s" % relativo)

    print("%d override(s) aplicado(s)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
