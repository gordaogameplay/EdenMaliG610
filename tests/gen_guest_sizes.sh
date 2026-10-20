#!/usr/bin/env bash
# Gera tests/guest_sizes.h com os tamanhos das rotinas do guest.
#
# Calcula-os a partir do objecto, lendo os simbolos exported pelo assembler.
# Sem isto o teste teria de subtrair enderecos de funcoes em C++, o que nao e
# uma expressao constante.
set -eu

OBJ="$1"
OUT="$2"

if command -v aarch64-linux-gnu-nm >/dev/null 2>&1; then
    NM=aarch64-linux-gnu-nm
else
    NM=nm
fi

# $NM -S imprime: endereco tamanho tipo nome
sizes=$($NM -S "$OBJ" | awk '$3=="T"||$4=="T"{print $2, $4}')

emit() {
    local sym="$1" out="$2"
    local sz
    sz=$(echo "$sizes" | awk -v s="$sym" '$2==s{print $1}')
    echo "constexpr std::size_t $out = 0x${sz:-0};"
}

{
    echo "// Gerado por gen_guest_sizes.sh — nao editar a mao."
    echo "#pragma once"
    echo "#include <cstddef>"
    emit guest_entry      kGuestTextSize
    emit guest_fault      kGuestTextFaultSize
    emit guest_one_svc    kGuestTextOneSize
} > "$OUT"

cat "$OUT"
