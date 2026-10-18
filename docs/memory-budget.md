# Presupuesto de memoria

PS3: 256 MB XDR (principal) + 256 MB GDDR3 (RSX). La aplicación recibe unos
209–213 MB de XDR; los segmentos del ELF (código, datos y la cola NOBITS de
una caché de código) se descuentan de ahí (medido en Salsa64 con HEN:
214 016 KB de memoria de usuario; ver `docs/cell-desde-salsa64.md` §2.2 y §5).

Cifras en MB. **Previsto** = estimación de diseño (todavía no hay núcleo);
**Medido** se llena en la PS3 real. Se actualiza en cada fase.

## Memoria principal (XDR)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| RAM principal de la DS | 4 | | Una copia, little-endian |
| WRAM compartida + WRAM del ARM7 + ITCM/DTCM | 0,2 | | 32 KB + 64 KB + 32 KB + 16 KB |
| VRAM (9 bancos A–I) | 0,7 | | ~656 KB |
| Intérpretes de CPU (`src/cpu`): 2 × `struct acore` (520 B) + tablas de despacho (80 KB) + memoria de ITCM y DTCM (48 KB) | 0,15 | | Fase 1.1; código ≈ 68 KB en el segmento de texto |
| **Máquina de la DS (`struct nds`, Fase 1.2 y 1.3)**: RAM principal 4 MB + VRAM 656 KB + WRAM 96 KB + firmware generado 256 KB + paleta/OAM 4 KB + BIOS propia 20 KB + registros, FIFO y tablas de la ruta rápida (16 KB) + memoria de guardado del cartucho 1 MB (1.3) | 6,12 | 6,12 (`sizeof struct nds` = 6 418 032 B, del log del programa; eran 5 369 344 B en la 1.2) | Ya incluye los 4 MB de RAM principal, los 0,7 MB de VRAM y los 0,2 MB de WRAM de las filas de arriba (no sumarlas dos veces) |
| **Motor de geometría (`gx_t` dentro de `struct nds`, Fase 3.1)**: dos cuadros de salida de 295 168 B (6144 vértices de 32 B y 2048 polígonos de 48 B), cola de 512 entradas, pilas de matrices y estado | 0,6 | 0,58 (`sizeof(gx_t)` = 604 928 B; `sizeof struct nds` pasa de 8 749 184 a 9 354 112 B) | Ya está dentro de la fila de `struct nds`; los cuadros se copian por DMA a los SPE de la 3.2 |
| **Render 3D (Fase 3.2, dentro de `struct nds`)**: capa 3D por juego de instantáneas 2 × 192 KB (256 × 192 × 4 B), contexto del rasterizador para el host/PPE 186 KB, imagen auxiliar de comparación 192 KB | 0,75 | | La autoprueba de la PS3 suma 1,6 MB estáticos (un `gx_t`, una VRAM de prueba y dos imágenes) |
| Tablas de páginas de punteros (sin fastmem) | 0,5 | | Por regiones; VRAM por bancos de 16 KB o más |
| Cartucho (ROM) | ≤ 64 entera, o 32 de caché | | **Fase 48:** si la ROM pasa de 64 MB (`ROM_WHOLE_MAX_MB`) el archivo queda abierto y `src/core/cart.c` lee páginas de 256 KB (`ROM_PAGE_KB`) bajo demanda con una caché LRU de 32 MB (`ROM_CACHE_MB`, 128 páginas) más tablas de ≈ 0,1 MB; lectura adelantada (síncrona) de la página siguiente en acceso secuencial; cabecera y binarios ARM9/ARM7 se leen al inicio sin caché. El archivo `/dev_hdd0/salsads/rom-paged` fuerza fragmentos con cualquier ROM (pruebas). En el host: `--rom-paged PAGE_KB,CACHE_KB`, `--rom-whole-max MB`. Medido en el host con NSMB, SM64DS y MKDS, 1800 cuadros: 0,008–0,017 fallos por cuadro (15–30 páginas en total). Antes: Hasta 512 MB de ROM física; tope práctico por decidir. Fuera del heap de newlib, en páginas de 1 MB (`docs/cell-desde-salsa64.md` §5) |
| Caché del recompilador ARM → PPC64 | 32 | | Techo útil: `bl` alcanza ±32 MB (Fase 4). La prueba de la Fase 0.7 usa 4 MB (cola NOBITS del segmento de texto: se descuenta de la memoria de usuario) |
| Estado del recompilador | 8 | ≈ 12 (Fase 4) | Tablas de páginas de lectura y escritura (4 MB), páginas reales, físicas y su lista inversa (4 MB), 16 384 bloques (≈ 3,9 MB), 65 536 enlaces, destinos indirectos (256 KB), mapa de líneas de 64 B (66 KB); la caché de código es la de 4 MB de la cola del texto |
| Búfer de comandos RSX + memoria mapeada | 16 | | `rsxkit_init` |
| Registro de escrituras de video por línea (cuadro) | 1 | | Para los SPEs (2D/3D); doble búfer |
| Texturas decodificadas (3D) | 8 | | |
| Búferes de DMA y anillos de los SPEs | 4 | | |
| Audio | 1 | 0,3 (2 trabajos de 43 KB + anillo de 128 KB; `sizeof struct nds` pasó de 8 623 872 a 8 749 184 B con el estado y las escrituras del sonido) | Fase 2.2: anillo de 16 384 muestras estéreo flotantes (340 ms) en `audio_out.c` |
| Estados guardados | 10 | | Foto en segundo plano |
| Código, pilas, heap de newlib | 16 | | |
| **Total previsto** | **≈ 230** | | Con una ROM de 128 MB no cabe junto con todo lo demás en ~210 MB: decidir el tope de ROM o leerla por demanda |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers de pantalla + profundidad | 24 | | A 1080p (`docs/cell-desde-salsa64.md` §4.1) |
| Dos pantallas de la DS (texturas) | 1 | | 2 × 256×192×4 y sus copias. En la Fase 0.6 viven en memoria principal mapeada (RGBA5551, 2 juegos × 2 × 96 KB = 384 KB), no en GDDR3 |
| Destinos del 3D a escala interna 2×–4× | por decidir | | Se acota con el tope total de destinos (Salsa64 usa 112 MiB) |
| Posprocesado / filtros | por decidir | | |

Los dos juegos de instantáneas de video (`struct nds_video`: VRAM sucia, paletas, OAM, registro de cambios de 8192 entradas por motor, FIFO de memoria principal) ocupan ≈ 1,8 MB de la RAM principal del PPE.

## Local store de los SPEs

Cada programa SPE documenta su presupuesto en su cabecera (≤ 256 KB: código,
datos, búferes de DMA y pila; la holgura de la pila se mide al enlazar con
`tools/spu-stack.py`). Se resume aquí cuando existan.

| Programa | Código | Datos+búferes | Pila | Notas |
|---|---|---|---|---|
| `spu_gpu2d` (composición 2D de 2 pantallas, Fase 0.5) | 11,8 KB | 187 KB (contexto 186 KB: OBJ 2 × 128 × 512 B = 128 KB, teselas 17 KB, cambios 24 KB, resto 12 KB) | holgura 60 976 B | medido al enlazar (`spu-size`, `tools/spu-stack.py`); ≈ 199,7 KB |
| `gpu2d` (compositor de una pantalla, Fase 2.1; `src/spu/gpu2d`) | 38,5 KB | 198,2 KB (contexto 196 224 B: paleta unificada 42 KB, mapas afines 40 KB, OBJ 2 × 16 KB, teselas 4 × 2 × 2 KB, cambios 2 × 4 KB, el resto líneas y trabajos) | holgura ≈ 25 KB (mínimo 4 KB) | un SPE por pantalla; medido al enlazar |
| `snd` (mezclador de sonido, Fase 2.2; `src/spu/snd`) | 14,4 KB | 50,2 KB (estado 3,4 KB, escrituras 32 KB, muestras de 16 bits 2,5 KB, salida flotante 8 KB, escalón de la captura) | holgura 190 KB (mínimo 4 KB) | un SPE; medido al enlazar |
| `raster3d` (render 3D, Fase 3.2 y 3.4; `src/spu/raster3d`) | texto 56 032 B | bss 186 400 B (contexto 182 656 B: color 2 × 18 KB, profundidad 18 KB, atributos 18 KB, caché de texturas 64 KB + líneas de VRAM 5 KB + paleta convertida 1 KB, orden 16 KB, lista 4 KB, lotes 23 KB) | 18 768 B libres; usa 6 720 B (holgura 12 048 B, mínimo 8 192 B) | dos SPE, bandas alternas; medido al enlazar (3.4: línea vectorial, texto +9 KB, caché de líneas de VRAM de 80 a 40 líneas) |
| `spe-raster3d` (0.4) | 27,3 KB | 202,9 KB | 25,6 KB libres; usa 2,5 KB (holgura 23,1 KB, mínimo 8 KB) | Caché de texturas 112 KB, color 2 × 16 KB, profundidad 16 KB, cajas 16 KB, registros 12 KB; detalle en `tests/phase0/spe-raster3d/spu_main.c` |
