# Código y herramientas de terceros

Todo origen se registra aquí **antes** de copiar nada, con su licencia y la
comprobación de que es compatible con el destino. En `lib/rsxkit/` y
`lib/spukit/` (MIT) no entra código GPL. Nada de material de Nintendo o Sony:
ni ROMs comerciales, ni BIOS, firmware o claves de la DS.

SalsaDS es **GPL-2.0-or-later** (copyright de ricowp81); `lib/rsxkit/` y `lib/spukit/` son MIT.
No se copia código de melonDS ni de NooDS (GPL-3.0, incompatible con GPL-2.0-o-posterior);
de DeSmuME (GPL-2.0-o-posterior) sí se podría, registrándolo aquí antes.

## Bibliotecas propias (subtree)

| Carpeta | Origen | Licencia | Notas |
|---|---|---|---|
| `lib/rsxkit/` | github.com/ricowp81/rsxkit (`f8870f2`, 2026-10-16) | MIT | Su `THIRD_PARTY.md` registra lo que contiene (formato de fragment programs de PSL1GHT, fuente 8x13 de X11 en dominio público, Scale2x como algoritmo, contribución de imágenes del 2D con conformidad de su autor) |
| `lib/spukit/` | github.com/ricowp81/spukit (`7b01069`, 2026-10-16) | MIT | Código propio; compresión de bloques con el formato de LZ4 escrita desde la especificación |

## Código propio copiado de Salsa64

Salsa64 es del mismo autor (GPL-2.0-or-later en origen). Se copió y adaptó
(nombres, puerto, rutas) código y herramientas **propios**, no el adaptado de
terceros que Salsa64 registra en su propio `THIRD_PARTY.md`. Todos son del autor y
quedan bajo la licencia de SalsaDS (GPL-2.0-or-later).

| Archivo | Origen en Salsa64 | Notas |
|---|---|---|
| `mk/ps3.mk` | `mk/ps3.mk` | Sin las reglas de `FPO_CHECK` (llegan con la Fase 0.6 si hacen falta) |
| `src/platform/ps3/{log.c,log.h,paths.h,autotest.h}` | `src/platform/ps3/` | Puerto UDP 18195, carpeta `/dev_hdd0/salsads` |
| `tools/{build,docker-image,docker-image-ppc64,build-ppc64,qemu-ppc64,deploy-ps3}.sh`, `tools/udplog.py`, `tools/rpcs3-run.py`, `tools/rpcs3/*.yml`, `tools/spu-stack.py`, `docker/ppc64.Dockerfile` | los del mismo nombre | `rpcs3-run.py` conserva el nombre del candado de Salsa64 a propósito (RPCS3 compartido) |
| `tests/phase0/udplog/` | `tests/phase0/udplog/` | |
| `tests/phase0/jit-arm/ps3mapi.h` | `tests/phase0/jit-ppe/ps3mapi.h` (rama `jit-ps3mapi`) | Llamadas a PS3MAPI reescritas; los opcodes y el orden de argumentos se tomaron como referencia de github.com/bucanero/ps3mapi-lib (MIT) y github.com/crystalct/ps3mapidynarec (GPLv2) sin copiar su código |

## Datos de compatibilidad de guardado

| Archivo | Origen | Licencia | Notas |
|---|---|---|---|
| `src/core/save.c` (tabla `known[]`) | DeSmuME, `desmume/src/mc.cpp` (lista blanca por código de juego: `ASMK`, `AL3`, `AH5`, `AVH`, `AQ3` con dirección de un byte; `BDE`, `BPV` con dos; consultada el 2026-10-19) | GPL-2.0-o-posterior | Solo se tomaron los códigos y el dato (ancho de dirección), no código. La idea de deducir por el número de bytes de dirección es de su autodetección; la implementación es propia |
| `src/core/save.c` (Pokémon, Mario Kart DS, NSMB, SM64 DS) | Hechos de compatibilidad (tamaño del chip de cada juego), GBATEK y los archivos de guardado que se ven en uso | datos, no obra | DeSmuME no trae tabla de tamaños por juego (usa una base externa por CRC); no se tomó nada de melonDS ni de NooDS (GPL-3.0) |

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

Código propio de la Fase 1.3: `src/core/save.c`, `tests/roms/src/{ocio,carga,save}/`, `tools/{carga-gold.sh,melonds-dump.py}` y los cambios de `sched.c`, `spi.c` y `tests/phase1/nucleo/`; escrito desde GBATEK, sin código de melonDS, NooDS ni DeSmuME.

Código propio de la Fase 1.2: `src/core/` (máquina de la DS), `src/bios/` (BIOS en HLE y arranque directo), `src/host/cpurun.c`,
`tests/roms/src/{mem,irq,dma,ipc,bios,periph,demo}/` y `common/hw.h`, `tests/phase1/nucleo/`, `tools/{gen-bios-vectors,compare-image}.py`.
Escrito desde GBATEK; melonDS solo se lanza y se lee su memoria por GDB; sin código de melonDS, NooDS ni DeSmuME. Los
manejadores de IRQ de la BIOS son seis y nueve instrucciones ARM escritas para el contrato que documenta GBATEK
(ensambladas aparte con `arm-none-eabi-as`). Los datos comprimidos de las pruebas de la BIOS los generan compresores
propios (`tools/gen-bios-vectors.py`) desde la descripción de los formatos.

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
| GBATEK (Martin Korth), páginas por tema de problemkaputt.de: mapas de memoria, control de WRAM y VRAM, interrupciones, temporizadores, DMA, IPC, teclado (EXTKEYIN), SPI, firmware (cabecera y datos de usuario), pantalla táctil, gestión de energía, RTC, cartucho (puertos, protocolo y cabecera), funciones de la BIOS (resumen, Halt, aritméticas, copia de memoria, descompresión, varias), matemáticas, control de energía, memoria del cartucho y RAM principal, modos de video y captura | `src/core` y `src/bios` (Fase 1.2): comportamiento de cada periférico, formatos de los descompresores y datos de usuario del firmware | Leído el 2026-10-17; solo hechos de hardware, nada de texto ni código. Lo que GBATEK no dice y se decidió de otra manera (valores iniciales, cola de respuesta del táctil, calibración generada) está en el encabezado del archivo que lo decide |
| GBATEK (Martin Korth), «DS Cartridge Backup» (problemkaputt.de) | `src/core/save.c` (Fase 1.3): tipos y tamaños de EEPROM, FRAM y Flash, órdenes y bytes de dirección de cada uno, registro de estado (WIP, WEL, WP, SRWD), tamaños de página, qué contestan RDSR y RDID y cómo se infieren el ancho de dirección y el tamaño | Leído el 2026-10-17; solo hechos de hardware (y los nombres de los juegos de ejemplo para la tabla de tres códigos), nada de texto ni código |
| melonDS 1.1 (GPLv3), usado como emulador de referencia | Fase 1.3: qué valores deja en la RAM compartida del arranque (segunda copia del identificador del chip, CRC16 de la cabecera y del área segura, 5835h), el tamaño del archivo de guardado que crea para New Super Mario Bros. (8192 bytes) y Mario Kart DS (262144), y sus divergencias con GBATEK en el chip de guardado (tabla de `docs/fase1.md`) | Solo se lanzó y se leyó su memoria y sus registros por el stub de GDB; sin código de melonDS |
| Fragmentos de código de los respaldos del autor (unas decenas de instrucciones del ARM9 y del ARM7 de New Super Mario Bros., volcadas de la RAM de la emulación y desensambladas con objdump) | Entender, a mano, por qué dos juegos se detenían (la rutina de apagado y la espera sobre un indicador); la causa real se confirmó con el chip de guardado y las escrituras del firmware | Solo para diagnosticar: no se guardó ni se copió nada, y lo descubierto se describe por el comportamiento del hardware que faltaba |
| Desensamblado del `crt0` de BlocksDS y de los símbolos de libnds (objdump de la imagen de Docker; ambos son zlib/MPL-2.0) | Entender el apretón de manos por IPCSYNC del arranque, dónde pone libnds su manejador de IRQ y sus banderas de IntrWait, y los registros de las SWI | Es la herramienta de las ROMs de prueba, no material de Nintendo; no se copió ni se guarda su desensamblado |

| Fuente | Para qué | Notas |
|---|---|---|
| GBATEK (Martin Korth): reglas de alineación de la CPU ARM, LDM/STM, multiplicación, excepciones, transferencia de un solo dato (LDR/STR/PLD), PSR (MRS/MSR), CP15 (ID, registro de control, TCM) | `src/cpu/` (Fase 1.1): comportamiento por versión (ARMv4T / ARMv5TE), vectores y LR de cada excepción, registro de control y regiones de TCM | Leído el 2026-10-17 en las páginas por tema de problemkaputt.de; solo se tomaron hechos de hardware, nada de texto ni código |
| ARM Architecture Reference Manual (DDI 0100I), ARM7TDMI y ARM946E-S TRM | `src/cpu/`: semántica de las instrucciones ARM y Thumb, modelo de abortos, Q y saturación, multiplicaciones de 16 bits | Referencia de la semántica; la codificación y los casos de borde se escribieron de memoria del manual y se contrastaron con la ROM `cpuarm` en melonDS. No se copió texto |
| GBATEK (Martin Korth), secciones del 3D de la DS | `tests/phase0/spe-raster3d`: campos de TEXIMAGE_PARAM y PLTT_BASE, formatos de textura (directo A1BGR5, paletas de 16 y 256 con el índice 0 transparente), expansión de 5 a 6 bits, fórmulas de modulación y de mezcla alfa | Solo la descripción del hardware; no se copió código de ningún emulador |
| Documentación del SPU (intrínsecos de spu-gcc en `spu_internals.h` del toolchain) | `r3d_vec.h`: `spu_re` es `fi(frest)`; recíproco con un paso de Newton | Parte del toolchain (ps3toolchain) |

El rasterizador de `tests/phase0/spe-raster3d` (núcleo, referencia, escena y
programa SPU) es código propio; usa `lib/spukit` como biblioteca.

| Fuente | Para qué | Notas |
|---|---|---|
| GBATEK (Martin Korth), motores 2D, fondos, OBJ, ventanas, efectos de color, DISPCNT, captura y visualización de memoria principal | `src/gpu2d/` y `src/core/video.c` (Fase 2.1) | Solo hechos de hardware; ni texto ni código |
| melonDS 1.1 (GPL-3.0), solo ejecutado | Comparar la imagen del motor A con la de las ROMs propias (`tools/gfx-compare.py`) y medir líneas y mosaico por caja negra | **No se copió ni se leyó su código**; las fórmulas de color y el mosaico salieron de ajustar el comportamiento observado. Sin NooDS ni DeSmuME |

| Fuente | Para qué | Notas |
|---|---|---|
| GBATEK (Martin Korth), sonido de la DS (canales, formatos PCM8/PCM16/IMA-ADPCM/PSG y ruido, SOUNDCNT, SOUNDBIAS, captura) y FIFO de comandos del motor de geometría (GXSTAT, GXFIFO, IRQ 21) | `src/audio/`, `src/core/sound.c`, la FIFO de `src/core/io.c` (Fase 2.2) | Solo hechos de hardware; ni texto ni código |
| IMA ADPCM (especificación pública de IMA/DVI): tabla de 89 pasos y ajuste de índice | `src/audio/snd_mix.c`, `tools/gen-snd-data.py` | Datos del estándar, de dominio público; el codificador de prueba es propio |
| melonDS 1.1 (GPL-3.0), solo ejecutado | Comparar muestra a muestra el mezclador de sonido (`tools/snd-compare.py`), las tablas de las SWI 1Ah–1Ch (`tools/gen-bios-tables.py --check`) y el comportamiento de la FIFO de geometría (ROM `gx`) | **No se copió ni se leyó su código**; las fórmulas salieron de ajustar su comportamiento (capturas de SNDCAP y valores devueltos por la BIOS libre). Las tablas **no** se copiaron: se generan con su fórmula |

