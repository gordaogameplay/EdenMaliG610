#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 gordaogameplay
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lê e (opcionalmente) escreve registos do CRU do RK3588 via /dev/mem.

O CRU (Clock & Reset Unit) fica em 0xfd7c0000. O clock da GPU é
CLK_GPU_SRC, um COMPOSITE em RK3588_CLKSEL_CON(158):

    bits  0-4   divisor  (1..31)
    bits 12-14  mux      (0=gpll 1=cpll 2=aupll 3=npll 4=spll)

Objectivo: verificar se é possível reprogramar o spll (que alimenta só
clocks desligados) para uma frequência que, dividida pelo divisor inteiro,
dê exactamente 1 GHz — e assim evitar o PVTPLL.
"""
import os
import struct
import subprocess
import sys

CRU = 0xFD7C0000
CLKSEL = lambda n: CRU + n * 4
DEFMASK = 4  # RK3588_CLKSEL_CON stride = 4

GPU_SRC = 158          # mux + div da GPU
SPLL_CON = 0x200       # guess: precisa de confirmação; ver abaixo
PLL_CON = {            # base dos registos de PLL no CRU do RK3588
    "gpll": 0xC00, "cpll": 0xC20, "npll": 0xC40,
    "spll": 0xC60, "aupll": 0xC80,
}


def rd(addr):
    """Lê um registo de 32 bits do CRU."""
    out = subprocess.run(
        ["dd", f"if=/dev/mem", "bs=1", "skip=%d" % addr,
         "count=4", "status=none"],
        capture_output=True)
    if len(out.stdout) < 4:
        return None
    return struct.unpack("<I", out.stdout)[0]


def decode_gpu_src(val):
    div = val & 0x1F
    mux = (val >> 12) & 0x7
    return div, mux


PARENTS = ["gpll", "cpll", "aupll", "npll", "spll"]


def main():
    print("=== RK3588 CRU — análise do clock da GPU ===\n")

    v = rd(CLKSEL(GPU_SRC))
    if v is None:
        print("não foi possível ler o CRU (permissões em /dev/mem?)")
        return 1

    div, mux = decode_gpu_src(v)
    print(f"CLKSEL_CON({GPU_SRC}) @ 0x{CLSEL(GPU_SRC):08X} = 0x{v:08X}")
    print(f"  divisor = {div}   (freq = pai / {div})")
    print(f"  mux     = {mux} -> {PARENTS[mux] if mux < len(PARENTS) else '?'}")

    print()
    print("=== Porquê 1 GHz é inatingível ===")
    rates = {"gpll": 1608_000_000, "cpll": 1500_000_000,
             "aupll": 786_431_998, "npll": 850_000_000, "spll": 702_000_000}
    target = 1_000_000_000
    print(f"{'pai':<8}{'taxa':>14}{'div p/1GHz':>12}{'resultado':>14}")
    for name, rate in rates.items():
        # divisor inteiro mais próximo que fique <= 1 GHz
        d = max(1, rate // target)
        print(f"{name:<8}{rate/1e6:>11.1f}MHz{d:>12}{rate/d/1e6:>11.1f}MHz")

    print()
    print("=== O spll é o candidato ===")
    print("  spll alimenta: clk_dsihost0/1, clk_isp0_core, clk_fisheye*,")
    print("                 aclk_jpeg_decoder — todos com enable_count=0.")
    print("  Se o spll for reprogramado para 2000 MHz, então 2000/2 = 1000 MHz,")
    print("  e 1000 MHz passa a ser sintetizável sem PVTPLL.")

    print()
    print("=== Registos PLL (confirmação necessária antes de escrever) ===")
    for name, base in PLL_CON.items():
        a = CRU + base
        r = rd(a)
        print(f"  {name:<6} 0x{a:08X} = 0x{r:08X}" if r is not None
              else f"  {name:<6} 0x{a:08X} = (ilegível)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
