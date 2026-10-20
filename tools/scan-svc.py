#!/usr/bin/env python3
"""Confirma se o executavel principal de um titulo de Switch tem SVC.

O patcher reportou 0 SVC no `main` do Pokemon Shield. Ou o programa principal
nao chama o sistema (plausivel: quem chama SVC e o SDK), ou o varrimento
falhou. Esta script responde a isso procurando os encodings brutos, sem
passar pelo motor.
"""
import struct
import sys
from collections import Counter

SVC_SIG = 0xD4000000


def classify(word):
    # mesma logica do motor
    if (word & 0x1F) == 1 and ((word >> 21) & 0x7FF) == 0x6A0:
        return "SVC"
    if (word >> 20) == 0xD53:
        return "MRS"
    if (word >> 20) == 0xD51:
        return "MSR"
    if ((word >> 21) & 0x7FF) == 0x6A8:
        return "HINT"
    if ((word >> 23) & 0x7F) == 0x10:
        return "EXCL"
    return None


def main(path, limit=None):
    print(f"a varrer {path}")
    data = open(path, "rb").read()
    if limit:
        data = data[:limit]
    print(f"  {len(data)} bytes, {len(data)//4} palavras")

    counts = Counter()
    svc_numbers = Counter()
    examples = {}

    for i in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, i)[0]
        k = classify(w)
        if k:
            counts[k] += 1
            if k == "SVC":
                n = (w >> 5) & 0xFFFF
                svc_numbers[n] += 1
                if n not in examples:
                    examples[n] = (i, w)

    print(f"\n  contagens:")
    for k, v in counts.most_common():
        print(f"    {k:<6} {v}")

    print(f"\n  numeros de SVC mais frequentes:")
    for n, c in svc_numbers.most_common(10):
        off, w = examples[n]
        print(f"    svc #{n:<5} x{c:<6} em 0x{off:X}  (0x{w:08X})")

    # SVC real
    raw = sum(1 for i in range(0, len(data) - 3, 4)
              if (struct.unpack_from("<I", data, i)[0] & 0xFFE00000) == SVC_SIG)
    print(f"\n  palavras com assinatura SVC bruta: {raw}")
    return 0


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else None)
