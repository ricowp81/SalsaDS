#!/bin/sh
# Construye la imagen salsads-nds (BlocksDS fijado por digest, docker/nds.Dockerfile).
set -eu
cd "$(dirname "$0")/.."
IMAGE="${SALSADS_NDS_IMAGE:-salsads-nds:dev}"
mkdir -p build/logs
LOG=build/logs/docker-image-nds.log
echo "Construyendo $IMAGE (log en $LOG)"
docker build --progress=plain -f docker/nds.Dockerfile -t "$IMAGE" "$@" . >"$LOG" 2>&1 || {
	echo "Falló la construcción de $IMAGE. Últimas líneas:" >&2
	tail -n 30 "$LOG" >&2
	exit 1
}
echo "Listo: $IMAGE"
