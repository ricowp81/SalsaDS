# Fase 1 — Núcleo con intérprete

Plan del proyecto: ARM9 y ARM7 (ARM y Thumb) con intérprete, mapa de memoria con
bancos de VRAM, IRQ, temporizadores, DMA, IPC FIFO, cartucho (ROM ya
desencriptada), SPI con firmware en HLE, SWI de la BIOS en HLE y arranque directo.
Criterio: pasan las pruebas de CPU homebrew y un homebrew muestra imagen en el
modo de framebuffer.

| Paso | Estado |
|---|---|
| 1.1 Intérpretes de CPU (ARM9 ARMv5TE, ARM7 ARMv4T) | **hecho (2026-10-17)**, probado en el host y contra melonDS; sin tocar la PS3 |
| 1.2 Mapa de memoria, IRQ, temporizadores, DMA, IPC, cartucho, SPI, SWI en HLE, arranque directo y primera imagen | por hacer (plan abajo) |

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

## 1.2 Plan: mapa de memoria, E/S y primera imagen

Orden propuesto (cada paso con su ROM de prueba propia en `tests/roms/src`, comparada con
melonDS como `cpuarm`, y corrida en los intérpretes con `salsads-cpurun` en LE y BE):

1. **Mapa de memoria** (`src/core/`): los callbacks de `acore_bus` por región y por CPU:
   memoria principal (4 MB espejada hasta 0x02FFFFFF), WRAM compartida con WRAMCNT, WRAM
   del ARM7 (64 KB), paleta, OAM, **VRAM por bancos** (A–I con VRAMCNT; tabla de páginas de
   16 KB por CPU y por uso: LCDC, fondo/objetos A y B, textura, paleta extendida, ARM7),
   cartucho (ranura de GBA vacía), BIOS (ver 5), ITCM/DTCM ya en el núcleo, E/S por
   registro con tabla. Sustituye a `ndsmin.c` (que queda para las pruebas de CPU o se
   retira). Tabla de punteros por región para la ruta rápida; los sitios de escritura
   quedan marcados para la invalidación de código de la Fase 4.
2. **Planificador y tiempo**: una cola de eventos con el reloj del ARM9 como base (67 MHz,
   ARM7 a la mitad), intercalado en rebanadas (el ARM7 en un hilo aparte es la Fase 4:
   `docs/fase0.md`, 0.7). Las esperas de memoria entran por `c->cycles` desde los
   callbacks. Medir el costo de la rebanada con el intérprete.
3. **Interrupciones**: IME/IE/IF por CPU, fuentes (VBlank, HBlank, VCount, temporizadores,
   DMA, IPC, cartucho, teclas, ...), línea a `acore_set_irq`; HALTCNT del ARM7 y la espera
   de interrupción de CP15 del ARM9 (ya en el núcleo: `halted`).
4. **Temporizadores** (4 por CPU, encadenados, prescalers) y **DMA** (4 por CPU, modos de
   inicio: inmediato, VBlank, HBlank, memoria principal, cartucho, FIFO de sonido; las
   transferencias pasan por el mapa y respetan el PU del ARM9 solo si se modela).
5. **BIOS en HLE**: bloque de BIOS propio generado por el emulador (vectores que saltan al
   manejador del programa: ARM9 `[DTCM + 0x3FFC]`, ARM7 `[0x0380FFFC]`) y las SWI por
   `swi_hook` (Halt, VBlankIntrWait, IntrWait, Div, Sqrt, CpuSet, CpuFastSet, CRC16,
   BitUnPack, LZ77, Huffman, RLUnComp, SoundBias, GetCRC16, IsDebugger...). Nada de BIOS
   de Nintendo ni desensamblados: el contrato sale de GBATEK y de lo que usan las ROMs.
   `ndsmin.c` ya tiene lo mínimo (IsDebugger, Div, Sqrt, CpuSet, CpuFastSet).
6. **IPC**: IPCSYNC (ya hecho en `ndsmin.c`), IPCFIFOCNT/SEND/RECV (FIFO de 16 palabras
   por sentido, IRQ de «vacía» y «no vacía») y la memoria compartida.
7. **Cartucho**: ROM ya desencriptada (el autor aporta el respaldo; nada de claves ni
   KEY1/KEY2 en el repo), ROMCTRL/comandos de lectura, AUXSPI (guardado: EEPROM, flash,
   FRAM), cabecera y área segura sin cifrar.
8. **SPI y periféricos**: firmware con los datos de usuario generados (nombre, idioma,
   calibración del táctil), pantalla táctil (ADC), gestión de energía, RTC, teclas
   (KEYINPUT, EXTKEYIN), DIVCNT/SQRTCNT por hardware.
9. **Arranque directo**: cabecera copiada a 0x027FFE00, binarios cargados, estado de
   registros de la BIOS (CP15 del ARM9: ITCM 32 KB y DTCM 16 KB como los deja la
   BIOS real, `WRAMCNT = 3`, `POSTFLG`...), datos de usuario en 0x027FFC80 y el resto de
   la tabla que lee libnds.
10. **Primera imagen**: DISPCNT en el modo de visualización directa de VRAM (banco A como
    LCDC) y volcado del cuadro a PPM en el host (`salsads-run`), después al módulo de
    salida de la Fase 0.6. Criterio: `hola` y un homebrew simple muestran su texto y su
    imagen.

Pruebas por paso: ROMs propias para DMA, temporizadores, IPC y bancos de VRAM (ninguna de
terceros mientras no tenga licencia clara: `docs/fase0.md`, 0.2) y las suites de CPU ya
hechas como regresión.
