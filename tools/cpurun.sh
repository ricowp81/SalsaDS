#!/bin/sh
# Corre una ROM de prueba en los intérpretes de CPU de SalsaDS, nativo en la Mac
# (cc), sin Docker. Compila src/host en build/mac y ejecuta salsads-cpurun.
#   tools/cpurun.sh [opciones de salsads-cpurun] ROM.nds
#   tools/cpurun.sh --stats --arm7 build/roms/cpuarm.nds
#   tools/cpurun.sh bench [segundos]        armbench (instrucciones por segundo)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/mac"
CC="${CC:-cc}"
make -s -C "$ROOT/src/host" OUT="$OUT" CC="$CC" CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -I$ROOT/src" >&2
if [ "${1:-}" = "bench" ]; then
	shift
	exec "$OUT/armbench" "$@"
fi
exec "$OUT/salsads-cpurun" "$@"
