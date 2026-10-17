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

# --- ROMs de prueba en la máquina de src/core ---------------------------------------------------------------
# Las ROMs de tests/roms/homebrew (CPU, mapa de memoria, IRQ y temporizadores, DMA, IPC, SWI de la BIOS, periféricos)
# corren con arranque directo en la máquina de la Fase 1.2, en little-endian (gcc del contenedor) y en big-endian
# (qemu-ppc64), y deben dar OK como con melonDS (tools/melonds-run.py). Además se repiten con otros tamaños de tramo
# del planificador (la frontera para el ARM7 en otro hilo: docs/fase1.md) y se corre la demostración, que no termina:
# se comprueba que sus dos CPUs viven.
ROMS="hola cpu9 cpuarm mem irq dma ipc bios periph"
check_rom() { # orden rom log
	if grep -q "^OK " "$3"; then
		printf '%-4s OK     %s\n' "$1" "$(grep '^OK ' "$3")"
	else
		printf '%-4s FALLO  (ver %s)\n' "$1" "$3"
		tail -n 10 "$3"
		fail=1
	fi
}
check_demo() { # orden log
	frames=$(sed -n 's/^peek 0x023a0004 = //p' "$2")
	if grep -q "^peek 0x023a0000 = 0x4f4d4544" "$2" && [ -n "$frames" ] && [ $((frames)) -ge 100 ]; then
		printf '%-4s OK     demo: el ARM7 dejó su marca y contó %d cuadros\n' "$1" $((frames))
	else
		printf '%-4s FALLO  demo (ver %s)\n' "$1" "$2"
		tail -n 6 "$2"
		fail=1
	fi
}
if tools/build.sh -C src/host >build/logs/build-host-run.log 2>&1; then
	for rom in $ROMS; do
		tools/build.sh --sh "build/host-run/salsads-cpurun tests/roms/homebrew/$rom.nds" >"build/logs/rom-$rom-le.log" 2>&1
		check_rom "LE" "$rom" "build/logs/rom-$rom-le.log"
	done
	for q in 1 16 2048 20000; do
		for rom in irq ipc dma periph; do
			tools/build.sh --sh "build/host-run/salsads-cpurun --quantum $q tests/roms/homebrew/$rom.nds" >"build/logs/rom-$rom-le-q$q.log" 2>&1
			check_rom "LEq$q" "$rom" "build/logs/rom-$rom-le-q$q.log"
		done
	done
	tools/build.sh --sh "build/host-run/salsads-cpurun --frames 120 --peek 0x023A0000 --peek 0x023A0004 tests/roms/homebrew/demo.nds" >build/logs/rom-demo-le.log 2>&1
	check_demo LE build/logs/rom-demo-le.log
else
	echo "LE   FALLO  la compilación de src/host (build/logs/build-host-run.log)"
	fail=1
fi
if tools/build-ppc64.sh host >build/logs/build-ppc64-run.log 2>&1; then
	for rom in $ROMS; do
		tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun tests/roms/homebrew/$rom.nds >"build/logs/rom-$rom-be.log" 2>&1
		check_rom "BE" "$rom" "build/logs/rom-$rom-be.log"
	done
	tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun --frames 120 --peek 0x023A0000 --peek 0x023A0004 tests/roms/homebrew/demo.nds >build/logs/rom-demo-be.log 2>&1
	check_demo BE build/logs/rom-demo-be.log
else
	echo "BE   FALLO  la compilación ppc64 de src/host (build/logs/build-ppc64-run.log)"
	fail=1
fi
exit $fail
