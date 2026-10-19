#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compila las pruebas unitarias para Linux ppc64 big-endian (la ABI de la
# PS3) en build/ppc64, estáticas, para correrlas con qemu
# (tools/qemu-ppc64.sh). Imágenes: tools/docker-image-ppc64.sh.
#   tools/build-ppc64.sh        las pruebas unitarias (build/ppc64/salsads-unit)
#   tools/build-ppc64.sh host   el corredor de ROMs y el banco de los intérpretes de
#                               CPU (src/host) en build/ppc64-run
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSADS_PPC64_IMAGE:-salsads-ppc64-cross:dev}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "No existe la imagen $IMAGE: correr tools/docker-image-ppc64.sh" >&2
	exit 1
fi

# El gcc emulado como amd64 a veces muere con "internal compiler error:
# Segmentation fault" y al repetir compila bien: se reintenta hasta tres
# veces (make retoma donde quedó). Sin --platform: con la imagen local amd64,
# Docker (colima) dice que no la encuentra; sin la opción la usa y solo avisa.
mkhost() {
	docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ROOT:/src" -w /src "$IMAGE" \
		make --no-print-directory -C src/host CC=powerpc64-linux-gnu-gcc OUT=/src/build/ppc64-run \
		CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -mcpu=cell -mno-relative-jumptables -I/src/src -static ${SALSADS_PPC64_CFLAGS:-}"
}
if [ "${1:-}" = "host" ]; then
	for try in 1 2; do
		mkhost && exit 0
		echo "reintento $try" >&2
	done
	mkhost
	exit $?
fi

mk1() {
	docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ROOT:/src" -w /src "$IMAGE" \
		make --no-print-directory -C tests/unit CC=powerpc64-linux-gnu-gcc SAN= OUT=/src/build/ppc64 \
		EXTRA_CFLAGS="-mcpu=cell -mno-relative-jumptables ${SALSADS_PPC64_CFLAGS:-}" EXTRA_LDFLAGS=-static all
}
for try in 1 2; do
	mk1 && exit 0
	echo "reintento $try" >&2
done
mk1
