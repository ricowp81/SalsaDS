# Fase 4 — Recompilador ARMv5TE → PowerPC64 para el ARM9 (y ARMv4T para el ARM7)

El ARM9 corre en el PPE con un recompilador dinámico propio (ARM y Thumb), integrado en la máquina de la DS. El ARM7 usa
el mismo recompilador en su perfil ARMv4T (sección «El ARM7 en el recompilador», más abajo). Código propio (GPL-2.0-or-later), escrito desde el manual de arquitectura de ARM, GBATEK, la
Power ISA 2.02 y lo medido en la PS3 en la Fase 0 (§0.3 y §0.7); el diseño toma las ideas del recompilador MIPS → PPC64
de Salsa64 (bloques con enlace por parcheo de un salto, mapa de código para invalidar, caminos fríos aparte, caminos
lentos compartidos). No viene de ningún emulador (ni melonDS ni NooDS, GPLv3).

Estado: **correcto contra el intérprete en qemu-ppc64** (modo estricto y `--lockstep`). Tercera ronda (rama arm9-perf, abajo):
banderas entre bloques en el CR, ventanas caliente/fría, bucles de espera, sondeo de ROMCTRL; sin medir en la PS3. Primera medición en la PS3 (SM64DS en
juego, 2026-10-18): ARM9 13,3 ms por cuadro de trabajo (17–22 en escenas pesadas; 44–49 con el intérprete); la segunda ronda
(«Segunda ronda», abajo) está sin medir en la consola.

## Archivos

| Archivo | Qué es |
|---|---|
| `src/jit/arec.h` | API: crear, correr (`arec_run`, reemplaza a `acore_run`), mapa de la ruta rápida, avisos de escritura a código, estadísticas |
| `src/jit/arec_dec.c` | Decodificación de ARM y de Thumb a una representación común (`struct ai`), con los ciclos del intérprete |
| `src/jit/arec_emit.c` | Emisión de PowerPC64: banderas, operandos, memoria, saltos, salidas; rutinas comunes (entrada, salida, caminos lentos) |
| `src/jit/arec.c` | Despachador, caché de bloques, enlaces, destinos indirectos, invalidación, ayudantes en C |
| `src/jit/ppcenc.h` | Codificación de PowerPC (ampliada desde la 0.7) |
| `src/core/jit.c` | Conexión con la máquina: vista de captación, tabla de páginas, avisos (bus, ITCM, captura de sonido, CP15, WRAMCNT) |
| `src/host/jitrun.c` | `salsads-cpurun --jit`, `--jit-strict`, `--jit-far`, `--jit-nojl`, `--lockstep` |
| `tests/unit/test_arec.c` | Máquina mínima: programas al azar y casos dirigidos contra el intérprete |
| `tests/phase4/nucleo-jit/` | Paquete «SalsaDS JIT núcleo» (el programa de `tests/phase1/nucleo` con `NUCLEO_JIT`) |

## Cómo corre

### Registros

| PPE | Uso |
|---|---|
| r14–r28 | r0–r14 del guest, siempre (los bloques se enlazan sin pasar por memoria), con la parte alta en cero |
| r29 | `(ciclos − límite) << 16 \| instrucciones` desde la entrada (16 bits): una suma por salida lleva las dos cuentas |
| r30 | tabla de páginas de lectura (la de escritura está 2 MB después) |
| r31 | `struct acore` |
| r0, r3–r12 | temporales (r0 nunca como base de `addi` ni de un acceso: vale 0) |
| cr2–cr4 | banderas (no volátiles: sobreviven a las llamadas a C de los caminos lentos); entre bloques, en sus bits canónicos: N cr2.lt, Z cr2.eq, C cr3.lt, V cr4.lt |
| cr0, cr1, cr5–cr7 | temporales |

La entrada desde C (`arec_enter`, generada al principio de la caché) guarda r14–r31 y el CR, carga los registros
guest de `c->r[]` y salta al bloque; la salida común los devuelve a memoria, suma ciclos e instrucciones a `c->cycles` y
`c->icount` y vuelve con el tipo de salida (pc, enlace, estado en memoria; y si fue por presupuesto) y las banderas que
quedaron exactas.

### Bloques, presupuesto y modos

(Desde la tercera ronda la caché se reparte en ventanas de 32 KB: la sección caliente de cada bloque crece desde el principio de
la ventana y la fría desde el final, así que el código que corre queda junto; `AREC_OPT_NO_SPLIT` vuelve a ponerlas seguidas.)

Un bloque va desde `pc` (con el bit 0 = Thumb; el modo de la CPU no entra en la clave: los registros con banco se
cambian en memoria) hasta un salto incondicional, un salto condicional hacia atrás o 64 instrucciones; los saltos
condicionales hacia adelante quedan dentro (el camino tomado sale enlazado). El BL de Thumb se junta en una instrucción si
las dos mitades están en el bloque.

El prólogo de cada bloque (también al llegar por un enlace) mira el presupuesto:

- **modo rápido** (el de la PS3): corre si `ciclos < límite`; puede pasarse del final del tramo lo que dura el bloque (el
  intérprete se pasaba lo que dura una instrucción). Los eventos del planificador se atienden igual al final del tramo; lo
  que cambia es que la CPU los ve hasta un bloque tarde.
- **modo estricto** (`--jit-strict`, `--lockstep`): corre solo si cabe entero (`ciclos + máximo del bloque ≤ límite`);
  si no, el despachador ejecuta con el intérprete hasta el final del tramo. Mismos ciclos, instrucciones y momento de cada
  acceso que el intérprete.

Un acceso lento puede sumar ciclos (esperas del bus, la DMA que dispara una escritura, el motor de geometría lleno): el
ayudante deja `c->cycles` y el código lo recarga. En modo estricto, si lo que queda del bloque ya no cabe, sale.

Los ciclos son los del intérprete (1 por instrucción más los que suma cada manejador; MUL con los de su multiplicador,
calculados en línea); las instrucciones condicionales que no se ejecutan cuestan 1.

### Enlaces y destinos indirectos

Cada salida a dirección fija es un `b` a un trozo de la sección fría que vuelve al despachador con «enlázame»; el
despachador traduce el destino si hace falta y parchea el `b` para que salte directo al prólogo del destino. Al invalidar un
bloque se desparchean sus enlaces entrantes. Los saltos indirectos (BX, POP/LDM con pc, MOV pc, BLX) buscan el destino
en una tabla de 16 384 entradas (`pc | Thumb` → código) con `mtctr`/`bctr`; si no está, vuelven al despachador, que
vuelve a meter el bloque en la tabla (si otro bloque con el mismo índice lo había sacado, ya no se queda fuera). Las
llamadas a C son `bl` si llegan (en la PS3 la caché está detrás del texto) y si no `ld`/`mtctr`/`bctrl`.

### Banderas NZCV

Decisión de la 0.7: CR por instrucción, sin `mfcr` ni formas con punto en el camino caliente.

- Un análisis del bloque (hacia atrás: banderas vivas; hacia adelante: quién produce cada bandera y quién la lee) decide
  qué emite cada instrucción que fija banderas. Una resta (CMP, SUBS, RSBS) deja hasta tres comparaciones: `cmpw` en cr4
  (GE, LT, GT, LE salen de un bit), `cmplw` en cr3 (CS, CC, HI, LS) y `cmpwi res, 0` en cr2 (N, Z); V, si se necesita
  suelto, es `cr2.lt ^ cr4.lt`. Una suma: N/Z en cr2, el acarreo con `cmplw res, a` y V calculado en cr4. Las lógicas: N/Z
  y el acarreo del desplazador. Las condiciones que mezclan productores se arman con operaciones del CR (`crand`,
  `crxor`…) en un bit temporal.
- **Entre bloques las banderas viven en el CR** (tercera ronda, abajo): al entrar a un bloque están en sus bits canónicos (N
  cr2.lt, Z cr2.eq, C cr3.lt, V cr4.lt) y cada salida lleva las vivas ahí con una operación del CR por bandera que no esté ya
  (`canonicalize`). La entrada y la salida comunes las pasan entre el CR y `c->jit_nzcv` (mfcr una vez por salida al
  despachador); las llamadas al intérprete que leen o escriben banderas van por `ent_callf`, que hace lo mismo alrededor de la
  llamada. Las instrucciones de datos condicionales con S se traducen: antes y después de ellas las vivas quedan en sus bits
  canónicos. Lo que sigue en esta sección es la forma anterior, que queda con `AREC_OPT_FLAGS_MEM`.
- Forma anterior: las banderas se escriben en memoria (`c->jit_nzcv`, en enteros, ~13 instrucciones) **solo en la instrucción que las fija
  y solo si están vivas** en una llamada al intérprete o en una salida del bloque. «Vivas en una salida» sale de una vista
  adelante del código de destino: se decodifica en línea recta (siguiendo B y BL) hasta que las cuatro se escriben; si se lee
  alguna antes o hay un salto condicional, un salto indirecto o una llamada al intérprete, se dan por vivas. Lo mirado se
  limita a las cubetas de 1 KB del propio bloque y se suma a su rango de invalidación (si ese código cambia, el bloque se
  traduce otra vez).
- Una salida a mitad de bloque (después de un acceso lento que levantó una interrupción, o de CP15) saca las banderas del
  CR con `mfcr` y un ayudante (camino frío). En modo estricto salen las cuatro exactas en toda salida (la interrupción guarda
  el CPSR entero en el SPSR).
- Las instrucciones condicionales que fijan banderas van al intérprete (las dos ramas dejarían banderas distintas).
- Si el análisis no dejó una bandera ni en el CR ni en memoria (no debería pasar), el bloque se descarta y esa instrucción
  la ejecuta el intérprete (`bloques descartados por el análisis de banderas`: 0 en todas las pruebas).

### Memoria

- **Ruta rápida**: tabla de 262 144 punteros (páginas de 16 KB, todo el espacio de 32 bits, 2 MB) para leer y otra para
  escribir: RAM principal (con sus espejos), WRAM compartida del ARM9, y encima la DTCM y la ITCM como las ve la CPU según
  CP15 (base, tamaño, modo de carga), en el orden del intérprete. Acceso: `rlwinm`, `ldx`, `cmpdi`, `beq` frío, `rlwinm` y
  `lwbrx`/`lhbrx`/`lbzx` (o los de escritura): unas 6 instrucciones más la dirección. LDR no alineado (rota) va por la ruta
  lenta; LDRH/LDRSH del ARMv5 alinean.
- **Ruta lenta**: lo demás (E/S, VRAM, paleta, OAM, BIOS) por las funciones del intérprete (`mem_rd*`/`mem_wr*`), con
  `c->cycles` al día. Un camino lento compartido por tipo (lectura, escritura, intérprete) guarda lo necesario y llama al
  ayudante; desde el bloque son ~6 instrucciones en la sección fría.
- **LDM/STM**: una sola búsqueda de página si toda la lista cae en la misma página de 16 KB; si no, el intérprete hace la
  instrucción entera.
- La tabla se rehace cuando CP15 cambia las TCM y cuando cambia WRAMCNT (y se vacía la caché: la vista de captación
  cambió).

### Invalidación

- El espacio físico con código posible es el tramo de `struct nds` de `itcm` al final de `swram` (ITCM, DTCM, RAM, WRAM
  seguidas). Cada bloque registra su rango físico en cubetas de 1 KB y marca las líneas de 64 B que toca.
- Las páginas de escritura de 16 KB con código traducido tienen el puntero de escritura en NULL: las escrituras del código
  generado ahí van por la ruta lenta, que avisa. Las escrituras del intérprete, del ARM7, de la DMA y de la HLE de la BIOS
  pasan por el bus (o por la ITCM del núcleo) y avisan con una lectura del mapa de líneas.
- Una escritura a una línea con código invalida los bloques que la tocan; si alguno era el que corre (o cualquier otro), el
  bloque sale en la instrucción siguiente (el intérprete vería la instrucción nueva enseguida).
- La captura de sonido del host avisa; la del SPE de sonido escribe por DMA sin avisar (buffers de audio, no código).

### Lo que va al intérprete desde el bloque

En línea, con el intérprete solo en el camino frío (mismos ciclos: el camino frío descuenta lo que ya está pendiente):
MCR p15 c7 (caché y búfer de escritura; no-op salvo la espera de IRQ c7,c0,4 / c7,c8,2) y MRC p15 c1,c0,0 / c9,c1,0 /
c9,c1,1 fuera del modo usuario; MRS CPSR/SPSR; MSR CPSR_c sin cambio de modo (si habilita I o F con una línea pendiente sale
después de la instrucción) y MSR SPSR con banco; SWP/SWPB y LDRD/STRD por la ruta rápida (las dos palabras en la misma
página). LDR rd, [pc, #off] lee el literal al traducir si está en la memoria del propio código y dentro de sus cubetas (el
bloque se extiende a esos bytes: escribir el literal lo invalida).

Por el intérprete: MSR que cambia el modo o las banderas, SWI (la HLE de la BIOS), el resto de CP15, LDM/STM con `^` o lista vacía o (LDM) la base en la lista con escritura de vuelta,
SWP, LDRD/STRD, QADD y familia, SMULxy y familia, condicionales que fijan banderas, lo indefinido, BKPT y `BX PC` en Thumb.
La llamada deja los registros en memoria, el CPSR con las banderas y el bit T, `r15` como lo espera el intérprete y vuelve
al bloque si la instrucción no saltó, no cambió el estado, no hay una interrupción que tomar ni se invalidó código.
Excepciones, IRQ, HALT (WFI de CP15) e IntrWait salen al despachador, que las atiende con `acore_service` (las mismas
reglas que el bucle del intérprete).

### Respaldo

Sin PowerPC64 big-endian (la Mac) `arec_create` devuelve NULL y el núcleo sigue con el intérprete. En la PS3 el paquete
prueba la caché antes (hilo de prueba con plazo, marca en disco: `docs/fase0.md` §0.7); `nucleo-jit-nojit` en
`/dev_hdd0/salsads` fuerza el intérprete; en RPCS3 (texto de solo lectura, prueba automática) no se intenta.

## Cambios en el núcleo

- `struct acore`: avisos `tcm_hook` e `itcm_hook` y el estado del recompilador (`jit_*`); `acore_service`,
  `acore_step_insn`, `acore_exec_arm/thumb`, `acore_cond_ok`.
- Planificador: el ARM9 con `arec_run` si hay recompilador; modo de un tramo y decisiones forzadas para `--lockstep`.
- **Temporizadores**: al leer o escribir un temporizador se atienden antes sus desbordes ya vencidos. La CPU puede ir por
  delante del último evento; sin esto, el contador bajo (que se calcula de la hora) daba la vuelta antes de que el
  encadenado sumara uno, y una lectura de 32 bits de dos temporizadores iba hacia atrás (la ROM `irq` fallaba en modo
  rápido). Con el intérprete no cambia ningún resultado de las ROMs.
- Lecturas lentas: si atender esos desbordes levanta una IRQ, el bloque sale después de la instrucción, como con una
  escritura.

## Pruebas

- **`tests/unit/test_arec.c`** (en `salsads-unit`; en qemu-ppc64 ejecuta, en la Mac solo traduce): 300 programas al azar
  de ARM y 300 de Thumb en modo estricto (datos con los tres operandos, MUL/MLA/MULL, CLZ, cargas y almacenamientos de
  todos los tamaños por la ruta rápida, la lenta y no alineados, LDM/STM, saltos hacia adelante, vueltas condicionales por
  BX/MOV pc, MRS/MSR) y 100 de cada uno en modo rápido, contra el intérprete (registros, CPSR, ciclos, instrucciones,
  memoria); casos dirigidos: IRQ levantada por un almacenamiento a mitad de bloque, código que se reescribe dentro del
  bloque, BL/BLX/BX/POP {pc} entre estados, esperas del bus en modo estricto. Si falla, busca el prefijo más corto.
- **ROMs propias** con el recompilador en qemu (`tools/build.sh test-host`): las 12 de la lista y `carga` en modo rápido,
  tres con llamadas por CTR y sin búsqueda en línea, y las 12 con `--lockstep`.
- **`--lockstep`**: una segunda máquina idéntica con el intérprete; las dos avanzan tramo por tramo; después de cada
  bloque (sin enlaces) la referencia ejecuta las mismas instrucciones y se comparan registros, CPSR (las banderas exactas
  de esa salida), ciclos, instrucciones, bancos y los eventos del planificador; al final de cada tramo, las dos CPUs; cada
  2048 tramos, la memoria. La referencia toma las decisiones del tramo con el estado de antes (su ARM9 ya avanzó dentro
  del tramo del otro).
- **Juegos** (respaldos del autor, fuera del repositorio, `SALSADS_ROMS=… tools/qemu-ppc64.sh … /roms/x.nds`): ver
  «Resultados».

## Resultados (qemu-ppc64; los tiempos no valen)

| Prueba | Resultado |
|---|---|
| `test_arec` (600 programas estrictos, 200 rápidos, 4 dirigidos) | 812 comprobaciones, 0 fallos; 0 bloques descartados |
| 12 ROMs propias con `--jit` (modo rápido) | todas OK (hola, cpu9, cpuarm, mem, irq, dma, ipc, bios, periph, tablas, gx, geom) |
| las mismas con `--lockstep` | todas OK y sin diferencias (cpu9: 1,26 M comparaciones; irq: 3,1 M; dma: 2,8 M) |
| New Super Mario Bros., 1500 cuadros | `--lockstep` sin diferencias (99 M comparaciones, 10,4 M tramos); `--jit`: la misma imagen final que el intérprete |
| Mario Kart DS, 1500 cuadros | `--lockstep` sin diferencias (52,8 M comparaciones); `--jit`: mismo pc final de las dos CPUs; la imagen difiere (la demostración depende del momento exacto de los eventos, que en modo rápido la CPU ve hasta un bloque tarde) |
| Super Mario 64 DS, 1500 cuadros | `--lockstep` sin diferencias (56,2 M comparaciones); `--jit`: la misma imagen final |

Lo que cuenta el recompilador en esos 1500 cuadros (modo rápido; NSMB / MKDS / SM64DS):

| | NSMB | MKDS | SM64DS |
|---|---|---|---|
| bloques traducidos (instrucciones) | 6515 (47 323) | 12 981 (107 041) | 6152 (47 129) |
| palabras PPC por instrucción (caliente + fría) | 15,2 | 15,1 | 15,6 |
| código vivo al final (de 4096 KB) | 1423 KB | 2220 KB | 2828 KB |
| entradas al código generado por cuadro | 2057 | 2182 | 4509 |
| salidas por destino indirecto no encontrado, por cuadro | 177 | 337 | 1055 |
| llamadas al intérprete por cuadro | 774 (CP15 349, LDM/STM lento 210, MSR 100, MRS 94) | 2969 (SWP 1376, CP15 759, MSR 337, MRS 291) | 1819 (CP15 631, MSR 387, MRS 381, LDM/STM lento 261) |
| accesos por la ruta lenta por cuadro (lect. / escr.) | 3411 / 1462 | 5967 / 696 | 4830 / 4892 |
| invalidaciones (bloques) | 12 | 0 | 75 |

Antes de los caminos lentos compartidos y de la tabla de destinos de 16 384 entradas eran 20–24 palabras por instrucción
y NSMB salía 2460 veces por cuadro por destinos indirectos no encontrados (tabla de 4096, la mitad sin usar en ARM).

## Segunda ronda (rama fase41)

Medido en la PS3 con la primera versión (SM64DS en juego, por cuadro): 1716 entradas, 600 salidas por pc, 1091 por presupuesto,
1178 llamadas al intérprete (CP15 de caché, SWP, MRS/MSR); en escenas pesadas ~4170 entradas, 1985 salidas por pc y 2174 por
presupuesto. En el `DATO desglose` el ARM9 cuesta ~9 µs por tramo y casi nada por instrucción (un ajuste de 248 muestras: el
número de instrucciones no explica el tiempo, el de tramos sí): lo caro es entrar y salir (y, sospecha sin medir, el cambio
de CPU y de datos en las cachés del PPE), no el código traducido. Ojo al leerlo: el desglose sale de los cuadros impares y el
tiempo «limpio» de los pares; en un juego a 30 cuadros por segundo los impares son los de trabajo y los pares casi ociosos.

Cambios:

1. **Caídas al intérprete en línea** (ver arriba).
2. **Destinos indirectos**: la tabla se rellena después de un fallo. No hizo falta una pila de retornos: las vueltas de
   función ya caen casi todas en la tabla.
3. **Tramo de 1024 ciclos** con el recompilador en el paquete de la PS3 (el intérprete y `salsads-cpurun` siguen en 128;
   `--quantum N` en el host, `nucleo-jit-tramo` en la consola). El tramo ya se corta en el siguiente evento del planificador
   (`src/core/sched.c` no cambió), así que lo que se gana depende de los eventos: con 5100 eventos por cuadro (NSMB, SM64DS:
   el motor de geometría agenda muchos) los tramos bajan solo a la mitad y de 512 a 2048 casi no cambian; en MKDS (770
   eventos) bajan 6×. Pasar del evento (correr el ARM9 hasta un evento que lo afecte) es trabajo del planificador.
4. **Literales**: LDR relativo al pc leído al traducir (de ~11 instrucciones a 1–2).

Efecto en la precisión del tramo largo: el ARM7 y el ARM9 se alternan cada 1024 ciclos en vez de 128, así que una CPU ve lo
que la otra escribe (IPC, memoria compartida) hasta ~1000 ciclos tarde, además del bloque de retraso del modo rápido. Los
juegos que esperan a la otra CPU sondeando dan más vueltas: en qemu, las lecturas lentas por cuadro suben 1,5–3× (sondeo de la
E/S) y el ARM9 ejecuta un 2–10 % más de instrucciones por cuadro. Mario Kart DS, que ya cambiaba de escena con el modo rápido,
también la cambia aquí (la demostración va por otro circuito a los 1500 cuadros; con el intérprete y tramos de 1024 igual): el
juego sigue bien, el momento de los eventos no es el mismo. Las ROMs propias dan OK con tramos de 1 a 20 000 y `--lockstep`
con 1024 no tiene diferencias (las dos máquinas usan el mismo tramo).

Contadores en qemu, 1500 cuadros, por cuadro (antes: primera versión con tramo de 128; después: esta rama, tramo de 1024):

| | NSMB antes → después | MKDS antes → después | SM64DS antes → después |
|---|---|---|---|
| entradas al código generado | 4508 → 2024 | 2181 → 378 | 2057 → 1142 |
| tramos del planificador | 4335 → 2310 | 3367 → 561 | 2496 → 1417 |
| salidas por presupuesto | 3302 → 1851 | 1786 → 334 | 1863 → 1108 |
| salidas por destino indirecto no encontrado | 1054 → 30 | 337 → 3 | 177 → 17 |
| llamadas al intérprete | 1819 → 411 | 2968 → 248 | 773 → 242 |
| palabras PPC por instrucción (caliente) | 15,6 → 14,0 (7,2) | 15,1 → 13,5 (7,1) | 15,2 → 13,7 (7,2) |
| lecturas lentas | 4830 → 7107 | 5966 → 17 345 | 3410 → 5939 |

(Juegos según la cabecera de cada respaldo; en la tabla de «Resultados» de arriba las columnas de NSMB y SM64DS están
cambiadas: NSMB es el de 6152 bloques y 4509 entradas.) Lo que queda en el intérprete: LDM/STM a la E/S (la FIFO de geometría: ~250 por cuadro), la vuelta de la IRQ de la
BIOS (`SUBS pc, lr, #4` y el `LDM` de IE/IF, ~140 IRQ por cuadro en NSMB), MSR con cambio de modo y, en MKDS, CMP condicionales
(`CMPNE`, ~54 por cuadro).

Descartado: no escribir NZCV en las vueltas de función. Las funciones de comparación del EABI de ARM (`__aeabi_cfcmple` y
familia, flotante por software) devuelven el resultado en las banderas; sin saber a dónde vuelve, la salida tiene que dejarlas
exactas.

## Tercera ronda (rama arm9-perf)

Medido en la PS3 con los dos recompiladores (arm7-jit): New Super Mario Bros. en un nivel, ARM9 ~6–10 ms por cuadro; Mario Kart
DS en carrera, ~8,5–9,5 ms; en las cargas el ARM9 va al tope (22–28 ms). Meta: ARM9 ≤ 6 ms en juego y cargas más cortas.

### Perfil del host

`salsads-cpurun --jit --jit-prof PREFIJO` (en qemu-ppc64) guarda la entrada de cada bloque que corre y la descripción de cada
bloque traducido (palabras calientes por instrucción guest; con `AREC_PROF_CODE=1` también el código, para desensamblar);
`tools/jit-perfil.c` (`cc -O2 -o build/jit-perfil tools/jit-perfil.c`) lo lee: palabras por clase de instrucción, huella de
código caliente por cuadro, fallos de un modelo de L1 de instrucciones (32 KB, 2 vías) y de L2 (512 KB, 8 vías, con código C y
datos de relleno), de la ERAT de instrucciones, y los bloques más caros. `--packed` estima qué daría juntar las secciones
calientes; `--series N` ayuda a elegir escenas. Es un modelo para ordenar cambios, no da milisegundos de la PS3: con la
calibración de Salsa64 (1 ciclo por palabra, 41 por fallo de L1 y 380 por fallo de L2) explica ~2,4 ms de los ~8,5 que mide la
consola en la carrera de MKDS. `AREC_PROF_SLOW_FROM=N` cuenta los accesos lentos por dirección desde el cuadro N.

Lo que mostró (MKDS en carrera, cuadros 2500–3000; NSMB en el mapa del mundo 1 se parece):

- ~2,0 M palabras calientes por cuadro (9,9 por instrucción) y **1,2 MB de código caliente distinto por cuadro**: más que la L2.
  Perfil plano (el bloque más caro, 3 %).
- Por clase: LDR/STR 39 %, datos con S 22 % (11,4 palabras: casi toda bandera viva al salir del bloque se calculaba en enteros
  y se escribía en memoria), BX/BLX 7 % (17,5 palabras: la búsqueda de destinos copiada en cada salida), LDM/STM 11 %.
- Las secciones frías intercaladas con las calientes: con las calientes juntas la huella bajaría ~28 %.
- El bloque más corrido en los dos juegos era un sondeo de memoria (`ldr; cmp #0; bne` a sí mismo), ~4500 vueltas por cuadro.
- En las cargas, 91 000 lecturas de ROMCTRL por cuadro: el salto del sondeo de la fase46 casi nunca actuaba.
- Accesos lentos en la carrera (por cuadro): GXFIFO ~1900 escrituras, DIVCNT/SQRTCNT ~1300 lecturas, IME/IE/IF ~600. En el host
  una escritura a GXFIFO cuesta ~10–20 ns (con la geometría diferida, como en la PS3).

### Cambios

| Cambio | Interruptor (archivo en /dev_hdd0/salsads; bit de `arec_cfg.opts`) |
|---|---|
| Sondeo de ROMCTRL (y de SPICNT en el ARM7) salta al final del tramo si el evento ya venció (`nds_poll_skip`) | — (núcleo; también con el intérprete) |
| Banderas entre bloques en el CR (bits canónicos), `ent_callf`, datos condicionales con S traducidos | `jit-banderas-memoria` (`AREC_OPT_FLAGS_MEM`) |
| Pista estática «no se toma» en los saltos a caminos raros | `jit-sin-pistas` (`AREC_OPT_NO_HINTS`) |
| Búsqueda común de destinos indirectos (`ent_bx` / `ent_jl`) | `jit-salidas-copiadas` (`AREC_OPT_JL_INLINE`) |
| Ventanas de 32 KB: lo caliente abajo, lo frío arriba | `jit-sin-ventanas` (`AREC_OPT_NO_SPLIT`) |
| Bucles de espera: la vuelta consume el resto del tramo | `jit-sin-esperas` (`AREC_OPT_NO_IDLE`) |
| Desalineación de LDR en el índice de la tabla de páginas | solo `AREC_OPT_ALIGN_CHECK` (sin archivo) |
| MUL, LDM/STM, CMP #0, MOV con desplazamiento: menos palabras | — |

Con `jit-tiempos` el paquete mide con el timebase dónde se va el tiempo del ARM9 (`DATO jit-tiempo`: código generado neto,
accesos lentos, intérprete, despachador y traducción, µs de código por entrada) y el costo de cada acceso lento por región
(`DATO jit-lentos`, que sin el archivo da solo cuántos y los bucles de espera saltados).

### Resultados (host, qemu-ppc64; el modelo no son tiempos de la PS3)

Mismo corredor y mismos cuadros, con todas las formas anteriores (`--jit-opts 63`) contra las mejoras (tramo de 1024, modo
rápido). MKDS: la demostración en carrera, cuadros 2500–3000; NSMB: el mapa del mundo 1 con 3D, cuadros 3800–4400 (la partida del
autor; el nivel no se alcanzó con el guion de teclas).

| | MKDS antes → después | NSMB antes → después |
|---|---|---|
| instrucciones del ARM9 por cuadro (contador) | 202 600 → 183 900 (bucles de espera) | 297 600 → 283 500 |
| palabras calientes corridas por cuadro | 1 929 000 → 1 439 000 (−25 %) | 3 003 000 → 2 322 000 (−23 %) |
| palabras por instrucción | 9,52 → 7,82 | 10,09 → 8,19 |
| huella de código caliente por cuadro | 1202 → 780 KB (−35 %) | 612 → 420 KB (−31 %) |
| fallos del modelo: L1 de instrucciones / L2 | 41 300 / 10 200 → 24 200 / 6800 | 46 800 / 5400 → 26 800 / 4000 |
| modelo (1 ciclo por palabra, 41 por fallo de L1, 380 de L2) | 2,35 → 1,57 ms (−33 %) | 2,18 → 1,54 ms (−29 %) |
| entradas al código generado en 3000 / 4400 cuadros | 2,40 M → 2,29 M | 5,25 M → 5,25 M |
| datos con S, palabras por instrucción | 11,4 → 4,2 | 11,2 → 3,8 |
| BX/BLX, palabras | 17,5 → 5,5 | 17,8 → 6,1 |

Por cambio (MKDS, mismos cuadros): banderas en el CR, pistas y búsqueda común −15 % de palabras y −10 % de huella; ventanas −22 %
de huella (sin cambiar las palabras); bucles de espera −9 % de instrucciones (el sondeo pasa de 4440 a 225 vueltas por cuadro);
MUL/LDM/CMP #0/MOV y la alineación de LDR, −2…−4 % de palabras. Cargas (MKDS, cuadros 50–150, con el intérprete en la Mac): el ARM9
ejecuta un tercio de las instrucciones; el corredor, 5,4 → 1,6 ms por cuadro.

Estimación para la PS3 **[est]**: si el tiempo del ARM9 sigue a la huella y a las palabras como en Salsa64 (Δt ≈ 0,58·Δpalabras
+ 0,08·Δlíneas), −20…−30 % en juego (MKDS ~8,5–9,5 → ~6–7 ms; NSMB nivel ~6–10 → ~4,5–7,5 ms) y cargas a la mitad o menos; la
meta de 6 ms depende de cuánto pesen los accesos lentos y el despachador, que es lo que mide `jit-tiempos`.

Paridad: `tools/build.sh test-host` verde (LE, BE, SAN, test_arec con las dos formas, ROMs con `--jit`, `--lockstep`, resumen del
estado, ciclos); con los respaldos, 1500 cuadros de NSMB, SM64DS y MKDS: `--lockstep` sin diferencias y el mismo resumen del
estado que el intérprete en modo estricto (NSMB 109 M comparaciones, SM64DS 62 M, MKDS 75 M).

### Qué medir en la PS3

1. Mismas escenas (NSMB en un nivel, MKDS en carrera, una carga de cada uno) con el paquete tal cual: `DATO desglose` (ARM9),
   `DATO jit`, `DATO jit-lentos`. Comparar con los logs de arm7-jit.
2. Con `jit-tiempos`: cuánto es código generado, cuánto accesos lentos (y cuáles), cuánto intérprete y despachador. Decide la
   siguiente ronda (código: más compacto; accesos lentos: caminos rápidos para GXFIFO / divisor; despachador: tramos).
3. A/B de cada interruptor (uno por vez, misma escena, varias ventanas): `jit-banderas-memoria`, `jit-sin-pistas`,
   `jit-salidas-copiadas`, `jit-sin-ventanas`, `jit-sin-esperas`.

## Cuarta ronda (rama gx-rapido): la FIFO de geometría

Medido en la PS3 con `jit-tiempos` (paquete arm9-perf; ventanas de 120 cuadros de Mario Kart DS en carrera, ~750 polígonos por cuadro,
ARM9 14–15 ms en las ventanas pesadas): los accesos lentos del ARM9 llegan a 4,9–6,5 ms por cuadro y crecen con el 3D. Lo que dice el
log, por cuadro:

| | ventana A | ventana B |
|---|---|---|
| escrituras a GXFIFO y puertos de órdenes por la ruta lenta | 3399 × 0,41 µs = 1,39 ms | 4989 × 0,42 µs = 2,10 ms |
| escrituras a la E/S por la ruta lenta | 1197 × 2,03 µs = **2,43 ms** | 1590 × 2,18 µs = **3,47 ms** |
| lecturas de la E/S | 3544 × 0,31 µs = 1,10 ms | 2570 × 0,34 µs = 0,87 ms |
| intérprete desde los bloques (LDM/STM lento: 158 y 235 llamadas) | 1,52 ms | 2,13 ms |
| eventos «gx» (1132 por cuadro) | 1,88 ms | — |

### Qué dominaba (host: qemu-ppc64 con `--jit-prof` y conteo en el intérprete de la Mac; mismas escenas, cuadros 2500–3000)

No eran los comandos que la CPU escribe uno a uno, sino **la DMA de modo 7 hacia GXFIFO**. Por cuadro (media de los cuadros 2500–3000
de la demostración de MKDS): unas **15 000 palabras** pasan por la DMA (33 arranques de canal, ≈ 450 palabras cada uno) contra
**~1 000–1 800** que escribe la CPU (`STR` por la ruta lenta del recompilador, más los `STM` que ejecuta el intérprete). El «2 µs por
escritura a la E/S» de la tabla es un promedio engañoso: de ~1 500 escrituras a la E/S por cuadro (IME 210, DIVCNT y operandos 600,
IE/IF 160, SQRT 280, registros de DMA 150…) solo ≈ 33–49 son el CNT de un canal de DMA, y **ese write ejecuta la transferencia entera**
(`dma_run` en `sched.c`: hasta que la FIFO llega a la mitad; el resto sigue en los eventos «gx», que por eso suman 1,9 ms). Cada palabra
de la DMA costaba la ruta completa: `nds_gx_dma_ok` (llamada, `gx_fifo_level` aparte), `nds_bus_read` (búsqueda de región),
`nds_bus_write` (otra), `nds_io_write` (traza, reparto por dirección), `nds_gx_fifo_write` (`GX_ON`, reloj, parada),
`gx_fifo_write` y `nds_gx_schedule` (con el evento sin pendiente: recorrer los 4 canales de DMA para saber si alguno espera la FIFO).
Los puertos de órdenes (0x440–0x5FF) y GXSTAT, juntos, son < 100 accesos por cuadro; las lecturas de GXFIFO/GXSTAT, 0–11. Lo demás de
la E/S lenta no es de la geometría: IME/IE/IF, DIVCNT/SQRT (sondeo del divisor: 910 lecturas por cuadro) y los registros de DMA.

### Cambios

1. **Ráfaga de la DMA de modo 7** (`dma_run`, `nds_gx_dma_burst`, `gx_dma_burst`): con palabras de 32 bits, destino fijo en
   GXFIFO (0x04000400–0x0400043F), fuente creciente y memoria con puntero (RAM, WRAM; los espejos y las fronteras de región cortan
   la ráfaga y siguen en otra), un bucle dentro de `gx.c` hace por palabra lo mismo que la vía de siempre y en el mismo orden:
   `gx_advance` al reloj del ARM9, parada si la FIFO llegó a la mitad, `gx_fifo_write` (con `fifo_write_i` en línea), parada del
   ARM9 si la escritura llena la cola, agenda de la mirada de EV_GX (solo si no hay una pendiente: el bucle llama a `sched` en ese
   caso y no en cada palabra) y 4 ciclos. Los ciclos, la cola, los eventos y las IRQ quedan **idénticos**. Lo que no encaja (16 bits,
   destino creciente, fuente fija o decreciente, fuente en E/S o VRAM) sigue por la vía por palabra.
2. **Escrituras de la CPU por el bus corto** (`b9_w32` en `mem.c` → `nds_gx_cpu_write`): una palabra alineada del ARM9 en
   0x04000400–0x040005FF va directo a `nds_gx_fifo_write` / `nds_gx_port_write` sin pasar por `nds_bus_write` ni `nds_io_write`.
   Sirve al intérprete, a los LDM/STM que ejecuta el intérprete desde los bloques y a la ruta lenta del recompilador.
3. **Recompilador**: `h_wr` reconoce la palabra a esa ventana (y que no caiga en la ITCM/DTCM, que `mem_wr32` mira antes que el bus)
   y llama a `arec_cfg.gx_wr` (`jit_gx_wr` en `core/jit.c`) sin el reparto de regiones; cuenta aparte (`st.gx_wr`, tiempo
   `t_gx_wr`): **esos accesos ya no cuentan como accesos lentos de la región GXFIFO** en `DATO jit-lentos`. No se generó código
   en línea para esto: el trabajo propio de la escritura (`gx_advance`, cola, decodificar la palabra empaquetada) es el mismo, y lo
   que se ahorra de la entrada en C (unas 45 instrucciones de `ent_wr` y el reparto) son ~0,04 µs de ~0,2–0,4 por palabra **[est]**; no compensa
   la complejidad ni el riesgo de duplicar en ensamblador la semántica de la cola y de la parada por FIFO llena.
4. **Caché `dma9_gxw`**: «¿hay un canal del ARM9 armado esperando la FIFO?» se consultaba en cada escritura (`nds_gx_schedule`) y en
   cada mirada recorriendo los cuatro canales; ahora es un byte que se rehace donde cambia `cnt` o `latched` de un canal del ARM9.
5. `gx_fifo_level` en línea (se llamaba por palabra desde `geom.c`).

No se tocó el modelo de tiempo: no hay cambio de resultado salvo el de rendimiento (el emulado es bit a bit el mismo).

### Interruptor y qué mide el log

- `/dev_hdd0/salsads/gx-lento`: vuelve a la forma anterior (DMA palabra por palabra por el bus, escrituras de la CPU por la ruta
  lenta del recompilador, recorrido de los canales). El arranque lo dice (`gx: FIFO de geometría por …`) y la línea `jit: opciones del
  recompilador` lo lista. El host: `salsads-cpurun --gx-lento`.
- `DATO jit-lentos` trae ahora, además de los accesos lentos por región, «ya no van por la ruta lenta (por cuadro)»: escrituras a
  GXFIFO por la ruta corta del recompilador (con `jit-tiempos`, su costo), palabras de la CPU por el bus corto (incluye las de los
  LDM/STM) y palabras de DMA con el número de ráfagas. Con `gx-lento` los tres quedan en cero y la región GXFIFO vuelve a la tabla.
  `DATO jit-tiempo` sigue sumando las escrituras cortas a GXFIFO dentro de «accesos lentos (escrituras)» para que el A/B sea
  comparable.
- El tiempo de la DMA sigue dentro de las escrituras a la E/S que la arrancan («E/S … escr.») y de los eventos «gx»: la mejora se ve
  ahí (el promedio de µs por escritura a la E/S baja) y en el total de «accesos lentos», no en las filas de GXFIFO.

### Pruebas

- `salsads-gxparidad` (`src/host/gxparidad.c`, en `tools/build.sh test-host`: LE, SAN y BE con qemu): dos máquinas sin ROM con las CPUs
  paradas, una con las rutas cortas y otra con `gx_lento`; los mismos estímulos al azar (600 semillas × 32 pasos: DMA de todos los
  modos con fuentes en RAM, espejos y fronteras de región, WRAM, VRAM y E/S, desalineadas, decrecientes o fijas, destinos creciendo
  hacia los puertos, 16 y 32 bits, repetición, IRQ; palabras de la CPU a GXFIFO, puertos y sin alinear; el motor apagado y encendido;
  GXSTAT con IRQ por nivel; tramos de tiempo y cuadros con SWAP_BUFFERS) y, después de cada paso, **resumen de los ciclos del ARM9, la
  cola del motor (con el instante de cada entrada), el decodificador, los contadores, los canales de DMA, los eventos y las IRQ**:
  idénticos al bit, y la caché `dma9_gxw` igual al recorrido de los canales. Probado con mutaciones (ciclos de más, nivel de la mitad
  distinto, sin agendar la mirada, sin rehacer la caché, no cortar en el espejo, ignorar el motor apagado, puertos desde 0x450):
  todas se detectan.
- `test-host.sh`: el resumen del estado de las ROMs `gx`, `geom`, `dma` y `demo` con y sin `--gx-lento`, y en modo estricto con el
  recompilador contra el intérprete (también con `--gx-lento`) para `gx`, `geom` y `dma`.
- Con los respaldos del autor, 1500 cuadros de NSMB, SM64DS y MKDS: ver «Resultados» abajo.

### Resultados (host; no son tiempos de la PS3)

Paridad (respaldos del autor, 1500 cuadros; qemu-ppc64 salvo lo que se dice):

| | NSMB | SM64DS | MKDS |
|---|---|---|---|
| `--lockstep` (recompilador estricto contra el intérprete, con las rutas cortas) | sin diferencias, 109 M comparaciones | sin diferencias, 61,8 M | sin diferencias, 74,9 M |
| resumen del estado, `--jit-strict` con las rutas cortas / con `--gx-lento` / intérprete | los tres iguales | los tres iguales | los tres iguales |
| `--jit` (modo rápido) con y sin `--gx-lento`: resumen y imagen final | iguales | iguales | iguales |
| intérprete en la Mac, 1500 y 3000 cuadros, antes y después de los cambios (y con `--gx-lento`) | resumen idéntico | idéntico | idéntico |

Accesos lentos del recompilador en 1500 cuadros (modo rápido; escrituras lentas totales → con las rutas cortas): NSMB 7,34 M → 4,84 M
(2,49 M a GXFIFO ya no pasan por la ruta lenta general), MKDS 1,03 M → 0,75 M (0,29 M). Las lecturas no cambian.

Costo por palabra en el host (Mac, intérprete, órdenes baratas; `salsads-gxparidad --bench` contra `--bench --lento`): DMA 23 → 15 ns
por palabra, CPU 21 → 17 ns. Es un orden relativo: el PPE paga más por cada llamada.

Estimación para la PS3 **[est]** (MKDS en carrera pesada, por cuadro): de los ~5 ms que la DMA suma entre las escrituras a la E/S y los
eventos «gx», 1,5–2,5 ms; de los 1,4–2,1 ms de GXFIFO por la ruta lenta, 0,5–0,9; de los 1,5–2,1 del intérprete en LDM/STM, 0,3–0,6.
Total **2,5–4 ms**. Lo que queda es el trabajo propio del motor al ejecutar las órdenes (~8 000–12 000 por cuadro) y la parte de la
ruta lenta que no es de la geometría (IME, DIVCNT, registros de DMA).

### Qué medir en la PS3

Mismas escenas (NSMB en un nivel; MKDS en carrera pesada; una carga de cada uno), el paquete tal cual y luego con
`/dev_hdd0/salsads/gx-lento` (misma escena, varias ventanas): `DATO desglose` (ARM9 y eventos), `DATO jit-lentos` (µs por escritura a la E/S,
GXFIFO y «ya no van por la ruta lenta»), con `jit-tiempos` también `DATO jit-tiempo`. Si el ARM9 baja menos de lo estimado, el
siguiente paso es el costo propio por orden (`gx_advance_run`: anotar en el registro de la geometría diferida) o saltarse eventos «gx».

### Riesgos

- Es el mismo modelo de tiempo, comprobado al bit en el host; lo que no se puede comprobar fuera de la consola es el rendimiento.
- La ráfaga solo aplica a palabras con destino fijo en GXFIFO y fuente en memoria con puntero; lo demás sigue por la vía anterior.
- La caché `dma9_gxw` depende de que todo cambio de `cnt`/`latched` de un canal del ARM9 la rehaga (`sched.c`); `gxparidad` la
  compara con el recorrido de los canales después de cada paso.

## Paquete «SalsaDS JIT núcleo» (SLDS00009)

`tools/build.sh -C tests/phase4/nucleo-jit pkg` → `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg`. Es «SalsaDS núcleo»
(ROM en `/dev_hdd0/salsads/rom.nds`, o la ROM de carga embebida) con el ARM9 en el recompilador. Al arrancar prueba la
caché de código (hilo con plazo de 2 s) y lo dice en el log (`jit: …`); `nucleo-jit-nojit` fuerza el intérprete y
`nucleo-jit.marker` queda si la aplicación muere con código traducido (el siguiente arranque de esa versión usa el
intérprete; borrarlo para reintentar).

Qué mirar en `nucleo.log`, cada ~2 s:

- `DATO desglose`: **ARM9 en ms por cuadro** (meta: bajo ~8 ms; con el intérprete 44–49 ms en el juego 3D de la fase33),
  ARM7, eventos y planificador.
- `DATO MIPS`: instrucciones del ARM9 por segundo de tiempo dentro del ARM9.
- `DATO jit`: por cuadro, entradas, bloques traducidos y su costo, salidas por tipo, llamadas al intérprete, pasos que dio
  el intérprete desde el despachador (deberían ser ~0: es código que no se puede traducir), accesos lentos, invalidaciones;
  en total, bloques vivos, código usado de 4096 KB, vaciados (si suben seguido, la caché es chica) y bloques descartados
  (deberían ser 0).
- `DATO jit-llamadas`: las caídas al intérprete por clase y por cuadro (lo que no tiene camino rápido).
- `DATO jit-lentos`: los accesos por la ruta lenta por región (GXFIFO aparte) y los bucles de espera saltados; con
  `/dev_hdd0/salsads/jit-tiempos`, también `DATO jit-tiempo` (dónde se va el tiempo del ARM9) y el costo de cada acceso lento.
- La línea `jit: opciones del recompilador`: qué interruptores de la tercera ronda están puestos.
- La línea `jit:` del arranque: si la prueba de la caché pasó o por qué se usa el intérprete, y el tramo del planificador
  (1024 por omisión; un número de 16 a 20 000 en `/dev_hdd0/salsads/nucleo-jit-tramo` lo cambia, para comparar en la misma
  escena con 128).

En RPCS3 (texto de solo lectura) corre el respaldo con el intérprete: sirve para ver que el paquete arranca, no para el JIT.

## El ARM7 en el recompilador

El ARM7 (ARM7TDMI, ARMv4T) corre con el mismo traductor (`src/jit/arec*`), que ahora lee `c->v5` para saber qué CPU es. Es una
instancia aparte (`n->jit7`) con su caché de código, sus tablas y su mapa de memoria. En la PS3 medido con el intérprete: 3–4 ms por
cuadro en todos los juegos (≈ 9 MIPS dentro de su CPU; NSMB nivel 12,3 ms = ARM9 5,6 + ARM7 3,8 + eventos 2,7; MKDS carrera 20–21 =
13,9 + 3,2 + 3,3). Meta: ARM7 < 1 ms por cuadro. **Sin medir en la PS3 con el recompilador.**

### Qué cambia respecto del ARM9

| Tema | ARM9 (ARMv5TE) | ARM7 (ARMv4T) | En el traductor |
|---|---|---|---|
| CLZ, QADD y familia, SMULxy, BLX (registro e inmediato), BKPT, LDRD/STRD, PLD | sí | indefinidas | `callout` (el intérprete las trata como indefinidas, `acore_arm.c`) |
| condición 1111 | espacio incondicional (BLX inmediato, PLD) | «nunca» | `AI_NOP` de 1 ciclo (y `acore_exec_arm` ya no la ejecuta en ARMv4) |
| CP15 (MCR/MRC p15) | rutas rápidas en línea (`FK_CP15NOP`, `FK_MRC`) | no hay coprocesador: indefinida | `callout` sin `call_ro` (el CPSR que guarda la excepción lleva las banderas) |
| LDR / LDM a pc | cambia de estado por el bit 0 | no cambia (ARM: alinea a 4; Thumb POP {pc}: a 2) | `xchg = v5`; en el emisor el destino sale de r3 con la alineación del estado |
| LDRH impar / LDRSH impar | alinea | LDRH rota por 8; LDRSH se porta como LDRSB | el emisor manda las cargas de media palabra con el bit 0 puesto al camino lento (los ayudantes ya usaban `c->v5`) |
| STM con la base en la lista y escritura de vuelta | guarda la base vieja | guarda la nueva si no es el primer registro | `callout` solo en ese caso (ARM y Thumb STMIA) |
| máscara de MSR | Q (bit 27) | sin Q | `FK_MSR` sobre SPSR usa `0xF00000FF` |
| TCM, DTCM, ITCM | sí | no | la tabla de páginas es solo el bus |
| mapa de memoria | `rgn_*[ARM9]` + TCM | `rgn_*[ARM7]`: RAM principal, WRAM compartida, WRAM propia (64 KB en 0x03800000); BIOS 7 (HLE) en 0–0x3FFF | `src/core/jit.c`: `code_ptr7`, `remap7` |
| reloj | ciclos del ARM9 | ciclos del ARM7 (el planificador le da `(tramo + 1) / 2`) | ninguno: el traductor cuenta ciclos de la CPU |
| HALT | CP15 c7 (espera de IRQ) | SWI Halt / IntrWait (HLE) | las SWI van por el intérprete (`h_call`); si la CPU queda en HALT o salta, el bloque sale |

Lo demás (banderas, ciclos, bloques, enlaces, destinos indirectos, ruta lenta, modo rápido y estricto, `--lockstep`) es el mismo código.
MUL con S sigue dejando C y V como estaban (como el intérprete).

### Memoria y caché

- **Caché de código**: la zona de 4 MB del segmento de texto pasa a 5 MB (`REC_CACHE_BYTES`) y se reparte: el ARM9 conserva los 4 MB y el
  ARM7 usa el último MB (`nds_jit_cfg.code7_bytes`; en el host, los mismos 5 MB). Las dos quedan dentro del alcance de `bl` al texto.
  Con la caché de PS3MAPI (experimental, 8 MB) el ARM7 también se queda con el último MB.
- **Tablas del ARM7** (heap, ≈ 5,6 MB; el ARM9 ≈ 12): las dos tablas de punteros de 2 MB (lectura y escritura, el código las indexa con
  toda la dirección), 4096 bloques como tope (`cfg.max_blocks`), enlaces ×4, y las matrices de apoyo del mapa acotadas a las páginas de
  0 a 0x04000000 (`cfg.map_pages`: solo hay RAM y WRAM ahí; todo lo demás va por la ruta lenta).
- **Invalidación**: el espacio físico de los dos es el de `struct nds` desde `itcm`; el del ARM9 acaba en el final de `swram` y el del ARM7
  en el final de `wram7`. Toda escritura (`NDS_CODE_WRITE`, DMA, HLE de la BIOS, captura de sonido) avisa a los dos. Un cambio de WRAMCNT
  rehace y vacía los dos.
- Cada juego: `nds_init` recrea las dos instancias con la misma configuración; `game_close` vacía las cachés.

### Interruptores y seguridad en la PS3

- `/dev_hdd0/salsads/arm7-nojit`: el ARM7 con el intérprete (el ARM9 sigue con el recompilador). `nucleo-jit-nojit` sigue forzando el
  intérprete en las dos.
- **La marca `nucleo-jit.marker` protege a las dos CPUs**: se pone mientras hay código traducido de cualquiera y se quita al cerrar el
  juego, al salir desde el XMB o al apagar el JIT; si la aplicación muere con ella puesta, el siguiente arranque de esa versión no usa el
  recompilador en ninguna (borrarla para reintentar). El ARM7 solo se activa si la prueba de la caché del ARM9 pasó.
- La línea `jit:` del arranque dice qué lleva el ARM7: `el ARM7 con el recompilador (caché propia de 1024 KB, tablas propias)`, o
  `con el intérprete` (con el motivo si es `arm7-nojit` o falta de memoria; si no hay recompilador, `el ARM9 y el ARM7 con el intérprete`).
- `DATO jit7` (cada ~2 s) trae lo mismo que `DATO jit` para el ARM7: entradas por cuadro, bloques, llamadas al intérprete, ruta lenta,
  invalidaciones, uso de la caché. `DATO desglose` ya daba el ARM7 en ms por cuadro.

### Pruebas

- `tests/unit/test_arec.c` corre ahora en dos perfiles (ARM9 y ARM7): los mismos programas al azar (ARM y Thumb, estrictos y rápidos) con lo de
  ARMv5 sustituido por equivalentes válidos (una de cada 20 queda indefinida, para probar el camino) más LDM/STM con la base en la lista, y casos
  dirigidos de ARMv4T (LDR/LDM/POP a pc con el bit 0, LDRH y LDRSH impares por la ruta rápida y la lenta, STM y STMIA con la base en la lista,
  condición 1111, instrucciones de ARMv5 indefinidas).
- `tools/test-host.sh`: las ROMs con `--jit` (ahora las dos CPUs), con `--jit-no9` (solo el ARM7), con `--jit-no7`; `--lockstep` compara las dos
  CPUs bloque por bloque (`jitrun.c`: un aviso por CPU); y el resumen del estado de la máquina (`--state-hash`: registros, RAM, WRAM, VRAM,
  paleta, OAM e imágenes) en modo estricto con el ARM7 solo, el ARM9 solo y los dos, contra el intérprete, en `cpuarm`, `irq`, `periph`, `demo`
  y `carga`.

## Qué falta

- **Medir en la PS3** (lo dice el paquete): ms del ARM9 por cuadro, cuánto pesan las entradas por tramo (unas 2000–4500 por
  cuadro: el tramo del planificador es de 128 ciclos del ARM9), las llamadas al intérprete y la ruta lenta.
- **ARM7 en el recompilador**: hecho (sección anterior); falta medirlo en la PS3 y, después, llevarlo al hilo 1.
- **Medir la segunda ronda en la PS3** (mismas escenas, tramo de 1024 y de 128 con `nucleo-jit-tramo`).
- **Optimizaciones** según lo que diga la consola: que el planificador deje correr al ARM9 más allá de los eventos que no lo
  tocan (los del motor de geometría, sobre todo) o detecte los bucles de sondeo de IPC; entrada y salida más baratas (hoy guardan
  18 registros y usan `mfcr`/`mtcrf` de varios campos); LDM/STM a la E/S sin el intérprete; CMP condicionales; fusión cmp+salto;
  la caché de código (4 MB) llega al 70 % en SM64DS en 1500 cuadros: más grande o desalojo por mitades como en Salsa64.
- Contrastar en una DS el efecto del modo rápido (la CPU ve los eventos hasta un bloque tarde).
- Tercera ronda: medir en la PS3 (sección «Tercera ronda», «Qué medir en la PS3»). Candidatos que quedan, según lo que diga
  `jit-tiempos`: caminos rápidos para GXFIFO y el divisor (accesos lentos), rutinas compartidas de acceso a memoria (menos huella
  a cambio de bl/blr), suma de 64 bits ADDS+ADC, LDM/STM más cortos, menos salidas por tramo.
