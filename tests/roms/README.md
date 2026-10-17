# ROMs de prueba de la DS

Fuentes de las ROMs homebrew propias con las que se prueba SalsaDS. Las
construidas están en `homebrew/` (con `SHA256SUMS` y `AVISOS.md`). Nunca hay
aquí ROMs comerciales, BIOS, firmware ni claves de Nintendo.

| ROM | Qué hace |
|---|---|
| `hola` | Escribe en la VRAM en el modo de visualización directa (banco A como LCDC, sin motores 2D) y reporta OK por el marco de pruebas |
| `cpuarm` | **ARM9 y ARM7** (ARMv5TE y ARMv4T), ARM y Thumb: los grupos de `cpu9` (en el ARM7, los de ARMv4T) más los nuevos: acceso no alineado y sus diferencias entre versiones, LDM/STM con la base en la lista y con el PC, listas vacías y el banco de usuario, modos y registros con banco, SPSR, cambio de estado (BX, BLX, `ldr pc`, `ldm {pc}`, `pop {pc}`, `mov pc`) y los formatos 5 a 19 de Thumb; solo en el ARM9, las excepciones con los vectores en la ITCM (SWI, indefinida, BKPT en Thumb y en ARM) y CP15 con las dos TCM. El ARM7 corre su propio programa y avisa al ARM9 por IPCSYNC de cómo terminó; el detalle queda en su bloque (`0x02390000`) |
| `cpu9` | ~65 000 comprobaciones de CPU del ARM9 (ARMv5TE): las 16 operaciones de datos con 4 combinaciones de banderas, el desplazador, las 14 condiciones (ARM y Thumb), multiplicaciones, CLZ, saturación (Q), multiplicaciones de 16 bits, LDR/STR no alineados, LDM/STM, SWP, cambio ARM/Thumb y las instrucciones de datos de Thumb. El modelo en C (`src/cpu9/model.h`) sale del manual de ARM y se validó contra melonDS |

## Marco de pruebas (`src/common/sds.h`)

Cada ROM reporta por dos vías sin ayuda humana:

1. **Bloque de resultados** en RAM principal, `0x02380000` (`struct
   sds_results`, little-endian): `magic` «SDSR», `state` (1 = terminó),
   `total`, `failed`, `verdict` (1 = OK, 2 = FALLO) y, del primer fallo, grupo,
   identificador (la línea de la fuente), valor obtenido y esperado.
2. **Pantalla de arriba** en modo de visualización directa de VRAM: una línea
   por grupo y una franja verde «TODO OK» o roja «FALLO». El banco A de VRAM
   (`0x06800000`, 256×192 en RGB555) se puede volcar con el emulador.

### Divergencias respecto de la documentación

Donde el comportamiento esperado viene de GBATEK o del manual de ARM y melonDS 1.1
hace otra cosa, la ROM **no falla**: lo anota como *divergencia* (`sds_diverge`,
`SDS_DIVERGE`, hasta 32 entradas en `+0x140` del bloque de resultados), y
`tools/melonds-run.py` y `salsads-cpurun` las imprimen aparte. Los intérpretes de
SalsaDS siguen la documentación y la comprueban en las pruebas unitarias. La lista
con cada caso y la razón está en `docs/fase1.md`.

El bloque de resultados tiene además un registro de los primeros 16 fallos
(`+0x40`: grupo, identificador, obtenido y esperado) y un marcador de avance
(`mark`, `+0x30`) para depurar ROMs que se cuelgan.

## Construir y correr

```sh
tools/build-roms.sh              # build/roms/*.nds
tools/build-roms.sh install      # además a tests/roms/homebrew/ y SHA256SUMS
MELONDS=/ruta/a/melonDS.app tools/melonds-run.py build/roms/cpu9.nds [--vram pantalla.ppm]
```

`tools/melonds-run.py` (con `--arm7` para ver también el bloque del ARM7 de `cpuarm`)
lanza melonDS (emulador de referencia, sin código suyo
en este repositorio) con su stub de GDB, lee el bloque de resultados con el
protocolo remoto de GDB y sale con 0 (OK), 1 (FALLO) o 2 (sin veredicto).

Las mismas ROMs corren sin emulador de referencia en los intérpretes de CPU de
SalsaDS (`src/cpu`) con el corredor del host: `salsads-cpurun ROM.nds` (compilado por
`tools/build.sh -C src/host`, o por `tools/cpurun.sh` en la Mac), que arranca la ROM
como el arranque directo, con una máquina mínima (`src/host/ndsmin.c`: no es el mapa
de memoria de la Fase 1.2), y lee el mismo bloque de resultados.
`tools/build.sh test-host` las corre en little-endian y en big-endian (qemu-ppc64).
