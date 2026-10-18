/*
 * SalsaDS — escenas sintéticas y aleatorias para el 2D (Fase 2.1): pruebas del compositor contra la referencia y
 * medición del costo en el SPE (tests/phase0/spe-gpu2d).
 *
 * Todo sale de un generador xorshift32 y de aritmética entera: la misma escena en la Mac (LE), en qemu-ppc64 (BE) y en
 * la PS3.
 */
#ifndef SALSADS_GPU2D_SCENE_H
#define SALSADS_GPU2D_SCENE_H

#include "gpu2d/gpu2d.h"

enum {
	GPU2D_SCENE_TIPICA,    /* 4 fondos de texto de 4 bits, algo de raster, 40 OBJ */
	GPU2D_SCENE_PESADA,    /* raster en cada línea en todos los fondos, 128 OBJ, cambios de paleta por línea */
	GPU2D_SCENE_EXTREMA,   /* peor caso de DMA: la fila de teselas de los 4 fondos cambia en cada línea */
	GPU2D_SCENE_AFIN,      /* dos fondos afines/extendidos rotados, OBJ afines */
	GPU2D_SCENE_EFECTOS,   /* ventanas, mezclas alfa, OBJ semitransparentes y mosaico */
	GPU2D_SCENE_FIJAS,     /* cuántas hay (la siguiente es la aleatoria) */
	GPU2D_SCENE_ALEATORIA = GPU2D_SCENE_FIJAS,
	GPU2D_SCENE_COUNT
};

/* Memoria de un cuadro (los dos motores); todo alineado a 128 bytes. */
#define GPU2D_SCENE_CHG_CAP 8192
typedef struct gpu2d_scene_mem {
	uint8_t *vram;        /* G2D_VRAM_SIZE + G2D_VRAM_ZERO */
	uint8_t *pal[2];      /* 1 KB cada una */
	uint8_t *oam[2];
	gpu2d_change *chg[2]; /* GPU2D_SCENE_CHG_CAP entradas (y 2 de holgura) */
	uint8_t *mmem;        /* GPU2D_H × 512 */
} gpu2d_scene_mem;

/* Llena la memoria y los descriptores (sin `out_ea`) de los dos motores con la escena `scene`; `seed` la varía. En las
 * aleatorias, los 12 bits altos de `seed` (la semilla son los 20 bajos) apagan características como en la ROM gfxrand
 * de tests/roms (ver rand.c): 0x001 OBJ, 0x002 ventanas, 0x004 efectos, 0x008 mosaico de fondos, 0x010 OBJ afines,
 * 0x020 OBJ semitransparentes, 0x040 OBJ de ventana, 0x080 OBJ de mapa de bits, 0x100 fondos afines, extendidos y
 * grandes, 0x200 capa 3D, 0x400 raster (cambios por línea), 0x800 mosaico de OBJ. */
void gpu2d_scene_build(gpu2d_frame *f, const gpu2d_scene_mem *m, int scene, uint32_t seed);

#endif
