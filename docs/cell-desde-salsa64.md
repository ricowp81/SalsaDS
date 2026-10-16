# Trabajar con la PS3 y el Cell: lo aprendido en Salsa64

Síntesis práctica para SalsaDS. Salsa64 es un emulador de Nintendo 64 para PS3 (homebrew con PSL1GHT, HEN o CFW) que lleva el CPU de la N64 en el PPE con un recompilador dinámico, el RSP, el audio y el JPEG en SPEs y el dibujo en el RSX. Su código y sus documentos están en https://github.com/ricowp81/Salsa64. Este documento resume lo que ahí se midió; no sustituye a las fuentes, que se citan entre paréntesis por nombre de archivo de Salsa64 (`docs/…` salvo que se diga otra ruta).

## Cómo leer las cifras

| Marca | Significa |
|---|---|
| **[PS3]** | medido en una PS3 Slim (CECH-3011A, firmware 4.91 con HEN) real |
| **[RPCS3]** | medido o visto en RPCS3 (no concluyente para rendimiento, SPEs ni JIT) |
| **[host]** | medido en la Mac (nativo o qemu-ppc64); sirve para proporciones, no para tiempos |
| **[pub]** | cifra de documentación pública del Cell (Cell BE Programming Handbook, CBEA) citada por las fuentes |
| **[diseño]** | propuesta o estimación que las fuentes no midieron |
| **(verificar)** | dato que las fuentes no respaldan del todo o que se contradicen |

Dos diferencias de fondo con Salsa64: la N64 es big-endian como la PS3 y la DS es little-endian (ahí Salsa64 no tiene nada medido: el costo de `lwbrx`/`stwbrx` en el PPE no está en las fuentes, **(verificar)** en la Fase 0 de SalsaDS); y la DS corre dos CPUs (67 y 33 MHz) donde la N64 corre una de 93,75 MHz. Las proyecciones de este documento hacia la DS son inferencias y se marcan como tales.

---

## 1. El PPE para emuladores

### 1.1 Qué es

Un núcleo PowerPC de 3,2 GHz, **en orden**, con dos hilos de hardware (SMT) que comparten unidades de ejecución, L1 y L2 (cache-ppe.md §1, uso-cell.md §1). Cifras públicas que Salsa64 usó para razonar (cache-ppe.md §1, **[pub]**):

| Dato | Valor |
|---|---|
| L1 de instrucciones | 32 KB, 2 vías, líneas de 128 B; su contenido no está garantizado en la L2 |
| L1 de datos | 32 KB, 4 vías, líneas de 128 B; **escritura directa sin asignar**: todo almacenamiento baja a la L2 por una cola de 16 entradas |
| L2 | 512 KB compartidos por los dos hilos, 8 vías; un acierto cuesta unos 41 ciclos |
| Memoria XDR | un fallo de L2 cuesta unos 120 ns más (~380 ciclos) |
| ERAT | 64 entradas de instrucciones y 64 de datos; **cada entrada cubre 4 KB aunque la página sea grande**, así que el alcance es 256 KB cada una; un fallo con acierto en la TLB cuesta ~24 ciclos |
| Saltos | predictor de 4 K × 2 bits, pila de enlaces de 4 entradas, **24 ciclos** por salto mal predicho, sin predicción de destino para `bctr` |
| Load-hit-store | una carga que alcanza un almacenamiento todavía en cola no se reenvía: 30–40 ciclos |
| FPU y multiplicaciones | `fadd`/`fmul`/`frsp`/`fsel`/`fctid`/`fcfid` 10 ciclos; `fdiv` 56–74; `fsqrt` 66–84 (sin pipeline); enteros 2; `mulli` 6; `mulld` 15 (detienen el pipeline); cargas 2 |

El PPE no tiene movimientos directos entre registros enteros y de punto flotante: todo valor pasa por memoria (perf-log.md, «Líneas base de la Fase 1»).

### 1.2 Instrucciones que son microcódigo

Una instrucción microcodificada vacía el pipeline y cuesta **≥ 11 ciclos** (rec-ppe.md, «Lo que el PPE hace en microcódigo»; Handbook tabla A-1, **[pub]**):

- todas las **formas con punto** (`andi.`, `andis.`, `and.`, `add.`, `rlwinm.`, `mulld.`…);
- los **desplazamientos y rotaciones por registro** (`slw`, `srw`, `sld`, `srd`, `sraw`, `srad`, `rldcl`, `rlwnm`);
- `lwa`/`lha` (cargas con extensión de signo), `lmw`/`stmw`;
- `mtcrf` de **más de un campo** (`mtocrf` de uno: 1 ciclo), `mcrfs`, `mtfsf*`;
- `mfcr` ~34 ciclos; `mffs` 11–28; un acceso que cruza 32 bytes, ~55 (cache-ppe.md §1).

`fcmpu` deja su bit del CR **al ciclo siguiente**: un `bc` inmediato espera toda la latencia de la FPU. Por eso el recompilador de Salsa64 nunca emite formas con punto: ANDI es `rlwinm` (máscara de unos seguidos) o `li`+`ori`+`and`; los bits se prueban con `rlwinm`+`cmpwi`; la salida a C usa varios `mtocrf`; los valores de punto flotante se clasifican con sus bits en enteros y no con comparaciones de punto flotante seguidas de salto (rec-ppe.md).

Los desplazamientos por registro no tienen alternativa (los pares LWL/LWR de la N64 salen byte a byte); en el laboratorio del recompilador son ~7,6 por 1000 instrucciones y valen ~0,08 ciclos por instrucción: no son palanca (lab-recompilador.md, «Qué dice el laboratorio» 6).

Para SalsaDS (**[diseño]**): las banderas NZCV del ARM no se pueden calcular con formas con punto ni leyendo el CR en cada instrucción; el costo de cada alternativa (perezosas, N/Z con `cmpw`, etc.) no está medido en Salsa64 y es justo lo que mide la Fase 0.3.

### 1.3 Lecciones del intérprete (medidas en la consola)

El primer intérprete de la CPU de la N64 daba **8,8 MIPS [PS3]** (~250 ciclos por instrucción hasta para un NOP) y llegó a **21,2 MIPS** en dos rondas (perf-log.md, «Líneas base de la Fase 1»). Lo que lo mejoró, en orden de impacto:

1. Estado del bucle en registros del PPE; no releer lo recién escrito (load-hit-store).
2. Nada de funciones cortas con marco de pila en el camino caliente.
3. Evitar `lwax`/`lha`, desplazamientos variables y formas con punto en código caliente.
4. `-mno-relative-jumptables` en todo el código del PPU (las tablas de `switch` relativas usan `lwax`).
5. Un solo despacho (la tabla incluye `SPECIAL`), sin pasar valores entre registros enteros y de FPU.
6. Detectar **bucles de espera** del juego y saltarlos: 65,6 → 35,0 ms por campo en un homebrew de pruebas (×1,9) con 45,6 % del tiempo saltado (perf-log.md, «Bucles de espera»).

Lo que un intérprete no puede quitar es la cadena dependiente de cada instrucción (cargar palabra → extraer índice → cargar dirección → `mtctr` → `bctr`), unos ~93 ciclos por instrucción aun para un NOP; ese techo lo rompe el recompilador. **Inferencia para la DS**: el ARM9+ARM7 piden del orden de 100 M de instrucciones por segundo, así que un intérprete de ~20 MIPS no alcanza para velocidad completa y el recompilador no es opcional (la N64 pide hasta 93,75 MIPS; cpu-vr4300.md/perf-log.md «Recompilador»). Los MIPS de Salsa64 no son comparables 1:1 con ARM sin medir.

### 1.4 Cuánto cuesta una instrucción emulada en código nativo

Con recompilador, GoldenEye (el juego más exigente de CPU) cuesta **~15,5–17 ns por instrucción MIPS [PS3]**, unos 50–54 ciclos, de los que solo 10–14 son trabajo; los ~40 restantes son esperas (cache-ppe.md §2, estimación sin PMU: los contadores del Cell son privilegiados). El reparto aproximado, medido en el laboratorio del recompilador sobre el código que se emite (lab-recompilador.md, «Qué dice el laboratorio»; **[host]** calibrado con la consola):

- se ejecutan **~10,7 palabras de PPC por instrucción MIPS** (6,8 de cuerpo y prólogo, 3,95 de rutinas compartidas);
- el estado del emulador pasa por memoria: por cada 100 instrucciones invitadas, ~58 cargas y ~52 almacenamientos a la estructura de la CPU (la FPU es el 55–67 %);
- load-hit-store dentro del cuerpo: ~10–14 eventos por 100 instrucciones (~3,6 ciclos por instrucción);
- fallos de caché de instrucciones: ~4,8 ciclos por instrucción (9 %);
- el 99,7 % de las instrucciones son nativas.

fastmem.md §1 afirma que un «bench de huella» en la consola mostró el fetch de código como ~37 de esos ~40 ciclos de espera; **(verificar)**: no encontré esa sección en perf-log.md y cache-ppe.md §5 sólo propone el bench.

Modelo de dos parámetros ajustado a la consola: **Δt ≈ 0,58·Δ(palabras que corren) + 0,08·Δ(líneas de caché del cuerpo)**, útil para ordenar cambios y descartar los que no mueven nada, no para prometer números (±50 % por paso). Una calibración posterior de un cambio concreto dio una elasticidad de ~0,4 por palabra relativa (lab-recompilador.md, «Caché de destinos de JR»).

Lección que SalsaDS debe heredar: **el costo lo manda la huella de código que se ejecuta, no el recuento de instrucciones**. Cada instrucción invitada eran ~21 palabras de PPC emitidas (84 bytes, dos tercios de una línea) y el PPE lee del orden de 4 GB/s de código (cache-ppe.md §1). Bajar de 20,9 a ~13 palabras emitidas por instrucción dio −6…−10 % en la consola (perf-log.md, «Código compacto»; lab-recompilador.md, «Calibración»).

### 1.5 Dos hilos de hardware: reparto y costo

Reparto de Salsa64 (hilos-ppe.md):

| Hilo | Prioridad | Trabajo |
|---|---|---|
| Emulación (`sysThreadCreate`, pila de 256 KB) | 1000 | la CPU invitada, eventos, arranque y sondeo de tareas a SPEs, copias del framebuffer |
| Presentación (hilo principal del proceso) | 1001 | traducción de la salida de la HLE a comandos del RSX, flip en vsync, DualShock 3, callbacks de sysutil, escritura de guardados |
| Latido | 500 | cada 0,5 s revisa que los otros den vueltas y, si uno se queda quieto > 2 s, escribe dónde está cada uno |

Lecciones:

- Los dos hilos comparten el núcleo: lo que hace el hilo 1 se lo quita al hilo 0. Medido: **~0,13 ms de emulación perdidos por ms de trabajo del hilo 1 [PS3]**; en GoldenEye el hilo 1 trabaja 2,9 ms por campo (15,8 %) y cuesta ~2,5 % del campo (hilos-ppe.md «Cuánto le quita…»; cache-ppe.md §3.7; perf-log.md).
- **Nunca espera activa**: el hilo de presentación duerme en semáforos con tiempo de espera y las esperas del RSX usan `sysUsleep`. Lo mismo `spukit_wait` (primero gira ~50 µs con prioridad baja del hilo de hardware, luego duerme de a 20 µs; lib/spukit/README.md).
- **La emulación nunca espera al vsync**: la presentación frena al flip; si la emulación va más rápido, se salta el campo más viejo y se muestra el más nuevo (hilos-ppe.md «Si un hilo va más rápido…»).
- **Ritmo** (`pacer`): la emulación duerme hasta la hora del siguiente campo, la referencia avanza lo que duró el campo en ciclos emulados, **sin deuda** (si el atraso pasa de ~4 campos se reinicia) y sin esperas largas (hilos-ppe.md «Ritmo»).
- Comunicación entre hilos: todo preasignado, un productor y un consumidor por cola, barreras `lwsync`; buzón de controles con seqlock; flancos de botones con OR atómico; estados con banderas monótonas (hilos-ppe.md «Qué cruza entre los hilos»). La emulación tiene más prioridad que la presentación y una espera de seqlock debe dormir tras ~64 vueltas.
- **Fin de tarea de SPE sin bloquear** (sondeo): en vez de esperar al SPE dentro del hilo de emulación (10–27 ms por tarea en Zelda, 20–53 % del hilo), el evento de fin de tarea se reprograma cada ~0,11 ms emulados mientras el CPU sigue; si el CPU estaba en un bucle de espera, duerme 100 µs reales antes de contestar para que el reloj emulado no corra 100× (hilos-ppe.md «Fin de tarea sin bloquear»).
- Copiar 150–300 KB por campo en el PPE cuesta 0,20–0,30 ms por campo y ensucia la L2 compartida; la copia por DMA de un SPE evitaría tocarla (cache-ppe.md §3.7, **[diseño]** no implementado).
- Descartado con números: un hilo 1 «ayudante de precarga» (~13 % de impuesto por SMT contra 10–15 % de techo; neto ~0), SPE «acomodador» de datos para el PPE, caché por software en un SPE, páginas grandes para la caché de código (el ERAT cubre 4 KB por entrada), la gestión de reemplazo RMT (privilegiada) (cache-ppe.md §3.9–3.13, sección 6).

---

## 2. JIT en el PPE

### 2.1 Qué memoria ejecutable funciona

Prueba bloqueante de la Fase 0 (`tests/phase0/jit-ppe`; jit-ppe.md): escribir `mulli; addi; blr`, limpiar cachés, llamar con un **descriptor ELFv1 propio** (entrada y TOC) y esperar `f(5)=22`; reescribir y esperar 24 (si da 22, la caché de instrucciones quedó vieja).

Resultado **[PS3]** con HEN (2026-10-10):

| Memoria | Resultado |
|---|---|
| `.data`, `.bss`, `.data.rel.ro` (mismo segmento de 256 MB que `.text`), heap de newlib, `sysMemoryAllocate` (64 KB y 1 MB), `sys_mmapper` | **mata el proceso al ejecutar** (el no-ejecutable se aplica por página, no por segmento) |
| Una zona dentro de **`.text`** | **OK**: se escribe, ejecuta, reescribe y vuelve a ejecutar |

Así que el JIT solo puede vivir en el segmento de texto. En **RPCS3** pasa lo contrario (`.text` es de solo lectura y no ejecuta código de datos: su recompilador solo tiene el código de los módulos cargados), de modo que **el JIT solo se prueba en la consola** (jit-ppe.md, rpcs3.md).

### 2.2 Dónde vive la caché del recompilador

Decisión (jit-ppe.md «Decisión»; `mk/rec-cache.ld`): una **cola NOBITS al final del segmento de texto**, no una zona de nops dentro de `.text`.

- Un script de enlace con `INSERT AFTER .sys_proc_prx_param` define una sección `NOLOAD` alineada a 1 MB dentro del mismo segmento `R E` (`: hdr_text`). El segmento queda con `memsz > filesz`: lv2 lo rellena con ceros, no ocupa espacio en el EBOOT ni hay que descomprimirlo.
- Se pasa con `-T rec-cache.ld -T lv2.ld` **en ese orden**: al recibir un `-T`, gcc deja de agregar `lv2.ld` solo, e `INSERT` solo encuentra su destino en un script que venga después.
- Una zona de 32 MB de nops dentro de `.text` también funciona (viaja comprimida: ~31 KB en el EBOOT) pero parte `.text` y obliga al enlazador a meter stubs de salto largo (`.branch_lt`).
- Probado **[PS3]**: escribir, ejecutar y reescribir hasta el último byte de una cola de 32 MB; funciona con el `.gnpdrm.pkg` y con el `.pkg` normal (los dos llevan el mismo EBOOT NPDRM).
- **Los segmentos del ELF se descuentan de la memoria de usuario**: total 214 016 KB (209 MB) y 143 680 KB disponibles con un segmento de 64 MB. Reservar la caché ahí cuesta lo mismo que pedirla con `sysMemoryAllocate` (jit-ppe.md, memory-budget.md).
- Tamaño: empezó en 16 MB y terminó en **32 MB, que es el techo** porque `bl`/`b` alcanzan ±32 MB (los saltos al preámbulo de rutinas compartidas deben alcanzarlo desde el final de la caché). Las funciones en C quedan a < 32 MB de los primeros tramos y se llaman con `bl` directo; cada bloque decide al compilarse (`calls_near`) y los del final usan `mtctr`/`bctrl` (rec-ppe.md «Caché de código»). Hay una contradicción menor entre documentos: jit-ppe.md recomienda 16 MB «para empezar» y memory-budget.md/rec-ppe.md documentan 32 MB después; vale lo posterior.

### 2.3 Limpieza de cachés

Tras escribir código: `dcbst` por línea de 128 B, `sync`, `icbi` por línea, `isync`. Costo medido **[PS3]**: 64 KB en 10–13 µs, 32 MB enteros en 5,4–5,5 ms (un tercio de un cuadro: aceptable para reiniciar o cambiar de ROM, no en juego; en juego se invalida por bloques) (jit-ppe.md).

### 2.4 CFW y el respaldo al intérprete

El JIT solo se validó con HEN. Con **CFW Evilnat 4.91** el reporte de un usuario (issue #2 de Salsa64) fue una carga infinita: nunca salía «recompilador activo». La hipótesis de la época es que el cargador mapea el texto sin escritura, la primera escritura de la caché da un fallo de página y lv2 deja el hilo detenido (no mata el proceso como con HEN) (jit-cfw.md «Qué se colgaba», marcada «lo más probable»).

Actualización (2026-10-16): el mismo usuario confirmó que la v0.2.6 arranca todos sus juegos en CFW sin desactivar el recompilador, y el issue #2 se cerró. No se supo cuál de los cambios lo resolvió.

El respaldo (jit-cfw.md «Respaldo automático al intérprete», `src/platform/ps3/rec_cache.c`):

1. un **hilo de prueba** al arrancar (prioridad 2000, por debajo de los del frontend) consulta la página (syscall 351), comprueba que la cola llegue en ceros, escribe y ejecuta `f(5)=22`, reescribe y espera 24, repite en los últimos 128 bytes y escribe una palabra en cada página de 64 KB;
2. al abrir un juego se espera el resultado hasta 2 s; si no vuelve, se usa el intérprete y se dice en el log;
3. si el fallo **mata el proceso** en vez de colgar el hilo, una marca (`jit-prueba.txt` con la versión) hace que el siguiente arranque no repita la prueba.

Con HEN la prueba termina en < 1 ms. Hay además una app de diagnóstico para usuarios («JIT test») que prueba otras vías (PS3MAPI `SET_PROC_MEM`, `PROC_PAGE_ALLOCATE` ejecutable, `sys_dbg_write_process_memory`), con estados `OK/FALLO/COLGADO/CRASH`, y cada vía en su hilo con límite de tiempo (jit-cfw.md). La tabla de resultados de esa prueba está **vacía** en el documento; las vías de PS3MAPI quedan como **(verificar)**. Observación posterior al documento: el usuario del issue confirmó que una versión posterior arranca todo en CFW y el issue se cerró (no está en jit-cfw.md).

Regla para SalsaDS: llevar desde el día uno la prueba de la caché y el respaldo al intérprete; el diseño debe funcionar sin JIT.

### 2.5 No hay fastmem

La idea: mapear la RAM invitada en una ventana de direcciones donde lo que no es RAM «falla» y el fallo se atiende en un camino lento, para emitir accesos de 3–4 palabras en vez de 7–8 (fastmem.md §1). Hace falta que lv2 avise de los fallos de página: `sys_mmapper_enable_page_fault_notification` (syscall 327). **[PS3]**: devolvió `0x80010002` (EINVAL); la conclusión vigente en la cabecera de fastmem.md es «bloqueado». **(verificar)**: el §7 de fastmem.md cuenta tres arranques, dos con 327 aceptado y la aplicación muriendo en la fase de parche, y uno con EINVAL; la conclusión vigente es la del EINVAL.

Qué sí quedó medido de lv2 (fastmem.md §7, **[PS3]**): regiones de 256 MB/512 MB/1 GB en `0x80000000` (2 GB no; hasta 4 regiones de 256 MB), 256 objetos de 64 KB, mapear 64 KB 8,2 µs, espejo del mismo objeto en dos direcciones sí, `change_address_access_right` (336) EINVAL (sin solo lectura por página), una lectura fuera de la L2 ~70 ns.

La alternativa que sí funcionó es la **base en caché** (fastmem.md §8): se comprueba una vez el rango del registro base y los accesos siguientes del bloque son `lwz d, disp(Tb)`. Dio −5…−7 % en el host y **ningún beneficio claro en la consola** (GoldenEye 16,63 contra 16,0–16,3 ns por instrucción antes). Las tablas de páginas de punteros que prevé el plan de SalsaDS son coherentes con esto (la DS mapea por regiones).

### 2.6 Lecciones de diseño del recompilador que aplican a cualquier CPU invitada

Todas de rec-ppe.md y lab-recompilador.md salvo indicación.

**Estructura**
- Recompilador propio en vez de portar uno de Wii/GC: el de 32 bits, con ABI de otra plataforma y atado al núcleo de mupen, habría costado más que escribir uno desde el manual de Power ISA. El ABI de PSL1GHT es ELFv1 de 64 bits (descriptores de función de 24 bytes, TOC en `r2`, `r13` reservado).
- **Un registro del invitado cabe en un registro del PPE** (la N64 es de 64 bits, el PPE también); para la DS (ARM de 32 bits) sobran registros: la pregunta de qué fijar en qué registro es de SalsaDS.
- Asignación por bloque de los registros invitados más usados (12 de r14–r25), registro de «ciclos al entrar», registro con `sched->next` (próximo evento) recargado tras cada llamada a C, registro de la estructura de CPU. Algunos registros invitados «fijos» entre bloques dieron **+7 % [PS3]** (sp, ra, v0, v1, a0–a3).
- Cada registro modificado se escribe en memoria **apenas recibe su último valor del bloque**, no todos a la salida (para que el bloque siguiente no lea mientras el almacenamiento sigue en la cola). Escribirlo **justo después** de calcularlo es peor (−37 % en un bucle de enteros: el `std` espera al valor y detiene el pipeline); escribirlo dos instrucciones después tampoco compensó (−2,6 %).
- **Exactitud de tiempo**: un bloque solo corre en nativo si cabe entero antes del próximo evento (`cycles + n ≤ next`), lo revisa al entrar también por un enlace; si no, el intérprete avanza un bloque básico. Así cada evento se atiende en el mismo ciclo que con el intérprete solo.
- **Superbloques**: un salto condicional hacia adelante no corta el bloque; el camino tomado sale enlazado y el no tomado sigue en el mismo bloque con los registros en el PPE. Cada corte de bloque cuesta prueba de evento, escritura y recarga de registros (load-hit-store) y un enlace. GoldenEye ejecutaba 5,3 instrucciones por bloque antes de los superbloques; el laboratorio da después ~12 por entrada (medias medidas en momentos y con métodos distintos).
- **Enlaces** por parcheo: una salida a dirección fija deja `nop; bl exit_link; <destino>`; el despachador sustituye el `nop` por un `b` directo. Saltos indirectos (JR): se busca el destino desde el propio código; una **tabla directa de 1024 entradas** vpc→entrada acertó 92,4 % en la consola y rindió −1,05 % de todo el hilo de emulación (~−1,3 % del nativo) (lab-recompilador.md «Caché de destinos de JR»).
- **Rutinas compartidas en un preámbulo** para lo frío (caminos lentos de memoria, FPU): el sitio de acceso pasó de ~47 a unas pocas palabras; la media por bloque bajó de 316 a 248 palabras y luego a ~13 por instrucción. Compactar el código es la palanca, no colocar los bloques calientes juntos: **juntar 762 bloques calientes en 1 MB no cambió nada [PS3]**.
- **Caminos lentos al intérprete** para lo raro o con mucho estado; lo que el camino nativo no pueda dar bit a bit va al intérprete (en la FPU: operandos fuera de una franja segura, redondeo dirigido, NaN, desnormales). Se estima el rango con los bits en enteros. Pruebas exhaustivas contra el camino completo (6000–8000 instrucciones aleatorias por clase).
- **Modos del invitado que cambian el código** (Status.FR en la N64): no vaciar la caché al cambiar; compilar **gemelos** por modo y elegir en la entrada del bloque. En Donkey Kong 64 pasó de ~20 % de velocidad a 100 % [PS3] (959 vaciados en 30 s antes; 4 173 a 0 vaciados en el host).
- **Invalidación por escritura de código**: un contador por trozo de 128 bytes (`code_flags`) que revisan los almacenamientos nativos, los del intérprete y las escrituras externas (DMA). Se vacía entera con un reset, al cambiar la ROM o cargar un estado, o al agotarse ids o enlaces. SalsaDS tendrá la complicación de VRAM mapeada, ITCM/DTCM y escrituras a código desde la otra CPU.
- **Desalojo por tramos** en vez de vaciar todo: la caché se parte en 8 tramos, se asigna linealmente y se mata solo lo que vive en el tramo más viejo. Con vaciado total, el conjunto vivo (~7000 bloques) se recompilaba de golpe: campos de 52 ms [PS3].
- **Bucles de espera**: el código generado consulta la misma tabla de bucles de espera que el intérprete; cada bucle de espera de verdad cuesta una salida a C por evento.
- **Bloques fuera de la zona normal** (código por TLB, ROM): claves por dirección virtual, bloques «desprendidos» con copia de bytes para sobrevivir a remapeos. Probable equivalente en la DS: código en regiones remapeables (bancos de VRAM, WRAM compartida).

**Verificación (lo más importante)**
- Un **modo lockstep**: una segunda máquina con el intérprete, comparada con el recompilador después de cada bloque (CPU, controladores y, cada N bloques, la RAM). Encontró en las primeras pasadas errores que ninguna prueba unitaria veía (registros con basura en las instrucciones saltadas, grupos de accesos solapados que se pisaban un registro base) (rec-ppe.md «Resultados»; fastmem.md §8). Un caso que ni el lockstep ni las pruebas cubrían (un almacenamiento como cabeza de grupo en el delay slot de un branch-likely) apareció como congelón en la consola (fastmem.md §8): la consola sigue siendo el juez final. Hay que construir el lockstep antes que el recompilador.
- Pruebas unitarias con el recompilador en **modo estricto** (nunca interpreta lo que puede compilar) y con las llamadas por `bl` y por `mtctr`.
- Una suite externa de CPU (en Salsa64, nemu64-test): intérprete y recompilador deben dar las mismas fallas, el mismo hash y el mismo ciclo final. Resultado en la consola: ×4,36 (7,9 → 1,8 s, 104 MIPS) en esa suite y 156,6 MIPS contra 21,3 en un bucle mixto (perf-log.md «Recompilador»).

**Medir el recompilador**
- Sin PMU, la herramienta es el **muestreo por tiempo desde el otro hilo** (rec-ppe.md «Muestreo»): la emulación deja dos bytes (id del bloque actual y categoría) y un hilo de prioridad 1100 los lee cada 200 µs (5000 muestras/s). Costo: dos palabras por bloque y ~5 % medido en la consola. Contar entradas a bloques engaña (los más entrados son los de 2–4 instrucciones): lo que cuesta es tiempo.
- El **laboratorio** (`tools/lab/`, lab-recompilador.md): el emisor es C puro que genera bytes de PowerPC en un arreglo sin ejecutarlos, así que el mismo `rec_emit.c` se compila nativo en la Mac y traduce los bloques reales de un juego. Responde en segundos preguntas sobre el código emitido (palabras por instrucción, load-hit-store, huella en L1I/L2) y se calibra contra la consola. Reprodujo el cuadro medido en la PS3: GoldenEye 15,57 (consola) contra 16,39 (laboratorio) ns por instrucción antes de los cambios.
- **El host engaña en magnitud**: la FPU compacta quitó 24–32 % de palabras calientes [host] pero solo −2…−8 % de ns por instrucción en la consola (fastmem.md §9). Toda decisión de rendimiento se confirma en la consola.
- El PPE tiene **ruido de ventana**: variaciones de ±3 % entre ventanas de 5 s son normales; compare medianas de 8–13 ventanas pesadas con protocolo fijo (fastmem.md §9, lab-recompilador.md).

---

## 3. SPEs

### 3.1 Qué hay y cómo se programan

- 8 SPEs en el chip, 7 activos en la PS3, 1 del sistema: **6 para la aplicación** (uso-cell.md §1; `spukit_init(6)`). No asumir el séptimo.
- Cada SPE: 256 KB de **local store** para código, datos y pila; sin caché ni acceso directo a la memoria principal; SIMD de 128 bits; DMA por el MFC con latencia de cientos de ciclos.
- Salsa64 compila los programas SPU con `spu-gcc` 9.5.0, `-O2 -mdual-nops -fmodulo-sched -ffunction-sections -fdata-sections -Wl,--gc-sections -fstack-usage` y los **embebe como ELF** en el ejecutable del PPU (mk/ps3.mk; `SPU_PROGS`, `<nombre>_SRCS`).
- **Un SPE por tarea fija y trabajadores persistentes**: la HLE gráfica en un SPE, el audio en otro, un pool de cuatro para JPEG, compresión LZ y RSP a bajo nivel. Crear/destruir hilos SPU por cuadro es caro; un trabajador duerme en su mailbox mientras no hay trabajo (uso-cell.md §2, spukit README).
- Código portable: el mismo código C de la HLE gráfica compila en el host, en el PPE y en el SPE, con una capa de E/S distinta (memoria directa o DMA). Todo lo que corre en un SPE se prueba antes en el host (docs/proyecto.md de Salsa64, rsp-lle.md).

### 3.2 spukit (cola + timbre por mailbox)

Biblioteca MIT, repo propio (lib/spukit/README.md; ver §9). Modelo:

- Un **trabajador** es un hilo SPU persistente en su propio grupo.
- El PPE **publica** trabajos en una cola en memoria principal: descriptor de **128 bytes** (`kind` + hasta 112 bytes de parámetros, normalmente direcciones) y toca un **timbre** (escribir en la mailbox; una llamada al sistema por publicación). Cola de **8 lugares**; si está llena, `spukit_post` espera.
- El SPU atiende todo lo publicado en orden y, por cada trabajo, escribe un **resultado de 128 bytes** (error, duración en ticks del decrementador, 112 bytes propios) y luego el número del último trabajo terminado. **Antes de eso espera todos sus DMA**, así que lo que escribió a memoria principal es visible cuando el PPE ve el trabajo como terminado. El resultado vale hasta que se publiquen 8 trabajos más.
- Un timbre de más solo despierta al SPU para encontrar la cola vacía; uno perdido no importa porque el SPU relee la cola completa.
- API PPE: `spukit_init`, `spukit_worker_start(elf, name, arg)` (hasta 6), `spukit_post`, `spukit_done`, `spukit_wait`, `spukit_worker_stop`, `spukit_get_stats`, `spukit_tick_hz` (timebase de 79,8 MHz). API SPU: `spukit_run(ctrl_ea, handler, ctx)`, `spukit_dma_get/put` (cualquier tamaño múltiplo de 16, en trozos de 16 KB, sin esperar), `spukit_dma_wait(mask)`, `spukit_get_unaligned`, `spukit_ticks`.
- Etiquetas DMA **0–30 para el manejador; la 31 es del bucle**.
- Costo de memoria: ~2,3 KB de memoria principal y ~0,5 KB de local store por trabajador.
- Incluye compresión LZ de bloques de 64 KB en formato de bloque de LZ4 con el mismo código en PPE, SPE y host (mismos bytes), usada para estados guardados (spukit README; estados.md).
- Principio de uso: **trabajo por lotes, no diálogo fino**. Una lista de comandos por cuadro, nunca un mensaje por triángulo (docs/proyecto.md de Salsa64; hilos-ppe.md).

### 3.3 Reglas de DMA

De spukit, de las convenciones de código de Salsa64 y de la HLE gráfica (lib/spukit/README.md, docs/proyecto.md de Salsa64, texturas.md):

- Direcciones alineadas a 16 bytes (mejor 128) y tamaños múltiplos de 16; transferencias de hasta 16 KB cada una. Lectura de dirección no alineada: traer el tramo alineado y desplazar (`spukit_get_unaligned` necesita `size + 32` bytes en el búfer).
- **Doble búfer**: procesar un bloque mientras llega el siguiente. Cada transferencia con su etiqueta; esperar solo la máscara de etiquetas que se necesita.
- **Escribir en memoria que otro procesador lee a la vez requiere esperar el DMA antes de que esa memoria se reutilice**: ver §3.7.
- Para estructuras compartidas con el PPE de 128 bytes, el SPE usa `getllar`/`putllc` (reserva de línea de 128 B) y el PPE `lwarx`/`stwcx` (`__atomic_*`) sobre las mismas palabras (rsp-lle.md «En el SPE»).
- Escrituras parciales: transferir de a 8 bytes o múltiplos de 16 **sin reescribir la memoria vecina** (rsp-lle.md).
- **Leer la GDDR3 del RSX desde el PPE o un SPE es lentísimo** (cifra pública ~16 MB/s; en la consola, leer el búfer del RSX para una captura frenó la presentación ~400 ms). Lo que un SPE o el PPE deba leer se deja en memoria principal mapeada al RSX (`rsxkit_map_main`) y el RSX la dibuja o lee desde ahí (uso-cell.md §1, hilos-ppe.md).

### 3.4 Presupuesto del local store

Se documenta en la cabecera de cada programa SPE y en la tabla de memory-budget.md. Ejemplos medidos con `spu-size`:

| Programa | Código | Datos+búferes | Pila |
|---|---|---|---|
| HLE gráfica (SPE 0) | 100 KB | ~144 KB | ~11,7 KB (holgura 1968 B en v0.2.6) |
| HLE de audio | 45 KB | ~34 KB | ~176–208 KB |
| JPEG vectorizado | 11,7 KB | ~7 KB | ~237 KB |
| Compresor LZ | 5,5 KB | ~143 KB | ~100 KB |

(memory-budget.md «Local store de los SPEs»; uso-cell.md §1.) Lo que cuesta pasarse de presupuesto aparece abajo en pila.

### 3.5 Holgura de pila medida al enlazar (`tools/spu-stack.py`)

En el SPE la pila baja desde `__stack` (arriba del local store) hacia `_end`; **nada avisa si se pasa: pisa las últimas variables de `.bss`** (en el SPE 0, los búferes de DMA de vértices, matrices y cargas) (tools/spu-stack.py; perf-log.md «Pila del SPE 0»).

- La herramienta suma, por la cadena de llamadas más honda desde `_start`, el marco de cada función: de los `.su` de `-fstack-usage` y, para funciones sin `.su` (libgcc, newlib), del prólogo del desensamblado (`ai $1,$1,-N`). Un marco `dynamic` (alloca) es un error.
- Las llamadas salen de `spu-objdump -d` (`brsl`/`brasl` y saltos de cola). **Las llamadas indirectas (`bisl`) no se pueden seguir**: una función alcanzable que las tenga se declara con `--edge DE:A` o se ignora con `--ignore`, y si no, es un error. Lo mismo la recursión (la cuenta no tendría tope).
- Sale con 1 si la holgura baja de `--min`; el Makefile (`<nombre>_STACK_MIN`, `_STACK_ARGS`) hace que **el enlace falle** (mk/ps3.mk). En el SPE 0 el mínimo es 1536 B y la cadena más honda ronda los 9,5 KB.
- Regla: **una llamada indirecta nueva en código SPU exige declararla** en el Makefile; lo olvidado hace fallar el enlace, no ocultar el problema.
- Anécdota con moraleja: la holgura del SPE 0 llegó a −3008 B tras varias ramas de arreglos gráficos; se recuperó compilando el combiner con `-Os` (−5,6 KB de código) pero eso multiplicó por 2,7–3,6 el costo de ese tramo en la consola; se arregló memorizando el resultado. Medir en la consola cualquier `-Os` de emergencia (perf-log.md «Pila del SPE 0»).

### 3.6 Numérica y vectorización en el SPE

- Una versión escalar en el SPE puede ser **más lenta que el PPE**: la IDCT del JPEG tardaba 140 µs por macrobloque en el SPE contra 100 en el PPE; vectorizada en 4 carriles pasó de 41,9 ms a **2,86 ms por sala [PS3]** (×14,7) (perf-log.md «Quitarle trabajo al PPE»). En el SPE se vectoriza o no gana nada.
- **El SPU redondea la simple precisión hacia cero**, no al más cercano: cualquier HLE que deba dar lo mismo en el PPE, el host y el SPE va en **enteros** (o doble). Las pruebas de coincidencia PPE contra SPE al arrancar («hash de texturas en el SPE: igual al del PPE») apagan la función si difieren (texturas.md, perf-log.md).
- Mover trabajo a un SPE libera el hilo de emulación pero no acorta el tirón si el juego espera el resultado: cuenta la duración, no el hilo.
- Las «bandas de líneas» para el rasterizador del plan de SalsaDS son **[diseño]**: Salsa64 no rasteriza en SPEs (el RDP va al RSX), así que no hay números de rasterizado en SPE.
- Referencia de lo que sí se midió: el RSP de la N64 a bajo nivel en un SPE (intérprete) corre a **226–283 ns por instrucción del RSP (3,5–4,4 MIPS), 14–18× más lento que el RSP real [PS3]** (rsp-lle.md; estado.md); por eso el trabajo se movió a un recompilador de RSP a código SPU (§3.8).

### 3.7 Concurrencia SPE↔PPE: errores que costaron días

Tomados del trabajo del RSP a bajo nivel (rsp-lle.md; docs/estado.md y rsp-jit.md de la rama del recompilador del RSP, aún no integrada en main). **Marcados «probable» los que no se reprodujeron en el host.**

1. **Publicar estado al otro procesador antes de terminar de escribir** (causa probable; corrección aún sin validar en la consola al escribir esto). El SPE publicaba la memoria local y ponía las banderas de «detenido» atómicamente, pero al cerrar el trabajo **volvía a escribir** esa memoria y los registros de DMA al bus. Un CPU que sondea el registro de estado ya había cargado la tarea siguiente al ver la bandera, y la segunda escritura la pisaba: la tarea siguiente arrancaba con la cabecera y el microcódigo de la anterior (determinista: la misma basura en tres corridas). Regla: **la bandera que dice «terminé» se escribe la última, y después no se escribe nada más**. El host, en modo lockstep, no lo reproducía porque no hay concurrencia real; solo la consola.
2. **DMA sin esperar con búferes reutilizados** (corrección aún sin validar en la consola). Al volcar tramos de una lista de comandos a una captura sin esperar el DMA, el consumidor veía «ya terminé» y reutilizaba el búfer fuente con su siguiente DMA, de modo que la captura llevaba comandos ajenos. Otra variante: la cabecera de un anillo leída justo tras un DMA sin esperar (`words=0`) hacía instalar como «libre» un trozo ocupado. Regla: **esperar la etiqueta antes de que el origen se reutilice**; los tiempos del host nunca lo exponen.
3. **Colisiones de huellas FNV-1a de 32 bits.** Una caché de código identificada por una huella FNV-1a de 32 bits sobre 65 palabras colisiona en la práctica (73 colisiones en 240 000 tareas de una suite): con una caché persistente se ejecutaba código de otro programa y salía geometría basura. Remedio: dos huellas independientes por trozo y un detector (pruebas que cuentan colisiones y trozos desactualizados). Regla: **una huella de código de 32 bits no identifica**; usar 64 bits o dos huellas, o comparar contenido.
4. **Estado compartido que se «consume» desde dos lados**: una interrupción pedida por cada fin de comando se perdía cuando un solo evento se reprogramaba; se arregló con una cola y contadores pedidas/levantadas/en cola. Regla: eventos que cuentan se encolan, no se reprograman.
5. **Latido para cuelgues**: cuando un juego «se cuelga» el diagnóstico útil fue un hilo que mira cada pocos segundos el estado de los dos lados (pc del SPE, registros de estado, último trabajo) y lo escribe en el log.
6. Para esperas de fin de tarea: sondeo con tiempo real dormido (hilos-ppe.md), nunca un bloqueo indefinido en el hilo de emulación.

### 3.8 JIT hacia el SPU: el local store ejecuta lo que llegue por DMA

Una consecuencia buena del Cell: **el local store ejecuta código traído por DMA sin las restricciones de lv2 que sufre el PPE**. Salsa64 lo aprovecha en un recompilador de RSP (código MIPS del RSP) a código SPU (docs/rsp-jit.md, en la rama del recompilador del RSP; aún no está en main, por lo que las siguientes cifras son del trabajo en curso y no están integradas). Lo que contó, de lab-recompilador.md «RSP → SPU» y del propio estado del trabajo:

- Se traducen **trozos de 64 instrucciones** de la IMEM de 4 KB; ~11,7 palabras SPU por instrucción invitada (19 en las vectoriales; los saltos cuestan 18 ciclos si se toman: sin `hbr` no hay pista de salto).
- **Arena** en el local store (del orden de 94–98 KB, 64 ranuras) con **desalojo LRU**; cuando el programa invitado tiene más trozos que arena (~64 trozos de ~2,9 KB no caben) se desalojaba sin parar.
- **Segundo nivel en memoria principal**: un anillo de 1 MB de código reubicable (se llama con `bra/brasl`), con un índice de 128 entradas en el local store; los trozos se traen por DMA cuando faltan.
- Registro de tramos y capturas del DMA van por páginas a memoria principal porque el local store se queda sin sitio (2 KB + 4 KB en LS contra 1 MB en principal). La pila del SPE de esa rama quedó en el mínimo (holgura 4096 B = el mínimo) y cualquier byte más obliga a quitar algo.
- Medido **[PS3]** en el trabajo en curso: 13–15 ns por instrucción del RSP en un juego de carreras (cerca de los 16 ns del RSP real a 62,5 MHz) y 49–55 ns en un juego con muchos trozos mientras el anillo traía ~75 trozos por tarea. Varias rondas de correcciones de sincronía (§3.7) seguían sin validar en la consola al escribir esto.

Valor para SalsaDS: **si el PPE no da para las dos CPUs, un SPE puede recompilar el ARM7 a código SPU** (su WRAM de 64 KB cabe en el local store; lo caro es cada acceso a memoria principal por DMA). Es una hipótesis **[diseño]**; el plan de SalsaDS ya la deja como alternativa.

### 3.9 Cómo se depura y se prueba lo del SPE

- **Primero en el host**: la lógica portable (HLE, rasterizador, intérprete del RSP) se prueba en la Mac en little-endian y en qemu-ppc64 en big-endian. Los programas SPU se prueban con la misma lógica compilada para el host, con una capa de E/S «por DMA» simulada.
- **RPCS3**: valida que el programa SPU arranca y da resultado, pero no reproduce la concurrencia ni los tiempos; **no sirve para validar sincronización entre SPEs ni rendimiento** (rpcs3.md; docs/proyecto.md de Salsa64). Para código SPU generado en tiempo de ejecución, el trabajo del recompilador de RSP indica que RPCS3 solo sirve con el decodificador de SPU en modo intérprete (configuración aparte, `salsa64-spu-interp.yml`, no integrada en main) **(verificar)**.
- Cuelgues que RPCS3 no reproduce: pidieron un latido por hilo, una línea de log por paso, y el log por UDP porque el archivo pierde las últimas líneas ante un apagado forzado.

---

## 4. RSX

### 4.1 La biblioteca rsxkit

Capa gráfica genérica del RSX, MIT, que no sabe nada de ningún sistema emulado (lib/rsxkit/README.md). Piezas:

| Pieza | Para qué sirve |
|---|---|
| `rsxkit_init`, `rsxkit_frame_begin/end` | inicialización del RSX y del video (primer modo disponible: 720p, 1080, 480, 576), doble búfer, flip sin esperar |
| `rsxkit_fb_*` | **framebuffer 2D**: sube una imagen de memoria principal a una textura lineal y la dibuja escalada (`FIT`, `INTEGER`, `STRETCH`; filtro lineal o puntual; proporción configurable). Formatos RGBA5551, XRGB1555, RGB565, RGBA8888, XRGB8888 en el orden de bytes de la PS3: **el RSX convierte con formato de textura y remapeo de canales, no el PPE** |
| `rsxkit_target_*`, `rsxkit_bind`, `rsxkit_depth_*` | destinos de render con **espacio lógico** que se estira (el núcleo dibuja en sus coordenadas, la escala interna la pone el tamaño del destino); un destino como textura; copia del destino a memoria principal (`rsxkit_target_read`) |
| `rsxkit_draw_triangles` + fragment programs | dibujo 3D: combinación lineal de dos texturas y color de vértice (tres programas precompilados) o un **programa generado en tiempo de ejecución** |
| `rsxkit/fpgen.h` | generador de microcódigo de fragment program NV40 en tiempo de ejecución, con caché por hash de la descripción (128 programas en 96 KB de memoria local) |
| `rsxkit/post.h` | posprocesado: superficies intermedias (RGBA8/RGBA16F), programas `.fpo` de cgcomp con constantes parcheables, pasadas de pantalla completa encadenadas, reducción con filtro de caja, etiquetas y **marcas de tiempo del RSX** |
| `rsxkit/text.h`, `rsxkit/ui.h` | texto con fuente de mapa de bits en un lienzo en memoria; 2D para menús (rectángulos con degradado, texto escalable, ondas) |
| `rsxkit_map_main` | hace visible para el RSX memoria principal (alineada a 1 MB, múltiplo de 1 MB) para vértices y texturas que escribe la CPU o un SPE, sin copias |
| `rsxkit_fence*` | barreras para reutilizar búferes que el RSX estaba leyendo |

Reglas de uso (README): todas las llamadas desde el mismo hilo (excepto `rsxkit_fence_done`), **sin asignaciones tras la inicialización**, rsxkit recuerda el estado y solo reenvía lo que cambió, y `rsxkit_vertices` invalida las cachés de vértices y texturas del RSX (llamarlo cada vez que cambió la memoria). Límites de tablas: 4 framebuffers 2D, 16 destinos y 16 profundidades vivos, 24 superficies de posprocesado, 32 programas, hasta 8 texturas por pasada. Tamaños: 2 búferes de color y uno de profundidad de pantalla (11 MB a 720p, 24 MB a 1080), 3 MB de memoria principal mapeada para el 2D.

Para SalsaDS el uso evidente: dos texturas de 256×192 por cuadro con `rsxkit_fb` o `rsxkit_target`, distribuciones con `rsxkit_layout`, filtros con `post.h`.

### 4.2 Generador de fragment programs (`fpgen`)

No hay compilador Cg en la consola; para lo que la combinación lineal no representa, rsxkit **emite el microcódigo del RSX** directamente (README; rsx-notes.md «Microcódigo del fragment program»):

- 4 palabras de 32 bits por instrucción, cada una con las mitades de 16 bits intercambiadas; una fuente constante (tipo 2) lleva los 16 bytes siguientes.
- Solo se puede leer **un registro de entrada por instrucción**; una segunda entrada hay que pasarla por `MOV` a un temporal.
- El resultado es `R0`; cgcomp hace las operaciones en fp16 y la última escribe `R0` con saturación; `TEX` va en precisión completa.
- Hay que quitar el **bit 31 de SRC2** que cgcomp pone en `MUL`, `DP*`, `DIV` y `NRM3`: desactiva la corrección de perspectiva (con w ≈ 2000 el término vale casi cero). El generador deja el bit en 0 y `fprog.c` lo limpia en cada programa subido.
- Para cargarlo: `num_regs`, `fp_control` y las máscaras `texcoords/texcoord2D/texcoord3D`.
- Se verifica sin consola con un **intérprete del microcódigo** que compara los programas generados con los que compila cgcomp para la misma fórmula (tests/unit/test_fpgen.c, tools/fpgen-ref.sh). El codificador no depende de PSL1GHT y compila en el host.
- Un juego usa decenas de programas distintos: la caché los guarda por hash de la descripción canónica; conviene que quien traduce ponga los términos en orden canónico.
- Costo: 1 instrucción por textura, 1 por término más 1 por factor adicional; cambiar de programa son ~20 palabras en el búfer de comandos.

SalsaDS puede **no necesitar fpgen** (el 3D de la DS es de función fija y el plan de SalsaDS prevé rasterizarlo en SPEs); sí lo necesitaría si dibuja el 3D en el RSX con combinaciones de textura y color como los modos de mezcla de la DS.

### 4.3 Trampas de cgcomp y de PSL1GHT con el RSX

(rsx-notes.md «Trampas de PSL1GHT» y «Fragment programs de posprocesado con cgcomp»; filtros.md «Errores de cgcomp»)

- `rsxSetVertexProgramConstants` está roto (deja palabras sin escribir en el FIFO; `count` son floats, no vec4): usar `rsxLoadVertexProgramParameterBlock`.
- `rsxSetTimeStamp` deja el dato del reporte en la palabra equivocada y el RSX escribe en cualquier parte (cuelgue en RPCS3): rsxkit escribe `GET_REPORT` (método 0x1800) a mano.
- `rsxLoadTexture` arma el formato como `format << 8 | 0x8000`: los `GCM_TEXTURE_FORMAT_*` de PSL1GHT van sin el bit 0x80 de Sony; el formato 23 (R5G5B5A1, `GCM_TEXTURE_FORMAT_R5G6B5A1` por error) es idéntico a RGBA5551.
- cgcomp compila con `-fastprecision`: baja a fp16 lo que cree que aguanta, también coordenadas en píxeles. Usar `-Wcg,-nofastprecision` (**solo un** `-Wcg,…`: dos opciones separadas por coma tiran a cgcomp con violación de segmento).
- `?:`, `sign()` y comparaciones que eligen valor: Cg las compila con códigos de condición que el ensamblador de cgcomp codifica mal. Usar `step()` y `lerp` con 0/1. `tools/fpo-info.py --sin-condiciones` las detecta y el build falla (`FPO_CHECK := 1` en mk/ps3.mk).
- `oCol` se escribe en `R0`, que Cg también usa como temporal: si el color se escribe por componentes y luego se lee `R0`, la cuenta sale mal. Armar el color y escribirlo en una sola instrucción (`tools/cg-salida.py` lo detecta y corrige en el ensamblador).
- `-|R0.z|` sale como `.zxxx`; el color de varias salidas (MRT) no existe (`COLOR1` se escribe en `R0` encima del 0).
- Cg corre con `-q`: los avisos no se ven. `lerp(1.0, float3, k)` elige la versión escalar y trunca el vector (C7011 invisible); escribir los argumentos como vectores del mismo tamaño.
- `exp(x)` se expande mal: usar `exp2(x·log2 e)`; `pow` de un `float3` son 9 instrucciones.
- `TEXCOORD0` llega con error ~1e-6: `floor(pos)+0.5` donde haya empates.
- El tamaño de superficie/tijera tiene un máximo de **4095**: una tijera con `x > 4095` **colgó la consola** (F-Zero X a 5× con una tijera de x0 = 1023,75 con x1 menor); RPCS3 la acepta en silencio. rsxkit recorta las cuatro coordenadas y no manda dibujos con tijera vacía (rsx-notes.md «Tijeras fuera del destino»; hilos-ppe.md). La atribución de la causa a esa tijera estaba «por confirmar en la consola» al escribirse el documento.
- Texturas: pitch múltiplo de 64, offset alineado a 128; invalidar la caché de texturas tras reescribir; antes de esperar una etiqueta hay que vaciar el búfer de comandos (`rsxFlushBuffer`) o la espera no termina. El búfer de comandos de PSL1GHT tiene un callback que corrompe el FIFO al cambiar de segmento: rsxkit pide el espacio antes de cada operación (README «Recursos»).
- Desactivar cortes por hardware: `rsxSetZMinMaxControl(ctx, FALSE, TRUE, FALSE)`; orientación: con y hacia arriba, frente antihorario (`GCM_FRONTFACE_CCW`).

### 4.4 El XMB pisa el estado del RSX

Medido **[PS3]**: con el menú del sistema (botón PS) encima, y un rato después de cerrarlo, **el sistema dibuja en cada flip de la aplicación y pisa estado del RSX** (constantes del vertex program, programas, arreglos); lo dibujado con estado «ya puesto» sale negro. Con `gcmSetWaitFlip` en el búfer de comandos cada flip tardaba 2,3–4,5 s y los eventos de sysutil se atendían cada varios segundos (ese fue el cuelgue de v0.1.0). Lo que rsxkit hace (README, hilos-ppe.md, y uso-ps3.md):

- el flip se espera **en el PPE** dentro de `rsxkit_frame_begin`, sin `gcmSetWaitFlip` en el RSX;
- `rsxkit_forget_state` tras cada `rsxkit_frame_end` (no se cachea estado entre cuadros) y `rsxkit_after_system` al cerrar el menú;
- `rsxkit_frame_ready` para no esperar con el sistema encima; **mientras el XMB está encima hay que seguir haciendo flips** o sale el reloj de espera;
- pausar la emulación y el sonido mientras el menú del sistema está abierto y atender `sysUtilCheckCallback`.

Regla para SalsaDS: **no saltarse la API de rsxkit** y no cachear estado del RSX que no pase por `forget_state`. Si algo sale negro tras el XMB, sospechar de estado cacheado nuevo.

### 4.5 Esperas al RSX con tope

Ninguna espera del PPE al RSX es infinita (hilos-ppe.md «Esperas al RSX con tope»): el gancho `rsxkit_set_wait_hook` se llama a los 0,5 s y cada 100 ms; a los 2 s deja una línea con qué se espera (etiqueta, valor esperado y actual, GET y PUT del búfer de comandos, si el RSX avanzó, qué se traducía); a los 12 s (45 s con el sistema encima) escribe el guardado diferido y sale (`forced_exit`). La espera de lugar en el búfer de comandos corre dentro del callback del sistema (sin fuente) y no puede llamar al gancho: la vigila el hilo de latido (`rsxkit_wait_now`). Motivo: un cuelgue con el RSX dejó la consola congelada, ni el botón PS respondía, y hubo que apagarla a la fuerza. Hay una bandera de prueba (`colgar-rsx`) que provoca los tres casos.

### 4.6 Límites y rendimiento del RSX

- GPU NV47 (familia G70/NV40), 500 MHz, 256 MB de GDDR3 a ~20 GB/s [pub]; 8 ROPs. Una pasada de pantalla completa a 1600×1200 en 32 bits mueve ~15 MB (23 MB con mezcla) (cifras públicas del RSX).
- **A 5× de la N64 (1600×1200) el RSX va justo**: con interpolación de cuadros, Zelda a 5× perdía la mitad de los cuadros intermedios y la zona del estanque translúcido pesa más; se probó bajar a 3× (interpolacion.md; no es una cifra de rendimiento pura).
- **Sin tile regions ni Z-cull**: rsxkit no llama a `gcmSetTileInfo/gcmBindTile` ni a `gcmSetZcull/gcmBindZcull` (comprobado en el código de `lib/rsxkit/src`). Es lo que más promete para ganar ancho de banda sin perder calidad y **no está probado** (no hay mediciones; la compresión no se debe adivinar: `comp = 0` primero). Otras ideas sin probar: fundir pasadas de pantalla completa, intermedios de 16 bits, mezcla apagada en lo opaco (**[diseño]**).
- Los filtros CRT del autor (en la parte GPL, ver §9) se miden con marcas de tiempo del RSX entre pasadas: a 1080p con 3× y reducción en dos pasadas, máscara 7,4 ms, RGB 9,4, S-Video 10,6, compuesto 13,2 y Trinitron 10,1 **[PS3]**; la reducción de 5× a la resolución de la consola cuesta 3,1 ms por leer una superficie de 7,6 MB (filtros.md «Costo en el RSX»; perf-log.md). Lo que va por píxel de salida crece 2,25× de 720p a 1080p.
- La **escala interna** debe acotarse con el tope de memoria de destinos (112 MiB de los 256 MB de GDDR3 para destinos, scratch y profundidades en Salsa64) y cada destino elige su escala entera (memory-budget.md «Escala interna y tope de los destinos»).
- RPCS3 mezcla mal dentro de una sola llamada de dibujo (lo dibujado antes en la misma llamada no se ve): rsxkit manda una llamada por elemento del 2D para verse igual en los dos (rsx-notes.md).
- RPCS3 compila los shaders en segundo plano: la primera corrida tras cambiar un fragment program puede saltarse dibujos; repetir antes de concluir. Para leer el framebuffer desde el PPE hay que activar «Write Color Buffers» (rsx-notes.md, rpcs3.md).

### 4.7 Lo genérico de filtros, texturas y framebuffer

Filtros (filtros.md):
- Los shaders de escritorio calculan todo por píxel de pantalla; en el RSX el costo lo manda el número de instrucciones por píxel, así que se repartieron en **pasadas**: lo que no cambia de un píxel al vecino se calcula una vez en una tabla pequeña (tamaño de la imagen del juego o de un punto de fósforo), y la pasada final solo combina.
- Superficies intermedias `W16Z16Y16X16` (4×fp16) y `A8R8G8B8`, leídas como textura con coordenadas en texeles (`GCM_TEXTURE_FORMAT_UNRM`) y borde en 0.
- Verificación: una referencia en C (`tools/crt-ref.c`) en doble precisión que se compara píxel a píxel con capturas de pantalla entera (filtros.md «Verificación»); RPCS3 traduce el microcódigo a GLSL, la consola puede diferir en el filtrado fp16 y el modo `BORDER` (comprobación al primer uso).
- Las capturas de pantalla no deben leerse desde el PPE (≈2 s de imagen quieta): el RSX redibuja la pasada final en una superficie de memoria principal y un hilo aparte escribe los archivos.

Texturas (texturas.md; ejemplos de N64 que son genéricos como patrón):
- **Identidad por contenido**: hash de 64 bits de los bytes que lee la decodificación (ronda de xxHash32 en ocho carriles) en lugar de marcas por página, porque las escrituras de la CPU no avisan y marcar cada almacenamiento costaría en el camino caliente. Con SIMD cuesta ~1 µs por textura de 4 KB contra ~100 µs de decodificar. El PPE, el host y el SPE dan el mismo hash (se comprueba al arrancar).
- Las texturas viven en una región de memoria principal mapeada al RSX (8 MB), administrada como **anillo con desalojo**: lo que el RSX aún puede leer no se toca; se sabe cuánto avanzó el RSX con barreras por tarea.
- Resultado en la consola: HLE gráfica de 14,0–30,6 ms por tarea a 5,1–9,6 ms (×2,8–3,3) (perf-log.md).
- Para SalsaDS: la VRAM de la DS en bancos mapeables requerirá una identidad parecida (contenido o versión por banco) para no reconvertir texturas.

Framebuffer (framebuffer.md): los juegos que leen de vuelta lo dibujado (efectos de pantalla) obligan a copiar RSX → memoria principal. El mecanismo: reducir el destino con un quad sobre una superficie en memoria principal mapeada, esperar la barrera y convertir. Es caro; se decide por juego cuándo emularlo. En la DS esto aparece con la captura de pantalla (`DISPCAPCNT`) **[diseño]**.

---

## 5. Memoria principal

- La PS3 tiene 256 MB XDR de los que la aplicación recibe **~209–213 MB** (214 016 KB de memoria de usuario medidos con HEN, menos los segmentos del ELF, incluida la cola NOBITS de la caché de código) (memory-budget.md, jit-ppe.md). Hay que tener un `docs/memory-budget.md` con previstos y medidos desde el primer día; el de Salsa64 suma ~189 MB previstos contra ~214 disponibles con una ROM de 64 MB.
- **Páginas**: el heap, la ROM, la pila y los búferes se reparten en páginas de 4 KB, 64 KB o 1 MB; el ERAT cubre 4 KB por entrada pase lo que pase (cache-ppe.md §1).
- **Sin asignaciones en el bucle de emulación**: todo preasignado al arrancar (docs/proyecto.md de Salsa64; rsxkit README).
- **El heap de newlib no devuelve huecos bajo un tope ocupado** (memory-budget.md «Fragmentación del heap y la ROM de 64 MB»): tras abrir y cerrar varios juegos, las reservas permanentes del primer juego (una foto de estados de 8,4 MB, anillos del perfilado, búferes de filtros) quedaban por encima de la ROM, y las ROMs de 64 MB dejaban de caber (un `memalign` devolviendo NULL a los 15 ms). **Reproducido en RPCS3**, **arreglo [RPCS3] validado, pendiente de la consola**: la ROM sale de páginas de 1 MB del sistema (`sysMemoryAllocate`) y se devuelve entera al cerrar el juego. Regla: **nada grande y duradero con `memalign`/`malloc` mientras haya una ROM (o cualquier búfer grande y transitorio) en el heap**; lo grande y duradero va fuera del heap o se reserva al arrancar; lo pasajero sí puede ir en el heap. Si falla `sysMemoryAllocate`, reservar al arrancar. Hay un reporte del heap por fases (`heap_report`: `mallinfo`, `sbrk(0)`, syscall 352 y la mayor reserva que cabe).
- La memoria compartida con el RSX se mapea con `rsxkit_map_main` (alineada y múltiplo de 1 MB) y se cuenta en el presupuesto de XDR. Salsa64 mapea ~28 MB para la salida de la HLE, texturas y comandos.
- Presupuesto de memoria local del RSX (GDDR3): framebuffers de pantalla + destinos de render (escala × tamaño al cuadrado × 4 bytes) + posprocesado; con un tope por superficie de 4096 y un tope total (memory-budget.md).
- Para SalsaDS: la RAM de 4 MB y la VRAM de ~656 KB caben holgadamente; lo que presupuestar es la caché del recompilador (32 MB es el techo útil), texturas decodificadas, destinos de render a escala alta, y los búferes del DMA/anillos de los SPEs.

---

## 6. Toolchain y flujo de trabajo

### 6.1 Imagen Docker

(toolchain.md; tools/docker-image.sh; tools/build.sh)

| Componente | Versión |
|---|---|
| ps3toolchain | commit `e13aa539…` (2026-09-13) |
| GCC PPU | 13.2.0 (`powerpc64-ps3-elf`, `--with-cpu=cell`); GCC SPU 9.5.0; binutils 2.22 (parcheado) |
| newlib | 1.20.0 |
| PSL1GHT | commit `f649a08f…` (2026-06-29) |
| NVIDIA Cg Toolkit | 3.1 (2012) x86_64 |
| Base | `debian:12-slim` |

- Toolchain **nativo arm64** (9 min para la imagen completa en una Mac de 4 CPU). `cgcomp` carga `libCg.so` con `dlopen()` y el Cg Toolkit solo existe para x86_64: se corre bajo `qemu-x86_64-static` (`docker/cgcomp-wrapper.sh`). El Cg Toolkit es propietario: la imagen es solo de uso local, **no se publica** en un registro.
- `tools/build.sh [-C dir] [objetivo]` corre `make` dentro de la imagen; `tools/build.sh --sh '…'` otro comando. Calcula la versión del árbol (`git describe`) en el host, porque dentro de Docker el `.git` de un worktree apunta a una ruta del host que no está montada.
- **No usar `-Os` en el PPU** (ICE de GCC 13.2 en `rs6000_savres_routine_name`); `mk/ps3.mk` usa `-O2 -mcpu=cell -mno-relative-jumptables`. Compilar con `-Os` el combiner de un SPE sí se hizo (§3.5).
- `mk/ps3.mk` reemplaza los Makefiles recursivos de los samples: objetos en `build/<programa>/obj`, programas SPU y shaders embebidos con `bin2s`, variables `TITLE`, `APPID`, `SPU_PROGS`, `FPO_CHECK`, `CGCFLAGS`.
- De los 31 samples de PSL1GHT, 18 compilan; las fallas son de los samples (Makefile viejo `ppu.mk`, ps3libraries ausentes, firma vieja de `rsxInit`).

### 6.2 Pruebas big-endian en la Mac

(rec-ppe.md «Pruebas en el host»; tools/qemu-ppc64.sh; tools/test-rec.sh)

- El núcleo se compila para Linux ppc64 big-endian (misma ABI ELFv1 que PSL1GHT) con `gcc-powerpc64-linux-gnu` de Debian 12 y se corre con `qemu-ppc64-static` (imagen aparte: `tools/docker-image-ppc64.sh`, `docker/ppc64.Dockerfile`).
- **CPU de qemu: `power9`**. `-cpu 970` (el más parecido al PPE) da resultados erróneos (el intérprete falla 15 pruebas que pasan en la PS3) y `power5+` se cuelga; con `power7/8`, `sin`/`cos`/`floor` de la glibc estática mueren con SIGILL por `mffscrn`. El emisor del recompilador se limita **a mano a Power ISA 2.02** (sin `isel`, `popcntd`, `ldbrx`) porque `power9` acepta instrucciones que el PPE no tiene.
- La caché del recompilador en el host va en `.bss` con `mprotect` para que las llamadas sean `bl` como en la PS3; `SALSA64_REC_FAR=1` prueba el camino lejano.
- Un compilador cruzado que corre emulado como amd64 a veces muere con «internal compiler error»; repetir compila (el script reintenta una vez).
- `tools/test-rec.sh` corre unitarias en cuatro modos, la suite de CPU, estados guardados, perfilado y, con `--lockstep`, la comparación bloque a bloque (~10 min). Observación: devuelve 1 sin decirlo cuando falla la pasada del recompilador; mirar el log.
- Para este tipo de pruebas **la máquina invitada de SalsaDS es little-endian**: las pruebas deben correr en LE (Mac) y BE (qemu) para cazar errores de orden de bytes.

### 6.3 Paquetes `.pkg`, título y APPID

(docs/proyecto.md de Salsa64, mk/ps3.mk, uso-ps3.md)

- El `.pkg` se arma con `make pkg` (`sfo`, `make_self_npdrm`, `package_finalize`) dentro del contenedor; el título y el ID de aplicación salen de `TITLE` y `APPID` (4 letras y 5 dígitos), `CONTENTID = UP0001-<APPID>_00-…`.
- **Package Manager del XMB lista por el título del `PARAM.SFO`, no por el nombre del archivo.** Si varias variantes comparten título, el usuario no encuentra la nueva. Regla de Salsa64: **cada paquete de prueba con TITLE único (con número de ronda)** y APPID propio si no debe pisar a otra app instalada (mismo APPID = misma app). Se compila con `TITLE="…" APPID=… PROGRAM=…` y se borra antes `build/<programa>/obj/pkg` o el `PARAM.SFO` no se regenera.
- Cuidado con abrir una app vieja creyendo que es la nueva: Salsa64 muestra arriba a la derecha del menú `vX.Y.Z+N · hash` y, si el paquete no es el publicado, `título · APPID`; la **primera línea del log** es «<programa> <git describe> (<TITLE>, <APPID>)». Mirar esa línea antes de concluir de una prueba (uso-ps3.md «Versión»).
- **Instalar no ejecuta**: el paquete se instala desde Package Manager y se abre desde la columna Juego. **HEN se apaga al reiniciar la consola**; sin HEN no hay FTP de webMAN ni se instalan paquetes de homebrew (observado durante el desarrollo; no está en docs/).
- El `.pkg` **no es reproducible**: dos armados desde el mismo commit dan SHA distintos con el ELF idéntico. Para saber si un paquete trae el código del commit, comparar el SHA-1 del `.elf` o mirar que el build recompiló los `.c` tocados.
- El tamaño de `EBOOT` casi no depende de la caché NOBITS (no ocupa espacio); `make_self_npdrm` comprime cada segmento con zlib (jit-ppe.md).

### 6.4 Reloj de Docker y objetos viejos

El reloj de la imagen Docker iba **~6 h adelantado** al del host: los objetos del contenedor salen «más nuevos» que las fuentes editadas hace menos de 6 h y `make` no las recompila (el log avisa «Clock skew detected»). Dos paquetes de la consola salieron idénticos byte a byte pese a cambios de código. Regla: **antes de armar un paquete para la PS3, o de una corrida que valide un cambio, `rm -rf build/<programa>`** (o `make -B`) y comprobar en el log que las fuentes tocadas aparecen como `CC`. `.SECONDARY` en mk/ps3.mk hace que borrar objetos sueltos no los recompile si el ELF está al día.

### 6.5 Despliegue por FTP y `ps3load`

(tools/deploy-ps3.sh; uso-ps3.md)

- `tools/deploy-ps3.sh ftp <archivo>`: sube un `.pkg` a `/dev_hdd0/packages/` y cualquier otra cosa a `/dev_hdd0/salsa64/` por el FTP de webMAN MOD (`curl -T`). `ps3load` corre un `.self` por TCP (la app `ps3load` en la consola escucha en 4299). La IP va en `tools/ps3.conf` (ignorado por git).
- Para configuración y banderas de depuración, Salsa64 usa **archivos en `/dev_hdd0/salsa64/`** (`sin-recompilador`, `lle`, `capturas`, `cajanegra`, `turbo`, `perfil-activo`, `log.cfg`…) que el programa lee al abrir cada juego y que se crean por FTP: permiten A/B en una sola corrida con un solo paquete (uso-ps3.md «Archivos de configuración»; cache-ppe.md §5). Cuidado: algunas son globales a todas las apps; renombrarlas a `.off` al terminar.
- **Los archivos de la consola que importan se bajan al terminar la corrida**: el archivo de log se reescribe al abrir otra app.

### 6.6 Logger UDP

(tools/udplog.py; src/platform/ps3/log.c; uso-ps3.md «log.cfg»)

- El programa escribe cada línea al archivo `/dev_hdd0/salsa64/<programa>.log` (con `fflush`, línea por línea) y por UDP. Por omisión, difusión al puerto 18194; `log.cfg` fija `ip[:puerto]` de destino (unicast).
- Cada línea lleva un número de secuencia; `tools/udplog.py` imprime la IP de origen, resume las perdidas/duplicadas y tiene `--until TEXTO` para automatizar pruebas.
- **Con unicast y varios receptores en el mismo puerto, cada paquete llega a uno solo** (macOS entrega a uno): con varios programas escuchando en la misma Mac, el log de la consola caía en el receptor equivocado. Regla: **un receptor por puerto**; SalsaDS debe usar **un puerto distinto del de Salsa64** (ya lo prevé su plan) o leer el log por FTP.
- Las corridas de RPCS3 en la misma Mac también emiten por broadcast al mismo puerto, y un receptor las mezcla con las de la consola.
- **Bajar el log por FTP al terminar cada corrida** (`curl ftp://IP/dev_hdd0/salsa64/<programa>.log`): tiene todo lo de la ejecución en curso y no se reescribe hasta abrir otra app. Si la hora del archivo no cambió, el programa no llegó a `main()` o abrieron otra app.
- Para cuelgues que matan el proceso o apagan la consola, **el archivo pierde las últimas líneas**: la traza por UDP dice dónde fue (una línea de log por paso del arranque o de la operación sospechosa).
- Un tropiezo de scripts: `tools/build-ppc64.sh` fija `OUT=build/ppc64` por dentro; `OUT=…` del entorno no hace nada, y `ld` reemplaza el archivo, así que un qemu en curso sigue con el binario viejo.

### 6.7 RPCS3 sin interfaz

(rpcs3.md; tools/rpcs3-run.py; tools/rpcs3/*.yml)

- `tools/rpcs3-run.py <programa>` lanza `rpcs3 --no-gui --config tools/rpcs3/salsa64.yml <elf>`, crea el archivo de modo autotest (el programa sale solo), sigue `TTY.log` hasta la línea `RESULTADO` y copia `RPCS3.log` a `build/logs/`. Códigos de salida: 0 con `RESULTADO` sin `FALLO`, 1 con `FALLO`, 2 sin `RESULTADO`.
- La config activa «Write Color Buffers» (para leer el framebuffer desde el PPE), red conectada (para que el UDP llegue a la Mac) y «Exit RPCS3 when process finishes». Hay un segundo YAML con el decodificador de PPU en intérprete.
- **RPCS3 vacía `/dev_hdd0/tmp` en cada arranque**: los archivos del programa van en `/dev_hdd0/<carpeta propia>/`.
- **No mandar SIGTERM mientras se cierra** (aborta con «Emulation object is unavailable»); solo SIGKILL si se cuelga. Los errores fatales en `--no-gui` dejan una ventana de error en un proceso aparte (`rpcs3 --error …`) que el lanzador detecta y cierra.
- **RPCS3 es una sola instancia con un solo `dev_hdd0` y un solo `TTY.log` para todos los worktrees**: dos corridas a la vez se pisan y dan «sin RESULTADO» falsos. `rpcs3-run.py` toma un candado compartido; los scripts que llaman a RPCS3 sin pasar por él siguen expuestos. Antes de concluir que una prueba de RPCS3 falló, comprobar si había otra corrida (`pgrep -x rpcs3`).
- RPCS3 puede matar el RSX de un juego a los ~16 s con una verificación interna; no sirve para A/B de ese juego. Para comparar dos builds del RSX en la consola se usa un volcado de la HLE reproducido en cada app, que se cierra sola y deja la imagen para comparar píxel a píxel.
- Firmware 4.92 (`PS3UPDAT.PUP`, descargado por el usuario y nunca incluido en el repositorio), instalado con `rpcs3 --installfw` (rpcs3.md).

### 6.8 Documentación de uso

uso-cell.md reparte el uso de PPE, SPEs y RSX **por juego** con medianas de ventanas de 5 s (y p90) y marca cada cifra como [pub], [med] o [est]; es un buen modelo de cómo documentar el reparto de SalsaDS. uso-ps3.md es la guía de usuario (instalación, dónde poner ROMs, archivos de configuración, caja negra, «Si el juego parece colgado»). Los tiempos por juego de uso-cell.md **son de la versión de entonces** y deben leerse con cuidado.

---

## 7. Trampas de PSL1GHT y lv2

| Trampa | Efecto y remedio | Fuente |
|---|---|---|
| `sysFs*` de libsysfs (`sysFsOpen/Write/Read/Mkdir/Stat`) sin `sysModuleLoad` | **cuelga la consola entera**; hubo que apagarla con el botón y reactivar HEN. RPCS3 no lo reproduce. Usar `sysLv2Fs*` de `<sys/file.h>` o el stdio de newlib | observado en la consola (estados guardados) |
| `rsxSetTimeStamp` | deja el dato del reporte en la palabra equivocada; el RSX escribe en cualquier parte. Escribir `GET_REPORT` a mano | rsxkit README, rsx-notes.md |
| `rsxSetVertexProgramConstants` | deja palabras sin escribir en el FIFO; usar `rsxLoadVertexProgramParameterBlock` | rsx-notes.md |
| Callback del búfer de comandos de PSL1GHT | corrompe el FIFO al cambiar de segmento: pedir el espacio antes de cada operación | rsxkit README |
| `GCM_TEXTURE_FORMAT_R5G6B5A1` | en realidad es R5G5B5A1 (= RGBA5551 de la N64) | rsx-notes.md |
| Escribir en `.text` (HEN) | permitido; en datos, ejecutar **mata el proceso**. En CFW puede dejar el hilo colgado (no mata el proceso) | jit-ppe.md, jit-cfw.md |
| Salir por el XMB con hilos SPU vivos | **colgó la consola** al cerrar: el cierre forzado de Salsa64 para los SPEs y luego llama a `sysProcessExit` | estado.md; `forced_exit` en src/frontend/main.c |
| `gcmSetWaitFlip` en el búfer de comandos | el XMB retiene los flips segundos | hilos-ppe.md |
| Espera infinita al RSX (flip/etiqueta/FIFO) | congela la consola; poner topes | hilos-ppe.md |
| Tijera o superficie de más de 4095 píxeles | cuelga el RSX | rsx-notes.md |
| Syscall 327 (`sys_mmapper_enable_page_fault_notification`) | EINVAL: no hay fastmem | fastmem.md |
| `sys_mmapper_change_address_access_right` (336) | EINVAL: sin solo lectura por página | fastmem.md |
| Atributos de página (syscall 351) | solo de páginas mapeadas; útil para registrar el tamaño de página de la caché | fastmem.md, jit-cfw.md |
| `sys_memory_get_user_memory_size` (352) | PSL1GHT no la envuelve; se llama directamente | jit-ppe.md |
| Prioridades de hilos | en lv2 un número menor es mayor prioridad: Salsa64 usa latido 500, emulación 1000, presentación 1001, muestreo 1100, vaciado del perfilado 1500, prueba del JIT 2000 | hilos-ppe.md, rec-ppe.md, jit-cfw.md, perfilado.md |
| `-Os` en el PPU | ICE de GCC 13.2 con ciertos samples | toolchain.md |
| Archivos de log | un apagado forzado pierde las últimas líneas; usar UDP | uso-ps3.md, observado |
| `/dev_hdd0/tmp` en RPCS3 | se vacía en cada arranque | rpcs3.md |
| Leer el búfer del RSX desde el PPE (capturas) | detiene la presentación ~400 ms | hilos-ppe.md |
| `sin`/`cos`/`floor` de la glibc estática bajo qemu | usan `mffscrn` (Power ISA 3.0): SIGILL con `-cpu power7/8`; usar `power9` | rec-ppe.md |


---

## 8. Medir

- **RPCS3 no sirve** para rendimiento, para sincronización entre SPEs ni para el JIT del PPE (rpcs3.md, docs/proyecto.md de Salsa64). Sirve para comprobar que arranca, ver imágenes y ejecutar el autotest. Un recompilador LLVM del PPU y un intérprete dan MIPS no comparables con la consola (29–36 MIPS contra 8,8 en la consola con el mismo código).
- **Todo número de rendimiento sale de la PS3 real**, con el mismo paquete y protocolo fijo (misma escena, misma ventana de tiempo, ventanas de 5 s, mediana de las ventanas pesadas, p10/p90) y con el A/B por archivo-bandera en la misma corrida cuando se pueda. `docs/perf-log.md` es una tabla con fecha, commit, prueba, entorno, antes, después y notas; cada optimización deja su fila (perf-log.md).
- El log de estadísticas cada 5 s es la fuente: campos/s, MIPS, % en bucles de espera, ms de emulación por campo y el peor campo, hilo 1, RSX por cuadro, SPE por tarea; sirve `tools/jit-perfil.py` para ns por instrucción en las ventanas pesadas.
- **Cifras que no se miden en la consola se estiman por modelo** (el laboratorio, §2.6) y se marcan [est]; el modelo sirve para ordenar, no para prometer.
- **Contadores de hardware del Cell** (PMU) son privilegiados: no hay datos de ciclos de espera por causa. Se sustituye por muestreo por tiempo, contadores propios con `mftb` (timebase de 79,8 MHz) y el decrementador del SPE.
- **App de perfilado** (perfilado.md, tools/perfilado/perfilado.py): una variante de la aplicación con todo el registro encendido que deja una sesión por partida en el disco (`perfilado/<juego>-<CRC>/<n>.sesion`) con contexto, ventanas de 5 s, tirones por causa, compilaciones por motivo, traza de bloques, JR, muestreo, y volcados; tope de 200 MB. Los archivos **no llevan ni un byte del juego** (direcciones, cuentas, tiempos, huellas de un sentido e ids). Los registros van a anillos en memoria (2 MB el hilo de emulación y 512 KB el de presentación) que un hilo de prioridad 1500 vacía cada medio segundo; si se llena, se pierde y se cuenta. El lector `perfilado.py` tiene `resumen`, `comparar` (dos sesiones; flecha en lo que empeora más de 10 %), `juegos`, `traza`, `json`. Es una buena herramienta para copiar y adaptar.
- **Un tirón** (campo de emulación > 25 ms) se explica con lo que ocurrió en él (bloques compilados, desalojos, espera al SPE, espera por búferes del RSX, copias, JPEG) (perfilado.md).
- Costos de medir: el muestreo ~5 %, contar entradas a bloques ~5 %; si algo pasa del 10 % se apaga (perfilado.md «Costo»).
- Medir cada ejecución con una pausa o el menú del sistema abierto infla el camino lento: excluir esas ventanas.
- **Medir la propia herramienta**: la medición del hilo de presentación (`mftb`, reloj de «despierto») y el A/B con una «carga de mentira» en la misma corrida separan lo que el hilo 1 cuesta por ms de trabajo del sesgo de que las escenas pesadas traen más trabajo (hilos-ppe.md).
- **El ruido**: variaciones de ±3 % entre ventanas son normales; un cambio < 3 % no se concluye con una sola corrida. Una hipótesis de «lo mide el host» sobreestimó 3–4× lo que la consola dio (§2.6).

---

## 9. Qué reutilizar y cómo

### 9.1 Bibliotecas MIT con repo propio

`rsxkit` y `spukit` son MIT y tienen repositorio propio (docs/bibliotecas.md):

| Biblioteca | Repositorio | En Salsa64 |
|---|---|---|
| rsxkit | https://github.com/ricowp81/rsxkit | `lib/rsxkit/` |
| spukit | https://github.com/ricowp81/spukit | `lib/spukit/` |

Se traen con `git subtree` (el plan de SalsaDS ya prevé `lib/rsxkit` y `lib/spukit` así):

```sh
git remote add rsxkit https://github.com/ricowp81/rsxkit.git
git remote add spukit https://github.com/ricowp81/spukit.git
git subtree add --prefix=lib/rsxkit rsxkit main --squash   # (o sin --squash)
git subtree add --prefix=lib/spukit spukit main --squash
# para traer cambios después:
git subtree pull --prefix=lib/rsxkit rsxkit main
```

Las reglas de docs/bibliotecas.md: nada de `src/` en `lib/`, nada de código GPL, todo origen registrado en `docs/THIRD_PARTY.md` y en el `THIRD_PARTY.md` de la biblioteca; un commit que toca `lib/` no debe tocar otras carpetas. Al mandar cambios de vuelta: `git subtree push --prefix=lib/rsxkit rsxkit main`. **(verificar)** el flag `--squash` y la dirección de la sincronización: bibliotecas.md describe `pull` y `push` pero no `add`; el `add` de arriba es el uso estándar de git subtree, no un comando citado de Salsa64.

Estado de cada una:
- **rsxkit** hace: inicialización del RSX y video, ciclo de cuadro, framebuffer 2D, destinos de render, 3D con combinación lineal o fragment programs generados, texto, 2D para menús, posprocesado. **No** incluye todavía la conversión de texturas en SPE. Los filtros concretos no van aquí: los pone quien usa rsxkit con sus programas. Contiene contribuciones: una fuente de dominio público (8x13 de X11), Scale2x/EPX (algoritmo, sin copiar código) y el soporte de imágenes del 2D de un colaborador, también MIT con su conformidad (lib/rsxkit/README.md «Origen del código»; THIRD_PARTY.md).
- **spukit**: cola + timbre, trabajadores persistentes, compresión LZ de bloques (formato de LZ4 escrito desde la especificación, sin fuentes de LZ4).

### 9.2 Lo que NO se puede reutilizar libremente

El resto de Salsa64 es **GPLv2 o posterior**. docs/THIRD_PARTY.md distingue el código propio del adaptado. Resumen de lo que SalsaDS **no** debe copiar sin decidir antes su licencia (y sin avisar en su propio THIRD_PARTY.md):

- **Código adaptado de proyectos GPL** (GPLv2+; no relicenciable a MIT, y entra en SalsaDS solo si SalsaDS es GPL compatible): `src/rsp/audio/alist.c`, `abi.c`, `mp3.c` (de mupen64plus-rsp-hle, con sus avisos de copyright) y `src/rsp/jpeg/jpeg.c` (de mupen64plus-rsp-hle). Es específico de la N64.
- **Código con licencia mixta**: `src/frontend/cover.c` incluye el desinflado de `puff.c` de Mark Adler (licencia zlib) bajo `GPL-2.0-or-later AND Zlib`; `src/rsp/audio/mp3_lut.c` lleva la ventana de síntesis de MPEG-1 tomada de FFmpeg (LGPL 2.1+, compatible con GPLv2+); `src/rsp/lle/vu.c` y `vmem.c` reescriben en C la semántica de ares (ISC).
- **Solo consulta, sin copiar código**: GLideN64, Mupen64Plus (video y núcleo), Not64/Wii64, angrylion-rdp-plus (licencia no comercial de MAME), RPCS3, el port de SM64. El recompilador, el intérprete, la HLE gráfica y el núcleo de Salsa64 son código propio escrito desde manuales públicos.
- **Filtros CRT del autor** (`src/frontend/filters/`): GPL, **fuera de `lib/rsxkit`** a propósito. Quien quiera filtros de CRT en SalsaDS debe reescribirlos sobre `rsxkit/post.h` o aceptar GPL.
- El Cg Toolkit y el ejecutable de cgcomp (NVIDIA, propietario) son locales; no se publica la imagen Docker.
- Reglas de ROMs/BIOS/IPL3 en `docs/THIRD_PARTY.md`: nada de material de Nintendo o Sony; las ROMs de prueba se compilan con toolchains libres (en Salsa64, nemu64-test MIT y propias).

Contra una GPL de SalsaDS: el código propio del recompilador, el intérprete y la HLE de Salsa64 es GPLv2+ y se puede reutilizar si SalsaDS adopta GPLv2+ (o GPLv3+); no puede ser MIT.

### 9.3 Herramientas que conviene copiar y adaptar

| Herramienta | Para qué | Notas para adaptar |
|---|---|---|
| `tools/build.sh`, `tools/docker-image.sh`, `docker/Dockerfile`, `docker/cgcomp-wrapper.sh` | build en Docker | cambiar el nombre de la imagen; compartir la imagen es opcional |
| `mk/ps3.mk`, `mk/rec-cache.ld` | reglas de build, SPU embebido, paquetes, caché NOBITS | `TITLE/APPID/PROGRAM` propios |
| `tools/deploy-ps3.sh`, `tools/udplog.py`, `src/platform/ps3/log.c` | envío por FTP/ps3load y log UDP | puerto distinto (18194 es de Salsa64) |
| `tools/rpcs3-run.py`, `tools/rpcs3/*.yml` | RPCS3 sin interfaz | ruta de datos propia en `dev_hdd0` |
| `tools/qemu-ppc64.sh`, `tools/build-ppc64.sh`, `tools/test-rec.sh`, `docker/ppc64.Dockerfile` | pruebas big-endian y lockstep | la suite y los modos son de la N64 |
| `tools/spu-stack.py` | holgura de pila de SPU | ya es genérico (solo pide `--bin` y objetos) |
| `tools/cg-salida.py`, `tools/fpo-info.py`, `FPO_CHECK` | corregir y revisar fragment programs de cgcomp | genéricos |
| `tools/lab/` (rec-lab, lab-analiza.py, lab-cache.py) | laboratorio del recompilador | atado al emisor de MIPS; la idea es reusable |
| `tools/perfilado/perfilado.py` y el registro de sesiones | perfilado de sesiones de juego | el formato es de Salsa64 |
| `tools/fpgen-ref.sh` y el intérprete de microcódigo de pruebas | verificar fragment programs sin consola | viene con rsxkit |
| `src/platform/ps3/rec_cache.c` | prueba y respaldo de la caché de código | muy aprovechable |
| `src/frontend/pacer.c` (ritmo), `hilos-ppe.md` | estructura de dos hilos con ritmo exacto | portable y con pruebas |

---

## Lecturas por tema

Todas en https://github.com/ricowp81/Salsa64/blob/main/<ruta>.

| Tema | Documento | Enlace |
|---|---|---|
| Visión general del proyecto y fases | `docs/proyecto.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/proyecto.md |
| Estado cronológico y decisiones (largo; buscar por tema) | `docs/estado.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/estado.md |
| JIT en el PPE: mecanismos, tamaño de caché, decisión | `docs/jit-ppe.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/jit-ppe.md |
| JIT con CFW y respaldo al intérprete | `docs/jit-cfw.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/jit-cfw.md |
| Script de enlace de la caché de código | `mk/rec-cache.ld` | https://github.com/ricowp81/Salsa64/blob/main/mk/rec-cache.ld |
| Diseño del recompilador | `docs/rec-ppe.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/rec-ppe.md |
| Laboratorio del recompilador | `docs/lab-recompilador.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/lab-recompilador.md |
| Jerarquía de memoria del PPE (cachés, ERAT, palancas) | `docs/cache-ppe.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/cache-ppe.md |
| Dos hilos, ritmo, esperas al RSX con tope | `docs/hilos-ppe.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/hilos-ppe.md |
| Fastmem (bloqueado) y base en caché | `docs/fastmem.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/fastmem.md |
| RSP a bajo nivel en SPE (sincronía PPE↔SPE) | `docs/rsp-lle.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/rsp-lle.md |
| spukit | `lib/spukit/README.md` | https://github.com/ricowp81/Salsa64/blob/main/lib/spukit/README.md |
| Pila de SPE | `tools/spu-stack.py` y `docs/perf-log.md` («Pila del SPE 0») | https://github.com/ricowp81/Salsa64/blob/main/tools/spu-stack.py |
| rsxkit | `lib/rsxkit/README.md` | https://github.com/ricowp81/Salsa64/blob/main/lib/rsxkit/README.md |
| Notas del RSX, cgcomp y PSL1GHT | `docs/rsx-notes.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/rsx-notes.md |
| Filtros en el RSX (pasadas) | `docs/filtros.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/filtros.md |
| Caché de texturas por contenido | `docs/texturas.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/texturas.md |
| Copias RSX ↔ memoria principal | `docs/framebuffer.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/framebuffer.md |
| Presupuesto de memoria y heap | `docs/memory-budget.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/memory-budget.md |
| Toolchain (Docker, PSL1GHT) | `docs/toolchain.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/toolchain.md |
| RPCS3 | `docs/rpcs3.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/rpcs3.md |
| Uso del Cell por juego (modelo de reporte) | `docs/uso-cell.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/uso-cell.md |
| Guía de usuario y archivos de configuración | `docs/uso-ps3.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/uso-ps3.md |
| App de perfilado y su lector | `docs/perfilado.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/perfilado.md |
| Mediciones (antes/después, PS3 real) | `docs/perf-log.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/perf-log.md |
| Bibliotecas con repo propio | `docs/bibliotecas.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/bibliotecas.md |
| Código y licencias de terceros | `docs/THIRD_PARTY.md` | https://github.com/ricowp81/Salsa64/blob/main/docs/THIRD_PARTY.md |
| Build, despliegue, RPCS3, log | `tools/build.sh`, `tools/deploy-ps3.sh`, `tools/rpcs3-run.py`, `tools/udplog.py`, `tools/qemu-ppc64.sh`, `tools/test-rec.sh` | https://github.com/ricowp81/Salsa64/tree/main/tools |
| Reglas de make de PS3 | `mk/ps3.mk` | https://github.com/ricowp81/Salsa64/blob/main/mk/ps3.mk |
