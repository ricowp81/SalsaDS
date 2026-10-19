# Guía del proyecto

Resumen de objetivos, reglas y arquitectura de SalsaDS. El estado al día está
en `estado.md`; los resultados de cada fase, en `fase0.md`…`fase3.md`; las
mediciones, en `perf-log.md`; los orígenes de lo que no es propio, en
`THIRD_PARTY.md`.

## Objetivos

1. Juegos de DS a velocidad completa en la PS3 real, empezando por títulos 2D
   y 3D ligeros.
2. El Cell como fortaleza: SPEs para el 3D, el 2D, el audio y el posprocesado;
   el PPE para las CPUs.
3. Salida en TV: dos pantallas con distribuciones configurables, render 3D
   interno a mayor resolución y filtros.
4. Código abierto bajo GPL-2.0-o-posterior; `lib/rsxkit/` y `lib/spukit/`
   siguen MIT.

No son objetivos por ahora: DSi, cámaras, Wi-Fi, micrófono real, accesorios de
la ranura de GBA, modo GBA, precisión de ciclo.

## Reglas

- Toolchain de la PS3: ps3toolchain / PSL1GHT únicamente; nunca el SDK de Sony.
- Nunca ROMs comerciales, BIOS de la DS o la DSi, firmware, tablas de claves ni
  material de Nintendo o Sony, ni desensamblados de la BIOS o el firmware.
- Arranque directo: se cargan los binarios ARM9/ARM7 de la cabecera, las SWI
  van por HLE y los datos de usuario del firmware los genera el emulador.
- Pruebas automatizadas solo con ROMs homebrew propias o de libre
  distribución; los juegos comerciales los prueba el autor con respaldos
  propios, fuera del repositorio.
- Todo código de terceros se registra en `THIRD_PARTY.md` antes de copiarlo,
  con licencia compatible. En `lib/` (MIT) no entra código GPL. No se copia
  código de melonDS ni de NooDS (GPL-3.0).

## Arquitectura

| Unidad | Responsabilidad |
|---|---|
| PPE, hilo 0 | ARM9 y ARM7 intercalados (intérprete; recompilador ARM/Thumb a PPC64 después), mapa de memoria, IRQ, timers, DMA, IPC |
| PPE, hilo 1 | presentación, entrada, frontend, E/S |
| SPE 0–1 | rasterizador 3D por software (bandas de líneas) |
| SPE 2 | composición 2D de las dos pantallas por línea |
| SPE 3 | mezcla de los 16 canales de sonido |
| SPE 4–5 | escalado y filtros |
| RSX | presentación de las dos pantallas con su distribución |

Hay 6 SPEs disponibles; no se asume el séptimo. El trabajo va por lotes: un
registro de escrituras a los registros de video por línea durante el cuadro y
los SPEs componen el cuadro entero con ese registro.

La DS es little-endian y la PS3 big-endian: todo acceso a la memoria emulada
pasa por funciones de lectura y escritura LE explícitas, y las pruebas
unitarias corren en los dos órdenes (host LE y qemu-ppc64 BE).

## Convenciones

- C99/C11. El núcleo (CPU, memoria, 2D, 3D, audio) es portable y compila en el
  host; lo que corre en un SPE se prueba antes en el host.
- Código SPE: estructuras alineadas a 16 bytes (mejor 128), DMA en múltiplos de
  16 bytes, doble búfer, presupuesto del local store en la cabecera de cada
  programa (≤ 256 KB).
- Sin asignaciones dinámicas en el bucle de emulación.
- Cada optimización con medición antes y después en `perf-log.md`; nunca se
  concluye sobre rendimiento con RPCS3 ni con el host.
- Cada archivo propio lleva `SPDX-License-Identifier: GPL-2.0-or-later`.
- Mensajes de commit en español, narrativos.
- Repositorio público: autor `ricowp81`; sin datos personales.

## Estructura

```
docker/    imágenes del toolchain (PS3, ppc64 para qemu, ARM para las ROMs)
src/core/  mapa de memoria, VRAM por bancos, IRQ, timers, DMA, IPC, cartucho, SPI
src/cpu/   intérpretes ARMv5TE / ARMv4T (ARM y Thumb)
src/bios/  HLE de las SWI y arranque directo
src/gpu2d/ composición 2D portable (host, PPE y SPE)
src/gpu3d/ motor de geometría (y, después, rasterizador) portable
src/audio/ mezcla de canales
src/jit/   prototipo del recompilador ARM a PPC64
src/spu/   programas SPE
src/frontend/, src/platform/ps3/, src/host/   menú y entrada, plataforma, ejecutable del host
lib/       rsxkit y spukit (MIT, también en repos propios)
tests/     unitarias (LE/BE), ROMs de prueba y su fuente, pruebas de la Fase 0
tools/     build, despliegue a la PS3, RPCS3, receptor UDP, comparación con melonDS
docs/      estado, fases, THIRD_PARTY, perf-log, memory-budget
```
