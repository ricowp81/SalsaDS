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

Ver `docs/fase0.md` (0.2): toolchain ARM libre (BlocksDS), licencias
verificadas y suites de pruebas de terceros.
