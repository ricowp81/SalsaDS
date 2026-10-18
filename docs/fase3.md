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
| Vértice | VTX_16: 1.3.12; VTX_10: 1.3.6 (`<< 6`); VTX_DIFF: 1.0.9 (`<< 3`) | XY/XZ/YZ conservan la coordenada que falta |
| Espacio de recorte | `(x, y, z, 1) × clip`, cada componente en 64 bits y `>> 12` | |
| Textura | 1.11.4 | modos de TEXIMAGE_PARAM bits 30-31: 0 sin transformar; 1 `(s, t, 1, 1) × T`; 2 normal × T + (s, t); 3 vértice × T + (s, t) |
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
(orden de los planos, redondeo de la interpolación), el modo de textura 1-3, los costes por polígono, cara de área cero, el
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
