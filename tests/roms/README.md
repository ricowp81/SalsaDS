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
| `bios13` | Como `bios` pero con la SWI 0x13 (Huffman por callbacks), que en una DS real no volvía: pendiente. Prueba la hipótesis de que la BIOS usa r2 como búfer temporal de 0x200 bytes para el árbol y llama a Close (antes r2 = 0 y Close = 0). `bios` no la llama (la SWI 0x17 tampoco) |
| `periph` | División y raíz por hardware, cartucho (B7/B8, sondeo y DMA), SPI (firmware, táctil, energía), RTC, EXTKEYIN y el arranque |
| `save` | El chip de guardado por AUXSPI (EEPROM de 0,5 KB, de 8 y 64 KB, FRAM de 32 KB y Flash): lo detecta con RDSR, RDID y los espejos de direcciones y comprueba registro de estado, WREN/WRDI, escritura sin WREN, lectura de vuelta, páginas que dan la vuelta, vuelta al final de la memoria y, en Flash, borrar página y sector, programar y escribir página. El emulador pone el chip con un archivo de guardado del tamaño correspondiente (`salsads-cpurun --save-size N`); el tipo y el tamaño detectados quedan en `+0x34` y `+0x38` del bloque de resultados |
| `ocio` | Las dos CPUs esperando el VBlank con IntrWait y nada más (decenas de instrucciones por cuadro): mide cuánto cuesta un cuadro ocioso del planificador |
| `carga` | Trabajo continuo en las dos CPUs, sin HALT: una mezcla de operaciones de datos, copias de bloques, bytes, multiplicaciones y una ordenación en Thumb (`src/carga/kernel.h`), repetida sin fin y comparada en cada vuelta con valores calculados en el host (`tools/carga-gold.sh`). Los contadores quedan en `0x023A0000` (`shared.h`); es la ROM que embebe «SalsaDS núcleo» para medir los MIPS del intérprete |
| `demo` | Demostración animada sin veredicto (la embebe «SalsaDS núcleo»): cuadrado que rebota, teclas, táctil y fondo del motor B |
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

## Correr las ROMs en una DS real (flashcart R4)

Las ROMs de pruebas (`hola`, `cpu9`, `cpuarm`, `mem`, `irq`, `dma`, `ipc`, `bios`, `periph`, `save`) escriben además un
**log de texto en la SD**. Las de gráficos (`gfx*`), `demo`, `ocio` y `carga` no.

1. Copiar los `.nds` de `homebrew/` a una carpeta de la SD (por ejemplo `SalsaDS-pruebas/`) y lanzarlos desde el menú del
   flashcart. Las ROMs usan el DLDI de libnds: el cargador del flashcart lo parchea al arrancar (lo normal en R4 con
   kernel Wood R4, Lazy, Akaio, etc.); si el kernel no lo hace, no hay SD para la ROM y la prueba corre igual, solo sin log.
2. Esperar a que termine (la mayoría, segundos; `cpu9` y `cpuarm` más). En la pantalla de arriba salen los grupos, la
   franja verde/roja y, en la fila 2, `SD:LOG OK` o `SD:NO` (sin SD o montaje fallido).
3. Si hubo divergencias en ESTE hardware (valor distinto del documentado), aparece `A: VER DIVERGENCIAS N`: **A / derecha**
   pasa de página y **B / izquierda** vuelve. Cada divergencia son dos filas: `DIV <grupo> L<línea> ARM9|ARM7` y
   `H<hw> D<doc> M<melonDS>` (amarillo si el hardware coincide con melonDS, rojo si con ninguno; `M?` = sin dato de melonDS).
   Sirve para leerlas en una foto si el log falla.
4. **START** intenta volver al menú del cargador (`exit(0)`, bootstub); si el flashcart no lo provee, la pantalla pide
   «Apaga la consola para volver al menú».
5. El log queda en `/SalsaDS-pruebas/resultados/<rom>.txt` (carpeta creada si no existe). Cada corrida se **añade** al
   final con `=== corrida N ===`. Contiene: ROM y commit con el que se construyó (`SDS_GITREV`, `+` si había cambios sin
   commitear), veredicto, los grupos de cada CPU (`ARM9`/`ARM7`) con total y fallos, el primer fallo y el registro de los
   16 primeros (grupo, identificador = línea de la fuente, obtenido, esperado) y **todas** las llamadas de divergencia con
   `hw=` (este hardware), `doc=` (GBATEK o manual de ARM) y `melon=` (melonDS 1.1, `?` si no está medido), más a qué
   coincide el hardware. Esto incluye los casos en que el hardware coincide con la documentación (melonDS equivocado), que
   son los que más interesa saber.
6. **Durante las pruebas no se toca la SD.** El log y el progreso se acumulan en un búfer de RAM (32 KB) y solo al final,
   tras restaurar el estado de libnds (`restore_libnds`: DMA y temporizadores parados, IRQ enmascaradas, EXMEMCNT, WRAMCNT,
   TCM, PU y cachés como al arrancar la ROM), se monta la SD y se escribe. Motivo: escribir a la SD con el PU y las cachés
   apagados hizo caer `bios` en la pantalla de excepción de libnds. **Si la ROM se cuelga, el búfer se pierde**: lo que
   queda es la fila 14 de la pantalla (`> GRUPO X` o `> SWI 0x13`, cada paso delicado usa `sds_progress`); no hay perro
   guardián (los temporizadores y las IRQ los usan las propias pruebas, y un cuelgue dentro de la BIOS no se puede
   interrumpir). Si hay más de 10 grupos, la lista sube una fila y se ven los últimos.
   La SWI 0x17 (Diff8bitUnFilterVram) es de la GBA y no está en la BIOS de la DS: `bios` no la llama (solo con
   `-DBIOS_SWI17`, para comparar emuladores).
7. **Excepciones del ARM9** (instrucción indefinida, abortos): las ROMs que usan `sds_init` instalan su manejador
   (`setExceptionHandler`): pintan en rojo en la pantalla de arriba el grupo, el PC, las dos palabras antes del PC
   (`I-4`, `I-8`: la instrucción ofensiva es una de ellas según el modo) y los registros, anotan lo mismo en el log y
   tratan de escribirlo (restaurando el estado). El grupo `MODOS` de `cpuarm` ya no escribe el bit T con
   `msr cpsr_c, #0xFF` (impredecible en ARMv5; en la DS real cambiaba a Thumb y ejecutaba basura): solo con `-DMSR_T_TEST`.
8. En un flashcart los grupos `CARTUCHO` y `CART ARM7` de `periph` miden el R4, no el cartucho de la ROM: si el chip ID
   (B8) es 0/FFFFFFFF o B7 no devuelve los bytes conocidos, sus comprobaciones pasan a informativas y el log lo anota
   (`NOTA: ... no aplican en flashcart`). La lectura de SQRTCNT justo tras escribir también es informativa: en hardware el
   cálculo (13 ciclos) puede haber terminado ya.

Cómo funciona (`src/common/sds.c`): al arrancar `sds_init` monta la SD con `fatInitDefault()` de libnds (FatFs + DLDI,
BlocksDS); al terminar, `sds_finish` dibuja primero la pantalla y después escribe el log con `fopen/fprintf`. El ARM7
deja sus grupos y divergencias en `SDS_X7_ADDR` (`0x023B0000`, `struct sds_x7`) para que el ARM9 los añada al log. Las
divergencias se declaran con `SDS_DIVERGE(hw, doc)` o `SDS_DIVERGE_M(hw, doc, melonds)`. En SalsaDS y melonDS no hay
DLDI: el montaje falla, `SD:NO` y todo lo demás (bloque de resultados en RAM, `melonds-run.py`, `salsads-cpurun`) queda
igual. Las ROMs se construyen con `tools/build-roms.sh` (que pasa el commit a `SDS_GITREV`).

## ROMs de gráficos 2D (`gfx*`, `src/gfx`)

Montan escenas en los motores 2D (Fase 2.1) y dejan en RAM (`0x02380800`) una lista de píxeles con color conocido
(`sds_expect_pixel`). `salsads-cpurun --expect --same-ab --vs-ref` comprueba la lista, que A y B den lo mismo y que el
compositor coincida con el renderizador de referencia; `tools/gfx-compare.py` compara el motor A con melonDS (la ROM dispara
DISPCAPCNT y melonDS entrega el banco A por GDB). `gfxrand` saca una escena aleatoria de la semilla que `tools/rom-seed.py`
escribe en el código de juego; `tools/gfx-bisect.py` y `tools/gfx-bisect-obj.py` aíslan qué característica u OBJ hace
diferir a las dos. `gfxprobe`, `gfxobjmap*` y `gfxomap*` son sondas de melonDS y no se instalan. Detalle en `docs/fase2.md`.
