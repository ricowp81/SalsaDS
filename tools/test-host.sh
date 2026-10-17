#!/bin/sh
# Pruebas unitarias del host en los dos órdenes de bytes:
#   LE  gcc nativo del contenedor de la Mac (arm64 Linux, little-endian), con ASan/UBSan
#   BE  ppc64 big-endian (la ABI de la PS3) con qemu-user (tools/qemu-ppc64.sh)
# La DS es little-endian y la PS3 big-endian: el código que toca la memoria
# emulada se prueba en los dos. Cada corrida debe decir en qué orden fue
# («host: LE|BE») y terminar sin fallos; si no, el script falla.
#   tools/build.sh test-host
# Imágenes: tools/docker-image.sh (toolchain) y tools/docker-image-ppc64.sh.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/logs
fail=0

check() { # nombre orden_esperado log
	if grep -q "^host: $2\$" "$3" && grep -q '^total: .* 0 fallos$' "$3"; then
		printf '%-4s OK     %s\n' "$1" "$(grep '^total' "$3")"
	else
		printf '%-4s FALLO  (ver %s)\n' "$1" "$3"
		tail -n 15 "$3"
		fail=1
	fi
}

# --- little-endian -----------------------------------------------------------
tools/build.sh -C tests/unit run >build/logs/test-host-le.log 2>&1
check LE LE build/logs/test-host-le.log

# --- big-endian --------------------------------------------------------------
for img in salsads-ppc64-cross:dev salsads-qemu:dev; do
	if ! docker image inspect "$img" >/dev/null 2>&1; then
		echo "falta la imagen $img: tools/docker-image-ppc64.sh" >&2
		tools/docker-image-ppc64.sh >&2 || exit 1
		break
	fi
done
if tools/build-ppc64.sh >build/logs/build-ppc64.log 2>&1; then
	tools/qemu-ppc64.sh build/ppc64/salsads-unit >build/logs/test-host-be.log 2>&1
	check BE BE build/logs/test-host-be.log
else
	echo "BE   FALLO  la compilación ppc64 (build/logs/build-ppc64.log)"
	fail=1
fi

# --- ROMs de prueba de CPU en los intérpretes de src/cpu -----------------------
# Las ROMs de tests/roms/homebrew (hola, cpu9 y cpuarm, ARM9 y ARM7) corren con
# arranque directo en los intérpretes, en little-endian (gcc del contenedor) y en
# big-endian (qemu-ppc64), y deben dar OK como con melonDS (tools/melonds-run.py).
check_rom() { # orden rom log
	if grep -q "^OK " "$3"; then
		printf '%-4s OK     %s\n' "$1" "$(grep '^OK ' "$3")"
	else
		printf '%-4s FALLO  (ver %s)\n' "$1" "$3"
		tail -n 10 "$3"
		fail=1
	fi
}
if tools/build.sh -C src/host >build/logs/build-host-run.log 2>&1; then
	for rom in hola cpu9 cpuarm; do
		tools/build.sh --sh "build/host-run/salsads-cpurun tests/roms/homebrew/$rom.nds" >"build/logs/rom-$rom-le.log" 2>&1
		check_rom "LE" "$rom" "build/logs/rom-$rom-le.log"
	done
else
	echo "LE   FALLO  la compilación de src/host (build/logs/build-host-run.log)"
	fail=1
fi
if tools/build-ppc64.sh host >build/logs/build-ppc64-run.log 2>&1; then
	for rom in hola cpu9 cpuarm; do
		tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun tests/roms/homebrew/$rom.nds >"build/logs/rom-$rom-be.log" 2>&1
		check_rom "BE" "$rom" "build/logs/rom-$rom-be.log"
	done
else
	echo "BE   FALLO  la compilación ppc64 de src/host (build/logs/build-ppc64-run.log)"
	fail=1
fi
exit $fail
