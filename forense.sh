#!/bin/bash
# Forense do congelamento (runL família): extrai do log congelado
# o estado do protocolo no momento exato da paragem.
F="$1"
echo "== última atividade do log =="
stat -c "mtime=%y" "$F"
echo "== LPAUSA (todas) =="
grep -a "LPAUSA" "$F"
echo "== últimos LACORDEI =="
grep -a "LACORDEI" "$F" | tail -4
echo "== últimos DLOCK =="
grep -a "DLOCK" "$F" | tail -6
echo "== PARK/ACORDEI últimos 8 =="
grep -a "PARK\|ACORDEI" "$F" | tail -8
echo "== 24 linhas finais (sem ruído) =="
tail -60 "$F" | grep -a "svc #\|thrd\|arb\|gaten\|ipc \|#33\|kickoff" | tail -24
echo "== fimusters =="
fim=$(grep -a -c "boot\] fim" "$F")
echo "boot_fim=$fim"
