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
| Tablas de páginas de punteros (sin fastmem) | 0,5 | | Por regiones; VRAM por bancos de 16 KB o más |
| Cartucho (ROM) | 128 | | Hasta 512 MB de ROM física; tope práctico por decidir. Fuera del heap de newlib, en páginas de 1 MB (`docs/cell-desde-salsa64.md` §5) |
| Caché del recompilador ARM → PPC64 | 32 | | Techo útil: `bl` alcanza ±32 MB (Fase 4) |
| Estado del recompilador | 8 | | Mapa de bloques, banderas de código, enlaces |
| Búfer de comandos RSX + memoria mapeada | 16 | | `rsxkit_init` |
| Registro de escrituras de video por línea (cuadro) | 1 | | Para los SPEs (2D/3D); doble búfer |
| Texturas decodificadas (3D) | 8 | | |
| Búferes de DMA y anillos de los SPEs | 4 | | |
| Audio | 1 | | |
| Estados guardados | 10 | | Foto en segundo plano |
| Código, pilas, heap de newlib | 16 | | |
| **Total previsto** | **≈ 230** | | Con una ROM de 128 MB no cabe junto con todo lo demás en ~210 MB: decidir el tope de ROM o leerla por demanda |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers de pantalla + profundidad | 24 | | A 1080p (`docs/cell-desde-salsa64.md` §4.1) |
| Dos pantallas de la DS (texturas) | 1 | | 2 × 256×192×4 y sus copias |
| Destinos del 3D a escala interna 2×–4× | por decidir | | Se acota con el tope total de destinos (Salsa64 usa 112 MiB) |
| Posprocesado / filtros | por decidir | | |

## Local store de los SPEs

Cada programa SPE documenta su presupuesto en su cabecera (≤ 256 KB: código,
datos, búferes de DMA y pila; la holgura de la pila se mide al enlazar con
`tools/spu-stack.py`). Se resume aquí cuando existan.

| Programa | Código | Datos+búferes | Pila | Notas |
|---|---|---|---|---|
| (ninguno todavía) | | | | Fases 0.4 y 0.5 |
