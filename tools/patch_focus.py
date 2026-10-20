#!/usr/bin/env python3
# Patch do emuos-display: `game` passa a dar FOCO a janela do emulador.
#
# PORQUÊ
# ------
# `place()` movia a janela para a saída correcta e activava o ecrã inteiro, mas
# nunca lhe dava foco. E `focus-primary` — o único comando que focava alguma
# coisa — focava a janela do MENU (`menu_window`), não a do jogo.
#
# Resultado: o emulador desenhava-se no sítio certo e o teclado do compositor
# ia para o menu. O gamepad, que o wlroots envia para a janela focada do seat
# focado, também ia para o menu. Ou seja: o jogo corria à vista de todos e não
# recebia um único botão.
#
# Isto não é teórico — é a razão pela qual o controlo "não funciona" num
# emulador que está claramente em foco visual.
#
# REVERSIBILIDADE
# ---------------
# O backup está em /usr/local/bin/emuos-display.bak-<data>. Repor:
#
#     sudo cp /usr/local/bin/emuos-display.bak-20261018 /usr/local/bin/emuos-display
#
# O patch só ADICIONA linhas dentro de `place()`. Nada é removido.

import re
import sys

PATH = "/usr/local/bin/emuos-display"

with open(PATH, encoding="utf-8") as f:
    src = f.read()

if "FOCO: a janela do emulador" in src:
    print("ja esta' patchado")
    sys.exit(0)

antigo = '''place() {
    local id="$1" out="$2"
    [ -n "$id" ] || return 1
    "$SWAYMSG" "[con_id=$id] move container to output $out" >/dev/null 2>&1 || return 1
    "$SWAYMSG" "[con_id=$id] fullscreen enable"              >/dev/null 2>&1
    return 0
}'''

novo = '''place() {
    local id="$1" out="$2"
    [ -n "$id" ] || return 1
    "$SWAYMSG" "[con_id=$id] move container to output $out" >/dev/null 2>&1 || return 1
    "$SWAYMSG" "[con_id=$id] fullscreen enable"              >/dev/null 2>&1

    # FOCO: a janela do emulador tem de ficar focada, ou o teclado e o gamepad
    # do wlroots continuam a ir para a janela do menu. Uma janela em ecrã
    # inteiro que nao esta' focada parece correcta e nao responde a nada --
    # foi o que aconteceu com o Pokemon Shield (medido 2026-10-18).
    #
    # A ordem importa: focar ANTES de maximizar mandava o compositor activar
    # o ecrã inteiro de uma janela errada quando havia varias. Aqui e' depois.
    "$SWAYMSG" "[con_id=$id] focus" >/dev/null 2>&1
    return 0
}'''

if antigo not in src:
    print("ERRO: place() nao tem a forma esperada -- patch cancelado")
    print("      (o script foi entretanto editado; ver o backup)")
    sys.exit(1)

src = src.replace(antigo, novo, 1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(src)

print("patch aplicado: place() foca a janela")