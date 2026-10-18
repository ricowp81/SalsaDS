#!/bin/sh
# Construye las ROMs de prueba de la DS (tests/roms) con BlocksDS dentro de
# la imagen salsads-nds. Salida en build/roms/*.nds.
#   tools/build-roms.sh            construye todas
#   tools/build-roms.sh install    además las copia a tests/roms/homebrew/ y
#                                  actualiza SHA256SUMS (se commitean ahí)
#   tools/build-roms.sh --sh '...' un comando cualquiera en la imagen
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSADS_NDS_IMAGE:-salsads-nds:dev}"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "No existe la imagen $IMAGE: la construyo (tools/docker-image-nds.sh)" >&2
	"$ROOT/tools/docker-image-nds.sh" >&2
fi
# versión que queda grabada en el log de la SD: commit y, si hay cambios sin commitear, "+"
REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo desconocida)"
if [ -n "$(git -C "$ROOT" status --porcelain -- tests/roms/src tests/roms/Makefile 2>/dev/null)" ]; then REV="$REV+"; fi
run() {
	docker run --rm -i --user "$(id -u):$(id -g)" -e SDS_GITREV="$REV" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"
}
if [ "${1:-}" = "--sh" ]; then
	shift
	run sh -c "$*"
else
	run make -C tests/roms "$@"
fi
