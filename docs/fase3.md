# Fase 3.1 — Motor de geometría

El 3D de la DS tiene dos mitades: el **motor de geometría** (FIFO de comandos, matrices, vértices, iluminación, recorte,
viewport) y el **motor de render** (rasterizado, texturas, mezclas, niebla, bordes). Esta ronda es la primera: el motor de
geometría real en C portable, que deja por cuadro una lista de polígonos en coordenadas de pantalla. Rasterizarla en los SPE
es la 3.2. Hasta aquí la FIFO era «siempre vacía» y se tragaba las órdenes.

Código propio, escrito desde GBATEK. Ni se leyó ni se copió código de melonDS, NooDS ni DeSmuME (la licencia de SalsaDS está
sin decidir): melonDS sólo se ejecutó como referencia de comportamiento (`tools/melonds-run.py`, `tools/melonds-dump.py`).

## Archivos

| Archivo | Qué es |
|---|---|
| `src/gpu3d/gx.h`, `gx.c` | El motor, sin dependencias del resto del núcleo (C99, enteros de 64 bits, sin coma flotante): host, PPE big-endian y qemu |
| `src/core/geom.c` | Pegamento con la máquina: escrituras a GXFIFO y a los puertos de órdenes, lecturas de 0x04000600-0x040006A7, IRQ 21 por nivel, DMA de modo 7, VBlank |
| `src/core/io.c`, `sched.c`, `nds.h`, `nds_priv.h` | Ganchos (E/S, DMA con la FIFO, evento `EV_GX`, `struct nds.gx`) |
| `tests/unit/test_gx.c` | Pruebas unitarias del motor (LE y BE) |
| `tests/roms/src/geom/arm9.c` | ROM `geom`: lo que la CPU puede leer de vuelta, contra un modelo en C y contra melonDS |
| `tools/gx-png.py`, `salsads-cpurun --gx-dump` | Depuración: vuelca el último cuadro entregado y lo dibuja plano |

## El motor

### FIFO, tubería y tiempo

- **Entradas**: una orden con parámetros ocupa tantas entradas como parámetros; una sin parámetros, una. Las palabras
  empaquetadas de GXFIFO (0x04000400-0x0400043F, cuatro órdenes por palabra, los parámetros detrás en orden) se
  decodifican en un estado propio: `00h` se salta, una orden inexistente se ignora. Los puertos de órdenes
  (0x04000440-0x040005CC, `orden = (dirección - 0x04000400) / 4`) meten una entrada por escritura, y las órdenes sin
  parámetros se disparan con cualquier valor. Sólo cuentan las escrituras de 32 bits alineadas. Con el motor de geometría
  apagado (POWCNT1 bit 3) las escrituras se ignoran.
- **Cola**: 256 entradas de FIFO más 4 de tubería (260). GXSTAT cuenta sólo la FIFO: entradas − 4 (con 4 o menos, 0), y llega a
  256 con 260. **Igual que melonDS** (la ROM `geom` lo comprueba con un SWAP_BUFFERS delante, que detiene el motor sin
  depender del tiempo: 25 puntos entre 1 y 260 entradas, idénticos). «Menos de la mitad» = menos de 128; «vacía» = 0.
- **Tiempo**: todo es un contador de ciclos del ARM9; el motor corre a la mitad. Es **perezoso**: no tiene hilo ni evento por
  orden. `gx_advance(now)` ejecuta las órdenes cuyo comienzo (el instante en que llegó su último parámetro, o el fin de la
  orden anterior si es posterior) cae antes de `now`, y se llama antes de mirar cualquier estado (GXSTAT, resultados,
  escrituras a la FIFO, DMA, evento `EV_GX`, VBlank). Una orden empieza cuando termina la anterior; el efecto en el estado
  es instantáneo al empezar, y GXSTAT enseña «ocupado» hasta que termina.
- **Costes** (ciclos del motor, tabla de GBATEK): MTX_MODE 1, PUSH 17, POP 36, STORE 17, RESTORE 36, IDENTITY 19, LOAD 4x4 34,
  LOAD 4x3 30, MULT 4x4 35, 4x3 31, 3x3 28, SCALE 22, TRANS 22 (los MULT y TRANS suman 30 en el modo 2), COLOR 1, NORMAL 9 + luces
  encendidas, TEXCOORD 1, VTX_16 9, VTX_10/XY/XZ/YZ/DIFF 8, POLYGON_ATTR/TEXIMAGE_PARAM/PLTT_BASE 1, DIF_AMB/SPE_EMI 4,
  LIGHT_VECTOR 6, LIGHT_COLOR 1, SHININESS 32, BEGIN 1, END 1, SWAP_BUFFERS 392, VIEWPORT 1, BOX_TEST 103, POS_TEST 9,
  VEC_TEST 5. **Aproximado**: no se cobra el coste por polígono (recorte, ordenar) que sí existe en el hardware; el motor sale
  un poco más rápido de lo real. Nada de esto está medido en una DS real.
- **FIFO llena**: la escritura que no cabe frena a la CPU (el instante hasta el que hay que frenarla lo devuelve
  `gx_fifo_write`) hasta que el motor toma una orden. Si el motor está detenido en un SWAP_BUFFERS (la CPU real esperaría al
  VBlank) la escritura se pierde y se cuenta (`fifo_overflow`); ningún juego probado llega ahí.
- **IRQ 21** por nivel (GXSTAT bits 30-31: 1 = menos de la mitad, 2 = vacía), cada `NDS_GX_PERIOD` (1024 ciclos) mientras la
  condición dure, aunque el programa la confirme (NitroSDK la confirma al terminar y se perdía la petición). **DMA de modo 7**:
  una palabra por vez, sólo mientras la FIFO tiene menos de 128 entradas, 4 ciclos del ARM9 por palabra; el evento `EV_GX`
  (cada 256 ciclos, y sólo si hay IRQ por nivel o una DMA de modo 7 esperando) la reanuda. Sin ninguna de las dos cosas no
  hay eventos: antes de esta optimización eran ≈ 16 000 por cuadro en New Super Mario Bros.
- **SWAP_BUFFERS**: al ejecutarse el motor se detiene (GXSTAT «ocupado») hasta el VBlank (línea 192). Ahí `nds_gx_vblank` toma
  una foto de los registros del 3D (`DISP3DCNT`, color y profundidad de borrado, niebla, bordes, alfa, tabla de toon), entrega el
  cuadro y deja seguir al motor desde ese instante. El contador `RAM_COUNT` vuelve a 0.

### Formatos y redondeos

| Dato | Formato | Notas |
|---|---|---|
| Elementos de matriz | 1.19.12, `s32` | el producto suma cuatro productos en 64 bits y trunca con `>> 12` aritmético (hacia −∞); se guarda en 32 bits (desborda envolviendo) |
| Producto | `actual = operando × actual` | vector fila: el primer `glTranslate` que se llama es el último que se aplica |
| MTX_LOAD/MULT 4x3 | 12 parámetros en 4 filas de 3; columna 4 = (0,0,0,1) | 3x3: 3 filas de 3 con la fila de traslación en cero |
| MTX_SCALE | multiplica con la diagonal (sx, sy, sz, 1) | no toca la matriz vectorial en el modo 2 |
| Modo 2 | LOAD, MULT, TRANS, IDENTITY, PUSH, POP, STORE y RESTORE actúan sobre la posición y la vectorial a la vez | confirmado contra melonDS con ≈ 2000 comprobaciones aleatorias |
| Matriz clip | `posición × proyección`, se recalcula al cambiar una de las dos | 0x04000640 (16 palabras, fila mayor) |
| Matriz vectorial | 3x3 superior izquierda | 0x04000680 (9 palabras, fila mayor) |
| Vértice | VTX_16: 1.3.12; VTX_10: 1.3.6 (`<< 6`); VTX_DIFF: 10 bits con signo **sumados tal cual** (1/4096, ±0,125; ver 3.3) | XY/XZ/YZ conservan la coordenada que falta |
| Espacio de recorte | `(x, y, z, 1) × clip`, cada componente en 64 bits y `>> 12` | |
| Textura | 1.11.4 | modos de TEXIMAGE_PARAM bits 30-31: 0 sin transformar; 1 `(s, t, 1, 1) × T` en TEXCOORD; 2 normal × T + (s, t) **en NORMAL**; 3 vértice × T + (s, t) en cada vértice (reglas medidas en 3.3) |
| Color | RGB555 en la orden; **6 bits** en la salida: `c6 = (c5 << 1) + ((c5 + 31) >> 5)` | el recorte interpola los 6 bits |
| Normal, luz | 1.0.9; luz y normal se transforman con la vectorial (3x3, `>> 12`, saturadas a −512…511) | |
| Iluminación | niveles de 8 bits: difuso `max(0, −N·L) >> 10`; especular `2d² − 1` con semivector `(L + (0,0,−1)) / 2`, tabla de 128 de SHININESS; `(a·b·nivel) >> 13` y ambiente `(a·b) >> 5`; suma con emisión saturada a 31 | **sin contrastar con una DS real**; NORMAL siempre recalcula el color (con ninguna luz, la emisión) |
| Viewport | `sx = (x + w) · ancho / (2w) + x1`, `sy = (w − y) · alto / (2w) + (191 − y2)`, división entera truncada | ancho = x2 − x1 + 1; las y del viewport se miden desde abajo |
| Profundidad | `z24 = (clamp((z · 0x4000) / w + 0x3FFF, 0, 0x7FFF)) << 9` | máximo 0x7FFE00 |
| w | 32 bits, completo; `wshift` por polígono (0, 4, 8…) con `w >> wshift ≤ 0xFFFF` | la rasterización lo usa para el interpolado y el W-buffer |

### Polígonos

- **Ensamblado**: BEGIN_VTXS 0 = triángulos, 1 = cuadriláteros, 2 = tira de triángulos (el orden se invierte en los
  triángulos impares para conservar la cara), 3 = tira de cuadriláteros (a, b, d, c). POLYGON_ATTR se lleva a vigente en el
  BEGIN_VTXS; TEXIMAGE_PARAM y PLTT_BASE se toman al cerrarse el polígono. END_VTXS no hace nada.
- **Recorte** contra los seis planos (`−w ≤ x, y, z ≤ w`) con Sutherland-Hodgman en 64 bits (un cuadrilátero queda en 10
  vértices a lo más); los vértices nuevos interpolan posición, w, textura y color. Un polígono entero fuera de un plano se
  descarta; uno que cruza el plano lejano **sin** el bit 12 de POLYGON_ATTR se descarta, con él se recorta.
- **Cara**: signo del área en pantalla con 8 bits fraccionarios extra (frontal = antihorario a la vista, área ≤ 0); se descarta
  según POLYGON_ATTR bits 6 (traseras) y 7 (frontales). Área cero cuenta como frontal. **Antes** de reservar vértices, de modo
  que lo descartado no gasta RAM.
- **RAM de polígonos y de vértices** (2048 y 6144): un polígono sin recortar **comparte** los vértices que ya guardó una tira
  (como melonDS: la tira de 5 vértices ocupa 5, no 9); uno recortado trae vértices nuevos. Lo que no cabe se descarta y se
  cuenta (`dropped_poly`, `dropped_vert`).

### Pilas

- **Posición/vectorial**: 31 entradas, puntero de 6 bits (`GXSTAT` bits 8-12 enseñan los 5 bajos). PUSH con el puntero > 30:
  error (bit 15), no guarda, el puntero **sigue contando** (con 32 se ve nivel 0). POP resta con signo de 6 bits; fuera de
  0..30 da error y el puntero se queda en el nuevo valor. STORE/RESTORE con índice 31: error. Todo esto sale de comparar con
  melonDS (`geom`, grupo «pilas»); en una DS real no se ha medido.
- **Proyección** (1 entrada, GXSTAT bit 13) y **textura** (1 entrada, sin nivel): puntero de 1 bit con error en el segundo PUSH.
  **Divergencia con melonDS** tras varios errores seguidos (sus POP de más recargan la matriz guardada y mantienen el error; aquí
  no): ningún juego lo usa.
- El error se borra escribiendo 1 al bit 15 de GXSTAT; **no** reinicia el puntero.

### Órdenes de prueba

- **BOX_TEST**: ocho esquinas por la clip, seis caras recortadas contra el volumen; si alguna deja vértices, GXSTAT bit 1 = 1.
  Una caja que contiene al volumen da 0 (ninguna de sus caras lo toca): lo da también melonDS.
- **POS_TEST**: el vértice por la clip, resultado (x, y, z, w de recorte) en 0x04000620-0x0400062C; también deja el vértice
  como actual. **VEC_TEST**: el vector (1.0.9) por la vectorial, 1.3.12 en 0x04000630-0x04000634; si el resultado tiene el bit 12
  encendido se le encienden los bits 13 a 15 (como melonDS; con 1,0 = 0x1000 sale 0xF000). Se leen bien con lecturas de 16 bits;
  melonDS devuelve 0 en las de 32 bits.
- GXSTAT bit 0 sube desde que la orden entra a la cola hasta que termina.

## Salida para la 3.2 (`gx_frame`)

Un cuadro (`gx_frame`, 295 168 bytes; dos, uno se construye y otro se entrega):

- `vtx[6144]` de **32 bytes** (alineados a 16; el arreglo a 128): `sx, sy` (`s16`), `z` (24 bits), `w` (`s32`), `s, t` (1.11.4),
  `r, g, b` (6 bits) y dos palabras reservadas.
- `poly[2048]` de **48 bytes**: `attr`, `texparam`, `pltt`, número de vértices (3 a 10), banderas (`GX_POLY_FRONT/CLIPPED/
  TRANSLUCENT/WIRE`), `wshift`, extensión en pantalla (`xmin/xmax/ymin/ymax`) y `vidx[10]` (índices `u16` a `vtx`).
- `npolys`, `nverts`, `swap_params` (bit 0: ordenar translúcidos a mano; bit 1: W-buffer), los descartados, `seq` y la foto de
  los registros del 3D (`gx_regs`).
- Sin punteros: se copia por DMA tal cual. El orden de los polígonos es el de envío; **no se ordenan los translúcidos** (hay que
  hacerlo en la 3.2 con `ymin/ymax`, a menos que `swap_params` bit 0 lo prohíba).
- Entrega: `nds_gx_front(n)` da el último cuadro entregado. El que se construye es el otro; si el SPE sigue leyendo el
  entregado cuando llega el siguiente SWAP_BUFFERS, el motor lo vuelve a llenar (la DS real también: el render tiene un cuadro).
  Hay que terminar de rasterizar antes del VBlank siguiente o copiar el cuadro.

`sizeof(gx_t)` = 604 928 B (los dos cuadros, la cola de 512 entradas y el estado), y `struct nds` pasa de 8 749 184 a
9 354 112 B.

## Pruebas

1. **ROM `geom`** (2878 comprobaciones; `tests/roms/src/geom/arm9.c`): matrices contra un modelo en C
   (≈ 2000 comprobaciones aleatorias en los modos 0, 1 y 2 de MULT/LOAD/SCALE/TRANS), pilas, POS/VEC/BOX_TEST, unas 20 escenas
   de listas de polígonos con `RAM_COUNT`, SWAP_BUFFERS, cuenta de la FIFO y palabras empaquetadas. Da **OK en SalsaDS (LE, BE y
   con sanitizadores)** y en melonDS con una sola excepción, el bit «ocupado» tras 100 MTX_STORE (melonDS vacía la FIFO
   al escribirla). Todo lo leído de vuelta queda en una tabla en `0x02300000`; la comparación fila a fila con melonDS (tabla de
   unos 160 valores) sólo difiere en las lecturas de 32 bits de VEC_RESULT, el tiempo de la FIFO y la pila de proyección tras
   errores repetidos.
2. **Unitarias** (`tests/unit/test_gx.c`): matrices contra un modelo independiente (40 rondas), palabras empaquetadas, cuenta de
   la FIFO de 1 a 260 entradas, tiempos de órdenes, pilas, 15 casos de polígonos, la salida en pantalla (coordenadas, profundidad,
   viewport), POS/VEC/BOX_TEST, iluminación, los límites de 2048/6144 y la interpolación del recorte. Corren en LE (con ASan y
   UBSan) y en BE (qemu-ppc64).
3. **Juegos** (respaldos del autor, fuera del repositorio): New Super Mario Bros., Mario Kart DS y Super Mario 64 DS siguen
   llegando adonde llegaban (menú, archivo, mapa y nivel 1-1 con el reloj corriendo; la carrera de demostración; «Touch to start»),
   y su geometría sale con sentido (`--gx-dump` y `tools/gx-png.py`: la escena de la carrera se reconoce, Mario sale abajo a la
   izquierda en 1-1).

## Mediciones

Host (Mac, arm64 nativo), `salsads-cpurun --stats`; la geometría cuesta lo que tarda de más el mismo guion sin ella:

| Escena | Polígonos y vértices por cuadro (promedio / máximo) | Motor ocupado (DS) | Tiempo de geometría por cuadro en el host |
|---|---|---|---|
| NSMB, nivel 1-1 (6700 cuadros) | 481 / 1895 y 977 / 3638 | 14,0 % (78 292 ciclos del motor) | **0,64 ms** (24,38 s contra 20,11 s sin geometría) |
| NSMB, menú (4000 cuadros) | 593 / 1914 y 1145 / 3652 | 25,3 % | 0,68 ms |
| Mario Kart DS, demostración (3000 cuadros) | 986 / 1457 y 2749 / 4037 | 10,9 % | ≈ 0,2 a 0,6 ms (la Mac con otras cosas corriendo: 8,2 s contra 6,6-7,7 s en 3000 cuadros) |
| Super Mario 64 DS, «Touch to start» (3000) | 44 / 46 y 117 / 123 | 1,0 % | ≈ 0,15 ms (3,8-3,9 s contra 3,4 s en 3000 cuadros; ≈ 570 órdenes por cuadro) |

New Super Mario Bros. manda ≈ 2700 polígonos por cuadro de los que sobreviven el 22 %: el 55 % cae fuera del volumen de vista y el
23 % es de la cara que no se pide. El tiempo se reparte entre ejecutar órdenes (la matriz y el vértice), decodificar palabras
empaquetadas y recortar; ≈ 16 000 órdenes por cuadro, ≈ 80 ns por orden en la Mac.

**Cuidado con el PPE**: el PPE es en orden y varias veces más lento que un núcleo de la Mac para este tipo de código (enteros de
64 bits, divisiones). Si fuera 6-10 veces más, serían 4-6 ms por cuadro de los 16,7, demasiado para el hilo 0 que además corre el ARM9.
Hay que **medirlo en la PS3** («DATO geometria» en el log del paquete «SalsaDS núcleo», con reloj de perfil en los cuadros impares) y,
según el número, optimizar (sin divisiones de 64 bits en el viewport, matrices por filas, `outcode` antes de transformar todo) o
llevar el motor a un SPE (`gx_t` ya no depende del núcleo; las órdenes se pueden mandar por lotes).

## Divergencias con melonDS y lo que no se pudo contrastar

| Tema | SalsaDS | melonDS 1.1 |
|---|---|---|
| Lecturas de 32 bits de VEC_RESULT (0x04000630) | dan las dos mitades | 0 (las de 16 bits sí) |
| Nivel de FIFO con el motor ocupado (100 MTX_STORE seguidos) | ≈ 78 entradas (el motor cuesta 17 ciclos por orden, la CPU escribe cada ≈ 12) | 0 |
| Pila de proyección/textura tras varios errores seguidos | un POP de más no recarga la matriz | recarga la guardada y mantiene el error |
| Todo lo demás de la ROM `geom` | idéntico | idéntico |

Sin contrastar con una DS real (se marca `[sin contrastar]` en el código): la iluminación (anchos y redondeos), el recorte
(orden de los planos, redondeo de la interpolación), los costes por polígono, cara de área cero, el
`wshift`, el 6 bits de los colores y el signo del bit 12 de VEC_RESULT. La ROM `geom` se puede correr en una DS real
(`docs/ds-real.md`): sus `SDS_DIVERGE` anotan en la SD qué valor da el hardware.

## Qué sigue para la 3.2

1. **Rasterizador en SPE** sobre `gx_frame`: `ymin/ymax` por polígono para repartirlo en bandas; interpolar `w`, `z`, color y
   textura con `wshift`; translúcidos ordenados por Y (o a mano con `swap_params` bit 0); z-buffer o W-buffer según `swap_params`
   bit 1; DISP3DCNT, niebla, toon, bordes y alfa salen de `gx_regs`. El prototipo de la Fase 0.4 (`tests/phase0/spe-raster3d`)
   ya mide 4,2 ms con 2048 triángulos en 2 SPE a 1x.
2. **Texturas y paletas** de la VRAM (cuatro bancos de textura y cuatro de paleta, `TEXIMAGE_PARAM`, `PLTT_BASE`): lo decide la
   3.2 con la caché de la Fase 0.4 (que se desbordaba con 24 texturas).
3. **Dónde corre el motor**: medir en la PS3 («DATO geometria»); si pasa de ≈ 2 ms, SPE.
4. Orden de los translúcidos, `SWAP_BUFFERS` con el rasterizador asíncrono (¿el motor se detiene hasta que el SPE termina el
   cuadro anterior?), y la copia del 3D al motor A (captura de pantalla, modo de visualización 3D, bit 3 de DISPCNT).


# Fase 3.2 — Motor de render 3D

El motor de render de la DS sobre `gx_frame`: una referencia en C portable y la versión de los SPE, que dan **los mismos
bits** (host LE, qemu BE, PPE y SPE), y la capa 3D (BG0 del motor A) mezclada por el compositor 2D. Código propio desde
GBATEK y la comparación con melonDS **ejecutándolo** (ROM `gx3d`, `tools/gfx-compare.py`, `tools/r3d-fit.py`,
`tools/melonds-shot.py`); no se leyó ni copió código de melonDS, NooDS ni DeSmuME.

## Archivos

| Archivo | Qué es |
|---|---|
| `src/gpu3d/r3d.h` | API: `r3d_job` (un cuadro, su reparto en bandas y las ranuras de VRAM), píxel de salida (`R3D_PX`: 6 bits por canal + alfa de 5), contadores |
| `src/gpu3d/r3d_px.h` | **Las reglas**, funciones `static inline` en enteros de 32 bits que comparten la referencia y el SPE: polígono, aristas, líneas, interpolación, texturas, modos, profundidad, translúcidos, sombras, edge marking, niebla |
| `src/gpu3d/r3d_ref.c` | Referencia: el cuadro entero en memoria, polígono por polígono, cada texel leído de la VRAM |
| `src/gpu3d/r3d_core.c`, `r3d_core.h`, `r3d_io.h` | El rasterizador por bandas (el código del SPE; en el host y el PPE, con copias que comprueban las reglas del DMA) |
| `src/gpu3d/r3d_scene.c` | Escenas sintéticas con el motor de geometría real (pruebas y autoprueba de la PS3) |
| `src/spu/raster3d/` | Programa SPE (spukit) |
| `src/core/video.c`, `geom.c`, `src/gpu2d/*` | La capa 3D en la instantánea del cuadro, los ganchos de la PS3 y BG0 en el compositor y su referencia |
| `tests/unit/test_render3d.c`, `tests/roms/src/gx3d/arm9.c` | Pruebas unitarias y ROM `gx3d` (15 escenas por semilla) |

## Las reglas (r3d_px.h)

- **Orden**: opacos primero, translúcidos después (alfa 1-30 o textura A3I5/A5I3); cada grupo por la y de abajo y luego la de
  arriba, estable; con `swap_params` bit 0 los translúcidos van en el orden de envío. [melonDS: la escena de translúcidos sale idéntica]
- **Cobertura**: líneas `ytop..ybot-1` (de altura cero, su única línea). Dos cadenas desde el vértice de arriba; la izquierda
  por el signo del área. Aristas en punto fijo de 18 bits con la pendiente truncada hacia cero. Arista y-mayor: un píxel por
  línea (izquierda `piso(X)`, con 1/2^18 menos si la pendiente es negativa; derecha `piso(X)` exclusivo). Arista x-mayor: la x
  de la línea siguiente redondeada (izquierda `X(y+1) − 1/2^18`, derecha `X(y+1)`, o `− 1/2^18` si va a la izquierda), y su
  tramo «gordo» cuando va hacia afuera. Línea vacía por redondeo con los bordes iguales: un píxel. **Ajustado a melonDS** con la
  escena 13 (48 triángulos al azar) y `tools/r3d-fit.py`: de 866 píxeles distintos a 44; quedan ≈ 5 % de las líneas con un
  píxel de diferencia en un borde.
- **Interpolación** (la regla de 3.3 reemplaza lo que sigue para el color y las líneas ortográficas): factor con corrección por w, `f = piso(pos·w0·2^P / ((len−pos)·w1 + pos·w0))`, P = 9 en aristas (posición
  en y, o en x para las x-mayores) y P = 9 en la línea para texturas y w, 8 para el color; `a = (a0·(2^P−f) + a1·f) >> P`. El
  color se interpola con 3 bits más y la fracción llena. z lineal en pantalla (Z-buffer); la del W-buffer es la w interpolada
  `<< wshift`. Todo exacto en 32 bits (el factor: cociente en coma flotante corregido en enteros; con las w iguales y la z,
  por cociente y resto paso a paso).
- **Profundidad**: «menor» o «igual» (margen 0x200 en Z, 0xFF en W) [sin contrastar el margen]. Borrado con `CLEAR_COLOR`
  (color, alfa, niebla, id) y `CLEAR_DEPTH << 9` (0x7FFF llena los 9 bits bajos).
- **Píxel**: textura (7 formatos, incluido el 4x4 con sus 4 modos; repetir, espejo, recorte; índice 0 transparente) y modos
  modulate `((t+1)(v+1)−1)>>6`, decal, toon (el rojo del vértice indexa la tabla) y highlight (gris del rojo × textura + tabla),
  sombra (máscara con id 0 marca donde falla la profundidad; la sombra dibuja donde hay marca y otro id de opaco). Alfa 0 no se
  dibuja; alpha test con `ALPHA_TEST_REF`. Opaco: escribe color, profundidad, id, niebla y borde. Translúcido: no se mezcla dos
  veces con el mismo id, mezcla `(src·(a+1) + dst·(31−a)) >> 5` si `DISP3DCNT` bit 3 y el destino tiene alfa, alfa = máximo,
  profundidad solo con `POLYGON_ATTR` bit 11, niebla = la del destino y la suya.
- **Posproceso por línea**: edge marking (un píxel de borde de opaco con un vecino de otro id y más lejos toma el color de
  borde de su id) y niebla (densidad interpolada de la tabla de 32 desde `FOG_OFFSET` con pasos de `0x400 >> FOG_SHIFT`; color
  y alfa o solo alfa).
- **Pendiente** (registros ya leídos en `gx_regs`): **antialiasing** de aristas (bit 4), **imagen de fondo** (bit 14: hoy
  solo el color de borrado), polígonos de un punto (bit 13 de `POLYGON_ATTR`), los bits de desbordamiento de `DISP3DCNT`.

## En la PS3

- **Dos SPE, bandas alternas de 16 líneas** (12 por cuadro). Reparto de SPE: 2D A, 2D B, sonido y 3D × 2: **queda uno libre**
  (filtros, Fase 5). El PPE no hace nada por polígono: publica un trabajo de spukit por SPE.
- **Por SPE y cuadro**: trae los 2048 polígonos a trozos de 64 (doble búfer), clave de orden e índice en una palabra, radix de
  3 pasadas; por banda limpia color/profundidad/atributos, filtra la lista por `ymin/ymax`, trae polígonos (listas de DMA de 48 B)
  y vértices (listas de 32 B) en lotes de 16 con el siguiente en vuelo, dibuja, posprocesa y saca la banda (16 KB, doble búfer).
  Con edge marking la banda se dibuja con una línea más arriba y otra abajo (los vecinos del borde).
- **Caché de texturas** (el problema de la 0.4, que traía texturas enteras y se desbordaba al recorrer cada unidad la lista):
  **bloques de 256 texeles ya decodificados** (4 líneas × 64 texeles, o la textura en franjas si es más angosta), 64 bloques de
  1 KB, 4 vías con LRU por conjunto, llenados por demanda con el mismo `r3d_decode` de la referencia leyendo la VRAM por una
  caché de líneas de 128 B (2 vías; texturas y paletas aparte). Dura todo el cuadro (todas las bandas del SPE) y se vacía al
  empezar. Último bloque en un registro: casi todos los texeles no buscan. Aciertos en juegos (host, una unidad): Super Mario 64
  DS 99,89 % (54 bloques por cuadro, 0 desalojos), New Super Mario Bros. 99,54 % (111, 70), Mario Kart DS 99,41 % (428, 366).
- **Local store** (`src/spu/raster3d/raster3d.c`, medido al enlazar): texto 46,5 KB, datos 0,8 KB, bss 190 KB (contexto
  186 368 B), pila con 24,5 KB libres (la cadena más honda usa 4,4 KB; mínimo 8 KB).
- **Autoprueba al arrancar**: 9 sondas de aritmética (factor, lerp, aristas, divisiones, desplazamientos y multiplicaciones de
  64 bits, una línea entera) y 4 escenas sintéticas (2000, 431, 292 y 394 polígonos) en los dos SPE contra la referencia del PPE;
  si algo difiere rasteriza el PPE. En la prueba automática, además, cada 30 cuadros la capa 3D de los SPE contra la del PPE.
- **Línea `DATO raster3d`** en `nucleo.log`: ms por cuadro de cada SPE (mediana, p95, máx), espera del PPE en el VBlank,
  polígonos, píxeles, aciertos de la caché, bloques y desalojos, y el reparto del tiempo (orden, listas y borrado, DMA, dibujo,
  texturas, posproceso, salida).
- **RPCS3** (recompilador LLVM de SPU): sondas iguales, las 4 escenas con 0 píxeles distintos, la verificación durante la prueba
  con la ROM gx3d sin diferencias, `RESULTADO nucleo: OK`. Su **intérprete** de SPU da otros resultados en multiplicaciones de 32
  bits y desplazamientos de 64 (no sirve para esto) y su recompilador se colgó («Illegal instruction») con la primera versión,
  que tenía desplazamientos de 64 bits con signo: por eso todo `r3d_px.h` es de 32 bits y la sonda 5 lo vigila (informativa).

## Sincronía

El cuadro que el motor de geometría entrega en el SWAP_BUFFERS del VBlank N se **dibuja durante el cuadro N+1**, como en la
DS (que lo rasteriza mientras barre la pantalla):

1. **Línea 0 de N+1** (`nds_video_frame_begin`): se toma la instantánea del cuadro (VRAM, la de texturas incluida: los juegos
   suben texturas en el VBlank, antes de que el hardware llegue a esas líneas) y se lanza el trabajo 3D con el cuadro entregado
   (`gx_front`). Si el motor A no muestra BG0 3D en la línea 0 no se rasteriza; sin cuadro entregado o con el render apagado
   (`POWCNT1` bit 2) la capa es transparente.
2. **Línea 192 de N+1** (`nds_gx_vblank`, antes del SWAP_BUFFERS): el PPE espera el 3D (`wait3d`). Hace falta por dos cosas:
   el SWAP_BUFFERS devuelve ese cuadro al motor de geometría para llenarlo de nuevo, y en seguida se entrega el 2D de N+1,
   cuyo SPE del motor A lee la capa 3D línea por línea (`img3d_ea` de la instantánea, 1 KB por línea por DMA).
3. El 2D se compone mientras se emula N+2 (el cuadro de retraso de la Fase 2): la capa 3D queda en el mismo cuadro que su 2D.

El 3D tiene así el tiempo de emular 192 líneas (casi todo el cuadro) en dos SPE; si tarda más, el PPE espera (línea
`DATO raster3d`, «espera del PPE»). En el host todo es síncrono en la línea 0.

## La capa 3D en el compositor 2D

BG0 del motor A (DISPCNT bit 3, o modo 6): la línea de la imagen 3D desplazada por `BG0HOFS` (9 bits; lo que cae en 256..511 es
transparente), con su prioridad y las ventanas. **Todo píxel 3D** que queda arriba se mezcla con el objetivo 2 (si lo es la capa
de abajo) con **su propio alfa**, sin mirar el modo de BLDCNT ni EVA/EVB: `(3D·(a+1) + 2D·(31−a) + 16)/32` en 6 bits por canal
con los 6 bits del 3D (el opaco queda igual); sin objetivo 2, brillo si BG0 es objetivo 1. Igual en el compositor (SPE) y en su
referencia; contra melonDS la escena 11 es idéntica.

## Pruebas

1. **Unitarias** (`tests/unit/test_render3d.c`, LE con ASan/UBSan y BE con qemu): 40 escenas del motor de geometría real (hasta
   2000 polígonos, los 7 formatos, modos, alfas, W/Z, niebla, bordes, registros al azar) con 4 repartos (bandas de 8 y 16, 1 a 3
   unidades): **idénticas a la referencia**, sin transferencias que el MFC rechazaría, ≈ 78 000 desalojos de la caché, y el
   hash de la semilla 7 igual en LE y BE.
2. **ROM `gx3d`** (`tests/roms/homebrew`, una escena por semilla con `tools/rom-seed.py`): en `test-host.sh` en LE, BE y con
   sanitizadores: el código del SPE igual a la referencia en cada cuadro, los píxeles de prueba (`--expect`), el compositor 2D
   igual a su referencia y la imagen igual a `gx3d.golden`. Con captura (melonDS, una DS real) la ROM lee el banco A y anota con
   `SDS_DIVERGE` los píxeles de prueba que no coinciden.
3. **Contra melonDS 1.1** (`tools/gfx-compare.py build/roms/gx3d.nds 0..14`, imágenes en `build/r3d/cmp/`):

| Escena | Píxeles distintos | De más de 1 en 5 bits | Qué queda |
|---|---|---|---|
| 0 cobertura | 1656 | 500 | bordes (≈ 5 % de líneas) y el color de la tira de Gouraud |
| 1 Gouraud | 14 001 | 2914 | color ±1 (ver abajo) |
| 2 perspectiva | 391 | 80 | color ±1 y algún píxel de borde |
| 3 los 7 formatos y repetir/espejo/recorte | **0** | 0 | — |
| 4 translúcidos (ids, orden, profundidad) | **0** | 0 | — |
| 5 / 6 Z-buffer / W-buffer | 35 / 35 | 35 | un píxel de borde cada 5 líneas en una arista |
| 7 / 8 decal y toon / highlight | 3941 / 4957 | 2529 / 2653 | el color interpolado ±1 mueve el índice de la tabla de toon |
| 9 alpha test, niebla, edge marking | 1560 | 0 | niebla ±1 en una franja |
| 10 sombras | **0** | 0 | — |
| 11 3D sobre BG1 con mezcla | **0** | 0 | — |
| 12 profundidad «igual» | **0** | 0 | — |
| 13 / 14 triángulos al azar (cobertura / Gouraud) | 44 / 2239 | 44 / 1003 | bordes / color ±1 |

   **Divergencia principal (resuelta en 3.3)**: la interpolación del color de vértice. Con el modelo actual el 70 % de los canales coincide y el
   resto difiere en 1 (de 5 bits); ninguna variante de precisión, redondeo o posición probada con `tools/r3d-fit.py` lo explica.
   Visualmente no se nota salvo en toon/highlight, donde ±1 en el rojo cambia de entrada de la tabla en el borde de cada franja.
4. **Juegos** (respaldos del autor, fuera del repositorio; capturas en `build/r3d/juegos/`): New Super Mario Bros. 1-1
   (`nsmb-4500.png`: Mario dibujado, con su contorno y su sombreado; `nsmb-mario-zoom.png`), Mario Kart DS demostración
   (`mkds-1500.png`, `mkds-2500.png`: pista, karts y escenario) y Super Mario 64 DS «Touch to start» (`sm64ds-2900.png`). En las
   tres, `--vs-ref3d` da el código del SPE igual a la referencia en todos los cuadros. Contra melonDS (`tools/melonds-shot.py`,
   la escena por tiempo, a ojo: `sm64ds-vs-melonds.png`) la estrella de SM64DS **sale mal**: sus ojos son tiras negras en X y la
   textura de entorno (4x4 con coordenadas por normales, modo 2) tiene ruido. Los vértices de esos polígonos ya vienen así del
   motor de geometría (`--gx-dump`), así que es un pendiente de la 3.1 (matrices de los huesos o el modo 2 de texturas), no del
   rasterizador. NSMB no se comparó con melonDS (no se le pueden mandar teclas por guion). **Arreglada en 3.3.**

## Mediciones

Host (Mac, arm64, clang -O2; el código del SPE en escalar con una unidad), `--r3d-stats`:

| Escena | Polígonos por cuadro | Píxeles probados / escritos | ms por cuadro |
|---|---|---|---|
| New Super Mario Bros. (menú, archivo, mapa y 1-1; 3993 cuadros con 3D) | 643 | 45 454 / 20 591 | **0,95** |
| Mario Kart DS, demostración (1674) | 851 | 127 966 / 71 444 | **3,30** |
| Super Mario 64 DS, «Touch to start» (2799) | 46 | 149 428 / 115 150 | **1,52** |

El bucle de la línea sin divisiones enteras ni 64 bits bajó SM64DS de 2,43 a 1,52 ms y MKDS de 3,62 a 3,30 en el host, con los
mismos bits. **En la PS3 no está medido**: el SPE no tiene unidad escalar ni predice saltos y este bucle es escalar (un píxel por
vuelta, con saltos por modo y por prueba), como el compositor 2D de la 2.1 que costó 28 ms antes de vectorizarlo. En RPCS3 (que no
mide, solo orienta) la escena 9 de gx3d (31 000 píxeles) da 1,6 ms por SPE. Si la PS3 da más de ≈ 6 ms por SPE en MKDS, lo
siguiente es vectorizar la línea (4 píxeles por vuelta con el factor y los atributos en vectores, especializada por modo).

## Paquete

«SalsaDS núcleo» (SLDS00008) del commit `211d01e`: `build/salsads-nucleo-fase32.pkg` (SHA-256 en `docs/estado.md`). Con la ROM
gx3d (o un juego) como `/dev_hdd0/salsads/rom.nds`, el log trae `DATO raster3d` cada 120 cuadros.

## Qué sigue

1. **Medir en la PS3** (`DATO raster3d` con NSMB 1-1, MKDS y SM64DS) y, según el número, vectorizar el bucle de la línea.
2. La interpolación del color (±1) y los bordes que quedan (≈ 5 % de líneas), con más escenas de ajuste.
3. Antialiasing, imagen de fondo, polígonos de un punto.
4. La estrella de SM64DS: el motor de geometría (huesos y coordenadas de textura por normales).
5. Correr `gx3d` en una DS real (`docs/ds-real.md`): sus `SDS_DIVERGE` dicen qué hace el hardware en los píxeles de prueba.


# Fase 3.3 — Fidelidad del 3D

Tres arreglos contra melonDS (siempre **ejecutándolo**: la ROM `gx3d`, `tools/gfx-compare.py`; código propio desde GBATEK y las
mediciones; ni se leyó ni se copió código de melonDS, NooDS ni DeSmuME).

## La estrella de «Touch to start» de Super Mario 64 DS

Dos causas, las dos en `gx.c`, y las dos salían del volcado de órdenes de un cuadro (órdenes y parámetros del cuadro 2929):

1. **VTX_DIFF (0x28)** sumaba la diferencia por 8 (la tabla de 3.1 decía «1.0.9 `<< 3`»). El campo de 10 bits con signo se suma **tal
   cual** a la coordenada 1.3.12 (rango ±511/4096 = ±0,125). Los ojos de la estrella (una tira de triángulos de VTX_XY, VTX_YZ
   y VTX_DIFF) quedaban ocho veces más grandes: las tiras negras en X. La ROM `gx3d` y `test_gx.c` ya lo cubren.
2. **Coordenadas de textura generadas** (la textura de entorno de la estrella: 4x4 con el modo 2, la normal por la matriz de
   textura). Tres reglas, medidas con la escena 15 de `gx3d` (12 celdas con matrices distintas, comparadas con melonDS):
   - **Modo 2** (normal): se calcula **al llegar NORMAL**, no en el vértice: `s = TEXCOORD.s + ((n · M) >> 21)` con la 3x3 de la
     matriz (sin la fila de traslación). Un TEXCOORD posterior pisa el resultado con las coordenadas crudas. Antes: `>> 17` y se
     sumaba la traslación (16 veces más grande, de ahí el «ruido»).
   - **Modo 3** (vértice): `s = TEXCOORD.s + ((v · M) >> 24)`, sin traslación, en cada vértice.
   - **Modo 1**: `(s, t, 1, 1) × M` en TEXCOORD, y la traslación y la tercera fila se suman **sin** multiplicar por 16
     (`(s·m0 + t·m4 + m8 + m12) >> 12`).

   Estado nuevo en el motor: `cur_s/cur_t` (las coordenadas que llevará el próximo vértice). Escena 15: **idéntica** a melonDS
   (celdas de los modos 1 y 2; las del modo 3 con degradado fuerte también, desde la regla lineal exacta de abajo). Contra el
   juego (`build/r3d/juegos/sm64ds-star-despues.png`): la estrella sale con ojos, degradado y entorno como en melonDS (el borde
   suave del melonDS es su antialiasing, que aún no está).

## La regla de interpolación del color

Medida con escenas que aíslan una sola cosa (colores y largos al azar; el analizador cuenta los canales de 5 bits que difieren
y ajusta la regla; todas dan **cero diferencias** salvo lo anotado):

| Escena | Aísla | Resultado |
|---|---|---|
| 16 | la línea (rectángulos con un color por lado vertical; 2 a 240 píxeles, 5724 canales) | idéntica |
| 17 | las aristas y-mayores (color arriba y abajo; 2 a 180 líneas) | idéntica |
| 18 | la línea en perspectiva (`w` distinta a la izquierda y a la derecha, 1 a 7) | idéntica |
| 19 | las aristas en perspectiva | 12 píxeles (cuatro canales de 5532, todos en el rectángulo de 180 líneas con `w` 1:9, a un nivel) |
| 20 | el color por una arista y-mayor inclinada | idéntico en la arista izquierda (0 de 18 triángulos con diferencia) |
| 22 | la línea con aristas inclinadas (paralelogramos con un color por lado) | ver «denominador» |

La regla (en `r3d_px.h`):

1. **El color de vértice se interpola con 3 bits más**, `c9 = (c6 << 3) | (c6 ? 7 : 0)`: la fracción llena (redondea hacia
   arriba) **salvo en el 0**, que sigue siendo 0 (con 7 también en el 0 había 20 diferencias de 5724; ajustando una tabla por
   valor salieron cero con solo ese caso). Después `>> 3` a 6 bits.
2. **Con las `w` iguales (ortográfica) la interpolación es lineal y exacta**: `v(k) = a + piso((b − a) · k / L)`. Antes se pasaba
   por un factor de 8 o 9 bits (`piso(k · 2^P / L)`), de ahí el «±1 en el 30 % de los canales». Se calcula sin divisiones por
   píxel (cociente y resto por paso en la línea, `r3d_dda`; en las aristas, una estimación en coma flotante corregida en
   enteros, `r3d_lin`) y todo en 32 bits, igual en referencia, SPE, PPE y host. Vale también para las coordenadas de textura.
3. **Con `w` distinta (perspectiva)** el factor sigue siendo `piso(k · w0 · 2^P / ((L − k) · w1 + k · w0))`, con **P = 8 para el
   color en la línea** y P = 9 en las aristas y para la textura (de 6012 canales, 0 diferencias con P = 8; con P = 9, 63).
4. **Denominador de la línea**: `L = xr − xl` (píxeles cubiertos), y **uno más cuando la arista derecha es inclinada y y-mayor**.
   Se vio con los paralelogramos: en un rectángulo la línea de 40 píxeles interpola con 40; con las aristas inclinadas
   (36 paralelogramos, todas las líneas) con 41. Con la derecha vertical o x-mayor no cambia; ninguna variación del
   denominador mejora las x-mayores. No se sabe el mecanismo del hardware: es lo que reproduce las mediciones.
5. **Las aristas x-mayores**, con las `w` iguales, interpolan **por la y, en el extremo de afuera del tramo de la línea**: la
   izquierda que va hacia la izquierda y la derecha que va hacia la derecha usan `(y + 1 − y0) / dy`; las otras, `(y − y0) / dy`
   (antes, la posición en x del píxel). Con las `w` distintas se mantiene la regla anterior.
6. **Decal**: un texel de alfa 0 deja el color del vértice tal cual y de alfa 31 el del texel (la fórmula `(t·a + v·(31 − a)) >> 5`
   los dejaba 1/32 abajo). Con eso toon y highlight (escenas 7 y 8) salen **idénticos**.

## Diferencias con melonDS antes y después (`tools/gfx-compare.py build/roms/gx3d.nds N`, píxeles distintos de 49 152)

| Escena | 3.2 | 3.3 | Qué queda |
|---|---|---|---|
| 0 cobertura | 1656 | 179 | bordes (≈ 5 % de líneas, un píxel) |
| 1 Gouraud | 14 001 | 828 | color ±1 junto a las aristas x-mayores |
| 2 perspectiva | 391 | 107 | bordes y un nivel de color |
| 7 / 8 decal y toon / highlight | 3941 / 4957 | **0** / **0** | — |
| 9 niebla, edge marking, alpha test | 1560 | 1560 | niebla ±1 en una franja (sin tocar) |
| 13 / 14 triángulos al azar | 44 / 2239 | 44 / 930 | bordes / color junto a aristas x-mayores |
| 15 a 18 (nuevas: textura generada y color) | — | **0** | — |
| 19, 20, 21, 22 (nuevas) | — | 12, 544, 3638, 146 | arista derecha en perspectiva, aristas x-mayores |

Las escenas 3, 4, 10, 11 y 12 siguen idénticas. **Pendiente**: el color de los triángulos con tres aristas x-mayores (escena 21:
cada línea difiere en ±1 de 5 bits en ≈ 40 % de los canales; el extremo izquierdo se explica y el derecho no del todo), la
niebla (escena 9), los bordes de un píxel y el antialiasing.

## Juegos

Sin regresiones (código del SPE igual a la referencia en todos los cuadros, `--vs-ref3d`): Super Mario 64 DS «Touch to start»
(1,60 ms por cuadro en el host), Mario Kart DS (carrera de demostración, 2,47 ms) y New Super Mario Bros. (menú, 0,29 ms).
Capturas en `build/r3d/juegos/` (`sm64ds-star-despues.png`, `mkds-2500.png`, `nsmb-4500.png`).

## Captura de pantalla (DISPCAPCNT)

**No hecha** (era opcional). Sigue sin emularse: la ROM `gx3d` la dispara solo en melonDS. Pasos para la 3.4: el compositor 2D
del motor A escribe, línea por línea, la mezcla de la fuente A (la imagen del motor A o solo el 3D) y de la B (un banco de VRAM
o la FIFO de memoria principal) con EVA/EVB en el banco de captura (bits 16-17 de `DISPCAPCNT`, de 128×128 a 256×192) y apaga
el bit 31 al terminar el cuadro. Pide que el SPE escriba VRAM (hoy solo la lee), o que el PPE lo haga con la imagen de la línea.

## Pruebas nuevas

`tests/unit/test_gx.c` (`test_texgen`): VTX_DIFF, los tres modos de textura generada, y que un TEXCOORD después de NORMAL pisa el
modo 2. ROM `gx3d`: escenas 15 a 22 (en `test-host.sh` con imagen de referencia y `REF3D`), `gx3d.golden` con las 23 semillas. El
hash de la semilla 7 de `test_render3d.c` cambió con las reglas (igual en LE y BE).


# Fase 3.4 — Rendimiento del 3D en el SPE y juntas entre triángulos

Dos problemas, los dos vistos en la PS3 con los respaldos del autor: el 3D tardaba 26–36 ms por SPE y cuadro en New Super Mario Bros.
(un nivel) y Super Mario 64 DS (el PPE esperaba 29–32 ms al 3D en el VBlank), y en Super Mario 64 DS «a veces se ven juntas entre
los triángulos».

## Diagnóstico con las líneas `DATO raster3d` de la PS3 (`build/ps3-fase33/*.log`, 178 muestras)

Un ajuste por mínimos cuadrados de las columnas del `DATO` (suma de los dos SPE, por cuadro) da el costo de cada cosa:

| Qué | Costo medido | Dónde |
|---|---|---|
| Píxel dibujado (bucle de la línea escalar, sin texturas) | **210 ns** (≈ 670 ciclos) por píxel probado | 137 000 píxeles por cuadro en NSMB (un nivel) y 180 000 en SM64DS → 30–40 ms |
| Bloque de texeles decodificado | **73 µs** (≈ 780 ciclos por texel) | 300–500 bloques por cuadro → 15–30 ms (`texturas` del `DATO`) |
| Polígono en una banda (preparar + aristas) | 3,7 µs | 600–1500 pares banda·polígono: 2–6 ms |
| Fijos por cuadro (orden, lista y borrado, DMA, salida) | ≈ 1 ms por SPE | — |

No era un costo «por banda» ni un borrado: el tiempo es alto y casi constante porque esas dos escenas dibujan 130–180 mil píxeles
por cuadro (fondos y suelos grandes) y cada píxel costaba unos 670 ciclos. El bucle de la línea era escalar (un píxel por vuelta,
cinco interpoladores por línea con comparaciones, ~10 saltos sin predicción por píxel) y la decodificación de texturas leía cada
byte de la VRAM por una función con búsqueda en la caché de líneas (≈ 8 búsquedas por texel en el formato 4x4).

## Qué se hizo

1. **Línea en vectores** (`src/gpu3d/r3d_vrow.h`): la misma regla de `r3d_draw_row`, cuatro píxeles por vuelta con vectores de
   16 bytes (genéricos de GCC/clang; solo las multiplicaciones de 16 bits, las conversiones a coma flotante y la recíproca llevan
   intrínsecos del SPU), sin saltos por píxel (máscaras de carril) y sin divisiones por píxel: `a + piso((b - a)·k / L)` se evalúa
   como `a + q·k + piso(r·k / L)` y el factor en perspectiva es `r3d_pfac` en vectores (cociente en coma flotante y corrección
   entera: no depende del redondeo del SPU). Cuatro variantes (w iguales o distintas × con o sin textura); sombras, alambre, decal,
   toon, highlight, alpha test y translúcidos van dentro, con máscaras. La referencia (`r3d_ref.c`) sigue con `r3d_draw_row`.
2. **Texturas por tramos** (`tc_fill`): el bloque de 256 texeles se decodifica leyendo la VRAM por tramos de la fila (copia desde
   la caché de líneas de 128 bytes) y con la paleta ya convertida en el local store (se rehace solo al cambiar de paleta), en lugar
   de texel a texel con una función por byte. El formato 4x4 va por tile (índices, palabra de modo y colores una vez por tile).
   Ahora la caché de líneas de VRAM es de 32 + 8 líneas (antes 64 + 16): el espacio lo necesita el código nuevo.
3. **El texel que no está en el bloque anterior** se busca por bloque (`tc_get`); el caso común (los cuatro píxeles en el bloque
   anterior) es una comparación y cuatro cargas.
4. **Aristas sin aritmética de 64 bits ni módulo**: la z de los extremos (`r3d_zlerp`: cociente y resto de 32 bits en lugar de una
   división de 64 bits de software por extremo y línea) y el recorrido de la cadena de aristas (sin `%`). Mismos bits.
5. **Borrado de la banda** con almacenamientos de 16 bytes.
6. **Instrumentación nueva en el `DATO raster3d`**: líneas de polígono por cuadro, y dentro del tiempo de dibujo el reparto
   `polígonos` (preparar), `aristas` (`r3d_row_at`) y `píxeles` (la línea, texturas incluidas), además de `texturas`.

Local store del SPE (`raster3d`), medido al enlazar: texto 56 032 B (antes ≈ 47 000), bss 186 400 B (contexto 182 656 B, antes
186 368), pila con 18 768 B libres (la cadena más honda usa 6 720 B: holgura 12 048 B, mínimo 8 192). `tc_fill`, `tc_fill5` y la sonda de la autoprueba se
compilan con `-Os` en el SPE (son frías) para que quepa.

## Estimación para la PS3 (no medida)

- Línea vectorial: 25–40 ciclos por píxel (el prototipo de la 0.4, un relleno vectorial más simple, midió 152 ciclos por grupo de 4
  píxeles): SM64DS (90 000 píxeles por SPE) ≈ 1,2–2 ms; NSMB nivel (65 000) ≈ 1–1,5 ms.
- Decodificación: ≈ 2–3 µs por bloque en lugar de 73 → 0,3–0,6 ms por SPE.
- Aristas y preparación: ≈ 1–1,5 µs por línea de polígono y ≈ 1 µs por polígono·banda: 0,5–1 ms por SPE en estas escenas.
- Fijos: ≈ 1 ms. **Total esperado: 3,5–6 ms por SPE en SM64DS y NSMB (antes 36 y 27)**; la meta era ≤ 8 ms. En el host (Mac,
  mismo código escalar): SM64DS 1,63 → 0,83 ms, MKDS 2,44 → 1,94 ms.
- Si el número de la PS3 queda por encima de 8 ms, el `DATO` ya dice dónde: `píxeles` (la línea), `aristas`, `polígonos`, `texturas`.

## Juntas entre triángulos

**Causa: el recorte del motor de geometría** (`gx.c`, no las reglas de relleno). En Mario Kart DS (carrera, cuadro 3312) salían
1 283 de 3 500 cuadros con píxeles transparentes entre polígonos, hasta 253 por cuadro, en líneas diagonales a lo largo de las
aristas largas que cruzan el borde de la pantalla. Volcando el cuadro (`--frame-dump`) y rasterizando sus polígonos a mano: dos
polígonos que comparten una arista recortada por el plano de abajo terminaban en (254, 191) el uno y (254, 192) el otro. El recorte
(Sutherland-Hodgman) interpolaba el vértice nuevo en el sentido en que **cada polígono** recorría la arista (de `p` a `q`); los dos
polígonos la recorren al revés uno del otro, el redondeo entero da vértices distintos, y además el vértice no quedaba exactamente
sobre el plano (`w − y` = ±1), con lo que el viewport lo mandaba un píxel adentro: entre las dos aristas casi iguales queda una
rendija de un cuarto de píxel que, con el truncamiento de las aristas, deja un píxel sin dibujar por línea. Dos arreglos:

1. el vértice nuevo se interpola **siempre del lado de adentro al de afuera** (el mismo resultado para los dos polígonos);
2. su coordenada del plano (x, y o z) queda **exactamente** en ±w (sin la unidad de redondeo).

Con eso Mario Kart DS pasa de 34 908 píxeles con agujeros en 1 283 cuadros a 19 en 3 (los que quedan son texeles transparentes
de recortes de textura: están cubiertos por un polígono). Super Mario 64 DS no llega a juego en el host sin entrada (solo el título
y los menús, 0 agujeros antes y después); la causa es la misma, y falta confirmarlo en la PS3 con una zona de juego.

**Reglas de relleno de aristas**: la prueba con mallas (`tests/unit/test_r3d_vrow.c`, 1 200 cuadrículas de vértices al azar con
holgura de un cuarto de celda, ortográficas y en perspectiva, 17 millones de píxeles interiores) da **0 píxeles sin cubrir** y
≈ 0,4 % cubiertos dos veces (las aristas «y-mayores» con pendiente negativa incluyen un píxel de más; así lo hace melonDS). No hay
juntas por las reglas de relleno. Contra melonDS (`tools/gfx-compare.py`, escenas nuevas de `gx3d`):

| Escena | Qué es | Antes | Después |
|---|---|---|---|
| 23 | malla de triángulos vecinos dentro de la pantalla | 89 píxeles distintos | 32, ninguno sin dibujar |
| 24 | malla que sobrepasa la pantalla (aristas recortadas, ortográfica) | 61 | 62, ninguno sin dibujar |
| 25 | malla en perspectiva (w distinta por vértice) que sobrepasa la pantalla | **458, 358 sin dibujar en SalsaDS y dibujados en melonDS** | **27, ninguno sin dibujar** |

(la escena 25 con solo el sentido del recorte arreglado daba 546 y 351 sin dibujar: lo que faltaba era dejar el vértice sobre el
plano). Además, midiendo las aristas de exactamente 45° (|dx| = dy) se vio que melonDS las trata como **x-mayores si van hacia la
izquierda al bajar** y como y-mayores hacia la derecha (`r3d_ymajor`): escena 14 de 930 a 647 píxeles distintos, 20 de 544 a 404, 0
de 179 a 171, 13 de 44 a 43, 25 de 34 a 27 (con la regla vieja y el recorte nuevo), sin empeorar ninguna. El resto de las escenas (1 a 22) sigue igual que en 3.3.

## Herramientas y pruebas nuevas

- `tests/unit/test_r3d_vrow.c`: la línea vectorial contra la escalar (392 000 líneas al azar sin textura, todos los modos) y las
  mallas sin juntas; `UNIT_ONLY=r3d` corre solo las pruebas del 3D. Las pruebas unitarias compilan ahora `r3d_core.c` con
  `-DR3D_VERIFY_DECODE`: cada bloque decodificado se compara texel a texel con `r3d_decode` (0 diferencias en las 40 escenas y en
  los tres juegos con `salsads-cpurun`).
- `test_gx.c`: dos polígonos con una arista recortada en sentidos contrarios sacan el mismo vértice (300 casos).
- `tools/gfx-compare.py --mel-cache DIR` guarda y reutiliza las imágenes de melonDS (iterar una regla sin lanzar melonDS);
  `tools/cpurun.sh` acepta `CPURUN_OUT` y `CPURUN_CFLAGS` (variantes de una regla en binarios separados).
- `salsads-cpurun --holes` cuenta los agujeros de la capa 3D por cuadro, `--frame-dump N:prefijo` vuelca el cuadro de geometría
  (`.gx`) y la capa 3D (`.ppm`) de un cuadro; `tools/ppm-png.py` convierte capturas.
- La ROM `gx3d` tiene tres escenas más (23 a 25) y `gx3d.golden` cambió en las escenas 0, 5, 6, 13, 14 y 20 (regla de los 45°; el
  hash de la semilla 7 de `test_render3d.c` también).

## Qué probar en la PS3

Paquete «SalsaDS núcleo» (SLDS00008) de este commit (ruta y SHA-256 en `docs/estado.md`): SM64DS en una zona de juego, NSMB en un
nivel y MKDS en carrera; la línea `DATO raster3d` ahora trae el reparto `polígonos / aristas / píxeles`. Y mirar si siguen viéndose
juntas en SM64DS.


# Fase 3.5 — Eventos del planificador y el motor de geometría en un SPE

Medido en la PS3 con el JIT (SM64DS en juego, `build/ps3-jit/nucleo.log` y `nucleo-2.log`): la emulación de un cuadro costaba
26,6 ms = ARM9 13,3 + ARM7 2,3 + **eventos 10,4** + planificador 0,6; en otras escenas del mismo juego el ARM9 sube a 17,4 y los
eventos bajan a 5,4 (la suma no cambia). Y **la geometría en el PPE 7,0 ms por cuadro** (683 polígonos, 1617 vértices).

## Eventos

Contados por tipo (`salsads-cpurun --stats --gx-clock`, ahora con `eventos por cuadro`; en la PS3, la línea `DATO eventos`), SM64DS con
JIT, por cuadro: **4363 de 4925 eventos eran `EV_GX`**, 263 de línea, 263 de HBlank, 127 del cartucho, 12 de SPI y 11 de temporizadores.

- Por qué el tiempo de «eventos» se movía entre el ARM9 y los eventos sin cambiar la suma: el motor de geometría es perezoso, la
  primera que lo mira lo ejecuta (`gx_advance`): la CPU al escribir/leer la FIFO, o el evento `EV_GX`. Los 7 ms de geometría estaban
  repartidos entre «ARM9» y «eventos» según quién llegara antes (10,4 ms de eventos en un caso y 5,4 en otro); los eventos de verdad
  (planificar, vaciar, mirar la cola) son una parte chica. Por eso no fue el JIT quien «subió» los eventos: con el JIT el ARM9 escribe
  la FIFO más tarde dentro de su tramo y el evento llega primero.
- `EV_GX` miraba la FIFO cada 256 ciclos mientras hubiera IRQ 21 por nivel o una DMA de modo 7 esperando (4375 por cuadro). Casi todas
  las miradas no podían hacer nada: la condición (nivel de la FIFO) solo cambia cuando el motor empieza una orden; si el nivel ya
  cumple, la IRQ se repite a los 1024 ciclos. Ahora el evento va al primer punto de la rejilla de 256 ciclos (anclada en la última
  mirada, como antes) que llega al instante en que la condición puede cambiar (`gx_poll_need` en `src/core/geom.c`) y se recalcula
  al escribir la FIFO. SM64DS 4363 → 1116 por cuadro, NSMB 4362 → 1684, MKDS 2443 → 1006.
- El evento de **HBlank** solo se arma si hay IRQ de HBlank (DISPSTAT bit 4 de cualquier CPU) o una DMA de modo HBlank del ARM9; si se
  activa a mitad de línea se arma en ese momento con el instante que habría tenido (`nds_hblank_refresh`). −263 eventos por cuadro
  (NSMB lo usa: 245).
- Total por cuadro (host, 4000 cuadros con pulsaciones): SM64DS 5039 → **1497**, NSMB 5031 → **2304**, MKDS 3183 → **1437**.
- **Diferencias con el comportamiento anterior**: (1) una mirada vieja podía llegar *antes* de que el motor hubiera empezado la orden
  que bajaba el nivel, si la CPU ya había adelantado el motor; la IRQ 21 salía hasta 256 ciclos antes de que la condición se
  cumpliera, ahora sale en la primera mirada posterior. (2) Cada evento corta el tramo de las CPUs, aunque no haga nada: quitar los
  que no hacían nada cambia dónde caen las fronteras de tramo y por tanto el intercalado fino del ARM9 y el ARM7 (nada que la DS real
  fije: el planificador ya admite cualquier `--quantum`). Por eso el hash de la secuencia de cuadros de geometría de una partida
  cambia (SM64DS, NSMB y MKDS, 4000 cuadros con pulsaciones; con el HBlank eterno NSMB conserva el hash de antes), mientras que
  modo directo y diferido siguen dando el mismo hash entre sí. Las ROMs `irq`, `dma`, `ipc`, `periph`, `gx` y `geom` dan OK con
  todos los tamaños de tramo (1, 16, 2048, 20000) y en LE, BE, con sanitizadores y con el recompilador.
- No hecho: la línea (263 por cuadro) lleva la lógica de cuadro y no es perezosa; los eventos de cartucho y SPI son pocos.

## Motor de geometría en el SPE libre

El motor se parte en dos (`docs/fase3.md` 3.1 describe la geometría; el código está en tres archivos):

| Archivo | Qué es |
|---|---|
| `src/gpu3d/gx.c` | **Motor de tiempo** (PPE): FIFO y tubería, coste en ciclos de cada orden (`gx_cmd_cost`), GXSTAT, el error y el nivel de las pilas (solo punteros, `gx_sp_*`), SWAP_BUFFERS, IRQ 21 y DMA de modo 7 |
| `src/gpu3d/gx_exec.c` | **Ejecución** (PPE o SPE): matrices, vértices, luces, recorte, viewport y la lista de polígonos; el mismo C portable que antes, sin tiempo |
| `src/spu/geom/geom.c` | El SPE: un trabajador de spukit que ejecuta trozos del registro de órdenes con `gx_job_run` |
| `src/gpu3d/gx_loop.c`, `gx_selftest.c` | Ejecutor de prueba del host (mismas ventanas chicas que el SPE) y generador de registros de órdenes al azar (pruebas y autoprueba del SPE) |

- **Por qué se puede partir**: el tiempo (cuándo empieza cada orden y cuánto cuesta) no depende de lo que las órdenes calculan, solo del
  modo de matriz (MULT en el modo 2 cuesta +30), de POLYGON_ATTR (las luces de NORMAL), de los punteros de las pilas (error) y del
  SWAP_BUFFERS. El motor de tiempo los sigue sin ejecutar nada. Todo lo demás es ejecución pura.
- **Modo diferido**: cada orden que *empieza* (misma lógica de antes) se anota en un registro (`cmd, p0..pn-1`, 128 KB de palabras) en
  vez de ejecutarse; al llegar a 2048 palabras se publica un trabajo a spukit sin esperar, así que el SPE ejecuta mientras el PPE
  sigue emulando. Cada trabajo trae el trozo del registro y las direcciones del cuadro de salida y del bloque `gx_exec_out`
  (contadores, resultados de BOX/POS/VEC_TEST y, si se piden, las matrices clip y vectorial). El SPE traduce el registro por tramos de
  2048 palabras, escribe los vértices y polígonos en ventanas del local store (192 y 96) y las sube por DMA al cuadro en memoria
  principal cuando se llenan y al terminar.
- **Qué hace esperar al PPE** (`gx_sync`): leer GXSTAT solo si hay un BOX_TEST sin ejecutar (el bit 1), POS/VEC_TEST sin ejecutar,
  RAM_COUNT con órdenes pendientes, la matriz clip o vectorial (0x640-0x6A0) y el SWAP_BUFFERS en el VBlank (el cuadro tiene que estar
  completo). GXSTAT (lo que más se lee: ocupado, nivel de FIFO, error de pilas), la IRQ y la DMA son del motor de tiempo y no esperan.
  El resto del cuadro el SPE va por delante: al VBlank solo queda el último trabajo.
- **Sin diferencias de bits**: el mismo `gx_exec.c` corre en los dos. Pruebas: `test_gx_defer` (24 secuencias al azar de 4000 pasos con
  lecturas en instantes al azar de GXSTAT, RAM_COUNT, resultados y matrices, cuadros entregados, contadores: motor directo y diferido
  idénticos, 32 599 polígonos), `test_gx_job` (12 registros partidos en dos trabajos con ventanas de 40 vértices y tramos de 80
  palabras contra la ejecución directa), las ROMs `gx` y `geom` con `--gx-defer` (LE y BE) y `salsads-cpurun --gx-defer --gx-hash`
  (hash de cada cuadro de geometría entregado: SM64DS, NSMB y MKDS 4000 cuadros con pulsaciones, idénticos al modo directo y al motor
  de antes de partirlo). En la PS3, autoprueba al arrancar: 3 registros al azar (6000 y 2500 pasos) en el SPE contra el PPE.
- Cambios de datos: `gx_cvtx` guarda `sx, sy` del vértice ya guardado (el cuadro de salida puede estar ya en memoria principal cuando
  una tira reutiliza un vértice); los contadores del cuadro pasan de `gx_frame` a `gx_exec_out` y se copian al cuadro en el
  SWAP_BUFFERS; `gx_t` crece 128 KB (el registro).
- Local store del SPE (`geom`, medido al enlazar): texto 42 296 B (incluye `__divti3` del recorte), bss 34 608 B, pila con 184 256 B
  libres (la cadena más honda usa 4064).
- **Sin medir en la PS3**: el paquete trae la línea `DATO geometria-spe` (trabajos, órdenes y subidas DMA por cuadro, ms del SPE, ms
  que espera el PPE y cuántas esperas) además de `DATO geometria` (el tiempo que el PPE gasta ahora, anotando y publicando) y `DATO
  eventos`. Expectativa: el PPE pasa de ≈ 7 ms a ≈ 1 ms por cuadro (anotar 3-4 palabras por orden y el costo de spukit_post) y el SPE
  tarda alrededor de lo que el PPE tardaba dividido por la velocidad de un SPE en este código (sin vectorizar: enteros de 64 bits).
