/*
 * SalsaDS — compositor 2D por líneas (Fase 0.5).
 *
 * Una pantalla, 192 líneas, leyendo la VRAM de la memoria principal por DMA
 * y aplicando el registro de cambios por línea. Es el código del SPE; en el
 * host (y en el PPE) compila con kernels escalares para probar la
 * estructura. Presupuesto del local store del contexto: ver
 * sizeof(gpu2d_ctx) (el programa SPE lo imprime al arrancar).
 *
 * Tubería de dos etapas: mientras se compone la línea y, ya se pidió por
 * DMA lo que necesita la y + 1 (los registros de toda la pantalla están en
 * el registro de cambios, así que se sabe de antemano). Las etiquetas de
 * DMA son exactas, para no esperar de más:
 *   0..7   fondo b (b·2 + ranura): mapa y teselas de la fila de teselas
 *   8, 9   OBJ de las líneas pares / impares
 *   10, 11 salida de las líneas pares / impares
 *   12     arranque (OAM, paleta, cambios)
 * Caché de teselas: un fondo solo vuelve a traer su fila de teselas
 * (mapa + 33 teselas) cuando cambian la fila, el desplazamiento grueso, la
 * base de teselas, la base del mapa, el tamaño o la profundidad de color;
 * en un fondo sin efectos de raster, una vez cada 8 líneas.
 */
#ifndef SALSADS_GPU2D_COMPOSE_H
#define SALSADS_GPU2D_COMPOSE_H

#include "gpu2d/gpu2d.h"
#include "gpu2d/gpu2d_vec.h"

#define G2D_TILES      33      /* teselas por línea de un fondo (32 + la parcial) */
#define G2D_TILEBUF    2176    /* 33 teselas de 64 bytes, redondeado a 128 */
#define G2D_OBJ_ROW    512     /* fila de teselas de un OBJ de 64 px de 8 bits */
#define G2D_CHG_LS_CAP 3072    /* cambios por pantalla que caben en el local store */
#define G2D_LINE_VECS  48      /* 8 de margen a la izquierda, 32, 8 a la derecha */

typedef struct g2d_obj {
	int16_t x;
	uint8_t y, h, wt, flags, prio, bank;
	uint16_t tile;
	uint16_t pad[3];
} g2d_obj;
#define G2D_OBJ_HFLIP 1
#define G2D_OBJ_VFLIP 2
#define G2D_OBJ_C256  4
#define G2D_OBJ_VALID 8

typedef struct g2d_vis {
	uint8_t s, py;
} g2d_vis;

typedef struct g2d_job {
	uint32_t dispcnt;
	uint32_t waitmask;
	uint16_t bgcnt[GPU2D_NBG];
	uint8_t blank;
	uint8_t bg_on[GPU2D_NBG], bg_slot[GPU2D_NBG], bg_off[GPU2D_NBG], bg_py[GPU2D_NBG];
	uint8_t bg_tx0[GPU2D_NBG], bg_wide[GPU2D_NBG], bg_c256[GPU2D_NBG];
	uint8_t nvis[4];
	g2d_vis vis[4][GPU2D_NOBJ];
} g2d_job;

/* Contadores de una pantalla (diagnóstico). */
typedef struct gpu2d_stats {
	uint32_t bg_fetches;    /* filas de teselas traídas */
	uint32_t map_waits;     /* esperas intermedias por el mapa */
	uint32_t obj_fetches;   /* filas de OBJ traídas */
	uint32_t obj_drawn;     /* OBJ-línea dibujados */
	uint32_t chg_applied;
} gpu2d_stats;

typedef struct gpu2d_ctx {
	uint8_t objbuf[2][GPU2D_NOBJ][G2D_OBJ_ROW] __attribute__((aligned(128)));
	uint8_t tiles[GPU2D_NBG][2][G2D_TILEBUF] __attribute__((aligned(128)));
	uint8_t maps[GPU2D_NBG][2][128] __attribute__((aligned(128)));
	gpu2d_change chg[G2D_CHG_LS_CAP] __attribute__((aligned(128)));
	uint8_t oamraw[1024] __attribute__((aligned(128)));
	uint8_t palraw[1024] __attribute__((aligned(128)));
	uint16_t outbuf[2][GPU2D_W] __attribute__((aligned(128)));
	uint16_t pal[512] __attribute__((aligned(128)));
	g2d_v8 line[G2D_LINE_VECS] __attribute__((aligned(16)));
	g2d_v8 tmp[G2D_TILES + 1] __attribute__((aligned(16)));
	g2d_obj obj[GPU2D_NOBJ] __attribute__((aligned(16)));
	g2d_el lists[GPU2D_NBG][2][G2D_TILES] __attribute__((aligned(16)));
	uint32_t offs[G2D_TILES];
	g2d_job job[2];
	/* estado del planificador */
	uint32_t pr_dispcnt;
	uint16_t pr_bgcnt[GPU2D_NBG], pr_hofs[GPU2D_NBG], pr_vofs[GPU2D_NBG];
	uint32_t plan_cursor, comp_cursor;
	uint32_t bg_k0[GPU2D_NBG][2], bg_cbase[GPU2D_NBG][2], bg_sbase[GPU2D_NBG][2];
	uint8_t bg_cur[GPU2D_NBG];
	gpu2d_stats stats;
} gpu2d_ctx;

/* Compone una pantalla en e->out_ea (192 líneas de 512 bytes). Devuelve 0 o
 * un código negativo: -1 descriptor inválido (tamaños que no son potencia
 * de 2, regiones de VRAM que cruzan una frontera de 4 GB), -2 más cambios
 * de los que caben. */
int gpu2d_compose_engine(gpu2d_ctx *c, const gpu2d_engine *e);

#endif
