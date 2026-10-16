# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | hecho (2026-10-16) |
| 0.3 Microbenchmarks del PPE | programa listo y verificado en el host y en RPCS3; **faltan los números de la PS3 real** |
| 0.4 Rasterizador 3D en un SPE | pendiente |
| 0.5 Composición 2D en un SPE | pendiente |
| 0.6 Salida de dos pantallas | pendiente |
| 0.7 Prueba de JIT ARM → PPC | pendiente |

## 0.1 Esqueleto

Criterio: `tools/build.sh` compila un programa vacío de PS3 a `.pkg` y
`tools/build.sh test-host` corre una prueba en LE y en BE.

- `tools/build.sh -C tests/phase0/empty all pkg` → `build/salsads-empty/salsads-empty.pkg`;
  el `.elf` corre en RPCS3 y escribe `RESULTADO empty: OK`
  (`tools/rpcs3-run.py empty`).
- `tools/build.sh test-host` → `LE OK` y `BE OK` (12 comprobaciones cada una).

## 0.2 ROMs de prueba propias

Criterio: la ROM «hola» y una ROM de prueba de CPU se construyen desde el
repositorio y corren en el emulador de referencia. **Cumplido.**

- `tools/build-roms.sh` → `build/roms/hola.nds` y `cpu9.nds` (reproducibles:
  dos construcciones dan los mismos hashes; `tests/roms/homebrew/SHA256SUMS`).
- `tools/melonds-run.py build/roms/hola.nds` → `OK hola.nds: 5 comprobaciones`;
  `cpu9.nds` → `OK … 65187 comprobaciones en 9 grupos`. Control negativo: con
  una expectativa errónea del propio test la ROM dio `FALLO 168 de 65411` y
  el script lo reportó (código 1).
- El volcado de la VRAM (`--vram`) confirma que el texto y la franja de
  veredicto se dibujan en la pantalla de arriba por el modo de visualización
  directa. Detalles del marco de pruebas en `tests/roms/README.md`.

### Toolchain ARM y licencias (verificadas)

BlocksDS v1.24.0 (imagen oficial `skylyrac/blocksds:slim-latest`, fijada por
digest `sha256:779f6193…01a51` en `docker/nds.Dockerfile`; arm64 nativa),
`arm-none-eabi-gcc` 16.2.0 del toolchain Wonderful. Los textos de licencia se
leyeron de los archivos de la propia imagen el 2026-10-16:

| Componente | Licencia | Dentro de la ROM |
|---|---|---|
| libnds (`libs/libnds/licenses/LICENSE`) | zlib | sí (y FatFs, 1-cláusula BSD, sin aviso en binarios) |
| crt0 (`sys/crts/COPYING.zlib`) | zlib; los scripts `.ld`/`.mem`, MPL-2.0 | el crt0 sí; los scripts no |
| núcleo ARM7 por omisión (`sys/default_arm7`) | zlib, pero enlaza DSWiFi (MIT), Maxmod (ISC) y Mbed TLS (Apache-2.0/GPL-2.0+) | **no se usa**: las ROMs llevan un ARM7 propio mínimo (`tests/roms/src/arm7min`) |
| picolibc (libc de la toolchain) | MIT/BSD y otras permisivas por archivo (según la guía de licencias de BlocksDS; el `COPYING.picolibc` no viene en la imagen) | sí, 64 archivos objeto en `hola`; **falta copiar su aviso íntegro antes de redistribuir** (ver `tests/roms/homebrew/AVISOS.md`) |
| libgcc | GPL-3.0 con excepción de runtime | sí |
| ndstool (arma el `.nds`) | GPL-3.0 y MIT | herramienta, no entra en la ROM |

Fuente de la guía oficial: blocksds.skylyrac.net/docs/guides/licenses/
(consultada el 2026-10-16). El ícono de la cabecera es propio (el de BlocksDS
es un logotipo CC BY-ND). Registro en `docs/THIRD_PARTY.md` y avisos en
`tests/roms/homebrew/AVISOS.md`.

### Suites libres de CPU y hardware de la DS

Resultado de una búsqueda web del 2026-10-16 (no probé yo las suites):

| Suite | Licencia | Cubre | Veredicto |
|---|---|---|---|
| gba-suite (jsmolka) | MIT | ARM/Thumb ARM7TDMI (ARMv4T), memoria, PPU de GBA; se construye con FASMARM, sin binarios | Licencia clara. Útil como modelo para un ARM7 (ARMv4T) pero no corre tal cual en DS |
| RockWrestler (RockPolish) | **sin licencia visible** | ARMv5 (CLZ, saturación, BLX, multiplicaciones), IPC, división/raíz por hardware, WRAMCNT/VRAMCNT/TCM; resultado en pantalla | **No copiar ni redistribuir.** Se puede usar localmente para comparar. Pendiente: pedir licencia al autor |
| armwrestler-gba-fixed (destoer) | sin licencia visible | ARM/Thumb de GBA | No copiar |
| ARMWrestler original, ndstest, Normmatt, nocash, DS-Test | no localizadas / no verificadas | — | No usar hasta verificar |

No hay una suite de ARMv5 y hardware de DS con licencia clara confirmada: el
plan es **escribir las propias** (`cpu9` es la primera; faltan ARM7/ARMv4T,
DMA, timers, IPC, VRAM por bancos, 3D). La ROM de `cpu9` no copia nada de las
suites de terceros.

### Emulador de referencia

melonDS 1.1 (2025-11-18, GPL-3.0), zip oficial `melonDS-1.1-macOS-universal.zip`
(sha256 `79843a5e…18d`, coincide con el que publica GitHub; 24 MB), instalado
fuera del repositorio. El cask de Homebrew está deshabilitado desde
2026-09-01 (`fails_gatekeeper_check`), por eso la descarga oficial. Arranca
ROMs directamente con su BIOS libre integrada (no hacen falta archivos de la
DS).

- **Stub de GDB: sí, y el binario oficial lo trae.** Se activa con
  `Instance0.Gdb.Enabled = true` en `~/Library/Preferences/melonDS/melonDS.toml`
  (puertos 3333 ARM9 y 3334 ARM7). Se usa con Ctrl-C (`0x03`) para detener,
  `m<dir>,<largo>` para leer memoria y `c` para seguir; hay que mandar un
  acuse `+` al conectar, `qSupported` no se entiende y **`?` tumba el
  proceso**. melonDS aborta al reescribir su configuración si el TOML mínimo
  no tiene `[Instance0]` explícito.
- No hay modo sin interfaz (abre su ventana), ignora `SIGTERM` a veces y no
  respeta `HOME` para su configuración: `tools/melonds-run.py` respalda y
  restaura esa configuración, usa `SIGKILL` si hace falta y es una corrida a la
  vez.
- Trazas de instrucciones o consola de depuración: no encontradas (solo el
  stub de GDB).
- DeSmuME 0.9.13 (GPL-2.0; tiene `--arm9gdb`/`--arm7gdb` según su manual) y
  NooDS (GPL-3.0): no instalados ni probados. Nunca se copia código de ellos.

### Qué existe hoy de DS en la PS3

Búsqueda del 2026-10-16: **no encontré ningún emulador de DS usable en PS3.**
La receta de PS3 de libretro-super lista `desmume2015` con el campo de
activación en NO (no verificado qué significa exactamente), el buildbot
oficial de libretro no tiene carpeta PS3 y no hay ports de melonDS, NooDS ni
DeSmuME a PS3/Cell. Sin revisar: el foro de PSX-Place y repos de la escena.

## 0.3 Microbenchmarks del PPE (`tests/phase0/ppe-bench`)

Estado: programa y paquete listos (`SalsaDS bench`, APPID `SLDS00001`);
**sin números de la PS3 real todavía**: se subirá a la consola por quien
tiene permiso del autor. Los números de RPCS3 no valen (solo comprobé que
arranca, que las verificaciones internas pasan y que termina con
`RESULTADO ppe-bench: OK`).

Todo se mide con el timebase (`mftb`, 79,8 MHz = 40 ciclos de 3,2 GHz por
tick). Cada medición se calibra a ≥ 60 ms, se repite 7 veces y se reporta la
**mediana y el mínimo**. Las líneas `DATO …` del log (UDP 18195, y
`/dev_hdd0/salsads/ppe-bench.log` por FTP) son las que se copian aquí y a
`perf-log.md`.

### Cómo se verifica antes de medir

Los núcleos de ensamblador no se pueden correr en la Mac, así que se prueban
bajo qemu-ppc64 (big-endian) con `tools/build.sh test-host`
(`tests/unit/test_ppe_bench.c`): los cuatro bucles de banderas dan el mismo
estado final que una referencia en C con la semántica de ARM (con
desbordamiento con signo y acarreo), las cadenas de latencia dan el índice que
calcula C en las seis variantes, y el intérprete ARM mínimo da el resultado
esperado con los tres despachos en little-endian y en big-endian. El propio
programa repite esas comprobaciones en la consola antes de medir y termina con
FALLO si algo no coincide. Se revisó el desensamblado: en los núcleos no hay
formas con punto, ni `mfcr` en los bucles (solo el que GCC pone en el prólogo
de cada función por los campos de CR que se declaran como usados), ni
desplazamientos por registro.

### 1. Memoria little-endian (`lwbrx` y compañía)

Throughput: bucles de 4 accesos independientes a una línea de L1 caliente
alineada, con `ctr`/`bdnz`; se resta el mismo bucle con 4 `ori 0,0,0` (neto).
Variantes: carga/almacenamiento normal (lo que sería la memoria big-endian),
`lwbrx`/`stwbrx`/`lhbrx`/`sthbrx` y carga normal + rotaciones (`rlwinm` +
2 `rlwimi` en 32 bits, `rlwinm` + `rlwimi` en 16). Latencia: cadena
dependiente por una permutación de 1024 entradas (índice → `rlwinm` → carga →
[swap] → índice). Si `lwbrx` fuera microcódigo se vería ≥ 11 ciclos por
acceso en throughput.

**Qué decide:** si `lwbrx` ≈ carga normal → memoria de la DS en little-endian
tal cual con `lwbrx`/`stwbrx` (y tablas de páginas de punteros); si cuesta
mucho más, o si carga + rotaciones gana en latencia, el diseño de la memoria
emulada cambia (p. ej. guardarla en orden big-endian por palabras y
reordenar bytes solo en accesos de 8/16 bits).

### 2. Banderas NZCV

Bucle guest de 9 instrucciones ARM (`ADDS, CMP, BLT, ADDS, CMP, BHI, ADDS,
SUBS, BNE`: 7,55 por vuelta en promedio) con valores que hacen que haya
desbordamiento con signo (`BLT` = N≠V) y acarreo (`BHI` = C y no Z). Cuatro
implementaciones (los registros guest viven en registros del PPE; las ramas
guest son ramas del host):

| Clave | Estrategia |
|---|---|
| sin banderas (cota) | `cmpw`/`cmplw` + salto directo. No es una implementación válida de ARM: da el costo de las banderas por diferencia |
| (a) completas | N, Z, C, V a cada instrucción, empaquetadas como el CPSR, sin formas con punto: N con `rlwinm`, Z con `cntlzw`, C como bit 32 del resultado de 64 bits (**`addc`/`subfc` no sirven en PowerPC64: su CA es el acarreo del bit 63**), V con `xor/andc`; al consumir se extraen con `rlwinm` + `cmpwi` |
| (b) perezosas | se guardan `a`, `b'` (o `~b` en las restas) y el resultado (4 instrucciones); al consumir se calculan N, Z, C, V con operaciones lógicas (Z y N son 2 instrucciones, C y V ~6) |
| (c) N/Z en CR | `cmpwi cr0,res,0` (N/Z), `cmplw cr1,…` (C) y V en `cr2` con `cmpwi`; los saltos usan los bits del CR (`blt cr0`, `bgt cr1`), `crxor` para N≠V. El consumidor conoce el tipo de la instrucción anterior (como un recompilador por bloques) |

Se reporta ciclos por vuelta, por instrucción ARM y «sobre la cota». No se
modela aquí la eliminación de banderas muertas (aplica por igual a todas: en
este bucle la mitad de las instrucciones que escriben banderas las
sobrescriben antes de leerlas).

### 3. Despacho del intérprete

Intérprete mínimo de ARM (datos con desplazador, `LDR/STR`, `CMP`, `B<cond>`,
`SWI` de fin) que corre un bucle de 9 instrucciones por elemento sobre un
arreglo de 1024 palabras, con la memoria guest leída con `rd32le`. Tres
despachos que comparten los cuerpos de los manejadores: `switch` (con
`-mno-relative-jumptables`), tabla de punteros a función (en la ABI de la PS3
cada puntero es un descriptor) y `goto` calculado (código enhebrado). Se
reporta ns, MIPS guest y ciclos por instrucción guest. Las banderas del
intérprete son completas en C y su costo es común a los tres. Referencia: la
DS pide del orden de 100 MIPS (ARM9 a 67 MHz + ARM7 a 33 MHz); Salsa64 midió
8,8 → 21,2 MIPS en su intérprete (`docs/cell-desde-salsa64.md` §1.3), así que
lo esperable es que el intérprete solo no alcance y el recompilador sea
obligatorio.

### Qué mirar al correrlo en la PS3

1. La primera línea del log: `ppe-bench <versión> (SalsaDS bench, SLDS00001)`
   y `timebase 79800000 Hz`.
2. `RESULTADO ppe-bench: OK` (si dice FALLO, hay una línea `FALLO …` antes y
   los números no valen).
3. Memoria: `lwbrx`/`lhbrx`/`stwbrx`/`sthbrx` contra la carga normal, en neto
   y en latencia; y si carga + rotaciones queda por debajo en alguno.
4. Banderas: el orden de las tres estrategias en «ciclos por instrucción ARM»
   y cuánto suma cada una sobre la cota. Ojo con (c): `crxor` y los `cmp` a
   varios campos del CR podrían ser lentos si el PPE los trata como microcódigo.
5. Intérprete: MIPS de los tres despachos (el de punteros a función paga el
   descriptor ELFv1).
6. Repetir 2–3 veces si las medianas varían más de ±3 % (ruido de ventana
   conocido del PPE).

Tabla de resultados y decisión de memoria y banderas: **pendiente** de la
corrida en la consola.
