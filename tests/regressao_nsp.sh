#!/usr/bin/env bash
# Teste de regressao: o arranque sem G610_NSP nao deve produzir
# SetTerminateResult com o resultado fatal 0x00177A02 (FS/OutOfRange),
# porque o motor auto-descobre o NSP e serve o RomFS.
set -u
BIN="${1:-./build/g610_boot}"
EXEFS="${2:-$HOME/piDroid/pokemon/exefs}"
LOG="$(mktemp)"
timeout 45 env IA_NOOB=1 G610_TRAP_LIMIT=0 G610_KEYS="${G610_KEYS:-$HOME/piDroid/prod.keys}" \
  "$BIN" "$EXEFS" >"$LOG" 2>&1
rc=$?
if grep -q "0x00177A02" "$LOG"; then
  echo "FALHOU: 0x00177A02 presente no log"
  rm -f "$LOG"
  exit 1
fi
echo "OK: sem 0x00177A02 (rc=$rc)"
rm -f "$LOG"
exit 0
