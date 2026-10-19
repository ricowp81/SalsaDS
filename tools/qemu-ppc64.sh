#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Corre un programa de build/ppc64 con qemu-ppc64. El directorio de trabajo
# es la raíz del repo; QEMU_TIMEOUT (segundos, 600 por omisión) corta las
# corridas colgadas. stderr se junta con stdout dentro del contenedor (Docker
# no conserva el orden entre los dos).
#   tools/qemu-ppc64.sh build/ppc64/salsads-unit
#
# CPU power9 (SALSADS_QEMU_CPU cambia el modelo): con -cpu 970 (el más
# parecido al PPE) qemu 7.2 da resultados erróneos y power5+ se cuelga; con
# power7/power8 las funciones matemáticas de la glibc estática mueren con
# SIGILL (usan mffscrn, Power ISA 3.0). power9 acepta instrucciones que el
# PPE no tiene: el código que se emita a mano se limita a Power ISA 2.02 y el
# C se compila con -mcpu=cell. Detalle en docs/cell-desde-salsa64.md §6.2.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSADS_QEMU_IMAGE:-salsads-qemu:dev}"
# SALSADS_ROMS: carpeta de respaldos propios (fuera del repo) que se monta de solo lectura en /roms.
ROMS_MOUNT=""
[ -n "${SALSADS_ROMS:-}" ] && ROMS_MOUNT="-v ${SALSADS_ROMS}:/roms:ro"
exec docker run --rm -i --user "$(id -u):$(id -g)" $ROMS_MOUNT -e AREC_TRACE -e AREC_DUMP -e AREC_SEED -e AREC_PROF_SLOW_FROM -e AREC_OPTS -e AREC_V4 -e AREC_CUT -e AREC_THUMB -e AREC_FAST -v "$ROOT:/src" -w /src "$IMAGE" \
	sh -c 'cpu=$1; shift; exec timeout "$0" qemu-ppc64-static -cpu "$cpu" "$@" 2>&1' \
	"${QEMU_TIMEOUT:-600}" "${SALSADS_QEMU_CPU:-power9}" "$@"
