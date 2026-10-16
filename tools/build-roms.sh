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
run() {
	docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"
}
if [ "${1:-}" = "--sh" ]; then
	shift
	run sh -c "$*"
else
	run make -C tests/roms "$@"
fi
