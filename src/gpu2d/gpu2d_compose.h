/*
 * SalsaDS — compositor 2D por líneas (Fase 2.1).
 *
 * Una pantalla (un motor), 192 líneas, leyendo la instantánea de la VRAM de la memoria principal por DMA y aplicando el
 * registro de cambios por línea. Es el código del SPE; en el host (y en el PPE) compila con kernels escalares para
 * probar la estructura. Presupuesto del local store del contexto: ver sizeof(gpu2d_ctx) (el programa SPE lo imprime
 * al arrancar y docs/memory-budget.md lo reparte).
 *
 * Tubería de dos etapas: mientras se compone la línea y, ya se pidió por DMA lo que necesita la y + 1 (los
 * registros de toda la pantalla están en el registro de cambios, que se lee a trozos, así que se sabe de antemano).
 *   etapa 1 (plan_line)    aplica los cambios de la línea, decide qué hace falta (fila de teselas de los fondos de
 *                          texto, píxeles de los fondos afines y de los OBJ afines, fila de cada OBJ) y lo pide;
 *   etapa 2 (compose_line) espera lo pedido, dibuja las capas como índices de la paleta unificada (G2D_TRANSP =
 *                          transparente), las combina por prioridad y ventana, aplica mezclas y brillo y manda la
 *                          línea a memoria principal.
 * Etiquetas de DMA exactas, para no esperar de más (ver G2D_TAG_*).
 *
 * Paleta unificada (G2D_PAL_*): 0..255 fondos, 256..511 OBJ, 512.. paletas extendidas de fondo (4 ranuras de 4096) y
 * 16896.. la de OBJ (4096). Los colores quedan en BGR555 de la máquina que los lee.
 */
#ifndef SALSADS_GPU2D_COMPOSE_H
#define SALSADS_GPU2D_COMPOSE_H

#include "gpu2d/gpu2d.h"
#include "gpu2d/gpu2d_vec.h"

#define G2D_PAL_BG 0
#define G2D_PAL_OBJ 256
#define G2D_PAL_BGEXT 512
#define G2D_PAL_OBJEXT 16896
#define G2D_PAL_ALL 20992

#define G2D_TILES 33         /* teselas por línea de un fondo de texto (32 + la parcial) */
#define G2D_TILEBUF 2176     /* 33 teselas de 64 bytes, redondeado a 128 */
#define G2D_LINE_VECS 48     /* 8 de margen a la izquierda, 32, 8 a la derecha */
#define G2D_MAP_ARENA 40960  /* mapas residentes de los fondos afines y extendidos de 16 bits (los dos juntos) */
#define G2D_OBJ_ARENA 16384  /* por paridad de línea: filas de OBJ normales y píxeles de los afines */
#define G2D_PALCHG_MAX 96    /* cambios de paleta por línea; los que sobran se descartan (cuenta en stats) */
#define G2D_CHG_CHUNK 256    /* entradas del registro de cambios por trozo de DMA */
#define G2D_NOPS 128         /* OBJ por línea */

/* Etiquetas de DMA (0..30; la 31 es del bucle de spukit) */
enum {
	G2D_TAG_BG = 0,        /* 0..7: fondo b (b·2 + ranura): mapa y teselas */
	G2D_TAG_OBJ = 8,       /* 8, 9: OBJ de las líneas pares / impares */
	G2D_TAG_OUT = 10,      /* 10, 11: salida */
	G2D_TAG_START = 12,    /* paleta, OAM, descriptor */
	G2D_TAG_CHG = 13,      /* 13, 14: trozos del registro de cambios */
	G2D_TAG_AFF = 15,      /* 15..18: fondos afines (BG2/BG3 × paridad) */
	G2D_TAG_RAW = 19,      /* 19, 20: línea cruda de los modos 2 y 3 */
	G2D_TAG_EXT = 21,      /* paletas extendidas */
	G2D_TAG_MAP = 22,      /* mapas de los fondos afines */
	G2D_TAG_3D = 23,       /* 23, 24: línea de la capa 3D (Fase 3.2) */
};

/* OBJ decodificado (16 bytes) */
typedef struct g2d_obj {
	int16_t x;
	uint8_t y, flags;
	uint8_t w, h;
	uint8_t mode, prio;
	uint16_t tile;
	uint8_t pal, group;
	uint16_t pad;
} g2d_obj;
#define G2D_OBJ_VALID 1
#define G2D_OBJ_AFF 2
#define G2D_OBJ_DBL 4
#define G2D_OBJ_MOS 8
#define G2D_OBJ_C256 16
#define G2D_OBJ_HFLIP 32
#define G2D_OBJ_VFLIP 64

/* Qué hay que dibujar de un OBJ en una línea: lleva una copia del OBJ decodificado porque la etapa 1 de la línea
 * siguiente puede cambiar la OAM antes de que la etapa 2 de esta línea dibuje. */
enum { G2D_OP_ROW, G2D_OP_BMPROW, G2D_OP_AFF, G2D_OP_BMPAFF };
typedef struct g2d_op {
	g2d_obj o;
	uint8_t kind;
	uint8_t py;     /* fila dentro de la tesela (ROW) */
	uint16_t data;  /* desplazamiento de los datos en el área de la línea */
	uint16_t offs;  /* AFF: desplazamientos de cada píxel */
	uint16_t x0, n; /* AFF: primer píxel de pantalla y cuántos */
	uint16_t pad;
} g2d_op;

/* Fondo afín: lo que la etapa 1 deja para la 2 (por ranura) */
typedef struct g2d_abg {
	g2d_el list[256] __attribute__((aligned(16)));
	uint8_t data[256][16] __attribute__((aligned(128)));
	uint16_t off[256] __attribute__((aligned(16)));   /* desplazamiento del texel en `data`, o 0xFFFF */
	uint16_t base[256] __attribute__((aligned(16)));  /* base del índice de paleta (extendida) */
} g2d_abg;

/* Fondo de texto: lo que la etapa 1 deja para la 2 */
typedef struct g2d_job {
	uint32_t dc;
	uint32_t waitmask;
	uint16_t regs[G2D_NREGS];
	uint8_t kind;                 /* 0 blanca, 1 gráficos, 2 VRAM, 3 memoria principal */
	uint8_t winv[2];
	uint8_t bg_type[GPU2D_NBG];
	uint8_t bg_slot[GPU2D_NBG], bg_off[GPU2D_NBG], bg_py[GPU2D_NBG], bg_tx0[GPU2D_NBG];
	uint8_t bg_wide[GPU2D_NBG], bg_c256[GPU2D_NBG];
	uint16_t bg_extbase[GPU2D_NBG];  /* texto de 8 bits: base de la paleta extendida de la ranura, o 0 */
	uint8_t aff_ok[2];            /* BG2/BG3: hay datos para dibujar */
	uint8_t aff_type[2];
	uint8_t aff_slot;             /* paridad (= índice de g2d_abg) */
	uint16_t nops, npalchg, op_done;  /* op_done: operaciones ya dibujadas por vaciados del área */
	uint32_t palchg[G2D_PALCHG_MAX];
	g2d_op ops[G2D_NOPS];
} g2d_job;

/* Contadores de una pantalla (diagnóstico). */
typedef struct gpu2d_stats {
	uint32_t bg_fetches;    /* filas de teselas traídas */
	uint32_t map_waits;     /* esperas intermedias por el mapa */
	uint32_t obj_fetches;   /* filas de OBJ traídas */
	uint32_t obj_drawn;     /* OBJ-línea dibujados */
	uint32_t chg_applied;
	uint32_t aff_lines;     /* líneas de fondos afines planeadas */
	uint32_t aff_quads;     /* bloques de 16 bytes traídos para fondos y OBJ afines */
	uint32_t obj_flushes;   /* veces que el área de OBJ se llenó y hubo que vaciarla */
	uint32_t dropped;       /* cambios de paleta que no cupieron */
	uint32_t map_dropped;   /* fondos afines o extendidos cuyos mapas no cupieron juntos en la arena (no se dibujan) */
	uint32_t fx_lines;      /* líneas con efectos de color por píxel */
} gpu2d_stats;

/* Tiempos por etapa en ticks del decrementador del SPE (79,8 MHz), solo con -DG2D_PROF en el SPE (ceros si no): cada
 * campo es lo que gastó el SPE en esa etapa durante toda la pantalla. `plan_*` y `cl_*` están dentro de `plan` y `compose`. */
typedef struct gpu2d_prof {
	uint32_t init;       /* antes de la línea 0: paleta, OAM, cambios, ventanas verticales */
	uint32_t plan;       /* etapa 1 de todas las líneas (suma de plan_line) */
	uint32_t plan_text;  /* de eso: fondos de texto (mapas, teselas y su espera) */
	uint32_t plan_aff;   /* de eso: fondos afines y de mapa de bits */
	uint32_t plan_obj;   /* de eso: OBJ */
	uint32_t plan_wait;  /* de eso: esperas del DMA dentro de la etapa 1 */
	uint32_t compose;    /* etapa 2 de todas las líneas (suma de compose_line) */
	uint32_t cl_wait;    /* de eso: la espera de lo que pidió la etapa 1 y del búfer de salida */
	uint32_t cl_layers;  /* de eso: dibujar las capas (fondos y OBJ, ventanas) */
	uint32_t cl_final;   /* de eso: prioridades, efectos de color y conversión (el bucle de los 32 vectores) */
	uint32_t cl_put;     /* de eso: pedir el DMA de salida */
	uint32_t end_wait;   /* la espera final de las salidas */
	uint32_t pad[2];
} gpu2d_prof;

typedef struct gpu2d_ctx {
	/* paleta unificada */
	uint16_t pal[G2D_PAL_ALL] __attribute__((aligned(128)));
	/* fondos de texto */
	uint8_t tiles[GPU2D_NBG][2][G2D_TILEBUF] __attribute__((aligned(128)));
	uint8_t maps[GPU2D_NBG][2][128] __attribute__((aligned(128)));
	g2d_el lists[GPU2D_NBG][2][G2D_TILES] __attribute__((aligned(16)));
	uint32_t offs[G2D_TILES];
	/* fondos afines */
	uint8_t amap[G2D_MAP_ARENA] __attribute__((aligned(128)));
	g2d_abg abg[2][2] __attribute__((aligned(128)));
	/* OBJ */
	uint8_t oarena[2][G2D_OBJ_ARENA] __attribute__((aligned(128)));
	g2d_obj obj[GPU2D_NOBJ] __attribute__((aligned(16)));
	uint8_t oamraw[1024] __attribute__((aligned(128)));
	/* capas de la línea */
	g2d_v8 bg[GPU2D_NBG][G2D_LINE_VECS] __attribute__((aligned(16)));
	g2d_v8 oidx[2][G2D_LINE_VECS] __attribute__((aligned(16)));  /* por paridad de línea */
	g2d_v8 omet[2][G2D_LINE_VECS] __attribute__((aligned(16)));
	g2d_v8 owin[2][G2D_LINE_VECS] __attribute__((aligned(16)));
	g2d_v8 ocov[2][G2D_LINE_VECS] __attribute__((aligned(16)));  /* texeles transparentes de OBJ con mosaico (para el mosaico horizontal) */
	g2d_v8 allow[6][32] __attribute__((aligned(16)));
	uint32_t aoffs[256];
	uint32_t aaddr[GPU2D_W];  /* direcciones de los texeles de un fondo afín (primera pasada de plan_affine) */
	g2d_v8 tmp[G2D_TILES + 1] __attribute__((aligned(16)));
	uint16_t outbuf[2][GPU2D_W] __attribute__((aligned(128)));
	uint8_t rawline[2][512] __attribute__((aligned(128)));
	uint32_t l3d[2][GPU2D_W] __attribute__((aligned(128)));  /* línea de la capa 3D por paridad (Fase 3.2) */
	g2d_v8 a3d[32] __attribute__((aligned(16)));            /* su alfa (0..31) en la línea que se compone */
	uint32_t l3d_semi;                                       /* la línea tiene píxeles 3D translúcidos */
	/* registro de cambios (por trozos) */
	gpu2d_change chg[2][G2D_CHG_CHUNK] __attribute__((aligned(128)));
	g2d_job job[2];
	/* estado del planificador (etapa 1) */
	uint16_t regs[G2D_NREGS];
	uint32_t dc;
	int32_t ref[2][2], mref[2][2];
	uint8_t winv[2];
	uint8_t oam_dirty;
	uint32_t chg_next, chg_fetched;
	uint32_t bg_k0[GPU2D_NBG][2], bg_cbase[GPU2D_NBG][2], bg_sbase[GPU2D_NBG][2];
	uint8_t bg_cur[GPU2D_NBG];
	/* mapas residentes: qué hay cargado */
	uint32_t amap_key[2], amap_off[2], amap_len[2];
	uint8_t ext_loaded;
	/* OBJ de la línea que se planea */
	uint32_t oa_used;
	gpu2d_stats stats;
	gpu2d_prof prof;
} gpu2d_ctx;

/* Compone una pantalla en e->out_ea (192 líneas de 512 bytes). Devuelve 0 o un código negativo: -1 descriptor
 * inválido (tamaños que no son potencia de 2, páginas fuera de la VRAM). */
int gpu2d_compose_engine(gpu2d_ctx *c, const gpu2d_engine *e);

#endif
