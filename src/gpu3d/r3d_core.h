/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — contexto del rasterizador por bandas (r3d_core.c, Fase 3.2). Está a la vista para que el núcleo del host
 * (struct nds) y el programa SPE lo reserven sin memoria dinámica; nadie fuera de r3d_core.c toca sus campos.
 */
#ifndef SALSADS_GPU3D_R3D_CORE_H
#define SALSADS_GPU3D_R3D_CORE_H

#include <stddef.h>

#include "gpu3d/r3d.h"
#include "gpu3d/r3d_io.h"
#include "gpu3d/r3d_px.h"

#define R3D_BAND_MAX 16
#define R3D_ROWS (R3D_BAND_MAX + 2)
#define R3D_PBATCH 16
#define R3D_SORT_CHUNK 64
#define R3D_TC_WAYS 4
#define R3D_TC_SETS 16
#define R3D_TC_BLOCKS (R3D_TC_WAYS * R3D_TC_SETS)
#define R3D_LC_LINES 16  /* líneas de 128 bytes de la memoria de texturas */
#define R3D_LP_LINES 8   /* ídem, de la de paletas */
/* Caché de celdas (3.9): celdas de 4×4 texeles ya decodificados, y líneas grandes de la memoria de texturas y de paletas. */
#define R3D_CELL_WAYS 4
#define R3D_CELL_SETS_LOG2 7
#define R3D_CELL_SETS (1 << R3D_CELL_SETS_LOG2)
#define R3D_CELLS (R3D_CELL_WAYS * R3D_CELL_SETS) /* 512 celdas de 64 B = 32 KB */
#define R3D_CL_SHIFT 10                           /* líneas de 1 KB */
#define R3D_CL_BYTES (1 << R3D_CL_SHIFT)
#define R3D_CL_DLINES 16                          /* de la memoria de texturas: 4 conjuntos de 4 vías */
#define R3D_CL_PLINES 4                           /* de la de paletas: 1 conjunto de 4 vías */
#define R3D_CL_WAYS 4
#define R3D_TID_MAX 256                           /* texturas distintas por cuadro antes de vaciar la caché de celdas */

/* etiquetas de DMA (31 es de spukit) */
enum { R3D_TAG_HDR = 0, R3D_TAG_SORT = 1, R3D_TAG_POLY = 3, R3D_TAG_VTX = 5, R3D_TAG_OUT = 7, R3D_TAG_LINE = 9 };

#define R3D_TAIL_OFF offsetof(gx_frame, npolys)
#define R3D_TAIL_SIZE ((sizeof(gx_frame) - R3D_TAIL_OFF + 15u) & ~(size_t)15u)

struct r3d_ctx {
	uint32_t col[2][R3D_ROWS][R3D_W] __attribute__((aligned(128)));
	uint32_t dep[R3D_ROWS][R3D_W] __attribute__((aligned(128)));
	uint32_t att[R3D_ROWS][R3D_W] __attribute__((aligned(128)));
	uint32_t order[GX_MAX_POLYS] __attribute__((aligned(16))); /* clave << 11 | índice, ordenado */
	int16_t ylo[GX_MAX_POLYS], yhi[GX_MAX_POLYS];              /* por índice de polígono */
	uint16_t list[GX_MAX_POLYS] __attribute__((aligned(16)));
	gx_poly pb[2][R3D_PBATCH] __attribute__((aligned(128)));
	gx_vertex vb[2][R3D_PBATCH * GX_POLY_MAXV] __attribute__((aligned(128)));
	r3d_lel pl[2][R3D_PBATCH] __attribute__((aligned(16)));
	r3d_lel vl[2][R3D_PBATCH * GX_POLY_MAXV] __attribute__((aligned(16)));
	uint8_t tail[R3D_TAIL_SIZE] __attribute__((aligned(128)));
	/* Memoria de la caché de texturas: dos modos que no coexisten (R3D_F_TEXBLOCK elige el de bloques), así que comparten espacio. */
	union {
		struct { /* bloques de 256 texeles decodificados enteros (3.2 a 3.8) y líneas de VRAM de 128 B */
			uint32_t tcache[R3D_TC_BLOCKS][256] __attribute__((aligned(128)));
			uint8_t lines[R3D_LC_LINES + R3D_LP_LINES][128] __attribute__((aligned(128)));
			uint64_t ttag[R3D_TC_BLOCKS];
			uint32_t tstamp[R3D_TC_BLOCKS];
			uint32_t ltag[R3D_LC_LINES + R3D_LP_LINES];
			uint8_t lru[R3D_LC_LINES / 2];   /* por conjunto: 1 = la vía 1 fue la última usada */
			uint32_t pxtab[256];             /* paleta ya convertida (r3d_555 sin alfa) */
			uint8_t stage[512];              /* bytes de VRAM traídos */
		};
		struct { /* celdas de 4×4 y líneas de 1 KB (3.9) */
			uint32_t cell[R3D_CELLS][16] __attribute__((aligned(128)));
			uint8_t dline[R3D_CL_DLINES][R3D_CL_BYTES] __attribute__((aligned(128)));
			uint8_t pline[R3D_CL_PLINES][R3D_CL_BYTES] __attribute__((aligned(128)));
			uint32_t ctag[R3D_CELLS] __attribute__((aligned(16))); /* textura << 16 | fila de celdas << 8 | columna de celdas; ~0 = vacía */
			uint32_t cstamp[R3D_CELLS];
			uint32_t dltag[R3D_CL_DLINES], dlstamp[R3D_CL_DLINES];
			uint32_t pltag[R3D_CL_PLINES], plstamp[R3D_CL_PLINES];
			uint64_t tidbase[R3D_TID_MAX];   /* identidad (cur_base) de cada textura del cuadro */
			uint16_t tidhash[512];           /* tabla de dispersión: índice en tidbase + 1; 0 = libre */
		};
	} __attribute__((aligned(128)));
	uint64_t last_tag;
	const uint32_t *last_blk;
	uint64_t last_base;  /* identidad de la textura del último bloque usado (sin el bloque) */
	uint64_t cur_base;   /* ídem, de la textura del polígono que se dibuja */
	uint32_t last_id;    /* bloque (by << 4 | bx) del último usado; ~0 = ninguno */
	uint32_t clock;
	uint32_t btick[R3D_BT_WORDS] __attribute__((aligned(16))); /* ticks de cada banda de esta unidad (r3d_job.bt_ea) */
	uint32_t pal_addr, pal_n; /* de qué paleta es pxtab y cuántas entradas tiene (0 = ninguna) */
	/* celdas (3.9) */
	int cellmode;             /* 1 = caché de celdas; 0 = bloques */
	uint32_t ntid, tid;       /* texturas vistas en el cuadro y la del polígono que se dibuja */
	uint32_t last_cid;        /* celda (fila << 8 | columna) del último texel pedido; ~0 = ninguna */
	const uint32_t *last_cell;
	const uint8_t *dl_ptr, *pl_ptr; /* última línea usada de cada memoria: su dirección de línea y puntero */
	uint32_t dl_key, pl_key;
	/* trabajo */
	r3d_job job;
	r3d_rstate rs;
	r3d_stats *st;
};


/* La línea de un polígono sin textura con el código vectorial del SPE (r3d_vrow.h); no necesita contexto. col, dep y att
 * son de 256 palabras alineadas a 16. Para las pruebas contra r3d_draw_row y la sonda de la autoprueba. */
void r3d_core_row_notex(const r3d_rstate *rs, r3d_stats *st, const r3d_poly *p, const r3d_row *row, uint32_t *col, uint32_t *dep,
			uint32_t *att);

#endif
