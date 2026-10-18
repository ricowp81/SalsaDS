# Fase 4 — Recompilador ARMv5TE → PowerPC64 para el ARM9

El ARM9 corre en el PPE con un recompilador dinámico propio (ARM y Thumb), integrado en la máquina de la DS. El ARM7
sigue con el intérprete. Código propio (GPL-2.0-or-later), escrito desde el manual de arquitectura de ARM, GBATEK, la
Power ISA 2.02 y lo medido en la PS3 en la Fase 0 (§0.3 y §0.7); el diseño toma las ideas del recompilador MIPS → PPC64
de Salsa64 (bloques con enlace por parcheo de un salto, mapa de código para invalidar, caminos fríos aparte, caminos
lentos compartidos). No viene de ningún emulador (ni melonDS ni NooDS, GPLv3).

Estado: **correcto contra el intérprete en qemu-ppc64** (modo estricto y `--lockstep`); **sin medir en la PS3**: el
paquete «SalsaDS JIT núcleo» (SLDS00009) es para eso.

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
| cr2–cr4 | vistas de las banderas (no volátiles: sobreviven a las llamadas a C de los caminos lentos) |
| cr0, cr1, cr5–cr7 | temporales |

La entrada desde C (`arec_enter`, generada al principio de la caché) guarda r14–r31 y el CR, carga los registros
guest de `c->r[]` y salta al bloque; la salida común los devuelve a memoria, suma ciclos e instrucciones a `c->cycles` y
`c->icount` y vuelve con el tipo de salida (pc, enlace, estado en memoria; y si fue por presupuesto) y las banderas que
quedaron exactas.

### Bloques, presupuesto y modos

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
en una tabla de 16 384 entradas (`pc | Thumb` → código) con `mtctr`/`bctr`; si no está, vuelven al despachador. Las
llamadas a C son `bl` si llegan (en la PS3 la caché está detrás del texto) y si no `ld`/`mtctr`/`bctrl`.

### Banderas NZCV

Decisión de la 0.7: CR por instrucción, sin `mfcr` ni formas con punto en el camino caliente.

- Un análisis del bloque (hacia atrás: banderas vivas; hacia adelante: quién produce cada bandera y quién la lee) decide
  qué emite cada instrucción que fija banderas. Una resta (CMP, SUBS, RSBS) deja hasta tres comparaciones: `cmpw` en cr4
  (GE, LT, GT, LE salen de un bit), `cmplw` en cr3 (CS, CC, HI, LS) y `cmpwi res, 0` en cr2 (N, Z); V, si se necesita
  suelto, es `cr2.lt ^ cr4.lt`. Una suma: N/Z en cr2, el acarreo con `cmplw res, a` y V calculado en cr4. Las lógicas: N/Z
  y el acarreo del desplazador. Las condiciones que mezclan productores se arman con operaciones del CR (`crand`,
  `crxor`…) en un bit temporal.
- Las banderas se escriben en memoria (`c->jit_nzcv`, en enteros, ~13 instrucciones) **solo en la instrucción que las fija
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

MRS, MSR, SWI (la HLE de la BIOS), CP15, LDM/STM con `^` o lista vacía o (LDM) la base en la lista con escritura de vuelta,
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
- La línea `jit:` del arranque: si la prueba de la caché pasó o por qué se usa el intérprete.

En RPCS3 (texto de solo lectura) corre el respaldo con el intérprete: sirve para ver que el paquete arranca, no para el JIT.

## Qué falta

- **Medir en la PS3** (lo dice el paquete): ms del ARM9 por cuadro, cuánto pesan las entradas por tramo (unas 2000–4500 por
  cuadro: el tramo del planificador es de 128 ciclos del ARM9), las llamadas al intérprete y la ruta lenta.
- **ARM7 en el recompilador** (ARMv4T: las reglas de `v5` del intérprete — LDR pc sin cambio de estado, LDM/STM con la base,
  LDRH no alineado que rota, MUL que destruye C —, otra tabla de páginas y su propio espacio físico) y, después, en el hilo 1.
- **Optimizaciones** según lo que diga la consola: en línea las llamadas al intérprete más frecuentes (CP15 de caché, que es
  un no-op; SWP; MRS/MSR de solo I/F); LDRD/STRD; caché de destinos por sitio para los BX LR; banderas: no escribir NZCV en
  las vueltas de función cuando el que llama no las lee; fusión cmp+salto; tramos más largos para el ARM9 con el
  recompilador (menos entradas); la caché de código (4 MB) llega al 70 % en SM64DS en 1500 cuadros: más grande o desalojo
  por mitades como en Salsa64.
- Contrastar en una DS el efecto del modo rápido (la CPU ve los eventos hasta un bloque tarde).
