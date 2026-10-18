# Fase 1 — Núcleo con intérprete

Plan del proyecto: ARM9 y ARM7 (ARM y Thumb) con intérprete, mapa de memoria con
bancos de VRAM, IRQ, temporizadores, DMA, IPC FIFO, cartucho (ROM ya
desencriptada), SPI con firmware en HLE, SWI de la BIOS en HLE y arranque directo.
Criterio: pasan las pruebas de CPU homebrew y un homebrew muestra imagen en el
modo de framebuffer.

| Paso | Estado |
|---|---|
| 1.1 Intérpretes de CPU (ARM9 ARMv5TE, ARM7 ARMv4T) | **hecho (2026-10-17)**, probado en el host y contra melonDS; sin tocar la PS3 |
| 1.2 Mapa de memoria, IRQ, temporizadores, DMA, IPC, cartucho, SPI, SWI en HLE, arranque directo y primera imagen | **hecho (2026-10-17) en el host, qemu-ppc64 (BE) y RPCS3**; probado contra melonDS; medido en la PS3 real (la demostración a 60 Hz; ver 1.3) |
| 1.3 Cierre: cuadro ocioso barato, por qué se detenían New Super Mario Bros. y Mario Kart DS, guardado AUXSPI, ROM de carga y paquete nuevo | **hecho (2026-10-17) en el host, qemu-ppc64 (BE) y RPCS3**; probado contra melonDS; **falta medir el paquete nuevo en la PS3** (SLDS00008, commit 9f2a177) |

## 1.1 Intérpretes de CPU

Código en `src/cpu/` (`acore.h` es la interfaz; `acore_priv.h`, `acore.c`,
`acore_arm.c`, `acore_thumb.c`, `acore_cp15.c`). Un solo núcleo para las dos CPUs:
`struct acore` con la bandera `v5` (1 = ARM946E-S, 0 = ARM7TDMI) para lo que cambia
entre versiones. C99 portable, sin dependencias (solo `util/endian.h`); compila sin
cambios con el `ppu-gcc` de la PS3 (`-mcpu=cell -Werror`) y con gcc y clang del host.
Código propio escrito desde GBATEK y el manual de arquitectura de ARM
(`docs/THIRD_PARTY.md`): no hay código de melonDS, NooDS ni DeSmuME.

### Qué cubre cada CPU

| | ARM7 (ARMv4T) | ARM9 (ARMv5TE) |
|---|---|---|
| ARM: proceso de datos (16 operaciones, S, inmediato rotado, registro con desplazamiento inmediato o por registro) | sí | sí |
| MUL, MLA, UMULL, UMLAL, SMULL, SMLAL | sí | sí |
| LDR, STR, LDRB, STRB (todos los modos de indexación, desplazamientos del registro, T) | sí | sí |
| LDRH, STRH, LDRSB, LDRSH | sí | sí |
| LDM, STM (cuatro modos, escritura de vuelta, `^`, PC en la lista) | sí | sí |
| SWP, SWPB | sí | sí |
| B, BL, BX, MRS, MSR (CPSR y SPSR con máscara), SWI | sí | sí |
| CLZ, QADD, QSUB, QDADD, QDSUB, SMLAxy, SMLAWy, SMULWy, SMLALxy, SMULxy | indefinidas | sí |
| LDRD, STRD | indefinidas | sí |
| BLX (inmediato y registro), BKPT, PLD (no hace nada) | indefinidas (con cond = 1111 «nunca») | sí |
| Thumb: los 19 formatos (BL en dos mitades; BLX con inmediato en ARMv5) | sí | sí |
| CP15: ID, tipo de caché, tamaño de TCM, registro de control, regiones de ITCM y DTCM (base, tamaño, espejo, modo de carga), espera de interrupción; el resto (PU, caché) se guarda y se lee de vuelta | indefinida | sí |
| Modos usuario, FIQ, IRQ, SVC, aborto, indefinido y sistema con sus bancos; CPSR y SPSR | sí | sí (con el bit Q) |
| Excepciones: reinicio, indefinida, SWI, aborto de captación y de datos, IRQ, FIQ; vectores bajos (ARM7) o bajos/altos (ARM9 por CP15) | sí | sí |
| HALT / espera de interrupción; líneas de IRQ y FIQ por nivel | sí | sí |

Interfaz pensada para el recompilador de la Fase 4 (`acore.h`):

- Todo el estado vive en `struct acore` (520 bytes en 64 bits), con los registros visibles
  (`r[16]`, `cpsr`) primero: un recompilador usa la misma estructura, las mismas
  funciones de memoria del bus y las mismas rutinas de excepción
  (`acore_exception`, `acore_set_cpsr`, `acore_switch_mode`).
- Convención de PC: entre instrucciones `r[15]` = dirección de la próxima + 4 (ARM) o
  + 2 (Thumb); durante una instrucción vale lo que lee el programa (+ 8 / + 4). Un
  salto deja `destino + 4` / `+ 2`. `acore_pc()` da la dirección real.
- Memoria por callbacks (`struct acore_bus`: 8/16/32 bits, lectura, escritura y
  captación) con **direcciones siempre alineadas**: el núcleo resuelve la rotación del
  LDR, el LDRH impar de ARMv4, los accesos de bloque y SWP. La ITCM y la DTCM las lleva
  el propio núcleo (su ubicación sale de CP15) con la memoria que le conecta el dueño
  (`acore_attach_tcm`). El mapa de la Fase 1.2 implementa los callbacks y puede poner
  la tabla de páginas de punteros por dentro; el JIT la pondrá en línea.
- Abortos: un callback llama a `acore_signal_abort()`; el núcleo toma la excepción
  tras la instrucción **sin efecto de la escritura de vuelta** (modelo «base
  restaurada»).
- Ciclos: contador grueso (`cycles`: 1 por instrucción más los ciclos internos y de
  salto de la ARM7TDMI; una excepción 2–3). Los callbacks suman sus esperas con
  `c->cycles += n`. `acore_run(c, hasta)` ejecuta hasta ese ciclo; `acore_step` una
  instrucción. No es exacto en ciclos (no-objetivo del plan); la Fase 1.2 lo afina con
  el mapa de memoria.
- Interrupciones: `acore_set_irq` / `acore_set_fiq` ponen la línea (en la DS, IME & IE & IF);
  `acore_halt` / `acore_wake` ponen y quitan el HALT (que termina con IE & IF != 0 aunque
  IME o el bit I enmascaren la IRQ: el mapa de la 1.2 llama a `acore_wake` en ese caso).
- HLE de la BIOS: `swi_hook(c, comentario)`; si devuelve distinto de cero, la SWI se da
  por atendida y no se toma la excepción (el comentario de la SWI es `insn & 0xFFFFFF`
  en ARM y `insn & 0xFF` en Thumb; la función de la BIOS es `>> 16` en ARM).
- Lo que el JIT tendrá que añadir y aquí no está: invalidación de bloques por escritura
  de código (un gancho en la ruta de escritura del bus), conteo de ciclos por bloque y
  la entrada a una IRQ entre bloques.

Tablas de despacho: 4096 punteros por versión para ARM (bits 27–20 y 7–4) y 1024 para
Thumb (bits 15–6). Código `.text` ≈ 68 KB en el PPE más 80 KB de tablas (punteros de 8
bytes). La clasificación (`classify` en `acore_arm.c`, `th_classify`) es lo único que
conoce la codificación.

### Reglas por versión que implementa (la bandera `v5`)

| Caso | ARMv4T (ARM7) | ARMv5TE (ARM9) | Fuente |
|---|---|---|---|
| LDRH con dirección impar | lee el halfword alineado y lo rota 8 | lee el halfword alineado, sin rotar | GBATEK, alineaciones |
| LDRSH con dirección impar | se porta como LDRSB (byte) | halfword alineado con signo | ídem |
| LDR / LDM / POP con PC | sin cambio de estado (bit 0 ignorado) | el bit 0 del valor cambia a Thumb | GBATEK, LDR y LDM |
| MOV pc, ADD pc… | sin cambio de estado | sin cambio de estado | ídem |
| STM con la base en la lista y escritura de vuelta | la base original si es el primer registro, la nueva si no | siempre la original | GBATEK, LDM/STM |
| LDM con la base en la lista y escritura de vuelta | sin escritura de vuelta (queda lo cargado) | escritura de vuelta si es el único o no es el último | ídem |
| LDM/STM con lista vacía | transfiere R15; la base cambia ±0x40 | no transfiere; la base cambia ±0x40 | ídem |
| MULS / UMULLS… | N y Z; C y V sin cambio (en ARMv4 C queda «destruido»: no se modela) | N y Z; C y V sin cambio | GBATEK, multiplicación |
| CPSR | sin bit Q (los bits 27–8 no se guardan) | bit Q | ARM ARM |
| Condición 1111 | «nunca» | BLX con inmediato, PLD; el resto es indefinido | ARM ARM |
| BKPT | indefinida | aborto de captación (LR = dirección + 4) | GBATEK, excepciones |
| MSR | no cambia T; en usuario solo las banderas; modo inválido se ignora | ídem | GBATEK, PSR |

Comunes: STR y STM guardan el PC como dirección + 12; el PC se lee como + 8 (+ 12 con
desplazamiento por registro en ARM; + 4 en Thumb); LDRD/STRD con Rd impar o r14 son
indefinidos (decisión propia: la documentación dice «impredecible»). LR de cada
excepción (ARM / Thumb): indefinida y SWI `A+4` / `A+2`; aborto de captación `A+4` /
`A+4`; aborto de datos `A+8` / `A+8`; IRQ y FIQ `A+4` (la instrucción que no se ejecutó).

### Verificación

1. **Pruebas unitarias en el host** (`tests/unit/test_acore_{arm,thumb,sys}.c` y el banco
   común `acore_tm.h`, ≈ 8,0 M de comprobaciones; `tools/build.sh test-host`): en little-endian (gcc nativo del
   contenedor, con ASan y UBSan) y en big-endian (qemu-ppc64). La ALU, el desplazador y
   el Thumb se comparan con el modelo en C de `cpu9` (validado en melonDS); el resto,
   a mano por clase. Se comprobaron con **mutaciones** (12 cambios deliberados al
   núcleo: acarreo de la resta, rotación del LDRH, escritura de vuelta del LDM, LR del
   aborto, bit 0 del BL, carry del LSL por 32, bit Q, interworking de LDR, máscara del
   registro de control, SP de POP, T en la entrada de excepciones, STM con la base):
   las 12 se detectan.
2. **ROMs de prueba corridas en los intérpretes** (`salsads-cpurun`, `src/host/`):
   arranque directo con la máquina mínima de `src/host/ndsmin.c`, el `crt0` y libnds
   reales de BlocksDS (CP15, TCM, DMA, IPCSYNC con el ARM7). `hola` (5), `cpu9`
   (65 187 comprobaciones del ARM9) y `cpuarm` (65 409 en el ARM9 más 61 144 en el ARM7)
   dan **OK**, también compilados para PPC64 big-endian y corridos en qemu.
3. **Contra melonDS 1.1** (`tools/melonds-run.py --arm7`): `hola`, `cpu9` y `cpuarm`
   dan **OK**. En `cpuarm` melonDS coincide en todo lo comprobado salvo los casos de
   borde de la tabla siguiente, que la ROM anota como *divergencia* sin contarlos como
   fallo.

`cpuarm` (nueva, `tests/roms/src/cpuarm/`, ver `tests/roms/README.md`): los grupos de
`cpu9` en el ARM9 y los de ARMv4T en el ARM7, más acceso no alineado, LDM/STM con la
base en la lista, listas vacías, PC y banco de usuario, modos y SPSR, cambio de estado
(BX, BLX, `ldr pc`, `ldm {pc}`, `pop {pc}`, `mov pc`), los formatos 5 a 19 de Thumb, las
excepciones del ARM9 con los vectores en la ITCM y CP15 con las TCM. Suites de terceros:
no hay RockWrestler ni armwrestler en la Mac (se buscó); no se copió ninguna.

### Divergencias melonDS 1.1 contra la documentación

Los intérpretes de SalsaDS siguen GBATEK / el manual de ARM (y lo comprueban las
pruebas unitarias). **Ninguno de estos casos está verificado en una DS real**; si el
autor tiene hardware, son los que conviene contrastar antes de apoyarse en ellos.

| Caso | Documentación (SalsaDS) | melonDS 1.1 | Efecto práctico |
|---|---|---|---|
| ARM7: LDRH impar | halfword alineado rotado 8 | halfword alineado sin rotar | raro en juegos |
| ARM7: LDRSH impar | como LDRSB | halfword alineado con signo | raro |
| LDM/STM con lista vacía (ARM7 y ARM9) | base ±0x40; ARM7 transfiere R15 | sin cambio de base, sin transferencia | muy raro |
| ARM7: STM con la base en la lista, no la primera | guarda la base nueva | guarda un valor intermedio (base + 4 en la prueba) | raro |
| ARM7: STMIA de Thumb con la base en la lista | la base nueva | la original | raro |
| STM {pc} | dirección + 12 (como STR) | dirección + 8 (STR sí guarda + 12) | raro |
| ARM9: LDMIA de Thumb con la base primera de la lista | escritura de vuelta (ARMv5) | sin escritura de vuelta | raro |
| ARM7: bit Q del CPSR | no existe (0) | se guarda | raro |
| BKPT (ARM9) | aborto de captación | instrucción indefinida | raro (se usa para cuelgues) |
| TCM en modo de carga (bits 17 y 19 de CP15) | las lecturas van a la memoria principal, las escrituras a la TCM | sigue leyendo de la TCM | lo usan los cargadores de la SDK; revisar en la 1.2 |

### Lo que descubrió la ROM

- **libnds deja el PU del ARM9 configurado**: la página 0 (región 2, 4 KB) sin acceso
  para atrapar los NULL, y solo las regiones que usa; leer la ITCM por su espejo en 0 o
  poner los vectores en 0 aborta (en melonDS y, por el manual, en el hardware). Las
  pruebas que lo necesitan apagan el PU y las cachés (limpiando antes la de datos).
- **libnds deja las IRQ habilitadas** (IME y el bit I del CPSR): una IRQ en medio de las
  pruebas que mueven las TCM o cambian de modo rompe el programa. La ROM enmascara las
  interrupciones al arrancar (`msr cpsr_c, #0xDF`).
- **El ARM9 no ve lo que escribe el ARM7 en la memoria principal** (lo más probable: la
  caché de datos que modela melonDS): una lectura repetida del bloque de resultados del
  ARM7 devolvía un valor viejo aunque se invalidara por MVA, se limpiara toda la caché
  por índices o se leyera por el espejo de +4 MB, mientras el stub de GDB veía el valor
  nuevo. El ARM7 avisa de cómo terminó por IPCSYNC (E/S, que no pasa por la caché) y el
  detalle se lee con el emulador.
- **La ARM9 tiene prioridad en el bus de la memoria principal**: un giro continuo
  leyéndola deja sin acceso a la ARM7 (la espera en `cpuarm` gira en registros y consulta
  IPCSYNC de vez en cuando).
- El stub de GDB de melonDS soporta `g` (registros, incluidos los bancos y los SPSR) y
  pasos (`s`); una lectura de 0x340 bytes en un solo paquete no contestó, en trozos de
  0x100 sí. `tools/melonds-run.py` lee ahora el bloque, el registro de fallos y las
  divergencias en trozos y muestra el progreso si la ROM no termina.

### Rendimiento del intérprete (host)

Solo el **host**; no vale para la PS3 (`docs/perf-log.md`). `tools/cpurun.sh bench`
(`armbench`), 0,5–1 s por programa, resultado de cada bucle comprobado contra el
calculado en C antes de medir:

| Programa | Mac, clang -O2 (MIPS) | Contenedor, gcc -O2 (MIPS) |
|---|---|---|
| `mezcla` (el de la 0.3: 9 instr. por elemento) | 182,0 | 192,3 |
| `suma64` (ADDS + ADC) | 135,5 | 125,5 |
| `ramas` (dos CMP + salto por elemento) | 133,6 | 124,9 |
| `bytes` (LDRB/STRB, 9 instr. por byte) | 183,5 | 197,7 |
| `arm-mix` (LDM, ADD/EOR con desplazamientos, MLA, STR, SUBS, BNE) | 134,4 | 144,3 |
| `thumb-mix` (LDRB, ADDS, LSLS, EORS, STRB, CMP, BNE) | 186,0 | 125,5 |
| ROMs `cpu9` / `cpuarm` completas (ambas CPUs, con el arranque) | 104,8 / 106,3 | 79,9 / 109,7 |

Referencia: el intérprete mínimo de la 0.3 corre el mismo `mezcla` a 244–277 MIPS en la
misma Mac y dio **18,1 MIPS en la PS3**. El núcleo completo (todas las clases,
condiciones, modos, abortos, ciclos) es ≈ 1,5× más lento que aquel en el host
(182 contra 269 de `goto`), así que en el PPE cabe esperar del orden de 12 MIPS
**[est: escala del host, sin medir]**, lejos de los ~100 que pide la DS: el
recompilador (Fase 4) sigue siendo obligatorio, y el intérprete queda para arrancar,
depurar y como respaldo. Medir en la PS3 es trabajo de la Fase 1.2 (cuando el núcleo
corra una ROM allí).

### Pendiente de 1.1

- Probar en la PS3 real (rendimiento del intérprete en el PPE, `-mcpu=cell`): compila,
  no se ha corrido allí.
- Contrastar en hardware las divergencias de arriba.
- Excepciones de datos por el PU del ARM9 (permisos por región): hoy no se modelan; los
  abortos solo vienen del bus. Lo necesitarán los juegos que atrapen NULL con el PU.
- Caché de instrucciones y de datos del ARM9: no modeladas (los juegos que dependen de
  la incoherencia con DMA no se podrán reproducir; melonDS sí las modela).
- Ciclos exactos (esperas de la memoria principal, ARM9 vs ARM7, ITCM/DTCM sin espera).
- LDRD/STRD con direcciones no alineadas a 8: se tratan como dos palabras alineadas.

## 1.2 La máquina de la DS (`src/core`, `src/bios`)

Código propio escrito desde GBATEK (páginas por tema de problemkaputt.de, consultadas el 2026-10-17: ver
`docs/THIRD_PARTY.md`) y comparado, solo en comportamiento, con melonDS 1.1; nada copiado de ningún emulador.
Sustituye a la máquina mínima de la 1.1 (`ndsmin`, retirada): `salsads-cpurun` corre ahora las ROMs sobre
`struct nds` (≈ 3 400 líneas en `src/core` y `src/bios`; el estado ocupa 5,12 MB: `sizeof struct nds`, en `docs/memory-budget.md`).

| Archivo | Qué hace |
|---|---|
| `src/core/nds.h` | El estado entero de la máquina (`struct nds`), la API pública y las constantes de tiempo; la frontera para el ARM7 en otro hilo está en su cabecera |
| `src/core/mem.c` | Mapa de memoria de las dos CPUs, VRAM por bancos, callbacks del bus |
| `src/core/io.c` | Reparto de los registros de E/S (0x04000000) por dirección y tamaño |
| `src/core/sched.c` | Planificador de eventos, IRQ, DISPSTAT/VCOUNT, teclas, temporizadores, DMA |
| `src/core/ipc.c`, `cart.c`, `spi.c`, `math.c` | IPC (SYNC y FIFO), cartucho (ROMCTRL, ROMDATA, AUXSPI), SPI (firmware generado, táctil, energía) y RTC, DIVCNT/SQRTCNT |
| `src/core/nds.c` | Arranque de la estructura, ROM, ejecución, entrada y la imagen de las dos pantallas |
| `src/bios/bios.c`, `boot.c` | Bloques de BIOS propios, SWI en HLE, arranque directo y SoftReset |

### Mapa de memoria

| Región | ARM9 | ARM7 |
|---|---|---|
| 0x00000000 | ITCM (la lleva el núcleo; región virtual de 32 MB por CP15, espejo en 0x01000000) | BIOS propia (vectores y manejador de IRQ; 16 KB, el resto en cero) |
| 0x02000000 | RAM principal 4 MB, espejo cada 4 MB hasta 0x02FFFFFF | igual |
| 0x03000000 | WRAM compartida (32 KB) según WRAMCNT: 0 = todo, 1 = 2.ª mitad, 2 = 1.ª mitad, 3 = nada (lee cero); espejo en todo el rango | 0x03000000–0x037FFFFF: la parte compartida del ARM7 (3 = todo, 1 = 1.ª, 2 = 2.ª) o, con 0, espejo de la WRAM del ARM7; 0x03800000–0x03FFFFFF: WRAM del ARM7 de 64 KB repetida |
| 0x04000000 | E/S (los puertos sin definir leen 0); 0x04100000 FIFO de IPC y 0x04100010 datos del cartucho | E/S del ARM7; la WiFi (0x04800000) lee 0 |
| 0x05000000 / 0x07000000 | paleta y OAM (2 KB cada una, espejo); **las escrituras de 8 bits se ignoran** | no existen (leen 0) |
| 0x06000000 | VRAM por ventanas (abajo) | VRAM de los bancos C y D asignados al ARM7, 256 KB repetidos; aquí **sí** se escriben bytes |
| 0x08000000–0x0AFFFFFF | ranura de GBA vacía: lee unos (0xFFFF…, 0xFF en la parte de RAM) si la ranura es del ARM9, ceros si no | igual |
| 0xFFFF0000 | BIOS propia de 4 KB | — |

VRAM (GBATEK, «DS Memory Control - VRAM»): nueve bancos (A–D 128 KB, E 64, F 16, G 16, H 32, I 16 = 656 KB) en
un solo arreglo en el orden del modo LCDC. VRAMCNT los asigna por páginas de 16 KB a las ventanas de fondo A
(512 KB), fondo B (128 KB), OBJ A (256 KB), OBJ B (128 KB) y ARM7 (256 KB); cada ventana se repite dentro de su
región de 2 MB. Dos bancos sobre la misma página se leen con OR y se escriben los dos. Los usos que la CPU no ve
(texturas, paletas extendidas: MST 3, 4 y 5 de varios bancos) se anotan en `vramcnt` para las Fases 2 y 3.
Verificado con 43 asignaciones (banco, MST, OFS) escritas por la ventana y leídas de vuelta como LCDC.

### Tiempo, planificador y la frontera para el ARM7 en otro hilo

La base es el reloj del ARM9 (67,027964 MHz, `n->time`); el ARM7 corre a la mitad y el bus y los
temporizadores a 33,513982 MHz (= ciclos del ARM7). Una línea son 355 puntos × 6 ciclos de bus = **4260 ciclos
del ARM9**; un cuadro 263 líneas = 1 120 380 ciclos (59,8261 Hz). El HBlank empieza en el punto 256 (3072).

- `nds_sched_run`: tramos de `n->quantum` ciclos del ARM9 (128 por omisión) acotados por el siguiente evento; el
  ARM9 corre hasta el final del tramo y el ARM7 hasta la mitad. Una CPU puede pasarse por lo que dura su última
  instrucción (y por las esperas de un DMA que ella disparó): ese error de pocos ciclos es el que se acepta.
  Una CPU en HALT sin interrupción solo adelanta su reloj.
- Eventos (`EV_*`, uno por fuente): inicio de línea, inicio del HBlank, desborde de cada uno de los ocho
  temporizadores, el cartucho y el SPI. **El video no tiene estado**: línea, posición y banderas de DISPSTAT salen
  del instante en que se leen (`nds_line_at`/`nds_dot_at`); los eventos de línea solo sirven para las IRQ y los
  disparos de DMA. Los temporizadores son perezosos (contador y t0) con un evento por desborde.
- Las esperas de memoria **no** se modelan: 1 ciclo por instrucción en las dos CPUs (la ARM9 real corre a ~⅓ de
  eso con la RAM principal y sin caché, pero el cociente 2:1 entre las CPUs se conserva). El DMA cobra 4 (ARM9) o
  2 (ARM7) ciclos por unidad a la CPU dueña.

**Frontera para el hilo del ARM7 (Fase 4; `docs/fase0.md` 0.7).** Cada CPU posee su estado (`struct acore` y
`struct nds_cpuio`: IME/IE/IF, temporizadores, DMA, DISPSTAT, KEYCNT, registros generales). Lo único compartido
entre las dos es: (1) la memoria (RAM principal y WRAM compartida), (2) el IPC (`ipcsync_*` y las dos colas de
FIFO, cada una de un productor y un consumidor) y (3) las líneas de IRQ que una CPU levanta en la otra
(`nds_irq_raise`, el único camino de IRQ entre CPUs). Los registros de video, WRAMCNT y VRAMCNT los escribe solo el
ARM9; el cartucho, el SPI y el RTC pertenecen a quien tenga la ranura o al ARM7. Con el ARM7 dormido en un
semáforo de lv2 y despertado cada ≥ 100 µs (la latencia medida en la 0.7: ≈ 6700 ciclos del ARM9), el cambio es:
`nds_irq_raise` pasa a un OR atómico en IF más el aviso al hilo, el ARM7 lleva sus propios eventos (sus
temporizadores y el SPI) y los avisos entre CPUs llevan marca de tiempo. **Prueba de que la frontera aguanta**:
`tools/build.sh test-host` repite irq, ipc, dma y periph con tramos de 1, 16, 2048 y 20 000 ciclos
(20 000 ≈ 0,3 ms, 3 veces el despertar medido) y las nueve ROMs dan OK.

### Periféricos

| Pieza | Lo que está y lo que no |
|---|---|
| IRQ | IME/IE/IF por CPU con las 25 fuentes (IF se limpia escribiendo 1); línea = IME & IE & IF; el HALT del ARM7 termina con IE & IF ≠ 0 aunque IME esté en 0 (GBATEK); el WFI del ARM9 solo con la línea. Las IRQ de FIFO son por flanco |
| Video (tiempos) | VCOUNT 0–262, VBlank en 192–261, HBlank en cada línea (también en el VBlank), LYC de 9 bits, IRQ de VBlank/HBlank/VCOUNT por CPU. DISPSTAT y VCOUNT salen del instante de lectura |
| Teclas | KEYINPUT, KEYCNT (O/Y, IRQ por flanco), EXTKEYIN (X, Y, lápiz, tapa con su IRQ) desde `nds_set_input` |
| Temporizadores | 4 por CPU, prescalers 1/64/256/1024, cuenta ascendente encadenada, IRQ de desborde, recarga al arrancar, 32 bits como recarga + control. El bit de cuenta ascendente del timer 0 hace que no cuente nunca (como melonDS) |
| DMA | 4 canales por CPU; inicio inmediato, VBlank, HBlank (solo líneas visibles), «inicio de pantalla» (una transferencia por línea visible), cartucho (una palabra por aviso), repetir, destino con recarga, relleno por DMAFILL, IRQ de fin, cuenta de 21 bits (ARM9) o 14/16 (ARM7). No ven las TCM. Sin modo de memoria principal ni de FIFO de geometría |
| IPC | IPCSYNC con su IRQ, FIFO de 16 palabras por sentido con error, vaciado, lectura con la FIFO apagada y las IRQ de «envío vacío» y «recepción no vacía» por flanco (también al habilitarlas con la condición cierta) |
| Cartucho | ROM ya desencriptada: ROMCTRL, ROMCMD, ROMDATA por sondeo o por DMA, bloques de 4 bytes a 16 KB, IRQ de fin, B7 (lectura; por debajo de 0x8000 se lee 0x8000 + (dir & 0x1FF); da la vuelta en la página de 4 KB; se espeja más allá de la capacidad), B8 y 90h (identificador), 00h (cabecera), AUXSPI con el chip de guardado de la 1.3 (`save.c`). Sin KEY1/KEY2 |
| SPI | Firmware de 256 KB generado (cabecera con la dirección de los datos de usuario, dos copias con CRC16, JEDEC, WEL; desde la 1.3 también se escribe y se borra, en memoria), pantalla táctil (X, Y, Z1, Z2, 12 y 8 bits, cola de respuesta de 2 bytes; sin tocar: X = 0, Y = 0xFFF) con calibración exacta (ADC = 0x120 + 14·x; 0x0B8 + 19·y), gestión de energía (registros 0–4), IRQ de fin de byte. El RTC es el del puerto 0x04000138 con el protocolo bit a bit, fecha por BCD y 24 horas |
| Matemáticas | DIVCNT (tres modos, entre cero, -MAX / -1), SQRTCNT (32 y 64 bits), bandera de ocupado por tiempo; el resultado está listo al escribir |
| Varios | POSTFLG, HALTCNT, POWCNT1/2, EXMEMCNT/EXMEMSTAT, WRAMCNT/WRAMSTAT, VRAMCNT/VRAMSTAT. Los registros de los motores 2D, el 3D, el sonido, SIO y la captura solo se guardan y se leen de vuelta |

### BIOS en HLE

El bloque de BIOS de cada CPU lo genera `nds_bios_init`: tabla de vectores con salto a sí mismos, un manejador de
IRQ de seis o nueve instrucciones (ensambladas aparte con `arm-none-eabi-as` y comprobadas con su `objdump`) que
guarda r0–r3, r12 y lr y salta a `[DTCM + 0x3FFC]` (ARM9, base por CP15) o `[0x0380FFFC]` (ARM7, vía
0x04000000 − 4), y una instrucción de retorno para los callbacks. Las SWI se atienden en C por `swi_hook` (en el
ARM9 solo con los vectores altos; si el programa instala los suyos, la SWI es una excepción de verdad). Una
llamada sin implementar se anota una vez y se ignora; una CPU que cae en un vector de excepción de la BIOS sin
manejador queda en un `b .` que `salsads-cpurun` detecta e informa.

Hechas: 00h SoftReset (vuelve a la entrada de la cabecera), 03h WaitByLoop, 04h IntrWait, 05h VBlankIntrWait,
06h Halt, 07h Sleep (como Halt), 08h SoundBias, 09h Div, 0Bh CpuSet, 0Ch CpuFastSet, 0Dh Sqrt, 0Eh GetCRC16, 0Fh
IsDebugger, 10h BitUnPack, 11h LZ77 desde memoria, 12h LZ77 por callbacks (escritura de 16 bits), 13h Huffman por
callbacks, 14h RLE, 15h RLE por callbacks, 16h–18h filtros de diferencias (el 17h no figura en la lista de la DS)
y 1Fh CustomHalt. **1Ah–1Ch** (las tablas de seno, de tono y de volumen del ARM7) se hicieron en la Fase 2.2 (`docs/fase2.md`). IntrWait/VBlankIntrWait/Halt ponen la CPU en HALT y reejecutan
la SWI al despertar, con una bandera por CPU que recuerda que ya se descartaron los avisos viejos; las banderas
están en [DTCM+0x3FF8] / [0x0380FFF8] y las pone el manejador del programa (el de libnds lo hace).

### Arranque directo (de dónde sale cada valor)

Nada sale de la BIOS de Nintendo. Cabecera (0x170 bytes) en 0x027FFE00 y datos de usuario (0x70 bytes, la copia más
nueva del firmware generado) en 0x027FFC80: GBATEK. Identificador del chip en 0x027FF800 y 0x027FFC00 e indicador de
arranque normal en 0x027FFC40: convención de los cargadores directos (no verificada en GBATEK). ARM9: modo sistema
con I y F, r12 = lr = pc = entrada, CP15 con la ITCM de 32 KB (región virtual de 32 MB, que es lo que libnds usa
en 0x01000000) y la DTCM de 16 KB en 0x00800000 habilitadas, vectores altos (control = 0x00052078); pilas propias
SVC 0x00803FC0, IRQ 0x00803FA0 y sistema 0x00803EC0 (donde la BIOS suele dejarlas; libnds fija las suyas). ARM7:
lo mismo con pilas 0x0380FFDC, 0x0380FFB0 y 0x0380FF00. E/S: POSTFLG = 1, WRAMCNT = 3, EXMEMCNT = 0x6000,
KEYINPUT = 0x03FF, POWCNT1 = 0x0001, POWCNT2 = 1, IME = 0. El crt0 de BlocksDS hace un apretón de manos por IPCSYNC con el
otro procesador: los dos lados funcionan solos con esta máquina.

### Primera imagen

Modo de visualización directa de VRAM del motor A (DISPCNT bits 16–17 = 2, banco A–D por los bits 18–19, que tiene
que estar asignado al LCDC), el color de fondo (paleta 0) para el modo de gráficos del motor B y del A (**sin capas:
son de la Fase 2**), pantalla blanca para el modo 0, brillo maestro (MASTER_BRIGHT) y el intercambio de pantallas
(POWCNT1.15). `nds_screens` da las dos imágenes en RGB555; `salsads-cpurun --screens f.ppm` las vuelca (256×384).

**Contra melonDS** (`tools/melonds-run.py --vram m.ppm --regs` y `tools/compare-image.py`): `hola` y `mem` dan la
**misma imagen píxel a píxel** (256×192 idénticos) y los mismos DISPCNT, POWCNT1 y VRAMCNT en los dos. La demostración
(`demo`, animada) no puede compararse píxel a píxel (el ritmo difiere), pero corre en melonDS con los mismos
registros y la misma composición.

### ROMs propias y resultados

Cada ROM corre en las dos CPUs con el marco de pruebas de la 1.1 (resultado en RAM y en pantalla) y se construye
con `tools/build-roms.sh`; las comprobaciones salen de GBATEK, no de un emulador. **Todas dan OK en SalsaDS (little y
big-endian) y en melonDS 1.1**; lo que melonDS hace distinto y GBATEK no dice con claridad se anota como divergencia
(no cuenta como fallo, `docs/fase1.md` más abajo).

| ROM | Qué prueba | Comprobaciones ARM9 + ARM7 |
|---|---|---|
| `hola`, `cpu9`, `cpuarm` | las de la 1.1: ahora sobre la máquina nueva, con IRQ de verdad | 5; 65 187; 65 409 + 61 144 |
| `mem` | RAM y espejos, WRAM en los 4 modos vista por las dos CPUs, 9 bancos de VRAM en 43 asignaciones, solapes, espejos, VRAM del ARM7 y VRAMSTAT, paleta y OAM, puertos sin definir | 218 + 13 |
| `irq` | VCOUNT y DISPSTAT, IRQ de VBlank/HBlank/VCOUNT, prescalers, cascada, IRQ de temporizadores, IME/IE/IF, HALT (con IME = 0 en el ARM7), teclas; en las dos CPUs con libnds | 59 + 61 |
| `dma` | copias de 16/32 bits en los 4 canales, modos de dirección, relleno, IRQ de fin, canales simultáneos, VBlank con repetición, HBlank, VRAM y paleta, ancho de la cuenta del ARM7 | 68 + 63 |
| `ipc` | IPCSYNC y su IRQ, FIFO (llena, error, vaciado, FIFO apagada, IRQ por flanco) desde las dos CPUs | 65 |
| `bios` | Div, Sqrt, GetCRC16, CpuSet/CpuFastSet, LZ77 (memoria y callbacks), RLE, Huffman 8 y 4 bits, BitUnPack, diferencias, y que las SWI conserven r4–r11 | 54 + 49 |
| `periph` | DIV/SQRT por hardware, cartucho (B7/B8 por sondeo y por DMA, IRQ de fin, AUXSPI), SPI (firmware, táctil, energía), RTC, EXTKEYIN, puertos, estado de arranque, el ARM7 con la ranura | 73 + 28 |
| `demo` | demostración animada (sin veredicto): `test-host` comprueba que sus dos CPUs viven | — |

### Divergencias de melonDS 1.1 (contra GBATEK y SalsaDS)

Ninguna está verificada en una DS real. Las que SalsaDS no comparte se anotan en cada ROM como `SDS_DIVERGE`.

| Caso | GBATEK / SalsaDS | melonDS 1.1 |
|---|---|---|
| DMA en HBlank | una transferencia por línea visible | unas pocas más que las líneas (104 en la línea 100); en el VBlank no corre |
| DMA modo 3 («inicio de pantalla») | una por línea visible | una sola por habilitación |
| Lectura de una FIFO de IPC vacía | la última palabra | lo que quedó en esa posición del anillo |
| TMxCNT_H, SQRTCNT, EXMEMCNT | los bits sin uso leen cero | guarda todo lo escrito |
| IE con escritura de 8 bits; DIV_NUMER con 16 | funcionan | se ignoran |
| −MAX / −1 (resto) | no se dice | deja un valor viejo |
| Timer 0 con cuenta ascendente | (no se dice) | no cuenta; SalsaDS lo copia |
| Cartucho tras el arranque | comandos 00h/90h valen antes de KEY1 | ya está en KEY2: 00h y 90h no sirven (B7/B8 sí) |
| B7 por debajo de 0x8000; lectura que cruza 4 KB | 0x8000 + (dir & 0x1FF); da la vuelta en la página | otra cosa; no da la vuelta |
| Firmware | 256 KB generado | 128 KB; Z1 sin tocar = 0xFFF (SalsaDS: 0) |
| Ranura de GBA vacía | (no se dice) lee unos | 0xFE08 en 0x08000000 |
| **BIOS libre**: Div | resto con el signo del numerador (GBATEK: −1234 / 10 da −4) | valor absoluto; INT_MIN / 1 no termina |
| BIOS libre: Sqrt | 0xFFFF con las entradas de 32 bits más grandes | 0 |
| BIOS libre: Huffman (SWI 0x13) | descomprime | no la tiene |
| BIOS libre: Halt del ARM7 | conserva todos los registros | destruye r0 y r2: libnds7 la llama sin declarar clobbers, así que el compilador deja valores vivos y las ROMs que usan `swiHalt` fallaban según la disposición del código |

Dos hallazgos de formato que no dependen de ningún emulador: la variante de 16 bits del LZ77 por callbacks no admite
la distancia 1 (lee la posición anterior antes de escribirla; los vectores de la ROM `bios` usan distancia mínima
2) y en Huffman por callbacks el `r2` solo le llega al callback de apertura (libnds pasa 0): GBATEK habla de un búfer
temporal que la BIOS real no necesita.

### Rendimiento del intérprete con las dos CPUs y el planificador (host)

Solo el **host**, no vale para la PS3 (`docs/perf-log.md`). ROMs completas con arranque, IRQ, DMA y las dos CPUs;
Mac, clang -O2, tramo de 128 ciclos (el tiempo en HALT no cuesta):

| Programa | MIPS (ARM9 + ARM7 juntos) |
|---|---|
| `cpuarm` / `cpu9` (casi puro cómputo) | 99–106 (Mac); 114–126 (contenedor, gcc) |
| `irq`, `dma`, `ipc`, `periph`, `bios` (E/S, IRQ, esperas) | 79–85 |
| Mario Kart DS, Super Mario 64 DS, New Super Mario Bros. (600 cuadros) | 102–124 |

Coste del planificador, `cpuarm` en la Mac: tramo de 1 ciclo 27,8 MIPS; 16 → 77,2; 128 → 99,5; 1000 → 108,5;
20 000 → 103,7 (ruido de ±5 en corridas de 0,07 s). Con 128 ciclos por tramo el planificador cuesta ≈ 6 % frente a no
partir. En la PS3 el núcleo corre del orden de 12 MIPS **[est: escala del host, sin medir]**; el paquete mide el
tiempo de emulación por cuadro y lo escribe en su log (`DATO nucleo`).

### Paquete de PS3 «SalsaDS núcleo» (SLDS00008)

`tests/phase1/nucleo`: corre `struct nds` sobre una ROM y saca las dos pantallas con la salida de la 0.6 (rsxkit:
dos texturas de 256×192 RGBA5551 por cuadro, cuatro distribuciones con L2, escala ajustada, 720p o 1080p), el DS3 como
botones de la DS y el puntero del táctil (`controles.cfg` de la 0.6) y un cuadro de la DS por cada vsync. ROM: **la
demostración propia embebida** (`tests/roms/src/demo`); si existe `/dev_hdd0/salsads/rom.nds`, esa (un respaldo del
autor ya desencriptado o una ROM propia: se lee entera a memoria; las de 16 y 32 MB caben, las más grandes habrá que
verlas con `docs/memory-budget.md`). Línea de estado con L3 + R3. Log en
`/dev_hdd0/salsads/nucleo.log` con la versión (commit) y el APPID en la primera línea y una línea `DATO nucleo` cada
120 cuadros (emulación de un cuadro: mediana, p95 y máximo; intervalo del RSX; instrucciones por cuadro de cada CPU).
En RPCS3 (`tools/rpcs3-run.py nucleo`, prueba automática): 150 cuadros con entrada sintética, las dos CPUs de la
demostración vivas y su imagen la esperada: **RESULTADO nucleo: OK** (no mide rendimiento).

### Qué faltaba al cerrar la 1.2 (resuelto o reubicado en la 1.3; la lista vigente está al final del documento)

1. **Probarlo en la PS3 real**: que el paquete arranque, los MIPS del intérprete en el PPE (la estimación es ≈ 12), los
   60 Hz con la demostración, el DS3 y el puntero del táctil, y una ROM por `rom.nds`.
2. Guardados (AUXSPI: EEPROM, Flash, FRAM) y su archivo; SWI 1Ah–1Ch (sonido, Fase 2).
3. Averiguar por qué Mario Kart DS (el ARM7 lee el firmware sin parar por SPI y el video nunca se configura) y New
   Super Mario Bros. (los dos procesadores quedan esperándose con las IRQ enmascaradas) se detienen: hace falta una
   traza por juego; Super Mario 64 DS arranca más lejos (configura video, carga ~100 KB de VRAM y sigue con su lógica;
   escribe a la FIFO de geometría, que aún no existe).
4. Del plan que quedó fuera: DMA en modo de memoria principal y de FIFO de geometría, captura de pantalla, modo 3 de
   visualización (memoria principal), permisos y cachés del ARM9 (de la 1.1), esperas de memoria y ciclos exactos.
5. Contrastar en una DS real las divergencias de las dos tablas (la de la 1.1 y la de arriba).

## 1.3 Cierre de la Fase 1

Cinco cosas: el cuadro ocioso, por qué se detenían dos juegos, el guardado del cartucho, una ROM que mide los MIPS y un
paquete nuevo. Todo con ROMs propias; los respaldos del autor solo se usaron a mano para diagnosticar (nada de ellos
entra en el repositorio, ni capturas ni volcados).

### Cuadro ocioso barato

**Medido en la PS3 real** (paquete de 400ca88, `DATO nucleo` de la demostración): un cuadro casi ocioso (4,2 k
instrucciones del ARM9 y 114 del ARM7 por cuadro) costaba **2,73 ms** de emulación, y los de 28–39 k instrucciones,
8,4 ms; con New Super Mario Bros. como `rom.nds`, 114,5 ms con 539 k + 240 k instrucciones. Una recta por esas ventanas da
**≈ 2,1 ms fijos por cuadro + 0,16 µs por instrucción** (≈ 6,2 MIPS): 2,1 + 0,16 × 4,2 k = 2,8 ms y 2,1 + 0,16 × 39 k =
8,4 ms. El costo fijo no era de las CPUs.

**Causa.** El planificador cortaba el tiempo en tramos de 128 ciclos del ARM9 aunque las dos CPUs estuvieran en HALT o
IntrWait: un cuadro son 1 120 380 ciclos, **8753 tramos**, y cada uno recorría dos veces los 18 eventos, llamaba dos
veces al intérprete (que solo adelantaba el reloj) y comprobaba el HALT del ARM7. En el host eso son 177 µs por cuadro
ocioso; en el PPE, ≈ 0,24 µs por tramo (≈ 750 ciclos), es decir los ~2,1 ms.

**Corrección** (`sched.c`): con las dos CPUs sin interrupción pendiente el tiempo salta de una vez al siguiente evento (no
hay pasos finos); con una sola ociosa, esa solo adelanta su reloj sin llamar al intérprete; el mínimo de los eventos se
guarda en una caché en lugar de recorrerlos en cada tramo. Los resultados son idénticos (mismas instrucciones por cuadro
e imágenes iguales a las del binario anterior en `demo`, `irq`, `dma`, `ipc` y `periph`; las nueve ROMs de prueba y los
tramos de 1 a 20 000 ciclos siguen dando OK).

| Mac, clang -O2, host (no vale para la PS3) | Antes (400ca88) | Después | |
|---|---|---|---|
| `ocio` (las dos CPUs en IntrWait), 600 cuadros | 0,106 s = 177 µs por cuadro; 8753 tramos por cuadro | 0,0145 s = **24 µs por cuadro**; 13,5 tramos, 525 saltos ociosos y 526 eventos | ×7,3 |
| `demo` (≈ 4,7 k instrucciones por cuadro), 600 cuadros | 0,132 s = 220 µs por cuadro | 0,040 s = **67 µs** | ×3,3 |
| `carga` (las dos CPUs ocupadas), 120 cuadros | 146–148 MIPS | 145–146 MIPS | sin cambio |

Lo que queda por cuadro ocioso son los ~525 eventos de video (263 líneas y 263 HBlank): ≈ 24 µs en la Mac, **[est] ≈ 0,3 ms en
el PPE por escala**. Si el `DATO desglose` de la PS3 muestra que pesa, el siguiente paso es agendar los eventos de línea y de
HBlank solo cuando algo los usa (IRQ de HBlank/VCOUNT, DMA armado en esos modos) y dejar solo el de VBlank.

**Desglose en el log de la PS3** (`nucleo.log`, cada 120 cuadros, tres líneas): `DATO nucleo` (tiempo de emulación limpio, intervalo
del RSX, instrucciones por cuadro); `DATO desglose` (por cuadro, mediana en ms: emulación = ARM9 + ARM7 + eventos +
planificador; y aparte entrada, pantallas, conversión RGB555 → RGBA5551, dibujo, `frame_begin` y `frame_end`; más tramos,
saltos ociosos y eventos por cuadro) y `DATO MIPS` (instrucciones entre el tiempo dentro de cada CPU, y entre el tiempo
total de emulación). Los cuadros impares llevan un reloj de perfil dentro del planificador (el timebase: dos lecturas
por tramo) y los pares no, para que el tiempo «limpio» no incluya el costo de medir.

### Por qué se detenían New Super Mario Bros. y Mario Kart DS

Método: traza de instrucciones y de entradas y salidas por CPU en el corredor del host (`--trace`, `--break7/9` con anillo
de las últimas instrucciones y pila, `--watch7/9`, `--dbg` para SPI, IPC, IRQ y cartucho), y comparación con melonDS 1.1
leyendo su memoria y sus registros por el stub de GDB (`tools/melonds-dump.py`). Sirvió más comparar el estado final
(DISPCNT de los dos motores, VRAMCNT y el archivo de guardado) que buscar una primera instrucción distinta: las dos
máquinas no avanzan al mismo ritmo.

**New Super Mario Bros.** Ya configuraba el video (a los 250 cuadros: DISPCNT del motor A = C821103D, VRAMCNT igual que en
melonDS) y 5 cuadros después las dos CPUs dejaban de correr: el ARM9, desde una máquina de estados del propio juego, mandaba
un mensaje por la FIFO de IPC y esperaba una respuesta, y las dos quedaban en un bucle de HALT con las IRQ enmascaradas (la
rutina de apagado del programa). Lo que lo disparó fue el **chip de guardado**: desde el arranque el ARM7 lee el registro de
estado del chip por AUXSPI cada 5 ms (orden 05h) y SalsaDS contestaba FFh para siempre (sin chip: WIP puesto), lo que el juego
toma por un cartucho fallado. (La causa se confirmó añadiendo el chip: no se desensambló más allá de lo necesario para llegar ahí.)
Con una EEPROM de 8 KB el juego pasa de ahí y queda **idéntico a melonDS**: a los 900 cuadros DISPCNT_A = C8211F3D,
DISPCNT_B = 00011400 y VRAMCNT = 82 83 8B 81 83 85 8C 03 81 82, los mismos valores que melonDS a los 15 s, y el archivo de
guardado de 8192 bytes que el juego escribe es **byte a byte el que escribe melonDS**.

**Mario Kart DS.** El ARM7 leía el firmware «sin parar» y el video no se configuraba: lo que hacía era leer 1028 bytes del
firmware en 0x3FA00 (la zona de la configuración de Wi-Fi), activar WREN, escribir la página con la orden 0Ah, esperar con
RDSR y leerla de vuelta para verificarla, y repetirlo para siempre porque el firmware de SalsaDS aceptaba las escrituras
y las ignoraba. Con escrituras (WREN, 0Ah, 02h, DBh y D8h) que se ven al leer de vuelta, el juego configura el video como
melonDS: a los 100 cuadros DISPCNT_A = 00010600, DISPCNT_B = 00010305 y VRAMCNT = 81 00 84 82 ..., iguales; a los 900,
DISPCNT_A = 40011E10, DISPCNT_B = 40011C10 y VRAMCNT = 83 81 84 82 84 82 8A 03 82 82, iguales a los de melonDS a los 16 s.

**Super Mario 64 DS** (que no cambió con esta fase) llega al mismo punto que melonDS: el ARM9 espera en la misma dirección que en melonDS
a los 3 y a los 6 s, y DISPCNT de los dos motores y VRAMCNT son iguales. Lo que le falta de ahí es la geometría (la FIFO de comandos
del 3D, Fase 3).

De paso se igualó con melonDS la RAM compartida del arranque (la segunda copia del identificador del chip, los CRC16 de la cabecera
y del área segura y 5835h en 0x027FFC10): no era la causa de nada, pero son valores que dejan los cargadores y un juego puede mirarlos.

### Guardado del cartucho (AUXSPI)

`src/core/save.c`: EEPROM de 0,5 KB (dirección de 8+1 bits, órdenes 03h/0Bh/02h/0Ah), EEPROM de 8 y 64 KB y FRAM de 32 KB (16
bits), EEPROM de 128 KB (24 bits) y Flash de 256 KB, 512 KB y 1 MB (24 bits: lectura 03h y rápida 0Bh, escribir página 0Ah,
programar 02h, borrar página DBh y sector D8h, RDID), con WREN/WRDI, WRSR, el registro de estado (WIP siempre 0, WEL, WP, SRWD;
en la EEPROM de 0,5 KB los bits 4–7 en uno), páginas que dan la vuelta (16, 32 o 128 bytes en EEPROM; 256 en Flash; sin páginas
en FRAM) y vuelta al final de la memoria. La conversación sigue el bit de retener de AUXSPICNT y el bit de ocupado sale por tiempo
según la velocidad (4, 2, 1 MHz y 512 kHz). Fuente: GBATEK, «DS Cartridge Backup»; código propio.

El tipo se decide así: (1) el tamaño del archivo de guardado, si hay uno (512 B; 8 KB y 64 KB como EEPROM; 32 KB como FRAM; 128 KB
como EEPROM de 24 bits; 256 KB, 512 KB y 1 MB como Flash); (2) una tabla de tres entradas por código de juego (los tres
respaldos del autor; solo datos, tomados de GBATEK y del archivo que crea melonDS); (3) si no, EEPROM de 64 KB. El RDID de un
chip de Flash contesta 20h 40h y 12h/13h/14h (ST M45PE) según el tamaño. Por detectar «por el comportamiento» solo hay lo que
GBATEK describe (RDSR/RDID y bytes de dirección): una SDK declara el tipo y no lo pregunta, así que sin archivo ni tabla un
juego con otro chip verá uno equivocado hasta que haya archivo; es la limitación principal. Tope de 1 MB (la Flash de 8 MB
de un solo juego queda fuera). Escrituras y borrados terminan al instante y la protección por bloques se guarda sin aplicarse.

`nds_save_attach` carga un archivo, `nds_save_data` y `nds_save_take_dirty` dejan al frontend escribirlo. En «SalsaDS núcleo»: archivo por
juego en `/dev_hdd0/salsads/saves/<código de juego>.sav`, que se carga al arrancar y se escribe (por un archivo temporal)
unos segundos después de que el juego escriba y al salir. En el host: `salsads-cpurun --save FILE` y `--save-size N`.

**Prueba.** ROM propia `save`: detecta el chip con RDSR, RDID y los espejos de dirección y comprueba, según el tipo, estado,
WREN/WRDI, escritura sin WREN, lectura de vuelta, páginas, vuelta de la memoria y la Flash (borrado, programar y escribir página).
Pasa en los siete tamaños en little endian, big endian (qemu-ppc64) y con sanitizadores. Contra melonDS 1.1, con el código de
juego de New Super Mario Bros. (EEPROM de 8 KB) y de Mario Kart DS (Flash de 256 KB) puesto en la cabecera de una copia
(`tools/melonds-run.py --gamecode`), pasa con estas divergencias, anotadas en la ROM:

| Caso | GBATEK / SalsaDS | melonDS 1.1 |
|---|---|---|
| Escritura que cruza el fin de una página (EEPROM y Flash) | da la vuelta dentro de la página | sigue en la página siguiente |
| Borrar página (DBh) o sector (D8h) de Flash | deja FFh | deja 00h |
| Programar página (02h) de Flash sobre un byte con bits en 1 | solo baja bits (Y de lo que había) | deja 00h |
| RDID (9Fh) de una Flash | 20h 40h 12h (ST) | FFh FFh FFh |
| Escribir o borrar Flash | WIP siempre 0 (al instante) | WIP puesto un tiempo largo; las lecturas durante él devuelven 00h |

### ROM de carga y MIPS del intérprete

`tests/roms/src/carga`: las dos CPUs ocupadas sin entrar nunca en HALT con una mezcla de operaciones de datos con desplazamientos, copias y
sumas de bloques de palabras (LDM/STM), recorridos de bytes, multiplicaciones (MLA y producto largo) y una ordenación de mitades
compilada en Thumb (`kernel.h`), repetida sin fin; cada vuelta se compara con el valor esperado, calculado en el host con el
mismo `kernel.h` (`tools/carga-gold.sh`). Una iteración son ≈ 300 000 instrucciones del ARM9 y del ARM7 (con las semillas
distintas por vuelta); los contadores quedan en la RAM compartida. Da lo mismo en little y big endian: en 120 cuadros, 273
iteraciones del ARM9 y 174 del ARM7, sin errores.

**Host** (no vale para la PS3): 145–146 MIPS en la Mac (clang -O2) y 134–139 en el contenedor (gcc -O2), ARM9 y ARM7 juntos. Con
las dos CPUs ocupadas el ARM9 consume el reloj completo (1 120 380 instrucciones por cuadro) y el ARM7 la mitad.

**PS3: sin medir.** Referencia con lo ya medido (demostración y New Super Mario Bros. del 400ca88): ≈ 6–7 MIPS en total (ARM9 4,7 y
ARM7 2,1 con New Super Mario Bros.), por debajo de los ~12 que daba la escala del host. A ≈ 6,5 MIPS, un cuadro de `carga`
(1,68 M de instrucciones) costaría ≈ 260 ms; el `DATO MIPS` del paquete da el número real.

### Paquete de PS3 «SalsaDS núcleo» (SLDS00008, 1.3)

Mismo título y mismo APPID que el de la 1.2, construido desde el commit 9f2a177 (árbol limpio): `build/salsads-nucleo/salsads-nucleo.pkg`
(copia: `build/salsads-nucleo-9f2a177.pkg`), SHA-256 `8da0871ecb4e16e97569e3250055f0f4aef3d7ca8722e568dc627662408f92a4`. Lee
`/dev_hdd0/salsads/rom.nds` si existe; si no, corre la ROM de carga embebida. En RPCS3 (no mide rendimiento): `RESULTADO nucleo: OK`
(cuadros de la DS, iteraciones de las dos CPUs sin errores).

**Qué medir en la PS3**, con el log `/dev_hdd0/salsads/nucleo.log` (bajarlo por FTP al terminar):

1. **Carga embebida** (sin `rom.nds`): `DATO MIPS` (ARM9, ARM7 y juntas, dentro de cada CPU y sobre el tiempo total), `DATO desglose` (reparto del cuadro) y que los cuadros duren 16,7 ms o cuántos ms de más. Es el número que sustituye a la estimación de 12 MIPS.
2. **Cuadro ocioso**: con la demostración (`demo.nds` como `rom.nds`) la emulación de un cuadro debe bajar de 2,7 ms a unos 0,3–0,7 ms (el costo por instrucción sigue igual) y el desglose debe mostrar ~13 tramos, ~525 saltos y ~526 eventos por cuadro.
3. **New Super Mario Bros. y Mario Kart DS** como `rom.nds` (los respaldos del autor): deben dejar de apagarse (NSMB) y de quedarse leyendo el firmware (MKDS); en el log, que el guardado diga `guardado: ... chip tipo ...` y que aparezca `guardado escrito: ... .sav` unos segundos después de que el juego guarde. Sin capas 2D no hay imagen de juego todavía: se ve el color de fondo y los brillos.
4. Que el DS3 y el puntero táctil sigan igual que en la 1.2.

### Qué queda pendiente al cerrar la Fase 1

1. **Medir el paquete nuevo en la PS3** (lista de arriba).
2. SWI 1Ah–1Ch de la BIOS (tablas de seno, de tono y de volumen del ARM7): hechas en la Fase 2.2 (`docs/fase2.md`).
3. Super Mario 64 DS escribe a la FIFO de geometría, que aún no existe (Fase 3); hasta ahí coincide con melonDS.
4. Guardado: detección sin archivo ni tabla (hoy EEPROM de 64 KB), Flash de 8 MB, escritura con tiempo (WIP) y protección por bloques; ampliar la tabla de códigos de juego cuando haya más respaldos.
5. Eventos de video perezosos (arriba) si el desglose de la PS3 lo pide.
6. Del plan de la 1.2 que sigue fuera: DMA en modo de memoria principal y de FIFO de geometría, captura de pantalla, modo 3 de visualización (memoria principal), permisos y cachés del ARM9 (de la 1.1), esperas de memoria y ciclos exactos.
7. Contrastar en una DS real las divergencias de las tablas de la 1.1, la 1.2 y la de arriba (ninguna está verificada en hardware).
8. El intérprete en el PPE es lento (≈ 150 ns por instrucción en la PS3, del orden de 470 ciclos): el recompilador (Fase 4) sigue siendo obligatorio; mientras tanto, la ruta de memoria por llamadas a funciones es el sospechoso principal (a verificar con el perfil, no se ha tocado).

La fase siguiente (motores 2D reales) está en `docs/fase2.md`.
