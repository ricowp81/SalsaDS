# Avisos de las ROMs de prueba

Las ROMs `.nds` de esta carpeta son **código propio de SalsaDS** (fuentes en
`tests/roms/src/`) construido con BlocksDS. Lo que BlocksDS deja dentro del
binario, con la licencia que lleva en su propio árbol (textos leídos de la
imagen de Docker, BlocksDS v1.24.0, el 2026-10-16):

| Componente en la ROM | Licencia | Notas |
|---|---|---|
| `crt0` del ARM9 y del ARM7 (BlocksDS `sys/crts`) | zlib (Antonio Niño Díaz, 2023); los scripts del enlazador `.ld`/`.mem`, MPL-2.0 | El código objeto del `crt0` es zlib. Los scripts de enlace son MPL-2.0 y no quedan dentro de la ROM; su fuente está en github.com/blocksds/sdk |
| libnds (`libnds9`, `libnds7`) | zlib (texto abajo) | Incluye FatFs |
| FatFs (dentro de libnds) | Estilo BSD de una cláusula (ChaN) | Para binarios sin fuentes no pide aviso (su propio texto lo dice) |
| picolibc (biblioteca de C de la toolchain de Wonderful) | MIT/BSD y otras permisivas, por archivo | Ver `COPYING.picolibc` en github.com/picolibc/picolibc. **Pendiente** de copiar íntegro antes de redistribuir los `.nds` fuera de este repositorio: las licencias tipo BSD de tres cláusulas piden reproducir su aviso con los binarios |
| libgcc | GPL-3.0 con la excepción de runtime de GCC | La excepción permite binarios no GPL |
| dlmalloc (dentro de libnds) | no verificado su aviso | Pendiente de leer su cabecera |

**No** entran DSWiFi, Maxmod ni Mbed TLS (avisos MIT, ISC y Apache-2.0 de
BlocksDS): el ARM7 de estas ROMs es uno propio mínimo
(`tests/roms/src/arm7min`) en lugar del núcleo por omisión, que los enlazaría.
El ícono de la cabecera es propio (`tests/roms/src/icon.bmp`), no el logotipo
de BlocksDS (CC BY-ND 4.0).

## libnds (zlib)

```
Copyright (C) 2005-2011 Michael Noland (joat)
Copyright (C) 2005-2013 Jason Rogers (dovoto)
Copyright (C) 2005-2021 Dave Murphy (WinterMute)
Copyright (C) 2006-2008 Michael Chisholm (Chishm)
Copyright (C) 2005-2007 Michael Noland (joat)
Copyright (C) 2010 Richard Eric M. Lope BSN RN (Relminator)
Copyright (C) 2017 fincs
Copyright (C) 2020-2023 Gericom
Copyright (C) 2023-2025 Adrian "asie" Siekierka
Copyright (C) 2023-2025 Antonio Niño Díaz
Copyright (C) 2023 Epicpkmn11
Copyright (C) 2023 profi200

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use of
this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a product,
   an acknowledgment in the product documentation would be appreciated but is
   not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.
```

## crt0 de BlocksDS (zlib)

```
Copyright (c) 2023 Antonio Niño Díaz

(mismo texto zlib de arriba)
```

## Reconstruir

`tools/build-roms.sh install` reconstruye las ROMs con la imagen
`salsads-nds` (BlocksDS fijado por digest en `docker/nds.Dockerfile`) y
actualiza `SHA256SUMS`. La construcción es reproducible: dos corridas dan los
mismos hashes.
