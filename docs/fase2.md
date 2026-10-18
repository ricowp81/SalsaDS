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

Documentos relacionados: `docs/memory-budget.md`, `docs/perf-log.md`, `docs/estado.md`.
