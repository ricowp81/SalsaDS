# Código y herramientas de terceros

Todo origen se registra aquí **antes** de copiar nada, con su licencia y la
comprobación de que es compatible con el destino. En `lib/rsxkit/` y
`lib/spukit/` (MIT) no entra código GPL. Nada de material de Nintendo o Sony:
ni ROMs comerciales, ni BIOS, firmware o claves de la DS.

La licencia de SalsaDS está por decidir (pregunta abierta del plan del proyecto)
local): hasta entonces no se copia código GPL de otros proyectos.

## Bibliotecas propias (subtree)

| Carpeta | Origen | Licencia | Notas |
|---|---|---|---|
| `lib/rsxkit/` | github.com/ricowp81/rsxkit (`f8870f2`, 2026-10-16) | MIT | Su `THIRD_PARTY.md` registra lo que contiene (formato de fragment programs de PSL1GHT, fuente 8x13 de X11 en dominio público, Scale2x como algoritmo, contribución de imágenes del 2D con conformidad de su autor) |
| `lib/spukit/` | github.com/ricowp81/spukit (`7b01069`, 2026-10-16) | MIT | Código propio; compresión de bloques con el formato de LZ4 escrita desde la especificación |

## Código propio copiado de Salsa64

Salsa64 es del mismo autor (GPL-2.0-or-later en origen). Se copió y adaptó
(nombres, puerto, rutas) código y herramientas **propios**, no el adaptado de
terceros que Salsa64 registra en su propio `THIRD_PARTY.md`. Mientras no se
decida la licencia de SalsaDS, estos archivos se consideran del autor y su
licencia se fija con esa decisión.

| Archivo | Origen en Salsa64 | Notas |
|---|---|---|
| `mk/ps3.mk` | `mk/ps3.mk` | Sin las reglas de `FPO_CHECK` (llegan con la Fase 0.6 si hacen falta) |
| `src/platform/ps3/{log.c,log.h,paths.h,autotest.h}` | `src/platform/ps3/` | Puerto UDP 18195, carpeta `/dev_hdd0/salsads` |
| `tools/{build,docker-image,docker-image-ppc64,build-ppc64,qemu-ppc64,deploy-ps3}.sh`, `tools/udplog.py`, `tools/rpcs3-run.py`, `tools/rpcs3/*.yml`, `tools/spu-stack.py`, `docker/ppc64.Dockerfile` | los del mismo nombre | `rpcs3-run.py` conserva el nombre del candado de Salsa64 a propósito (RPCS3 compartido) |
| `tests/phase0/udplog/` | `tests/phase0/udplog/` | |

## Toolchain (no se distribuye con el repositorio)

| Herramienta | Uso | Licencia | Notas |
|---|---|---|---|
| ps3toolchain / PSL1GHT (`ps3dev`) | compilar los programas de PS3 | GPL/LGPL (GCC, binutils), MIT (PSL1GHT) | Nunca el SDK de Sony. Viene dentro de la imagen de Docker de Salsa64 |
| NVIDIA Cg Toolkit 3.1 | `cgcomp` (shaders del RSX) | Propietaria | Solo dentro de la imagen local de Docker: **la imagen no se publica** |
| Debian `gcc-powerpc64-linux-gnu`, `qemu-user-static` | pruebas big-endian en la Mac | GPL | Solo herramientas de prueba |

## ROMs de prueba y su toolchain

| Origen | Uso | Licencia | Notas |
|---|---|---|---|
| BlocksDS v1.24.0 (github.com/blocksds/sdk; imagen `skylyrac/blocksds` fijada por digest) | construir `tests/roms` | libnds y crt0 zlib; picolibc MIT/BSD; libgcc GPL-3.0 + excepción de runtime; ndstool GPL-3.0/MIT | Lo que acaba dentro de las ROMs y sus avisos: `tests/roms/homebrew/AVISOS.md` y `docs/fase0.md` (0.2) |
| melonDS 1.1 (github.com/melonDS-emu/melonDS) | emulador de referencia, solo para comparar | GPL-3.0 | Instalado fuera del repositorio; `tools/melonds-run.py` solo lo lanza y lee su memoria por GDB. **No se copia código suyo** |
| DeSmuME, NooDS | referencia posible | GPL-2.0, GPL-3.0 | Ni instalados ni consultados; no se copia código |
| gba-suite (jsmolka) | modelo para pruebas de ARMv4T | MIT | No se copió nada |
| RockWrestler, armwrestler-gba-fixed | — | sin licencia visible | **No se copian ni redistribuyen** |

Código propio: `tests/roms/src/` (marco `sds`, `hola`, `cpu9` y su modelo de
ARM, ARM7 mínimo, ícono) y `tools/melonds-run.py`.

Código propio de la Fase 1.1: `src/cpu/` (intérpretes ARM7/ARM9), `src/host/` (corredor de ROMs, máquina mínima y banco de los intérpretes), `tests/unit/test_acore_*.c` y `acore_tm.h`, `tests/roms/src/cpuarm/` y `tests/roms/src/common/sds7.c`; `tools/cpurun.sh`. Los grupos de `tests/roms/src/cpu9/` se movieron a `groups.h` para compartirlos con `cpuarm`. Sin código de melonDS, NooDS ni DeSmuME: melonDS se usa solo como emulador de referencia (se lanza y se lee su memoria por GDB con `tools/melonds-run.py`).

Código propio de la Fase 0.5 y 0.6, escrito desde GBATEK (documentación, sin
copiar código de ningún emulador) y desde las bibliotecas MIT del proyecto:
`src/gpu2d/` (referencia, compositor SPE/host y escenas sintéticas),
`src/frontend/{dspad,layout}.{h,c}` (entrada y geometría de las pantallas),
`tests/phase0/spe-gpu2d` y `tests/phase0/dual-screen`, `tools/raw-png.py`. De la Fase 0.7: `src/jit/` (codificadores ARM y PowerPC desde los manuales públicos, traductor, intérprete de referencia) y `tests/phase0/jit-arm`; `mk/rec-cache.ld` y `tests/phase0/jit-arm/rec_cache.c` están adaptados de la caché de código y su prueba de Salsa64 (código propio del autor). El
texto en pantalla usa la fuente 8x13 de X11 (dominio público) que ya trae
`lib/rsxkit`.
## Documentación consultada (sin código)

| Fuente | Para qué | Notas |
|---|---|---|
| GBATEK (Martin Korth): reglas de alineación de la CPU ARM, LDM/STM, multiplicación, excepciones, transferencia de un solo dato (LDR/STR/PLD), PSR (MRS/MSR), CP15 (ID, registro de control, TCM) | `src/cpu/` (Fase 1.1): comportamiento por versión (ARMv4T / ARMv5TE), vectores y LR de cada excepción, registro de control y regiones de TCM | Leído el 2026-10-17 en las páginas por tema de problemkaputt.de; solo se tomaron hechos de hardware, nada de texto ni código |
| ARM Architecture Reference Manual (DDI 0100I), ARM7TDMI y ARM946E-S TRM | `src/cpu/`: semántica de las instrucciones ARM y Thumb, modelo de abortos, Q y saturación, multiplicaciones de 16 bits | Referencia de la semántica; la codificación y los casos de borde se escribieron de memoria del manual y se contrastaron con la ROM `cpuarm` en melonDS. No se copió texto |
| GBATEK (Martin Korth), secciones del 3D de la DS | `tests/phase0/spe-raster3d`: campos de TEXIMAGE_PARAM y PLTT_BASE, formatos de textura (directo A1BGR5, paletas de 16 y 256 con el índice 0 transparente), expansión de 5 a 6 bits, fórmulas de modulación y de mezcla alfa | Solo la descripción del hardware; no se copió código de ningún emulador |
| Documentación del SPU (intrínsecos de spu-gcc en `spu_internals.h` del toolchain) | `r3d_vec.h`: `spu_re` es `fi(frest)`; recíproco con un paso de Newton | Parte del toolchain (ps3toolchain) |

El rasterizador de `tests/phase0/spe-raster3d` (núcleo, referencia, escena y
programa SPU) es código propio; usa `lib/spukit` como biblioteca.

