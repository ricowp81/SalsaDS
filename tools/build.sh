#!/bin/sh
# Ejecuta make (o cualquier comando con --sh) dentro de la imagen del toolchain.
#   tools/build.sh                 make all (programas de la Fase 0)
#   tools/build.sh -C tests/phase0/empty pkg
#   tools/build.sh --sh 'ppu-gcc --version'
#   tools/build.sh test-host       pruebas unitarias en LE y en BE (tools/test-host.sh)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSADS_IMAGE:-salsads-toolchain:dev}"

# Las pruebas del host usan sus propias imágenes (little-endian aquí, ppc64
# big-endian con qemu): las maneja su script.
if [ "${1:-}" = "test-host" ]; then
	shift
	exec "$ROOT/tools/test-host.sh" "$@"
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "No existe la imagen $IMAGE: la construyo (tools/docker-image.sh)" >&2
	"$ROOT/tools/docker-image.sh" >&2
fi

TTY=""
[ -t 0 ] && [ -t 1 ] && TTY="-t"

# Versión del árbol para los programas. Se calcula aquí: en el contenedor,
# el .git de un worktree apunta a una ruta del host que no está montada.
VERSION="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || true)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"

run() {
	docker run --rm -i $TTY --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-e SALSADS_VERSION="$VERSION" -e SALSADS_COMMIT="$COMMIT" \
		-v "$ROOT:/src" -w /src "$IMAGE" "$@"
}

if [ "${1:-}" = "--sh" ]; then
	shift
	run sh -c "$*"
else
	run make "$@"
fi
