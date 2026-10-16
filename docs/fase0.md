# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | hecho (2026-10-16) |
| 0.3 Microbenchmarks del PPE | en curso: programa listo; **faltan los números de la PS3 real** |
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
