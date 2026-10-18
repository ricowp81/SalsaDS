# Estado de SalsaDS

Bitácora de lo hecho, lo que quedó a medias y lo decidido. Lo más reciente
arriba. Las mediciones van en `perf-log.md`; los resultados de la Fase 0, en
`fase0.md`.

## 2026-10-18 — Eventos y geometría en un SPE (Fase 3.5)

- **Medido en la PS3 con el JIT** (SM64DS en juego): emulación 26,6 ms = ARM9 13,3 + ARM7 2,3 + eventos 10,4 + planificador 0,6; en otras
  escenas ARM9 17,4 y eventos 5,4 (misma suma). La geometría costaba 7,0 ms por cuadro en el PPE (≈ 670 polígonos). El reparto
  ARM9/eventos se movía porque el motor de geometría es perezoso y lo ejecuta quien lo mira primero (la CPU o el evento `EV_GX`).
- **Eventos**: de 4925 por cuadro, 4363 eran miradas de `EV_GX` cada 256 ciclos. Ahora solo se mira cuando el nivel de la FIFO
  puede haber cambiado (o toca repetir la IRQ 21), y el evento de HBlank solo se arma si alguien lo usa. Por cuadro en el host: SM64DS
  5039 → 1497, NSMB 5031 → 2304, MKDS 3183 → 1437. Contadores por tipo de evento (`DATO eventos`, `cpurun --stats --gx-clock`). Las
  ROMs `irq`, `dma`, `ipc`, `periph`, `gx`, `geom` siguen en OK con todos los tamaños de tramo; cambia el intercalado fino de las
  CPUs (fronteras de tramo) y la IRQ 21 por nivel ya no sale antes de que se cumpla el nivel (detalle en `docs/fase3.md`, 3.5).
- **Geometría**: motor de tiempo (PPE) y ejecución separados (`gx.c` / `gx_exec.c`); en modo diferido las órdenes se anotan en un
  registro y un SPE (el sexto, antes libre) las ejecuta por lotes con el mismo código; el PPE espera solo si la CPU lee un resultado de
  la ejecución o en el SWAP_BUFFERS. Mismos bits que el modo directo: pruebas unitarias nuevas (`test_gx_defer`, `test_gx_job`), ROMs
  `gx`/`geom` con `--gx-defer` en LE y BE, hash de cuadros (`--gx-hash`) igual en directo y diferido en SM64DS/NSMB/MKDS (4000 cuadros) y
  `--vs-ref3d` igual; autoprueba del SPE al arrancar (3 registros al azar, vértices/polígonos/resultados iguales). `test-host` verde
  (LE, BE, sanitizadores, recompilador, lockstep).
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `ef37479`: `build/salsads-nucleo-jit-fase35.pkg` (copia de
  `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg`), SHA-256 `c1e0aa503a86885e8827b814ce9d1474921bd36276f78c65be8481b537ebf7bc`. En
  RPCS3 (SM64DS, 400 cuadros): autoprueba de geometría igual y `RESULTADO nucleo: OK`. Líneas nuevas del log: `DATO eventos`,
  `DATO geometria-spe` (trabajos, órdenes y subidas DMA por cuadro, ms del SPE, ms y número de esperas del PPE); `DATO geometria` ahora
  mide lo que el PPE gasta anotando y publicando. **Sin medir en la PS3**: expectativa PPE ≈ 7 → ≈ 1 ms por cuadro; falta ver los ms del
  SPE (código escalar de 64 bits, `__divti3` en el recorte) y cuánto espera el PPE en el VBlank.
- Pendiente: medirlo en la consola; si el SPE pasa de ≈ 8 ms por cuadro, partir el trabajo (transformación/iluminación aparte del
  recorte) o vectorizar la transformación de vértices; el tiempo de `frame_begin` (6 ms por cuadro en el desglose de la PS3) es otra
  historia por mirar; `gx_t` creció 128 KB (registro de órdenes).

## 2026-10-18 — Recompilador del ARM9, segunda ronda (Fase 4.1)

- **Medido en la PS3 con la primera versión** (SM64DS en juego): ARM9 13,3 ms por cuadro de trabajo (17–22 en escenas
  pesadas), 1716–4170 entradas por cuadro, 600–1985 salidas por destino indirecto no encontrado, 1091–2174 por presupuesto,
  ~1180 llamadas al intérprete. El tiempo del ARM9 sigue a los tramos (~9 µs por tramo), no a las instrucciones.
- **Hecho** (`docs/fase4.md`, «Segunda ronda»): en línea MCR p15 c7, MRC p15 c1/c9, MRS, MSR CPSR_c sin cambio de modo y MSR
  SPSR, SWP/SWPB y LDRD/STRD; la tabla de destinos indirectos se rellena después de un fallo; LDR relativo al pc leído al
  traducir; tramo de 1024 ciclos en el paquete (`nucleo-jit-tramo` lo cambia); línea `DATO jit-llamadas`.
- **En qemu, 1500 cuadros, por cuadro**: llamadas al intérprete 1819 → 411 (NSMB), 2968 → 248 (MKDS), 773 → 242 (SM64DS);
  salidas por destino no encontrado 1054 → 30, 337 → 3, 177 → 17; entradas 4508 → 2024, 2181 → 378, 2057 → 1142; palabras PPC
  por instrucción 15,6 → 14,0, 15,1 → 13,5, 15,2 → 13,7. Misma imagen final que antes con el mismo tramo; `--lockstep` sin
  diferencias en los tres juegos con 128 y con 1024; `tools/build.sh test-host` verde.
- **Precisión del tramo de 1024**: las CPUs se ven hasta ~1000 ciclos tarde; los bucles de sondeo dan más vueltas (lecturas
  lentas ×1,5–3, ARM9 +2–10 % de instrucciones). En NSMB y SM64DS los tramos los cortan los eventos (~5100 por cuadro, del motor
  de geometría): solo bajan a la mitad. El siguiente paso para eso es del planificador (no se tocó `src/core/sched.c`).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)**: `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg` del commit 5f2f3ce, SHA-256
  `a5788b65e1b2e252c7face21f40180e25b420448da24196139064b88970b85ae`.
- Pendiente: medir en la consola (mismas escenas, y `nucleo-jit-tramo` con 128 para comparar); LDM/STM a la E/S, CMP
  condicionales, entrada y salida más baratas.

## 2026-10-18 — Dos fallos de New Super Mario Bros. vistos en la PS3 (rama fase24)

- **Mapa del mundo 1, líneas verdes sobre el camino de tierra: arreglado** (no era del 2D; era el 3D). El parche de tierra (cuadrilátero
  translúcido, W-buffer) y el suelo (triángulo opaco con aristas x-mayores) están casi en el mismo plano; la w de las aristas
  x-mayores se interpolaba por la x del píxel entero y saltaba entre líneas, de modo que en unas líneas la profundidad de los dos
  empataba y el parche perdía. Ahora la w y la z de esas aristas siguen la y (línea siguiente), medido contra melonDS; y los
  empates de profundidad con «menor» los gana un polígono frontal sobre uno de espalda, como melonDS. Detalle, escenas nuevas de
  `gx3d` (26 a 34) y lo que queda abierto: `docs/fase3.md`, 3.5.
- **Nivel 1-3, nubes del fondo como un bloque claro con bordes rectos: sin reproducir.** El host llega al mapa con un guion de
  teclas (`--press`, ahora hasta 512 entradas; el nivel 1-1 corre, con saltos y carrera), pero para llegar al 1-3 hay que pasar el 1-1
  y el 1-2 jugando y el guion ciego muere antes. Hace falta la partida del autor (`/dev_hdd0/salsads/saves/` en la PS3, 8 KB):
  con ella `salsads-cpurun --save PARTIDA --press ...` y `tools/melonds-shot.py` llevan a la escena. Lo único probable sin la
  escena: el bloque es del 3D (las plataformas en forma de hongo del nivel son 3D) y la regla de empates nueva (frontal sobre
  espalda) podría arreglarlo; **sin confirmar**. Si no, las pistas del autor (nube solo arriba y el resto cielo) apuntan a una
  capa que se repite en vertical o a una ventana: revisar con `tools/gfx-bisect.py` sobre el cuadro de esa escena.
- Herramientas: `--press` admite 512 entradas (antes 64). Escenas 26 a 34 de la ROM `gx3d`.
- Pruebas: `tools/build.sh test-host` verde (LE, BE y sanitizadores); `gx3d.golden` con 35 escenas; MKDS, SM64DS y NSMB con `--vs-ref3d`
  y `--vs-ref` iguales. SPE del 3D: texto 58 296 B, pila con 16 464 B libres (holgura 9 696 B; `r3d_end_wz` va fuera de línea: en
  línea la holgura bajaba a 7 392 y el enlace lo rechazaba).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)** con el recompilador, del commit e13daba: `build/salsads-nucleo-jit-fase24.pkg`
  (copia de `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg`), SHA-256 `cbf36d479d932a3afb647bd4e859a7f856ee107248536016c747178a6a0b8ae1`.
  **Sin probar en la PS3** (ni en RPCS3): falta confirmar que el mapa del mundo 1 ya no tiene líneas y mirar el nivel 1-3.

## 2026-10-18 — Recompilador del ARM9 (Fase 4)

- **El ARM9 corre con un recompilador ARMv5TE (ARM y Thumb) → PowerPC64** propio (`src/jit/arec*`, conectado a la máquina en
  `src/core/jit.c`): bloques por dirección y estado, enlace directo, tabla de destinos indirectos, presupuesto de ciclos en el
  prólogo, banderas NZCV como comparaciones en cr2–cr4 (en memoria solo donde las lee una llamada al intérprete o una salida,
  con vista adelante del código de destino), ruta rápida por una tabla de páginas de 16 KB (RAM, WRAM, DTCM, ITCM) y lenta por
  el bus, invalidación por escrituras a código (bus, ITCM, DMA, HLE), CP15/MSR/SWI/LDRD/SWP… por el intérprete desde el bloque.
  El ARM7 sigue con el intérprete. Diseño, pruebas y números en `docs/fase4.md`.
- **Correcto contra el intérprete en qemu-ppc64**: 800 programas al azar de ARM y Thumb y casos dirigidos (`test_arec`); las 12
  ROMs propias con `--jit` y con `--lockstep` (otra máquina con el intérprete, comparadas después de cada bloque); New Super
  Mario Bros., Mario Kart DS y Super Mario 64 DS 1500 cuadros en `--lockstep` **sin diferencias** (99, 53 y 56 M comparaciones).
- Modo rápido (el de la PS3): un bloque puede pasarse del final del tramo; para que eso no rompa las esperas por tiempo, los
  desbordes de temporizadores vencidos se atienden al leer o escribir un temporizador (la ROM `irq` fallaba sin eso; con el
  intérprete ningún resultado cambia).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)**: `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg` (`tests/phase4/nucleo-jit`) del commit
  7a90a6a, SHA-256 `400704fe1253af680131e890c02fd81e69264aa9f6fc17909e7919ac3ebe726a`,
  con la línea `DATO jit` y el ARM9 en `DATO desglose`. En RPCS3 corre el respaldo (intérprete) y da `RESULTADO nucleo: OK`.
  **Sin medir en la PS3**: la meta es el ARM9 bajo ~8 ms por cuadro (44–49 ms con el intérprete).
- Pendiente: medir en la consola; el ARM7 en el recompilador; en línea las llamadas al intérprete frecuentes (CP15 de caché,
  SWP, MRS/MSR); caché de código más grande o con desalojo parcial (SM64DS usa 2,8 de 4 MB en 1500 cuadros).

## 2026-10-18 — Rendimiento del 3D y juntas entre triángulos (Fase 3.4)

- **Medido en la PS3 antes** (`DATO raster3d`, 178 muestras): New Super Mario Bros. (nivel) 26,7 ms por SPE, Super Mario 64 DS
  36–37 ms (el PPE esperaba 29–32 ms al 3D), Mario Kart DS 6,6–15 ms. Un ajuste de las columnas da **210 ns por píxel** (≈ 670
  ciclos, el bucle de la línea escalar con ~10 saltos sin predicción por píxel), **73 µs por bloque de texturas decodificado** y 3,7 µs
  por polígono·banda; los fijos eran ≈ 1 ms. No era un costo por banda: esas escenas dibujan 130–180 mil píxeles por cuadro.
- **Hecho**: línea del 3D en vectores de 4 píxeles (`src/gpu3d/r3d_vrow.h`, mismos bits que `r3d_draw_row`, que sigue de referencia),
  decodificación de bloques de texturas por tramos con la paleta convertida en el local store (el formato 4x4 por tile), z de las
  aristas sin aritmética de 64 bits y cadena de aristas sin módulo, borrado con vectores, y el `DATO raster3d` con líneas de polígono
  y el reparto `polígonos / aristas / píxeles`. Local store: texto 56 032 B, pila con 18 768 B libres (holgura 12 048 B). **Esperado en la
  PS3: 3,5–6 ms por SPE en SM64DS y NSMB (meta ≤ 8); sin medir.** En el host SM64DS 1,63 → 0,83 ms, MKDS 2,44 → 1,94 ms.
- **Juntas entre triángulos**: la causa era el **recorte** (`gx.c`), no las reglas de relleno: el vértice nuevo se interpolaba en el
  sentido en que cada polígono recorría la arista (dos polígonos que comparten una arista recortada sacaban vértices a un píxel de
  distancia) y no quedaba exactamente sobre el plano. Ahora se interpola de adentro a afuera y queda en ±w. Mario Kart DS: de 34 908
  píxeles con agujeros en 1 283 cuadros a 19 en 3; escena nueva 25 de `gx3d` (malla en perspectiva recortada) contra melonDS de 458
  píxeles distintos (358 sin dibujar) a 27 (ninguno sin dibujar). Las reglas de relleno no dan juntas: 17 millones de píxeles de
  1 200 mallas al azar, 0 sin cubrir. De paso: las aristas de 45° hacia la izquierda son x-mayores (escena 14 de 930 a 647 píxeles
  distintos con melonDS). Detalle en `docs/fase3.md`, 3.4. **Falta confirmar en la PS3 que SM64DS ya no muestra juntas** (en el host
  no se llega a una zona de juego sin entrada).
- **Pruebas**: `test-host` (LE con sanitizadores y BE; la línea vectorial contra la escalar en 392 000 líneas, mallas sin juntas, recorte
  con aristas compartidas, bloques de texturas contra `r3d_decode` texel a texel, ROM `gx3d` con 26 semillas); juegos con `--vs-ref3d`
  iguales (SM64DS, MKDS, NSMB) y 0 diferencias de bloques; RPCS3: autoprueba 3D igual y `RESULTADO nucleo: OK`. Cambió el hash de la
  semilla 7 de `test_render3d.c`, `gx3d.golden` (escenas 0, 5, 6, 13, 14, 20 y las nuevas 23 a 25) y la ROM `gx3d`.
- Herramientas: `gfx-compare.py --mel-cache`, `CPURUN_OUT`/`CPURUN_CFLAGS` en `cpurun.sh`, `salsads-cpurun --holes/--frame-dump`,
  `tools/ppm-png.py`.
- **Paquete** «SalsaDS núcleo» (SLDS00008) `build/salsads-nucleo-fase34.pkg` del commit `2945dba`, SHA-256
  `73e6e3783dd1816c85645aa0bb70b518e23d2af98c73fdae50aaa58d09b566fe`. En RPCS3: autoprueba 3D igual (sondas y 4 escenas) y
  `RESULTADO nucleo: OK` con SM64DS, MKDS y NSMB.
- **Queda**: medir en la PS3; si el número de `píxeles` o `aristas` sigue alto, desplegar dos grupos de píxeles por vuelta (la cadena
  de factor → textura → color es larga) o calcular las aristas una vez por polígono·banda; el borde y la niebla (`posproceso`) siguen
  escalares (≈ 0,4 ms por SPE con edge marking).

## 2026-10-18 — Fidelidad del 3D (Fase 3.3)

- **La estrella de «Touch to start» de Super Mario 64 DS**: dos errores del motor de geometría (`gx.c`). VTX_DIFF sumaba la
  diferencia por 8 (se suma tal cual: los ojos eran ocho veces más grandes) y las coordenadas de textura generadas (la textura de
  entorno de la estrella) no seguían el hardware: el modo 2 se calcula al llegar NORMAL con `>> 21` y sin traslación (antes `>> 17`
  con traslación), el 3 con `>> 24` en cada vértice, el 1 en TEXCOORD sin multiplicar la traslación por 16. Hallado volcando las
  órdenes del cuadro y comparando con melonDS (escena 15 de `gx3d`, idéntica). Pruebas en `test_gx.c`.
- **Regla de interpolación del color** (medida con las escenas 16 a 22 de `gx3d`, que aíslan línea, aristas, perspectiva y aristas
  inclinadas): color con 3 bits más y fracción llena salvo en el 0; lineal exacto con las `w` iguales; con `w` distinta, P = 8 en la
  línea; un píxel más de denominador con la arista derecha inclinada y y-mayor; aristas x-mayores por la y en el extremo de afuera;
  decal con alfa 0 y 31. Contra melonDS: Gouraud de 14 001 a 828 píxeles distintos, toon y highlight de ~4 000 a **0**, escenas
  15 a 18 idénticas (detalle y lo que queda en `docs/fase3.md`, 3.3).
- **Queda**: el color de los triángulos con aristas x-mayores (±1 en ≈ 40 % de los canales de la escena 21), niebla (escena 9),
  bordes de un píxel, antialiasing, imagen de fondo y la captura de pantalla DISPCAPCNT (no hecha; plan en `docs/fase3.md`).
- **Pruebas**: `test-host` verde (LE con sanitizadores y BE; ROM `gx3d` con 23 semillas e imágenes de referencia nuevas); juegos sin
  regresión (`--vs-ref3d` igual en SM64DS, MKDS y NSMB). Cambió la ROM `gx3d` (escenas 15 a 22), no `geom`.
- **PS3**: el SPE del 3D usa las mismas reglas (texto 47,1 KB, pila libre 23,9 KB; `r3d_end_at` y `r3d_dda_init` salieron de línea para
  no inflar el bucle). Paquete «SalsaDS núcleo» (SLDS00008) `build/salsads-nucleo-fase33.pkg` del commit 9ce969d, SHA-256
  `8cb4badd2f5921f5dae7af47b0d9d4eb111ac15a91aa2429e6c461455ac6ea2b`. En RPCS3: autoprueba 3D igual y MKDS 400 cuadros `RESULTADO nucleo: OK`. **Sin medir en la PS3** (el bucle sigue escalar).
- Aparte: `tests/unit/core` (volcado de memoria de qemu, 1,8 MB) está commiteado por error en `main`; lo regeneran las corridas BE.

## 2026-10-18 — Motor de render 3D (Fase 3.2)

- **Rasterizador de la DS** sobre `gx_frame` (`src/gpu3d/r3d_*`, código propio desde GBATEK y melonDS ejecutándolo): orden de
  opacos y translúcidos, aristas en punto fijo ajustadas a melonDS, interpolación con corrección por w, Z/W-buffer, prueba
  menor/igual, translúcidos con id, alpha test, modulate/decal/toon/highlight/sombras, los 7 formatos de textura con
  repetir/espejo/recorte, edge marking y niebla. **Referencia** (cuadro entero) y **código del SPE** (bandas de 16 líneas, DMA por
  listas, caché de bloques de texeles decodificados con LRU) comparten las reglas en enteros de 32 bits y dan **los mismos bits**
  en el host LE, qemu BE, el PPE y el SPE. Pendientes: antialiasing, imagen de fondo, polígonos de un punto.
- **Capa 3D** como BG0 del motor A en el compositor 2D (SPE y referencia): BG0HOFS y la mezcla del 3D con el objetivo 2 con su
  propio alfa. Se dibuja en la línea 0 del cuadro siguiente al SWAP_BUFFERS y se espera en la línea 192 (detalle en `docs/fase3.md`).
- **Pruebas**: unitarias (40 escenas × 4 repartos, LE y BE), ROM `gx3d` (15 escenas, en `test-host` con imagen de referencia) y
  contra melonDS: 5 escenas idénticas (formatos de textura, translúcidos, sombras, 3D sobre 2D, prueba «igual»); el resto difiere
  en bordes (≈ 5 % de las líneas, un píxel) y en el color de Gouraud (±1 de 5 bits en ≈ 30 % de los canales).
- **Juegos** en el host: Mario sale dibujado en New Super Mario Bros. 1-1 y la carrera de Mario Kart DS se ve; la estrella de Super
  Mario 64 DS sale rota, pero sus vértices ya vienen mal del motor de geometría (pendiente de la 3.1). Host: 0,95 / 3,30 / 1,52 ms
  por cuadro.
- **PS3**: dos SPE para el 3D (2D A, 2D B, sonido, 3D × 2: queda un SPE libre), autoprueba al arrancar (sondas de aritmética y 4
  escenas contra el PPE) y `DATO raster3d`. Paquete «SalsaDS núcleo» (SLDS00008) del commit `211d01e`:
  `build/salsads-nucleo-fase32.pkg`, SHA-256 `77fa46e6972291609050993e2996ffea5ab3f4768fcc67832ec220bc9d1fa046`. En RPCS3:
  autoprueba igual, verificación durante la prueba igual, `RESULTADO nucleo: OK`. **Sin medir en la PS3**: el bucle de la línea es
  escalar y puede ser lento en el SPE; si pasa de ≈ 6 ms por SPE en MKDS, vectorizarlo.
- Arreglos de paso: desplazamientos de valores con signo en `gx.c` (UBSan). Herramientas: `tools/r3d-fit.py`,
  `tools/melonds-shot.py`, `rpcs3-run.py --rom/--frames`, `salsads-cpurun --ref3d/--vs-ref3d/--r3d-stats/--img3d`.

## 2026-10-17 — Motor de geometría (Fase 3.1)

- **El motor de geometría real** (`src/gpu3d`, C portable y sin dependencias; pegamento en `src/core/geom.c`) sustituye a la
  FIFO «siempre vacía»: GXFIFO empaquetada y puertos de órdenes, FIFO de 256 entradas con tubería de 4 y GXSTAT con niveles,
  ocupado e IRQ 21 por nivel; DMA de modo 7 que respeta la mitad de la FIFO; matrices (modos 0 a 3, pilas con error), vértices,
  BEGIN/END con los cuatro tipos de primitiva, iluminación, coordenadas de textura, recorte contra los seis planos, viewport,
  BOX/POS/VEC_TEST, RAM_COUNT y SWAP_BUFFERS en el VBlank. Deja por cuadro una lista de polígonos en pantalla (`gx_frame`, doble
  búfer, 32 B por vértice y 48 por polígono, sin punteros) lista para los SPE de la 3.2. Detalle, formatos y redondeos en
  `docs/fase3.md`.
- **Contrastado con melonDS** (sólo ejecutándolo): la ROM `geom` (2878 comprobaciones) da lo mismo en los dos salvo el tiempo de la
  FIFO, las lecturas de 32 bits de VEC_RESULT y la pila de proyección tras errores repetidos. Matrices (≈ 2000 comprobaciones
  aleatorias), pilas, recorte, caras y la cuenta de vértices de las tiras (se comparten) coinciden. Pruebas unitarias del motor en
  LE y BE.
- **Juegos**: New Super Mario Bros. (menú, archivo, mapa y 1-1), Mario Kart DS y Super Mario 64 DS siguen llegando adonde
  llegaban; NSMB manda ≈ 480 polígonos por cuadro en 1-1 (máximo 1895) y la geometría cuesta ≈ 0,64 ms por cuadro en la Mac.
- Paquete «SalsaDS núcleo» (SLDS00008) con `DATO geometria` (tiempo del motor en el PPE con el reloj de perfil): `build/salsads-nucleo-fase31.pkg`
  (commit `9e1c73f`), SHA-256 `1dc3567fe7eeca57721f82bdb6da9d9e0db48ce31005322f43dbba8e156d6e81`; compila y enlaza, **no probado en RPCS3 ni medido
  en la PS3**; el PPE es varias veces más lento que la Mac y la decisión PPE/SPE del motor depende de ese número.
- Pendiente: la 3.2 (rasterizador en SPE sobre `gx_frame`, texturas y paletas, orden de translúcidos, SWAP_BUFFERS con rasterizador
  asíncrono); medir el motor en la PS3; el modelo de iluminación y el recorte están sin contrastar con una DS real (correr `geom` ahí).

## 2026-10-17 — FIFO de geometría, sonido y compositor rápido (Fase 2.2)

- **New Super Mario Bros. ya entra a un nivel en el host.** El bucle de 620 883 instrucciones por cuadro no era el sonido: el
  ARM9 esperaba a `GXSTAT` (FIFO de comandos con menos de la mitad) y a la IRQ 21 para mandar su lista de geometría por DMA. Ahora
  la FIFO de geometría existe como «siempre vacía» (se traga las órdenes; IRQ 21 por nivel como en melonDS): título, menú,
  selección de archivo (táctil), intro, mapa y 1-1 con el reloj corriendo, a 91–94 MIPS. Sin 3D no se ve a Mario (Fase 3).
- **SWI 1Ah–1Ch** (tablas de seno, tono y volumen) generadas por fórmula y comprobadas contra las 1556 entradas de melonDS.
- **Sonido**: los 16 canales (PCM8/16, IMA-ADPCM, PSG, ruido), SOUNDCNT/SOUNDBIAS y las capturas, en `src/audio` (portable) con
  un SPE de mezcla (`src/spu/snd`), remuestreo lineal a 48 kHz en flotante (con el punto marcado para un FIR polifásico) y salida de
  audio de la PS3 (`src/platform/ps3/audio_out.c`). La ROM `snd` captura lo que sale del mezclador por hardware y
  `tools/snd-compare.py` lo compara con melonDS muestra a muestra: iguales, con dos divergencias documentadas (`docs/fase2.md`).
  Autoprueba del SPE contra el PPE al arrancar (0 diferencias en RPCS3).
- **2D en el SPE**: los 28 ms de la pantalla A y 8 de la B eran código escalar (multiplicación de 64 bits y división por
  software en los fondos afines; color final de un píxel por vuelta con una docena de saltos): vectorizado, y una vía rápida para
  los fondos que solo se desplazan. En RPCS3 (que cuenta instrucciones) con NSMB: A 14,8 → 4,7 ms, B 1,3 → 1,2; la PS3 lo dirá
  (`DATO spe-etapas`). Instrumentación por etapa en el SPE y contadores de DMA en el host.
- Herramientas: `--press`/`--shot` (entrada y capturas por guion), `--wav`, `--g2d-stats`, `--trace-from`, `--dump-cpu`, TCM visibles.
- Paquete «SalsaDS núcleo» (SLDS00008) con todo lo anterior: `build/salsads-nucleo-fase22.pkg` (commit `c4a738b`), SHA-256 `cce7721a908a362ee3aed51d673348361a34bb10a5fd9a33b20f67de0e37a601`; en RPCS3 OK.
- Pendiente: medir en la PS3 (2D por etapa, audio con faltas, 10 bits), 3D (Fase 3), FIR del remuestreo, HOLD y volumen general
  contra una DS real.

## 2026-10-17 — Motores 2D A y B (Fase 2.1)

El compositor de la 0.5 es ahora el renderizador de las dos pantallas: registro de cambios por línea en el núcleo
(`src/core/video.c`), `src/gpu2d/gpu2d_compose.c` (SPE y host, mismos píxeles), referencia independiente y un SPE por
pantalla en la PS3. Fondos de texto, afines y extendidos, OBJ de todos los tipos, ventanas, mezcla, brillo, mosaico,
paletas extendidas y modos de pantalla 0/1/2/3. Detalle, pruebas y divergencias con melonDS en `docs/fase2.md`.
Paquete «SalsaDS núcleo» (SLDS00008) con los SPE; New Super Mario Bros. llega al menú en el host. Pendiente: medirlo en la
PS3 real, la captura de pantalla, la capa 3D (Fase 3) y el sonido (2.2).

## 2026-10-17 — Cierre de la Fase 1 (1.3)

- **Cuadro ocioso**: el planificador cortaba el tiempo en 8753 tramos por cuadro aunque las dos CPUs estuvieran en HALT; eso eran los
  ≈ 2,1 ms fijos por cuadro medidos en la PS3 (2,73 ms de emulación con la demostración, 8,4 con 28–39 k instrucciones por cuadro:
  2,1 ms + 0,16 µs por instrucción). Ahora el tiempo salta al siguiente evento: en la Mac un cuadro ocioso baja de 177 a 24 µs
  (×7,3) y la demostración de 220 a 67 µs; la carga (ocupadas las dos CPUs) no cambia. Quedan ~525 eventos de video por cuadro.
  El paquete escribe `DATO desglose` y `DATO MIPS` en `nucleo.log` para repartirlo en la consola.
- **New Super Mario Bros. y Mario Kart DS**: no era la CPU ni el IPC. NSMB se apagaba porque no había chip de guardado (el ARM7 lee su estado
  por AUXSPI cada 5 ms y contestábamos FFh para siempre); MKDS repetía sin fin una escritura de la configuración de Wi-Fi en el firmware
  porque el firmware ignoraba las escrituras. Con las dos cosas, los dos configuran el video **igual que melonDS** (DISPCNT de los dos motores y
  VRAMCNT idénticos, y el guardado de NSMB, 8 KB, byte a byte igual al de melonDS). Super Mario 64 DS llega al mismo punto que melonDS.
- **Guardado del cartucho (AUXSPI)**: EEPROM de 0,5 KB, 8 KB, 64 KB y 128 KB, FRAM de 32 KB y Flash de 256 KB a 1 MB; el tipo sale del
  tamaño del archivo de guardado, de una tabla de tres códigos de juego o, si no, EEPROM de 64 KB. ROM propia `save` (siete tamaños,
  little y big endian y sanitizadores; contra melonDS con divergencias anotadas). El firmware de la consola también se puede escribir.
- **ROM de carga** (`carga`) y **ROM ociosa** (`ocio`): dos nuevas, propias, en `tests/roms/homebrew/`. `carga` mantiene las dos CPUs
  ocupadas con trabajo variado y comprueba sus resultados (145 MIPS en la Mac).
- Paquete **«SalsaDS núcleo» (SLDS00008)** del commit 9f2a177: `build/salsads-nucleo/salsads-nucleo.pkg`, SHA-256
  `8da0871ecb4e16e97569e3250055f0f4aef3d7ca8722e568dc627662408f92a4`. Lleva la ROM de carga embebida (o `rom.nds`), guardado por
  juego en `/dev_hdd0/salsads/saves/` y el desglose por cuadro. En RPCS3: OK. **Falta medirlo en la PS3.**
- Herramientas de diagnóstico nuevas en el host: trazas por `--dbg`, `--break7/9` con anillo y pila, `--watch7/9`, `--dump`,
  `tools/melonds-dump.py` (memoria y registros de melonDS por GDB). `tools/melonds-run.py` corre sobre una copia temporal.
- Higiene: cinco volcados de memoria de qemu (`*.core`, ≈ 40 MB) se habían colado en un commit anterior; se quitaron del árbol y se ignoran,
  pero siguen en la historia (si el repositorio se publica, conviene reescribirla antes).
- Pendiente (detalle en `docs/fase1.md`): medir en la PS3, SWI 1Ah–1Ch (sonido), FIFO de geometría (Fase 3), detección del chip de guardado
  sin archivo ni tabla, eventos de video perezosos si el desglose lo pide.

## 2026-10-17 — Máquina de la DS y primera imagen (Fase 1.2)

- `src/core` + `src/bios`: mapa de memoria de las dos CPUs (RAM, WRAM con WRAMCNT, VRAM por bancos con VRAMCNT,
  paleta, OAM), planificador de eventos con el ARM7 a la mitad del reloj del ARM9, IRQ por CPU, DISPSTAT/VCOUNT y
  VBlank/HBlank, temporizadores, DMA, IPC (SYNC y FIFO), teclas, cartucho (ROM desencriptada), SPI con firmware
  generado, táctil, energía y RTC, división y raíz por hardware, BIOS en HLE (bloques de BIOS propios + SWI) y
  arranque directo. Sustituye a `ndsmin`. Detalle, frontera para el ARM7 en otro hilo, tablas y divergencias en
  `docs/fase1.md`.
- **Primera imagen**: modo de visualización directa de VRAM del motor A y color de fondo del motor B. `hola` y `mem`
  dan en melonDS la misma imagen píxel a píxel y los mismos registros. `salsads-cpurun --screens` la vuelca.
- ROMs propias por paso (`mem`, `irq`, `dma`, `ipc`, `bios`, `periph`, `demo`): **todas OK** en SalsaDS (little y
  big-endian, y con tramos del planificador de 1 a 20 000 ciclos) y en melonDS 1.1; sus divergencias quedaron anotadas.
  Hallazgos de melonDS: su BIOS libre destruye r0 y r2 en el Halt del ARM7, da el resto de Div en valor absoluto, Sqrt
  0 con entradas grandes y no tiene Huffman; el DMA de HBlank y el de «inicio de pantalla» difieren de GBATEK.
- Paquete de PS3 **«SalsaDS núcleo» (SLDS00008)**: corre la máquina con la demostración propia embebida (o
  `/dev_hdd0/salsads/rom.nds`) y las dos pantallas de la 0.6. En RPCS3: OK. **Falta la PS3 real.**
- Respaldos del autor (a mano, sin capturas en el repo): Super Mario 64 DS arranca más lejos (configura video y
  VRAM y sigue con su lógica; usa la FIFO de geometría); New Super Mario Bros. configura video y se queda con los
  dos procesadores esperándose; Mario Kart DS no llega a configurar el video (el ARM7 lee el firmware sin parar).
- Decisión pendiente del autor: ninguna nueva. Pendiente técnico (guardados, SWI 1Ah–1Ch, trazas de los juegos, GX
  FIFO): lista en `docs/fase1.md`, «Qué falta para cerrar la Fase 1».

## 2026-10-17 — Intérpretes de CPU (Fase 1.1)

- `src/cpu/`: un núcleo (`struct acore`) para el **ARM9 (ARMv5TE)** y el **ARM7 (ARMv4T)**,
  ARM y Thumb, modos con banco, excepciones, HALT, líneas de IRQ/FIQ, memoria por
  callbacks con direcciones alineadas, un gancho de SWI para la HLE de la BIOS y, en la
  ARM9, CP15 mínimo con ITCM/DTCM (base, tamaño, espejo, modo de carga). Detalle,
  reglas por versión y plan de la 1.2 en `docs/fase1.md`.
- Pruebas: ≈ 8,0 M de comprobaciones unitarias en LE y BE (con mutaciones que las ponen a
  prueba) y las ROMs `hola`, `cpu9` y la nueva `cpuarm` (ARM9 y ARM7, ARM y Thumb,
  excepciones y TCM) corridas en los intérpretes (LE y BE) y en **melonDS 1.1: OK**.
  `tools/build.sh test-host` lo corre todo; `tools/cpurun.sh` corre una ROM en la Mac
  sin Docker; `tools/melonds-run.py --arm7` muestra también el bloque del ARM7, el
  registro de fallos y las divergencias.
- melonDS **diverge de la documentación** en varios casos de borde (LDRH/LDRSH impares del
  ARM7, listas vacías de LDM/STM, STM con la base en la lista, bit Q del ARM7, BKPT, modo de
  carga de las TCM): la ROM los anota aparte (no son fallos) y los intérpretes siguen
  GBATEK; ninguno está contrastado en una DS real (lista en `docs/fase1.md`).
- Rendimiento del intérprete en el **host**: 125–198 MIPS (Mac y contenedor); ≈ 1,5×
  más lento que el mínimo de la 0.3. En la PS3 sin medir (≈ 12 MIPS por escala [est]).
- Corredor `salsads-cpurun` + `src/host/ndsmin.c`: arranque directo con una máquina
  mínima (no es el mapa de la 1.2) que llega al `main` de las ROMs de BlocksDS con su
  `crt0` y libnds reales.
- Marco de pruebas de las ROMs: registro de los primeros 16 fallos, registro de
  divergencias, marcador de avance y variante para el ARM7 (`sds7.c`). `hola` y `cpu9`
  se reconstruyeron (hashes nuevos en `SHA256SUMS`; la construcción es reproducible).
- Pendiente para la 1.2: mapa de memoria con VRAM por bancos, planificador, IRQ,
  temporizadores, DMA, IPC FIFO, cartucho, SPI/firmware, BIOS en HLE, arranque directo y
  la primera imagen. Y probar el intérprete en el PPE real.

## 2026-10-17 — Prueba mínima del JIT ARM → PPC (Fase 0.7) y cierre de la Fase 0

- `tests/phase0/jit-arm` («SalsaDS JIT», SLDS00007) + `src/jit/`: traductor de un
  subconjunto de ARM (datos con S, ADC, LDR/STR/LDRB/STRB, B<cond>) a PowerPC64 con
  tres formas de banderas (perezosas, CR por instrucción, cmp+salto fusionados), un
  intérprete de referencia y cuatro bucles (el de la 0.3, suma64 con acarreo,
  ramas, bytes). **qemu-ppc64 ejecuta el código traducido** y da lo mismo que la
  referencia (registros, memoria, instrucciones).
- **Medido en la PS3 real (2026-10-17): el ARM9 cabe.** El peor bucle da 534,8 MIPS
  con CR por instrucción (8× los 67 MHz; intérprete de 0.3: 18,1 MIPS). ARM7 en el
  hilo 1: girar −31,9 %, dormir en un semáforo de lv2 −0,7 % a −9 %. **Fase 0
  cerrada**: arquitectura confirmada en `fase0.md` («Cierre de la Fase 0»).
- Caché de código: cola NOBITS de 4 MB en el texto (`mk/rec-cache.ld`), hilo de
  prueba con plazo de 2 s y marca en disco; si falla o cuelga (CFW), respaldo a los
  intérpretes. En RPCS3 el JIT no se intenta (solo lectura).
- Mide también el ARM7 en el hilo 1 del PPE: pérdida de MIPS del ARM9 y latencia de
  despertar con giro, giro de prioridad baja, `sysUsleep`, semáforo y cola de
  eventos de lv2.

## 2026-10-16 — Rasterizador 3D en SPEs (Fase 0.4)

- `tests/phase0/spe-raster3d` («SalsaDS raster», SLDS00002): escena sintética
  de 500, 1000 y 2048 triángulos con textura de la VRAM (por DMA) y color de
  vértice, z-buffer de 24 bits, dibujada por 1 o 2 SPEs a 1×, 2× y 4×, por
  bandas de líneas o tiles. Diseño, verificación y qué mirar en
  `docs/fase0.md` §0.4.
- Verificado: el mismo núcleo en el host (LE y BE) contra una referencia en C
  (modo id idéntico; color 99,96 % idéntico; todos los repartos con los mismos
  bits) y en RPCS3 (`RESULTADO … OK`).
- **Medido en la PS3 real (paquete de `255b0d2`): criterio cumplido.** 1× N =
  2048 en 2 SPEs: 4,19 ms (< 8); 2×: 7,96 ms; 4×: 19,61 ms (no cabe en 2
  SPEs). Medida ×1,6–2 sobre la estimación. Caché de texturas desbordada: ×5,4
  más lenta, primera decisión de la Fase 3. Detalle en `docs/fase0.md` §0.4,
  `perf-log.md` y `docs/datos/`. **Fase 0.4 cerrada.**
- Encontrado: `spu_re` de spu-gcc ya incluye el `fi`; uno más arruina el
  recíproco. El programa ahora autoprueba en cada SPE las funciones con
  intrínsecos contra el PPE.
- Queda como riesgo para la Fase 3: texturas que no caben decodificadas en el
  local store (con 24 texturas, la caché se vacía en cada unidad).
## 2026-10-16 — Composición 2D en un SPE y salida de dos pantallas (Fases 0.5 y 0.6)

- **0.5** (`tests/phase0/spe-gpu2d`, «SalsaDS 2D», SLDS00003): compositor 2D
  por líneas en un SPE (4 fondos de texto, 128 OBJ, VRAM por DMA, registro de
  cambios por línea) y una referencia píxel por píxel. Imágenes idénticas en el
  host (LE y BE) y en RPCS3 (SPE de verdad); el **tiempo (< 4 ms) falta medirlo
  en la PS3 real**. Detalle, límites del modelo y la estimación en `fase0.md` 0.5.
- **0.6** (`tests/phase0/dual-screen`, «SalsaDS pantallas», SLDS00004): dos
  texturas por cuadro con rsxkit en cuatro distribuciones a 720p y 1080p con
  escala entera y ajustada (distribución comprobada por píxeles en RPCS3), DS3 →
  botones de la DS y el puntero táctil (botón de toque mapeable, stick derecho o
  inclinación del Sixaxis). **Pendiente en la PS3 real**: los 60 Hz, 1080p, y
  cómo se sienten los dos modos del puntero (precisión, temblor, latencia); los
  ejes del Sixaxis son una suposición que se corrige en `controles.cfg`.
- Código nuevo reutilizable: `src/gpu2d/` (referencia, compositor, escenas),
  `src/frontend/{dspad,layout}.c` (entrada y geometría, sin PSL1GHT),
  `tools/raw-png.py` (volcados a PNG), `tools/rpcs3/salsads-1080.yml` (RPCS3 a
  1080p). El logger comprueba ahora que `/dev_hdd0/salsads` existe y lo dice.
- **APPID duplicado**: `tests/phase0/udplog` ya usaba SLDS00003. Los paquetes de
  0.5 y 0.6 siguen la asignación pedida (SLDS00003 y SLDS00004); si udplog y
  spe-gpu2d se instalan en la misma consola, el segundo reemplaza al primero.
  La Fase 0.4 (spe-raster3d) usa SLDS00002, que también tiene `empty`; el siguiente libre es SLDS00005 (pantallas giro usó SLDS00006).

## 2026-10-16 — Microbenchmarks del PPE (Fase 0.3)

- `tests/phase0/ppe-bench` (SLDS00001 «SalsaDS bench»): memoria LE con
  `lwbrx`/`stwbrx`/`lhbrx`/`sthbrx`, tres estrategias de banderas NZCV más una
  cota, y tres despachos de un intérprete ARM mínimo. Verificado contra
  referencias en C bajo qemu-ppc64 (`tools/build.sh test-host`) y arranca en
  RPCS3.
- **Medido en la PS3 real** (paquete de `08b7ef0`, subido por quien
  tiene el permiso del FTP): resultados, decisiones y log completo en
  `docs/fase0.md` §0.3, `perf-log.md` y `docs/datos/`. Decididos: memoria LE
  con `lwbrx` y compañía, banderas perezosas, y recompilador obligatorio
  (intérprete 16,5–17,9 MIPS contra ~100 que pide la DS). **Fase 0.3 cerrada.**
- Descubierto al escribirlo: en PowerPC64, el CA de `addc`/`subfc` es el
  acarreo del bit 63, no del 32; el C de ARM sale del bit 32 del resultado de
  64 bits con los operandos extendidos con ceros.

## 2026-10-16 — ROMs de prueba (Fase 0.2)

- `tests/roms`: marco de pruebas `sds` (resultado en RAM `0x02380000` y en
  pantalla), ROMs `hola` y `cpu9` (65 187 comprobaciones del ARM9), construidas
  con BlocksDS en Docker (`tools/build-roms.sh`) y validadas en melonDS 1.1
  con su stub de GDB (`tools/melonds-run.py`).
- ARM7 de las ROMs: uno propio vacío, para no enlazar DSWiFi/Maxmod/Mbed TLS.
- Falta: pruebas del ARM7 (ARMv4T), DMA, timers, IPC, VRAM por bancos; y
  copiar el aviso íntegro de picolibc antes de redistribuir los `.nds`.

## 2026-10-16 — Arranque: esqueleto (Fase 0.1)

Hecho:
- `lib/rsxkit` y `lib/spukit` entran por `git subtree --squash` desde sus
  repositorios (MIT). Cada uno en su propio commit y sin tocar otra carpeta.
- Build en Docker: `tools/build.sh` usa la imagen `salsads-toolchain:dev`, que
  se construye en segundos (`tools/docker-image.sh`) a partir de la imagen del
  toolchain de Salsa64 (ps3toolchain, PSL1GHT y cgcomp: más de una hora de
  compilación que no se repite). `docker/Dockerfile` explica cómo.
- `mk/ps3.mk` (reglas de PSL1GHT, SPU embebido, `.pkg`) y
  `src/platform/ps3/` (logger TTY + archivo + UDP) adaptados de Salsa64.
- Logger UDP en el **puerto 18195** (Salsa64 usa 18194): `tools/udplog.py`.
  Un receptor por puerto. Archivos de la consola en `/dev_hdd0/salsads/`.
- Pruebas del host en los dos órdenes de bytes: `tools/build.sh test-host`
  corre `tests/unit` con el gcc nativo del contenedor (little-endian, con
  ASan/UBSan) y con `qemu-ppc64` (big-endian, CPU `power9`; ver
  `docs/cell-desde-salsa64.md` §6.2). Cada corrida imprime `host: LE|BE` y el
  script falla si no coincide.
- `tools/rpcs3-run.py` y `tools/rpcs3/*.yml`: RPCS3 sin interfaz. Usa el
  **mismo candado** (`~/Library/Caches/rpcs3/salsa64-rpcs3.lock`) que Salsa64,
  porque el RPCS3, su `dev_hdd0` y su `TTY.log` son uno solo.
- `tools/deploy-ps3.sh` (FTP de webMAN MOD o `ps3load`; la IP va en
  `tools/ps3.conf`, ignorado por git).

APPIDs de SalsaDS: rango `SLDS`, distinto del `SLSA` de Salsa64.

| APPID | Programa |
|---|---|
| SLDS00001 | `tests/phase0/ppe-bench` («SalsaDS bench») |
| SLDS00002 | `tests/phase0/spe-raster3d` («SalsaDS raster»); también `tests/phase0/empty`, que solo se corre en RPCS3 |
| SLDS00003 | `tests/phase0/udplog` (y también `tests/phase0/spe-gpu2d`, «SalsaDS 2D»: ver el aviso de arriba) |
| SLDS00004 | `tests/phase0/dual-screen` («SalsaDS pantallas») |
| SLDS00006 | `dual-screen` con otro nombre («SalsaDS pantallas giro», modo GIRO) |
| SLDS00007 | `tests/phase0/jit-arm` (0.7) |
| SLDS00008 | `tests/phase1/nucleo` («SalsaDS núcleo», Fase 1.2) |

Pendiente (preguntas abiertas del plan, de la decisión del autor):
licencia del proyecto, juegos objetivo, aplicación independiente o núcleo
libretro, nombre del ejecutable.

## Qué existe hoy de DS en la PS3

No encontré ningún emulador de DS usable en PS3 (búsqueda del 2026-10-16;
detalle y fuentes en `docs/fase0.md`, 0.2).
