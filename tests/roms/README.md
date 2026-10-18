# ROMs de prueba de la DS

Fuentes de las ROMs homebrew propias con las que se prueba SalsaDS. Las
construidas están en `homebrew/` (con `SHA256SUMS` y `AVISOS.md`). Nunca hay
aquí ROMs comerciales, BIOS, firmware ni claves de Nintendo.

| ROM | Qué hace |
|---|---|
| `hola` | Escribe en la VRAM en el modo de visualización directa (banco A como LCDC, sin motores 2D) y reporta OK por el marco de pruebas |
| `cpuarm` | **ARM9 y ARM7** (ARMv5TE y ARMv4T), ARM y Thumb: los grupos de `cpu9` (en el ARM7, los de ARMv4T) más los nuevos: acceso no alineado y sus diferencias entre versiones, LDM/STM con la base en la lista y con el PC, listas vacías y el banco de usuario, modos y registros con banco, SPSR, cambio de estado (BX, BLX, `ldr pc`, `ldm {pc}`, `pop {pc}`, `mov pc`) y los formatos 5 a 19 de Thumb; solo en el ARM9, las excepciones con los vectores en la ITCM (SWI, indefinida, BKPT en Thumb y en ARM) y CP15 con las dos TCM. El ARM7 corre su propio programa y avisa al ARM9 por IPCSYNC de cómo terminó; el detalle queda en su bloque (`0x02390000`) |
| `mem` | Mapa de memoria de las dos CPUs: RAM y espejos, WRAM compartida en los 4 modos de WRAMCNT (la ve el ARM7 por el buzón), los 9 bancos de VRAM en 43 asignaciones, solapes, espejos, VRAM del ARM7 y VRAMSTAT, paleta y OAM (los 8 bits se ignoran) |
| `irq` | IRQ de VBlank, HBlank y VCOUNT, prescalers, cascada e IRQ de los temporizadores, IME/IE/IF, HALT y teclas, en el ARM9 y el ARM7 con libnds; la referencia de tiempo son los temporizadores 2 y 3 en cascada a 33,513982 MHz |
| `dma` | DMA de 16/32 bits en los 4 canales de cada CPU, direcciones, relleno (DMAFILL), IRQ, VBlank con repetición, HBlank, VRAM y paleta |
| `ipc` | IPCSYNC y las FIFO desde las dos CPUs (el ARM9 dirige, el ARM7 obedece por el buzón) |
| `bios` | SWI de la BIOS llamadas a mano (Div, Sqrt, GetCRC16, CpuSet/CpuFastSet, LZ77, RLE, Huffman, BitUnPack, diferencias) con datos de `tools/gen-bios-vectors.py`, y la conservación de registros |
| `periph` | División y raíz por hardware, cartucho (B7/B8, sondeo y DMA), SPI (firmware, táctil, energía), RTC, EXTKEYIN y el arranque |
| `save` | El chip de guardado por AUXSPI (EEPROM de 0,5 KB, de 8 y 64 KB, FRAM de 32 KB y Flash): lo detecta con RDSR, RDID y los espejos de direcciones y comprueba registro de estado, WREN/WRDI, escritura sin WREN, lectura de vuelta, páginas que dan la vuelta, vuelta al final de la memoria y, en Flash, borrar página y sector, programar y escribir página. El emulador pone el chip con un archivo de guardado del tamaño correspondiente (`salsads-cpurun --save-size N`); el tipo y el tamaño detectados quedan en `+0x34` y `+0x38` del bloque de resultados |
| `ocio` | Las dos CPUs esperando el VBlank con IntrWait y nada más (decenas de instrucciones por cuadro): mide cuánto cuesta un cuadro ocioso del planificador |
| `carga` | Trabajo continuo en las dos CPUs, sin HALT: una mezcla de operaciones de datos, copias de bloques, bytes, multiplicaciones y una ordenación en Thumb (`src/carga/kernel.h`), repetida sin fin y comparada en cada vuelta con valores calculados en el host (`tools/carga-gold.sh`). Los contadores quedan en `0x023A0000` (`shared.h`); es la ROM que embebe «SalsaDS núcleo» para medir los MIPS del intérprete |
| `demo` | Demostración animada sin veredicto (la embebe «SalsaDS núcleo»): cuadrado que rebota, teclas, táctil y fondo del motor B |
| `tablas` | Las SWI 1Ah, 1Bh y 1Ch del ARM7 (GetSineTable, GetPitchTable, GetVolumeTable) con todos los índices válidos, comparadas con las tablas que genera `tools/gen-bios-tables.py` por su fórmula (verificadas contra las 1556 entradas de melonDS) |
| `gx` | La FIFO de comandos del motor de geometría vista desde el ARM9, sin interpretar comandos: GXSTAT con la FIFO vacía, NOP por CPU y por DMA en modo 7, IRQ 21 por nivel en los modos «menos de la mitad» y «vacía». Da lo mismo en SalsaDS y en melonDS |
| `snd` | Sonido: los 16 canales (PCM8, PCM16, IMA-ADPCM, PSG y ruido; volumen, divisor, balance, bucles, una vez, tiempos), SOUNDCNT y la captura (SNDCAP0/1). Deja lo que sale del mezclador, capturado por hardware, en RAM (`0x02300000`, 1 KB por caso) y no tiene veredicto propio: `tools/snd-compare.py` lo compara muestra a muestra con melonDS. El ARM7 corre la lista de casos dos veces (la primera calienta: la fase del primer caso respecto de los ticks del mezclador es arbitraria) |
| `cpu9` | ~65 000 comprobaciones de CPU del ARM9 (ARMv5TE): las 16 operaciones de datos con 4 combinaciones de banderas, el desplazador, las 14 condiciones (ARM y Thumb), multiplicaciones, CLZ, saturación (Q), multiplicaciones de 16 bits, LDR/STR no alineados, LDM/STM, SWP, cambio ARM/Thumb y las instrucciones de datos de Thumb. El modelo en C (`src/cpu9/model.h`) sale del manual de ARM y se validó contra melonDS |

## Buzón entre las dos CPUs (`src/common/hw.h`)

Las ROMs `mem`, `irq`, `dma`, `ipc`, `bios` y `periph` dejan que el ARM9 dirija: `mb_call(orden, a0, a1, a2)` escribe una
orden en una estructura de RAM principal (`0x02390800`) y espera la respuesta del ARM7, que la atiende en su bucle. Las
dos CPUs ven la RAM al instante porque el ARM9 apaga el PU y las cachés antes (`sds_nocache`). El ARM7 de `mem` se
enlaza con `--defsym=__shared_wram_size=0` para que su código no viva en la WRAM compartida que la prueba mueve.

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

Las mismas ROMs corren sin emulador de referencia en la máquina de SalsaDS (`src/core`) con el corredor del host:
`salsads-cpurun ROM.nds` (compilado por `tools/build.sh -C src/host`, o por `tools/cpurun.sh` en la Mac), que arranca la
ROM como el arranque directo y lee el mismo bloque de resultados. Opciones útiles: `--frames N` (corre N cuadros y no
busca veredicto), `--screens f.ppm` (las dos pantallas), `--keys HEX` y `--touch X,Y` (entrada fija), `--quantum N`
(tramo del planificador), `--peek ADDR`, `--regs`, `--arm7`, `--stats`. `tools/build.sh test-host` las corre todas en
little-endian y en big-endian (qemu-ppc64), repite algunas con tramos de 1 a 20 000 ciclos y corre la demostración.

La ROM `save` con melonDS: este elige el chip de guardado por el código de juego de la cabecera, no por el archivo, así
que se le da el de un juego conocido: `tools/melonds-run.py --gamecode A2DE build/roms/save.nds` (EEPROM de 8 KB) o
`--gamecode AMCE` (Flash de 256 KB; la ROM, al ver ese código, no pregunta el RDID, que melonDS no contesta). melonDS
corre siempre sobre una copia en una carpeta temporal (así su `.sav` no cae junto a la ROM original).

La imagen se compara con melonDS así: `tools/melonds-run.py ROM.nds --vram melon.ppm --regs` y `salsads-cpurun --screens
sds.ppm --regs ROM.nds`, luego `tools/compare-image.py sds.ppm melon.ppm` (la pantalla de arriba contra el banco A) y
la comparación de la línea `regs`. `tools/gen-bios-vectors.py` genera `src/bios/vectors.h` y, con `--pattern`,
`src/periph/pattern.h` (bytes conocidos para leer el cartucho).

## ROMs de gráficos 2D (`gfx*`, `src/gfx`)

Montan escenas en los motores 2D (Fase 2.1) y dejan en RAM (`0x02380800`) una lista de píxeles con color conocido
(`sds_expect_pixel`). `salsads-cpurun --expect --same-ab --vs-ref` comprueba la lista, que A y B den lo mismo y que el
compositor coincida con el renderizador de referencia; `tools/gfx-compare.py` compara el motor A con melonDS (la ROM dispara
DISPCAPCNT y melonDS entrega el banco A por GDB). `gfxrand` saca una escena aleatoria de la semilla que `tools/rom-seed.py`
escribe en el código de juego; `tools/gfx-bisect.py` y `tools/gfx-bisect-obj.py` aíslan qué característica u OBJ hace
diferir a las dos. `gfxprobe`, `gfxobjmap*` y `gfxomap*` son sondas de melonDS y no se instalan. Detalle en `docs/fase2.md`.
