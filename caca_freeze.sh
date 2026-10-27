#!/bin/bash
# Caça ao congelamento: N corridas em paralelo, cada uma com o seu `cd`.
cd /home/emuos/EdenMaliG610
export XDG_RUNTIME_DIR=/run/emuos WAYLAND_DISPLAY=wayland-1
export G610_TRAP_LIMIT=0 IA_THREAD_TRACE=1
N="${1:-4}"
T="${2:-300}"
for i in $(seq 1 "$N"); do
  ( timeout "$T" ./build/g610_boot /home/emuos/piDroid/pokemon/exefs/ > "/tmp/runP$i.log" 2>&1
    echo "P$i EXIT=$?" ) &
done
wait
echo TODAS_FIM
