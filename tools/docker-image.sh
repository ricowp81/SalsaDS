#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Construye la imagen del toolchain de SalsaDS (salsads-toolchain) a partir de
# la de Salsa64 (docker/Dockerfile): tarda segundos si la base ya existe.
#   tools/docker-image.sh [args extra para docker build]
# Otra base: SALSADS_BASE=otra-imagen:tag tools/docker-image.sh
set -eu
cd "$(dirname "$0")/.."
IMAGE="${SALSADS_IMAGE:-salsads-toolchain:dev}"
BASE="${SALSADS_BASE:-salsa64-toolchain:dev}"
if ! docker image inspect "$BASE" >/dev/null 2>&1; then
	echo "No existe la imagen base $BASE (ps3toolchain + PSL1GHT + cgcomp)." >&2
	echo "Construirla con tools/docker-image.sh del repositorio de Salsa64" >&2
	echo "(github.com/ricowp81/Salsa64; tarda más de una hora) o fijar SALSADS_BASE." >&2
	exit 1
fi
mkdir -p build/logs
LOG=build/logs/docker-image.log
echo "Construyendo $IMAGE desde $BASE (log en $LOG)"
docker build --progress=plain -f docker/Dockerfile --build-arg "BASE=$BASE" -t "$IMAGE" "$@" . >"$LOG" 2>&1 || {
	echo "Falló la construcción de $IMAGE. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
echo "Listo: $IMAGE"
