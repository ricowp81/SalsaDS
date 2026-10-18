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
ROMS="hola cpu9 cpuarm mem irq dma ipc bios periph tablas gx"
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
# La ROM save (chip de guardado por AUXSPI) con un archivo de cada tamaño: el tipo que detecta tiene que ser el
# del tamaño (1 = EEPROM de 0,5 KB, 2 = EEPROM de 16 bits, 3 = FRAM, 4 = Flash) y todo debe dar OK.
SAVE_SIZES="512:1 8192:2 32768:3 65536:2 262144:4 524288:4 1048576:4"
check_save() { # orden tamaño tipo log
	t=$(printf '0x%08x' "$3")
	s=$(printf '0x%08x' "$2")
	if grep -q "^OK save" "$4" && grep -q "^peek 0x02380034 = $t\$" "$4" && grep -q "^peek 0x02380038 = $s\$" "$4"; then
		printf '%-4s OK     save %s bytes: tipo %s\n' "$1" "$2" "$3"
	else
		printf '%-4s FALLO  save %s bytes (ver %s)\n' "$1" "$2" "$4"
		tail -n 6 "$4"
		fail=1
	fi
}
# La ROM carga (las dos CPUs ocupadas sin parar) repite iteraciones que se comparan con valores calculados en el host:
# ninguna iteración debe dar otra cosa y las dos CPUs tienen que haber avanzado.
check_carga() { # orden log
	if grep -q "^peek 0x023a0000 = 0x47524143" "$2" && grep -q "^peek 0x023a0008 = 0x00000000" "$2" &&
		grep -q "^peek 0x023a0010 = 0x00000000" "$2" && [ $((0x$(sed -n 's/^peek 0x023a0004 = 0x//p' "$2"))) -ge 100 ] &&
		[ $((0x$(sed -n 's/^peek 0x023a000c = 0x//p' "$2"))) -ge 50 ]; then
		printf '%-4s OK     carga: %d iteraciones del ARM9 y %d del ARM7, sin errores\n' "$1" \
			$((0x$(sed -n 's/^peek 0x023a0004 = 0x//p' "$2"))) $((0x$(sed -n 's/^peek 0x023a000c = 0x//p' "$2")))
	else
		printf '%-4s FALLO  carga (ver %s)\n' "$1" "$2"
		tail -n 9 "$2"
		fail=1
	fi
}
CARGA_ARGS="--frames 120 --peek 0x023A0000 --peek 0x023A0004 --peek 0x023A0008 --peek 0x023A000C --peek 0x023A0010"
# Un cuadro ocioso (las dos CPUs en HALT): sin pasos finos, el planificador salta de un evento al siguiente; el ARM9 y
# el ARM7 ejecutan decenas de instrucciones por cuadro y el planificador corre menos de 20 tramos por cuadro.
check_ocio() { # orden log
	i9=$(sed -n 's/.* ARM9 \([0-9]*\) instr\..*/\1/p' "$2" | head -1)
	sl=$(sed -n 's/^planificador: \([0-9]*\)\..*/\1/p' "$2" | head -1)
	if [ -n "$i9" ] && [ "$i9" -lt 200000 ] && [ -n "$sl" ] && [ "$sl" -lt 20 ]; then
		printf '%-4s OK     ocio: ARM9 %s instr. en 600 cuadros, %s tramos por cuadro\n' "$1" "$i9" "$sl"
	else
		printf '%-4s FALLO  ocio (ver %s)\n' "$1" "$2"
		tail -n 4 "$2"
		fail=1
	fi
}
# --- Motores 2D (Fase 2.1) ----------------------------------------------------------------------------------------
# Las ROMs gfx* de tests/roms montan escenas conocidas en los motores 2D. Cada una corre 6 cuadros con arranque directo y
# se comprueba, según la ROM: la lista de píxeles que dejó (EXPECT), que los dos motores den la misma imagen (A==B) y,
# siempre, que el compositor por líneas (el código del SPE, aquí escalar) dé los mismos píxeles que el renderizador de
# referencia (REF). gfxrand corre varias semillas (tools/rom-seed.py las pone en una copia de la ROM). La comparación con
# melonDS es a mano (tools/gfx-compare.py): docs/fase2.md.
GFX_A="gfxtext gfxaffine gfxaffine1 gfxext16 gfxbmp8 gfxbmpd gfxobj gfxobjaff gfxobjx gfxwin gfxblend0 gfxblend1 gfxblend2 gfxblend3 gfxblend4 gfxblend5"
GFX_E="gfxraster gfxwinx gfxmode0 gfxmode2 gfxmode3"
GFX_R="gfxfx gfxfx1 gfxmos0 gfxmos1 gfxmos2 gfxmos3 gfxmos4 gfxmos5 gfxmos6 gfxmos7 gfxmos8 gfxmos9 gfxmos10 gfxmos11 gfxextpal gfxextpal1 gfxextpal2"
GFX_SEEDS="0 1 2 3 4 5 6 7 8 9 10 11"
GFX_OPT_A="--expect --same-ab --vs-ref"
GFX_OPT_E="--expect --vs-ref"
GFX_OPT_R="--vs-ref"
check_gfx() { # orden rom log patrones...
	o=$1 r=$2 l=$3
	shift 3
	ok=1
	for pat in "$@"; do
		grep -q "$pat" "$l" || ok=0
	done
	if [ $ok = 1 ]; then
		printf '%-4s OK     gfx %s: %s\n' "$o" "$r" "$(grep -E '^(EXPECT|A==B|REF)' "$l" | tr '\n' ' ')"
	else
		printf '%-4s FALLO  gfx %s (ver %s)\n' "$o" "$r" "$l"
		grep -E '^(EXPECT|A==B|REF)|no puedo' "$l" | head -n 6
		fail=1
	fi
}
# corre las ROMs de gráficos con `runner` (un comando que recibe el ejecutable, las opciones y la ROM) y las revisa
gfx_group() { # orden etiqueta runner... 
	o=$1 lab=$2
	shift 2
	for r in $GFX_A; do
		"$@" --frames 14 $GFX_OPT_A "tests/roms/homebrew/$r.nds" >"build/logs/gfx-$r-$lab.log" 2>&1
		check_gfx "$o" "$r" "build/logs/gfx-$r-$lab.log" '^EXPECT: .* 0 fallos$' '^A==B: OK$' '^REF: IGUAL$'
	done
	for r in $GFX_E; do
		fr=14
		[ "$r" = gfxraster ] && fr=6 # sus cambios por línea paran a los 6 cuadros: después la imagen ya no es la de la lista
		"$@" --frames $fr $GFX_OPT_E "tests/roms/homebrew/$r.nds" >"build/logs/gfx-$r-$lab.log" 2>&1
		check_gfx "$o" "$r" "build/logs/gfx-$r-$lab.log" '^EXPECT: .* 0 fallos$' '^REF: IGUAL$'
	done
	for r in $GFX_R; do
		"$@" --frames 14 $GFX_OPT_R "tests/roms/homebrew/$r.nds" >"build/logs/gfx-$r-$lab.log" 2>&1
		check_gfx "$o" "$r" "build/logs/gfx-$r-$lab.log" '^REF: IGUAL$'
	done
	for sd in $GFX_SEEDS; do
		"$@" --frames 14 $GFX_OPT_R "build/logs/gfxrand-$sd.nds" >"build/logs/gfx-rand$sd-$lab.log" 2>&1
		check_gfx "$o" "rand$sd" "build/logs/gfx-rand$sd-$lab.log" '^REF: IGUAL$'
	done
}
for sd in $GFX_SEEDS; do
	python3 tools/rom-seed.py tests/roms/homebrew/gfxrand.nds "build/logs/gfxrand-$sd.nds" "$sd" >/dev/null
done
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
	for sz in $SAVE_SIZES; do
		tools/build.sh --sh "build/host-run/salsads-cpurun --save-size ${sz%%:*} --peek 0x02380034 --peek 0x02380038 tests/roms/homebrew/save.nds" >"build/logs/rom-save-${sz%%:*}-le.log" 2>&1
		check_save LE "${sz%%:*}" "${sz##*:}" "build/logs/rom-save-${sz%%:*}-le.log"
	done
	tools/build.sh --sh "build/host-run/salsads-cpurun $CARGA_ARGS tests/roms/homebrew/carga.nds" >build/logs/rom-carga-le.log 2>&1
	check_carga LE build/logs/rom-carga-le.log
	tools/build.sh --sh "build/host-run/salsads-cpurun --frames 600 --stats tests/roms/homebrew/ocio.nds" >build/logs/rom-ocio-le.log 2>&1
	check_ocio LE build/logs/rom-ocio-le.log
	gfx_group LE le tools/build.sh --sh build/host-run/salsads-cpurun
	# las ROMs otra vez con ASan y UBSan (errores de memoria en el mapa, la VRAM por bancos o la E/S)
	if tools/build.sh --sh "gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Isrc src/host/cpurun.c src/core/*.c src/bios/*.c src/cpu/*.c src/gpu2d/*.c -o build/host-run/cpurun-san -lm" >build/logs/build-cpurun-san.log 2>&1; then
		for rom in $ROMS; do
			tools/build.sh --sh "build/host-run/cpurun-san tests/roms/homebrew/$rom.nds" >"build/logs/rom-$rom-san.log" 2>&1
			check_rom "SAN" "$rom" "build/logs/rom-$rom-san.log"
		done
		for sz in $SAVE_SIZES; do
			tools/build.sh --sh "build/host-run/cpurun-san --save-size ${sz%%:*} --peek 0x02380034 --peek 0x02380038 tests/roms/homebrew/save.nds" >"build/logs/rom-save-${sz%%:*}-san.log" 2>&1
			check_save SAN "${sz%%:*}" "${sz##*:}" "build/logs/rom-save-${sz%%:*}-san.log"
		done
		tools/build.sh --sh "build/host-run/cpurun-san $CARGA_ARGS tests/roms/homebrew/carga.nds" >build/logs/rom-carga-san.log 2>&1
		check_carga SAN build/logs/rom-carga-san.log
		tools/build.sh --sh "build/host-run/cpurun-san --frames 200 --touch 100,50 --keys 0x0510 --peek 0x023A0000 --peek 0x023A0004 tests/roms/homebrew/demo.nds" >build/logs/rom-demo-san.log 2>&1
		check_demo SAN build/logs/rom-demo-san.log
		gfx_group SAN san tools/build.sh --sh build/host-run/cpurun-san
	else
		echo "SAN  FALLO  la compilación con sanitizadores (build/logs/build-cpurun-san.log)"
		fail=1
	fi
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
	for sz in $SAVE_SIZES; do
		tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun --save-size "${sz%%:*}" --peek 0x02380034 --peek 0x02380038 tests/roms/homebrew/save.nds >"build/logs/rom-save-${sz%%:*}-be.log" 2>&1
		check_save BE "${sz%%:*}" "${sz##*:}" "build/logs/rom-save-${sz%%:*}-be.log"
	done
	tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun $CARGA_ARGS tests/roms/homebrew/carga.nds >build/logs/rom-carga-be.log 2>&1
	check_carga BE build/logs/rom-carga-be.log
	tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun --frames 600 --stats tests/roms/homebrew/ocio.nds >build/logs/rom-ocio-be.log 2>&1
	check_ocio BE build/logs/rom-ocio-be.log
	gfx_group BE be tools/qemu-ppc64.sh build/ppc64-run/salsads-cpurun
else
	echo "BE   FALLO  la compilación ppc64 de src/host (build/logs/build-ppc64-run.log)"
	fail=1
fi
exit $fail
