# Fase 2.1 — Motores 2D A y B

El compositor de la 0.5 (`tests/phase0/spe-gpu2d`) pasó a ser el renderizador real de las dos pantallas. El sonido es la 2.2
y no entra aquí.

## Arquitectura

- **Núcleo (`src/core/video.c`)**: al empezar el cuadro (línea 0) toma una instantánea de las páginas sucias de la VRAM
  (dos juegos que se alternan), las paletas, la OAM y los 56 registros de media palabra de cada motor, más las tablas de
  páginas de las vistas de fondos y OBJ (respetan el mapa por banco de `mem.c`), las ranuras de paletas extendidas y
  los bancos de la visualización de VRAM. Las escrituras de registros, paleta y OAM durante las líneas 0 a 191 van a un
  **registro de cambios** `{línea, registro, valor}`. En la línea 192 el cuadro se entrega (en la PS3, a los SPE; en el
  host se compone al pedir la imagen). No hay mensajes por línea.
- **Línea efectiva**: un motor dibuja la línea L en su HBlank; una escritura antes del punto 3072 de la línea L se ve en
  L, después, en L + 1 (medido contra melonDS con `gfxprobe`).
- **Compositor (`src/gpu2d/gpu2d_compose.[ch]`, `gpu2d_vec.h`, `gpu2d_fx.h`)**: una pantalla por llamada, 192 líneas,
  en dos etapas (planear la línea y + 1 pidiendo DMA con etiquetas exactas; componer la y). Es el mismo código en el SPE
  (núcleos SIMD) y en el host (núcleos escalares): dan los mismos píxeles. Contexto de 196 224 bytes; programa SPE
  (`src/spu/gpu2d/gpu2d.c`): texto 38 504 B, datos 800 B, bss 197 392 B, holgura de pila ≈ 25 KB (mínimo 4 KB); DMA
  siempre en múltiplos de 16 y sin reservas en el bucle.
- **Referencia (`gpu2d_ref.c`)**: renderizador independiente, píxel por píxel; es la verdad de las pruebas.
- **PS3 (`tests/phase1/nucleo`)**: un SPE por pantalla (spukit), salida directa a las texturas del RSX, un cuadro de
  retraso (se compone el k mientras el PPE emula el k + 1). Si los SPE fallan, cae al compositor del host.

## Qué está hecho

Fondos de texto (4 y 8 bits, tamaños, rotación de pantalla), **fondos afines y extendidos** (de 16 bits, de 256 colores,
mapa de bits, de color directo, con envolvimiento), OBJ normales, afines (con caja doble), de mapa de bits, semitransparentes
y de ventana, **ventanas** (WIN0, WIN1, OBJ, exterior; máquina de estados vertical), **mezcla alfa y brillo**
(BLDCNT/BLDALPHA/BLDY, MASTER_BRIGHT), **mosaico** de fondos y de OBJ, prioridades, paletas extendidas de fondos y OBJ,
modos de pantalla 0 (blanca), 1 (gráficos), 2 (VRAM) y 3 (memoria principal por la FIFO y DMA en modo 4). La capa 3D del
motor A es una capa vacía hasta la Fase 3. La captura de pantalla (DISPCAPCNT) no se emula.

## Pruebas

| Qué | Cómo | Resultado |
|---|---|---|
| Referencia contra compositor | `tests/unit/test_gpu2d.c`: 5 escenas fijas y 300 escenas aleatorias (`GPU2D_SEEDS`), LE, BE (qemu) y con sanitizadores | 0 fallos (en 21 motores los mapas afines no caben juntos en la arena y se omiten: `map_dropped`) |
| ROMs de mano (`tests/roms/src/gfx`, BlocksDS): texto, afín, extendido, mapa de bits, OBJ, OBJ afines, OBJ especiales, ventanas, mezclas, brillo, paletas extendidas, cambios por línea, mosaicos, modos 0/2/3 | `salsads-cpurun --expect --same-ab --vs-ref`: píxeles conocidos (OK/FALLO), A = B, compositor = referencia; `tools/test-host.sh` en LE, SAN y BE | todas OK |
| `gfxrand`, escenas aleatorias de la ROM (semilla en el código de juego) | `--vs-ref` con 12 semillas en `test-host.sh` | compositor = referencia |
| Contra melonDS | `tools/gfx-compare.py` (la ROM dispara DISPCAPCNT y melonDS entrega el banco A por GDB) | ver abajo |
| New Super Mario Bros. en el host | a mano, copias temporales | título y menú «Select a Game» salen completos; no se llegó al nivel (no hay entrada por guion) |

### Contra melonDS 1.1

- ROMs de mano y de mosaico, brillo, mezcla, ventanas, paletas extendidas: **idénticas píxel por píxel** (la lista completa
  está en `tests/roms/Makefile`; las ROMs de modos 2 y 3 se verifican por la lista de píxeles).
- Escenas aleatorias (40 semillas, motor A): 13 se omiten porque leen VRAM sin banco (melonDS espeja esas zonas, ver
  abajo); con OBJ de mosaico y OBJ de mapa de bits apagados, 29 de las 32 comparables son idénticas. Sin apagar nada, 9 de
  27.
- New Super Mario Bros.: la captura de pantalla de melonDS (a ojo, sin guardar imágenes) muestra la misma composición del
  título y del menú que SalsaDS: mismos fondos, texto, castillo, tuberías y colores.

### Divergencias conocidas con melonDS (no reproducidas)

1. **VRAM sin banco o con bancos solapados**: melonDS combina con OR los bancos que se pisan al dibujar y espeja las zonas
   de OBJ sin banco (p. ej. repite 32 KB dentro de 64 KB cuando solo F y G están mapeados). SalsaDS lee ceros en lo que no
   tiene banco y toma el banco de menor índice en los solapes. `crt0` deja VRAMCNT con valores por omisión: las ROMs ponen
   los bancos de forma explícita.
2. **Mosaico de OBJ con varios OBJ que se tocan**: el modelo (vertical por OBJ con bloques de pantalla; horizontal sobre
   la línea de OBJ con un pestillo) coincide en todas las ROMs de una pieza pero no en algunas combinaciones aleatorias.
   En un OBJ normal el primer bloque vertical puede empezar antes del OBJ (fila 255): se lee la VRAM que sigue, como
   melonDS; con vflip se lee la que precede.
3. **OBJ de mapa de bits**: alfa 0 no se dibuja (igual que melonDS); queda un caso aleatorio sin explicar (16 × 32 en 1D,
   alfa 3) donde melonDS pinta el rectángulo de un solo color.
4. **Mezcla con fondos de varias capas**: queda una semilla con 79 píxeles sueltos distintos (objetivo B) y otra con 2
   píxeles de diferencia de canal 8.
5. Aritmética de color en 6 bits (como melonDS) y salida RGBA5551; el modo 3 acepta la FIFO por DMA en modo 4 con 32
   avisos de 4 palabras por línea (aproximación); las escrituras de VRAM durante las líneas visibles no se siguen (la
   instantánea es de la línea 0); un mapa afín que no cabe en la arena de 40 KB no se dibuja (contador `map_dropped`).
6. La instrucción de escritura de 8 bits a la VRAM del ARM9 se ignora (también en melonDS): los datos de las ROMs se
   copian con escrituras de 16 bits.

## PS3 y tiempos

- Host (Apple Silicon, compositor escalar, una pantalla): típica 0,67 ms, pesada 0,61, extrema 0,69, afín 1,59,
  efectos 1,04.
- RPCS3 (sin valor para el rendimiento): «SalsaDS núcleo» con la ROM de carga embebida compone A en 0,52 ms y B en 0,50 ms
  por cuadro; cada 30 cuadros se verifica el SPE contra la referencia (0 diferencias). `spe-gpu2d`: 6 semillas, 0 diferencias.
- Pendiente en la consola real: tiempo de cada SPE por pantalla con New Super Mario Bros. y en las escenas AFIN y EFECTOS
  (en RPCS3 salen 18,5 y 13 ms, sin significado).


# Fase 2.2 — FIFO de geometría, tablas de la BIOS, sonido y velocidad del 2D

## 1. Por qué New Super Mario Bros. se quedaba en 620 883 instrucciones por cuadro

No era el sonido (ni las SWI 1Bh y 1Ch que el log anotaba antes): el ARM9 esperaba, dando vueltas en un bucle de cuatro
instrucciones en la ITCM, a que `GXSTAT` (0x04000600) dijera «FIFO de comandos con menos de la mitad» (bit 25), y después a que
la **IRQ 21** (FIFO de geometría) le avisara de mandar el siguiente trozo de su lista por DMA. El motor de geometría es de la
Fase 3 y la FIFO no existía: `GXSTAT` leía lo que se había escrito (0x80008000) y el bit nunca subía. Se encontró volcando la
ITCM del ARM9 (`--dump` ahora ve las TCM, como `--trace`), desensamblando con el objdump de la imagen de BlocksDS y mirando qué dirección
leía el bucle; la bandera que lo cerraba se vigiló con `--watch9`.

(Superado por la Fase 3.1, `docs/fase3.md`: la FIFO ya interpreta los comandos.) Lo que se hizo (sin interpretar comandos, eso es la Fase 3): GXFIFO (0x04000400) y los puertos de órdenes (0x04000440…) se tragan
lo que reciben; `GXSTAT` dice FIFO vacía (bits 25 y 26) y motor libre; el DMA en modo 7 arranca al armarse (la FIFO siempre tiene
sitio); y la IRQ 21 de los modos «menos de la mitad» y «vacía» se pide **por nivel**, cada 1024 ciclos del ARM9, mientras el modo
esté activo. Al principio se pedía una sola vez tras escribir la FIFO y no bastaba: el manejador de NitroSDK confirma la IRQ al
terminar (`IF = 1 << 21`) y esa confirmación borraba la petición, con lo que la lista se quedaba a medias. melonDS la pide por
nivel (la ROM `gx` lo comprueba; también exige encender el motor de geometría, POWCNT1 bits 2 y 3).

Resultado (host, a 91–94 MIPS): New Super Mario Bros. llega al menú (≈ cuadro 700), a «Select a file» (la ROM de guion toca con
`--press CUADRO:TECLAS[:X,Y]`), a la intro del archivo nuevo (≈ 3300 cuadros), al mapa de mundos y, con D-pad y A, al nivel 1-1
con el reloj corriendo (400 → 377 en 1000 cuadros). Sin 3D no se ven Mario, enemigos ni el mapa de arriba (Fase 3). Mario Kart
DS entra a su carrera de demostración y Super Mario 64 DS al «Touch to start». La imagen de los tres sigue siendo idéntica a la
referencia píxel por píxel (`--vs-ref`).

## 2. SWI 1Ah, 1Bh y 1Ch del ARM7 (tablas)

Se generan con su fórmula, sin copiar ninguna tabla, y se comprobaron contra **las 1556 entradas** que devuelve melonDS (ROM
`tablas`, `tools/gen-bios-tables.py --check`):

| SWI | Tabla | Fórmula |
|---|---|---|
| 1Ah GetSineTable | 64 | `round(sin(i · π / 128) · 32767)` |
| 1Bh GetPitchTable | 768 | `round((2^(i/768) − 1) · 65536)` |
| 1Ch GetVolumeTable | 724 | `min(127, round(K · 10^((i − 723) / 200)))`, K = 2048 (i < 483), 512 (< 603), 256 (< 663), 128 (resto); los últimos elementos de los tres tramos altos (602, 662, 722) valen 126 |

La del volumen se dedujo del comportamiento (cuatro tramos, escalón de 0,1 dB por índice) y las tres excepciones son lo que
devuelve melonDS; no se pudo comparar con una BIOS real. Un índice fuera de rango devuelve 0 (la BIOS real lee lo que haya
detrás de la tabla). Los márgenes de redondeo de las tres fórmulas (distancia al medio entero) son ≥ 3·10⁻⁴: `double` y la libm
de la PS3 dan lo mismo (la ROM da igual en little-endian, big-endian, y con sanitizadores).

## 3. Sonido

### Arquitectura

- `src/core/sound.c`: los registros del ARM7 (0x04000400–0x0400051F). Cada escritura se anota con su tick (ciclos del ARM7 / 1024)
  en un registro de hasta 2048 escrituras por cuadro; las lecturas (el bit 31 de SOUNDxCNT, el bit 7 de SNDCAPxCNT) salen de un
  espejo. Al cerrar el cuadro de video se arma un **trabajo** con las escrituras que ya tienen efecto en los ticks de ese cuadro
  (547 por cuadro) y se manda al mezclador. El resultado (qué canales siguen encendidos, la salida de audio) se recoge al cerrar el
  cuadro siguiente: **un cuadro de latencia, igual en el host y con el SPE**; el estado de ocupado que ve el ARM7 es el del
  último trabajo recogido más los arranques y paros que él mismo hizo después (ver `collect`).
- `src/audio/snd_mix.c`, código portable (host, PPE y SPE): los 16 canales (PCM8, PCM16, IMA-ADPCM, PSG en los canales 8–13 y
  ruido en el 14 y el 15; volumen, divisor, balance, bucles, «una vez»), SOUNDCNT (volumen general, origen de las salidas, canales
  1 y 3 fuera del mezclador), SOUNDBIAS, las dos capturas (PCM16 y PCM8, bucle o una vez, mezclador o canal 1/3) y el remuestreo a
  48 kHz. El estado de los canales (posición, contador del temporizador, valor e índice del ADPCM, LFSR) se arrastra entre
  trabajos en `snd_state` (3,4 KB).
- `src/spu/snd/snd.c`: el trabajador de spukit (un SPE, el tercero): trae el estado y las escrituras por DMA, corre `snd_run` y
  devuelve el estado, las muestras de 16 bits a 32,7 kHz y la salida flotante a 48 kHz. Los datos de los canales se leen por DMA
  de la memoria emulada en líneas de 128 bytes (una caché por canal, invalidada en cada trabajo) y los destinos de la captura se
  escriben leyendo y devolviendo el tramo de 16 bytes que los contiene. Local store: texto 14,4 KB, bss 49,4 KB, pila 190 KB libres.
- `src/platform/ps3/audio_out.c`: el puerto de audio de la PS3 (2 canales, 48 kHz, bloques de 256 muestras, hilo de prioridad 100),
  un anillo de 16 384 muestras (340 ms), antes de empezar a sonar se juntan 2048 (43 ms); si faltan datos se completa con silencio y
  se cuenta; el hilo de la emulación consulta `audio_out_ratio_adj()` antes de cada cuadro (control proporcional de la razón de
  remuestreo, ±2 %, para mantener ≈ 64 ms en el anillo). Si el puerto no se abre, el núcleo sigue sin sonido.
- Autoprueba al arrancar (en RPCS3 y en la PS3): el mismo trabajo en el SPE y en el PPE sobre una memoria de prueba propia, dos
  trabajos seguidos, con las muestras de 16 bits, el estado y lo capturado idénticos y la salida flotante con tolerancia (el
  SPU redondea distinto): RPCS3 da 0 diferencias. Si falla, mezcla el PPE.

### Modelo (verificado contra melonDS 1.1)

`tools/snd-compare.py` corre la ROM `snd` en SalsaDS y en melonDS y compara, caso por caso, lo que captura SNDCAP0/1 del
mezclador (izquierdo y derecho, 128 muestras cada uno). 57 casos: volumen (127, 64, 1, 0), divisor, balance (0, 1, 32, 126, 127),
volumen general, PCM8, varias voces y recorte, rampas a ocho periodos de temporizador (para fijar el arranque), una vez y con
HOLD, PNT, PSG con cinco ciclos de trabajo, ruido a cuatro frecuencias, dos IMA-ADPCM (con y sin valor inicial; bucles con PNT = 1
y 2), valores que revelan el redondeo (volumen 37, divisor 1, balance 77; volumen 120, divisor 1, balance 30), captura en PCM8 y la
lectura de vuelta de SOUNDxCNT y SNDCAPxCNT. **Resultado: iguales muestra a muestra y en las lecturas de vuelta**, con estas
excepciones documentadas (y automáticas en el script):

| Caso | SalsaDS | melonDS 1.1 |
|---|---|---|
| ADPCM con PNT = 0 (el bucle vuelve a la cabecera) | sin repetir la cabecera, con el estado de decodificación del punto de bucle (lo más cercano a GBATEK) | otro estado de decodificación en la segunda vuelta |
| SNDCAPxCNT pidiendo la salida de los canales 1/3 (bit 1) | captura esa salida (sin balance, con volumen y divisor) como dice GBATEK | captura el mezclador |
| Dos canales o capturas que arrancan en ticks distintos | la fase de arranque depende de cuándo cae cada escritura | idem (diferencia de ≤ 2 muestras al principio de unos pocos casos; el script lo acepta) |

Lo que se midió y se implementó así (algunas cosas contradicen la documentación o no figuran en ella):

- El mezclador saca una muestra cada 1024 ciclos del ARM7. El contador de cada canal sube 512 por tick y al llegar a 0x10000 pasa a
  TMR y toma una muestra. Una escritura hecha en el intervalo del tick I se ve desde el tick I + 1 (el arranque de un canal o de una
  captura se engancha en el límite del tick). El canal PCM/ADPCM empieza en la posición −3 (el tercer desbordamiento lee la muestra
  0); el PSG y el ruido, en 0.
- Volumen y balance: izquierda = `(m · v · (128 − p)) >> (14 + d)`, derecha = `(m · v · p) >> (14 + d)`, con v = 128 si VOL = 127,
  p = 128 si PAN = 127 y d ∈ {0, 1, 2, 4}: **un solo desplazamiento al final** (con redondeos intermedios cambian muestras
  sueltas). La suma de los canales se recorta a 16 bits y eso es lo que captura SNDCAP (antes del volumen general).
- Una vez: el canal se apaga al siguiente desbordamiento tras la última muestra y la salida queda en 0 **aunque HOLD esté puesto**
  (GBATEK dice que HOLD la retiene; melonDS no); el bit 31 se apaga. Con bucle, el salto a PNT no pierde ninguna muestra.
- ADPCM: la posición cuenta nibbles; la cabecera ocupa los 8 primeros; al llegar al último se cargan el valor y el índice
  iniciales (solo la primera vez) y la salida **no** cambia hasta decodificar el primer nibble; el estado de decodificación se
  guarda al llegar a PNT · 8 y se restaura al repetir. IMA estándar (89 pasos, ajuste −1 −1 −1 −1 2 4 6 8, tres sumandos).
- PSG: 8 pasos por ciclo; alto en los pasos ≥ 7 − duty (el 7 no sube nunca); ruido: LFSR de 15 bits, inicio 0x7FFF, 0x6000 si el bit
  que sale es 1 (salida −0x7FFF), un paso por desbordamiento.
- Capturas: PCM16 o PCM8 (el byte alto), una vez o en bucle, terminan solas y borran su bit de ocupado.
- Volumen general y SOUNDBIAS **no** se pueden medir con la captura (va antes): se siguieron de GBATEK (127 vale 128; apagado
  = silencio). Ídem el modo «como el original».

### Salida y filtro

Del mezclador salen muestras de 16 bits a 32 728,5 Hz que se convierten a flotante y se remuestrean a 48 kHz con **interpolación
lineal** (`snd_resample`). **Punto de enganche para un FIR polifásico**: esa función es la única que conoce el filtro; el estado
que arrastra entre trabajos está en `snd_state.hist` (con sitio para 32 muestras por canal) y `frac`; hay que sustituir su cuerpo
y, en el SPE, comprobar la holgura de la pila. La opción `SND_MODE_ORIGINAL` («como el original»: `audio.cfg` con
`original = si`, o `--snd-original` en el host) cuantiza a 10 bits con el sesgo (`(mezcla >> 6) + SOUNDBIAS` recortado a 0–1023)
antes de convertir. El host escribe un WAV de 48 kHz con `--wav ARCHIVO` (para oír o analizar).

### Qué hay en los juegos

New Super Mario Bros. (host): audio continuo desde el menú (≈ 1100–3300 cruces por cero por segundo; llega a 0 dBFS en un instante, 73 muestras
recortadas en 25 s), mezcla de 0,27–0,57 ms por cuadro en el SPE (RPCS3: no mide rendimiento). Mario Kart DS y Super Mario 64 DS también
producen audio. La música y los efectos no se han escuchado: solo se ve que existen y que no hay recortes sostenidos.

## 4. Por qué el 2D tardaba 28 ms en la pantalla A

La instrumentación nueva (con `-DG2D_PROF` el SPE mide con el decrementador cada etapa: `init`, `plan` con su desglose en texto,
afines, OBJ y esperas de DMA, `compone` con espera, capas, color final y salida, y la espera final; el programa de la PS3 las
escribe en la línea `DATO spe-etapas`) y los contadores de DMA del host (`--g2d-stats`: órdenes, bytes, listas y esperas por etapa)
mostraron, con las escenas de New Super Mario Bros.:

- **No es el DMA**: por cuadro, las dos pantallas juntas piden unas 1900 órdenes `get` (1850 de ellas de filas de OBJ), 430
  listas con 5200 elementos, 384 `put` y 820 esperas. Ni a 5 µs cada una darían 28 ms.
- **Es el código escalar del SPE**, que no tiene unidad escalar, no predice saltos (cada salto tomado cuesta ≈ 18 ciclos), hace la
  multiplicación de 64 bits y la división de 32 por software (cientos de ciclos) y accede a un `uint16_t` suelto con carga,
  rotación y máscara:
  1. `plan_affine` (fondos afines y extendidos; en el menú de NSMB, dos fondos de teselas de 16 bits en el motor A, 384 líneas por
     cuadro): por **píxel** hacía `i % mh` (tres veces), dos productos de 64 bits `ref + pa · xs`, un `switch` por tipo y una
     lectura del mapa. ≈ 625 ciclos por píxel × 98 000 píxeles ≈ 20 ms [est]: la diferencia entre 28,9 y 8,9 ms de las dos pantallas.
  2. El **color final** de cada línea (prioridades, mezcla alfa, brillo, conversión a RGBA5551) era un bucle escalar de un píxel
     por vuelta con una docena de decisiones por píxel (cada una un salto), la mezcla en bucles de tres canales, el vector de
     entrada volcado a memoria para leer cada píxel... ≈ 500 ciclos por píxel [est] × 49 000 píxeles ≈ 8 ms, **en las dos
     pantallas** (las dos tienen efectos en las 192 líneas): eso eran los 8,9 ms de la pantalla B, que casi no tiene fondos afines.
  3. `draw_affine` (un bucle escalar con saltos) y los mosaicos (un `%` por píxel).
- RPCS3 no ve la causa: su traductor ejecuta el código escalar a la velocidad del nativo del Mac y no modela los saltos ni las
  rutinas de software. Por eso el viejo daba 14,8 ms (A) y 1,3 ms (B) en RPCS3 contra 28,9 y 8,9 en la PS3. Sí sirve como
  **contador de instrucciones** (su tiempo es casi proporcional): lo usamos para repartir el cómputo por etapa y para comparar
  antes y después; el tiempo de la PS3 es el cómputo ÷ ≈ 3 más las esperas de DMA (la escena «típica» de la 0.5 da 1,9 ms en RPCS3 y 2,19 en la PS3: ahí manda el DMA).

### Qué se cambió (src/gpu2d)

- `plan_affine`: sin divisiones ni productos de 64 bits (la posición avanza sumando PA y PC con enteros de 32 bits, los lados son
  potencias de 2); sin mosaico, dos pasadas (direcciones sin ramas y deduplicado de los bloques de 16 bytes); y una vía rápida
  para el caso de PA = 1,0 y PC = 0 (un fondo que solo se desplaza, lo que usa NSMB): una lectura del mapa y un bloque por cada 8
  píxeles en lugar de una por píxel.
- `draw_affine`: ocho píxeles por vuelta (lecturas por carril y selecciones, sin ramas).
- El **color final** de ocho píxeles a la vez, sin saltos por píxel: la lista de capas en orden de pintado se arma una vez por
  línea, las paletas se leen por carril, y la mezcla alfa, el brillo y el brillo maestro son aritmética de 16 bits por carril
  (`g2d_v8_mul`, desplazamientos por carril); la parte cara de los efectos solo corre si algún carril la usa.
- Mosaicos horizontales sin divisiones.
- Instrumentación (`gpu2d_prof`, `G2D_STAGE`, contadores de DMA del host, estadísticas por motor en `--g2d-stats`).

### Resultados

Escenas de New Super Mario Bros. (menú «Select a Game»), pantalla A / B, ms por cuadro:

| | PS3 real (2.1, medido) | RPCS3 con el compositor de la 2.1 | RPCS3 con el de la 2.2 | PS3 con el de la 2.2 [est] |
|---|---|---|---|---|
| A | 28,1–28,9 | 14,8 | **4,7** (plan 1,4: afines 0,9; compone 3,4: capas 1,6, color final 1,8) | ≈ 2,5–4 |
| B | 7,8–9,1 | 1,3 | **1,2** (plan 0,24; compone 0,97) | ≈ 1,5–2,5 |

(La estimación de la PS3 es el cómputo de RPCS3 ÷ 3 más ≈ 2 ms de esperas de DMA; hay que medirla: el paquete lo escribe en
`DATO spe-etapas`, con las esperas de DMA de cada etapa separadas.) Escenas sintéticas de la 0.5 en RPCS3 (A, antes → ahora): típica 1,9 → 2,1 ms (las ramas de «¿algún carril usa los efectos?» cuestan unas décimas donde no se usan), pesada 2,0 → 2,1, extrema 2,7 → 2,8, afín (fondos y OBJ girando, 40 000 bloques por cuadro) 18,5 → 8,6 y efectos 13 → 11,9; en «efectos» lo que queda son los mosaicos de OBJ y de fondo y las ventanas (9 ms en «capas»), escalares, que New Super Mario Bros. no usa. La imagen sigue siendo igual a la referencia en las 300 escenas aleatorias del test
unitario, las ROMs de mano, 6 semillas en RPCS3 (SPE contra referencia: 0 diferencias) y los cuadros de NSMB, Mario Kart DS y
Super Mario 64 DS (`--vs-ref`).

## 5. Qué medir en la PS3

Paquete: `build/salsads-nucleo-fase22.pkg` (copia de `build/salsads-nucleo/salsads-nucleo.pkg`, hecha desde el commit `c4a738b`),
SHA-256 `cce7721a908a362ee3aed51d673348361a34bb10a5fd9a33b20f67de0e37a601`. En RPCS3: `RESULTADO nucleo: OK`, autoprueba del audio
(SPE contra PPE) sin diferencias.

Paquete «SalsaDS núcleo» (SLDS00008), con NSMB como `/dev_hdd0/salsads/rom.nds` (respaldo propio ya desencriptado): en el log
(`nucleo.log`) cada 120 cuadros, `DATO spe` (tiempo de cada pantalla), `DATO spe-etapas` (ms por pantalla y cuadro en cada etapa;
`plan (espera)` y `compone (espera)` son los bloqueos de DMA) y `DATO audio` (tiempo del SPE de audio, anillo, faltas, pico). Con
audio real: si hay faltas con la emulación más lenta que 60 cuadros por segundo es lo esperado (no se estira); `audio.cfg` con
`original = si` activa el modo de 10 bits.

## Qué falta

- 3D (Fase 3): sin él no se ve Mario ni el mapa de arriba. El `GXSTAT` de «siempre vacía» tiene que dejar su lugar a una FIFO de
  verdad con su profundidad (256 órdenes), el motor ocupado y las IRQ por flanco/nivel con sus tiempos.
- Sonido: filtro FIR polifásico (el punto está marcado), volumen general y sesgo contra una DS real, HOLD, los cuadros de latencia
  del audio con juegos que lean su estado de vuelta a cada cuadro, escuchar los juegos.
- 2D: medir en la PS3; mosaicos y efectos por píxel escalares; las esperas de DMA por línea (la escena típica de la 0.5 ya estaba
  limitada por ellas: ≈ 11 µs por línea).

Documentos relacionados: `docs/memory-budget.md`, `docs/perf-log.md`, `docs/estado.md`.
