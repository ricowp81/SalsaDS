# ROMs de prueba de la DS

Fuentes de las ROMs homebrew propias con las que se prueba SalsaDS. Las
construidas están en `homebrew/` (con `SHA256SUMS` y `AVISOS.md`). Nunca hay
aquí ROMs comerciales, BIOS, firmware ni claves de Nintendo.

| ROM | Qué hace |
|---|---|
| `hola` | Escribe en la VRAM en el modo de visualización directa (banco A como LCDC, sin motores 2D) y reporta OK por el marco de pruebas |
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

## Construir y correr

```sh
tools/build-roms.sh              # build/roms/*.nds
tools/build-roms.sh install      # además a tests/roms/homebrew/ y SHA256SUMS
MELONDS=/ruta/a/melonDS.app tools/melonds-run.py build/roms/cpu9.nds [--vram pantalla.ppm]
```

`tools/melonds-run.py` lanza melonDS (emulador de referencia, sin código suyo
en este repositorio) con su stub de GDB, lee el bloque de resultados con el
protocolo remoto de GDB y sale con 0 (OK), 1 (FALLO) o 2 (sin veredicto).
