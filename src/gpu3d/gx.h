/*
 * SalsaDS — motor de geometría de la DS (Fase 3.1): FIFO de comandos, matrices, vértices, iluminación, recorte y
 * viewport. Produce, por cuadro, una lista de polígonos en coordenadas de pantalla que rasteriza la Fase 3.2.
 *
 * Código propio, escrito desde GBATEK (documentación) y la comparación de comportamiento con melonDS ejecutándolo
 * (docs/fase3.md); no viene de ningún emulador. Portable: C99 con aritmética entera de 64 bits, sin coma flotante,
 * sin dependencias del resto del núcleo (corre en el host, en el PPE big-endian y se prueba con qemu). Las
 * estructuras de salida (`gx_vertex`, `gx_poly`, `gx_frame`) están pensadas para que un SPE las lea por DMA:
 * alineadas a 16 bytes, sin punteros, índices de 16 bits.
 *
 * Tiempo. Todo instante es un contador de ciclos del ARM9 (67,03 MHz); el motor de geometría corre a la mitad
 * (33,5 MHz, el reloj del bus), así que cada ciclo del motor son dos del ARM9. El motor es perezoso: no tiene hilo ni
 * evento propio, `gx_advance(now)` ejecuta los comandos cuyo comienzo cae antes de `now`. Quien lo use tiene que
 * llamarlo antes de mirar cualquier estado (el núcleo lo hace en cada acceso a los registros y en el evento EV_GX).
 *
 * Formatos de punto fijo (GBATEK) y sus anchos:
 *   matriz   1.19.12  (s32, 12 bits fraccionarios); el producto de matrices suma en 64 bits y trunca (>> 12)
 *   vértice  1.3.12   (s16) y VTX_10 1.3.6 (se lleva a 1.3.12 con << 6), VTX_DIFF 10 bits con signo sumados tal cual (1/4096)
 *   normal / luz  1.0.9 (s10)
 *   textura  1.11.4   (s16)
 *   color    RGB555 (5 bits por canal); en la salida se expande a 6 bits: c6 = (c5 << 1) + ((c5 + 31) >> 5)
 *   profundidad  15 bits << 9 (0 a 0xFFFE00)
 */
#ifndef SALSADS_GPU3D_GX_H
#define SALSADS_GPU3D_GX_H

#include <stdint.h>

#define GX_FIFO_ENTRIES 256u /* entradas de 40 bits (orden + parámetro) */
#define GX_PIPE_ENTRIES 4u   /* la tubería delante del motor; no cuenta en GXSTAT */
#define GX_QUEUE_MAX (GX_FIFO_ENTRIES + GX_PIPE_ENTRIES) /* entradas que caben (FIFO + tubería) */
#define GX_QRING 512u                                    /* anillo de la cola: potencia de 2, sin divisiones */
#define GX_MAX_POLYS 2048u /* polígonos por cuadro (Polygon RAM) */
#define GX_MAX_VERTS 6144u /* vértices por cuadro (Vertex RAM); los vértices de una tira se guardan una sola vez */
#define GX_POLY_MAXV 10u   /* un cuadrilátero recortado contra los seis planos tiene a lo más 10 vértices */
#define GX_STACK_POS 31u   /* entradas de la pila de posición y de la vectorial */
#define GX_NEVER UINT64_MAX

/* Bits de GXSTAT (0x04000600) */
#define GXSTAT_TEST_BUSY (1u << 0)   /* BOX/POS/VEC_TEST en curso */
#define GXSTAT_BOX_RESULT (1u << 1)  /* BOX_TEST: 1 = alguna parte dentro del volumen de vista */
#define GXSTAT_STACK_BUSY (1u << 14) /* push/pop/store/restore/identity en curso */
#define GXSTAT_STACK_ERR (1u << 15)  /* desbordamiento o subdesbordamiento de pilas (se borra escribiendo 1) */
#define GXSTAT_HALF (1u << 25)       /* menos de la mitad de la FIFO (< 128 entradas) */
#define GXSTAT_EMPTY (1u << 26)      /* FIFO vacía */
#define GXSTAT_BUSY (1u << 27)       /* motor de geometría ocupado */

/* Vértice de salida (32 bytes). Coordenadas de pantalla enteras (la DS no tiene subpíxeles en los vértices). */
typedef struct gx_vertex {
	int16_t sx, sy;    /* pantalla, en píxeles del viewport (puede salirse de 0..255 / 0..191) */
	uint32_t z;        /* profundidad: ((z * 0x4000 / w) + 0x3FFF) acotada a 0..0x7FFF, << 9 */
	int32_t w;         /* w del recorte (1.19.12), entera; la rasterización la normaliza con gx_poly.wshift */
	int16_t s, t;      /* coordenadas de textura (1.11.4), ya transformadas por la matriz de textura */
	uint8_t r, g, b;   /* color de 6 bits (0..63) */
	uint8_t pad;
	int32_t rsv[2];    /* reservados para la rasterización (p. ej. 1/w) */
} __attribute__((aligned(16))) gx_vertex;

/* Banderas de gx_poly.flags */
#define GX_POLY_FRONT 0x01u /* cara frontal (antihoraria en pantalla); si no, trasera */
#define GX_POLY_CLIPPED 0x02u
#define GX_POLY_TRANSLUCENT 0x04u /* alfa de 1 a 30 (POLYGON_ATTR bits 16-20) */
#define GX_POLY_WIRE 0x08u        /* alfa 0: alambre */

/* Polígono de salida (48 bytes). Sus vértices se indican por índice en `gx_frame.vtx`; los de las tiras se comparten. */
typedef struct gx_poly {
	uint32_t attr;     /* POLYGON_ATTR vigente en el BEGIN_VTXS que lo abrió */
	uint32_t texparam; /* TEXIMAGE_PARAM al cerrarse el polígono */
	uint32_t pltt;     /* PLTT_BASE (13 bits) */
	uint8_t nverts;    /* 3 a 10 */
	uint8_t flags;     /* GX_POLY_* */
	uint8_t wshift;    /* 0, 4, 8...: w >> wshift cabe en 16 bits (para el interpolado de la rasterización) */
	uint8_t pad0;
	int16_t ymin, ymax; /* extensión en pantalla (para ordenar y repartir en bandas) */
	int16_t xmin, xmax;
	uint16_t vidx[GX_POLY_MAXV];
	uint16_t pad1[2];
} __attribute__((aligned(16))) gx_poly;

/* Registros del 3D que el motor de render lee al hacerse el SWAP_BUFFERS (una foto por cuadro). */
typedef struct gx_regs {
	uint16_t disp3dcnt;       /* 0x04000060 */
	uint16_t clear_depth;     /* 0x04000354 */
	uint16_t clrimage_offset; /* 0x04000356 */
	uint16_t fog_offset;      /* 0x0400035C */
	uint32_t clear_color;     /* 0x04000350 */
	uint32_t fog_color;       /* 0x04000358 */
	uint16_t alpha_test_ref;  /* 0x04000340 */
	uint16_t edge_color[8];   /* 0x04000330 */
	uint8_t fog_table[32];    /* 0x04000360 */
	uint16_t toon_table[32];  /* 0x04000380 */
} gx_regs;

/* Un cuadro de geometría: lo que construye el motor y lo que consume el rasterizador. */
typedef struct gx_frame {
	gx_vertex vtx[GX_MAX_VERTS] __attribute__((aligned(128)));
	gx_poly poly[GX_MAX_POLYS] __attribute__((aligned(128)));
	uint32_t npolys, nverts;
	uint32_t swap_params; /* SWAP_BUFFERS: bit 0 = ordenar translúcidos a mano (1) o por Y (0); bit 1 = W-buffer */
	uint32_t dropped_polys, dropped_verts; /* los que no cupieron */
	uint32_t seq;         /* número de cuadro (cuenta los SWAP_BUFFERS) */
	gx_regs regs;
} gx_frame;

typedef struct gx_mat {
	int32_t m[16]; /* fila mayor: m[4 * fila + columna]; la traslación va en la fila 3 */
} gx_mat;

/* Vértice en el espacio de recorte (interno). */
typedef struct gx_cvtx {
	int32_t x, y, z, w; /* 1.19.12 */
	int32_t s, t;       /* 1.11.4 */
	int32_t r, g, b;    /* 0..63 */
	int32_t ram;        /* índice del vértice en la RAM de vértices si ya se guardó, o -1 (se comparte en las tiras) */
} gx_cvtx;

typedef struct gx_qent {
	uint32_t param;
	uint8_t cmd;
	uint64_t t; /* instante en que llegó */
} gx_qent;

typedef struct gx_stats {
	uint64_t cmds;          /* comandos ejecutados */
	uint64_t polys;         /* polígonos que llegaron a la lista */
	uint64_t culled;        /* descartados por cara */
	uint64_t clipped;       /* recortados (con vértices nuevos) */
	uint64_t rejected;      /* fuera del volumen de vista o cruzan el plano lejano sin permiso (suma de las tres siguientes) */
	uint64_t rej_outside, rej_far, rej_empty; /* enteros fuera de un plano / cruzan el lejano sin el bit 12 / el recorte los dejó sin área */
	uint64_t dropped_poly;  /* sin sitio en la RAM de polígonos */
	uint64_t dropped_vert;  /* sin sitio en la RAM de vértices */
	uint64_t swaps;
	uint64_t fifo_overflow; /* escrituras perdidas (FIFO llena con el motor parado en SWAP_BUFFERS) */
	uint64_t cycles;        /* ciclos del motor ocupados (ARM9, /2 = ciclos del motor) */
	uint64_t unknown_cmds;
	uint64_t frame_polys, frame_verts; /* suma de lo entregado por los SWAP_BUFFERS (para el promedio por cuadro) */
	uint32_t max_polys, max_verts;     /* máximos de un cuadro */
	uint64_t host_ticks;               /* tiempo del host gastado ejecutando órdenes (con `clock`) */
	uint64_t by_cmd[0x80];             /* órdenes ejecutadas, por código */
} gx_stats;

typedef struct gx {
	/* cola de entradas (FIFO de 256 + tubería de 4) */
	gx_qent q[GX_QRING];
	uint32_t qhead, qcount;
	/* decodificador de palabras empaquetadas de GXFIFO */
	uint32_t pk_word;
	uint32_t pk_idx;  /* siguiente byte de orden de la palabra (4 = palabra agotada) */
	uint32_t pk_left; /* parámetros que faltan de la orden en curso */
	uint8_t pk_cmd;

	/* tiempo */
	uint64_t cycle_free;  /* instante (ARM9) en que el motor termina lo que ejecuta */
	uint64_t stack_busy_until, test_busy_until;
	uint32_t test_pending; /* BOX/POS/VEC_TEST en la cola, sin ejecutar */
	uint64_t now;          /* el instante más tardío observado */

	/* estado de GXSTAT */
	uint32_t err, box_result, irqmode;

	/* matrices */
	uint32_t mode;
	gx_mat proj, pos, vec, tex, clip;
	int clip_dirty;
	gx_mat stk_proj, stk_tex;
	gx_mat stk_pos[GX_STACK_POS], stk_vec[GX_STACK_POS];
	uint32_t sp_proj, sp_pos, sp_tex;

	/* estado de vértices */
	int32_t vx, vy, vz;         /* último vértice (1.3.12) */
	uint32_t color5[3];         /* color actual (5 bits) */
	int32_t tex_s, tex_t;       /* TEXCOORD sin transformar */
	int32_t cur_s, cur_t;       /* coordenadas que llevará el próximo vértice (ya transformadas según el modo) */
	int32_t normal[3];          /* última normal, tal como se mandó (1.0.9) */
	uint32_t polyattr, polyattr_cur; /* pendiente y la del BEGIN_VTXS vigente */
	uint32_t texparam, pltt;
	uint32_t viewport;          /* x1 | y1 << 8 | x2 << 16 | y2 << 24 */
	uint32_t prim;              /* tipo de BEGIN_VTXS (0 a 3) */
	gx_cvtx asm_v[4];           /* ensamblado: vértices ya transformados al espacio de recorte */
	uint32_t asm_n;
	uint32_t asm_par;           /* paridad (tiras) */

	/* luces */
	int32_t light_vec[4][3];    /* 1.0.9, ya transformadas */
	uint32_t light_col[4][3];   /* 5 bits */
	uint32_t dif[3], amb[3], spe[3], emi[3];
	uint32_t shin_en;
	uint8_t shin[128];
	uint32_t shin_idx;
	uint32_t set_vtxcolor;

	/* resultados de las órdenes de prueba */
	int32_t pos_result[4];
	int16_t vec_result[3];

	/* cuadros */
	gx_frame frame[2];
	int bld;                   /* cuadro en construcción */
	int front;                 /* último cuadro entregado por SWAP_BUFFERS, o -1 */
	uint32_t swaps;            /* cuántos */
	int swap_pending;          /* SWAP_BUFFERS ejecutado: el motor espera al VBlank */
	uint32_t swap_param;
	gx_stats st;
	uint64_t (*clock)(void); /* opcional: reloj del host para medir `st.host_ticks` */
} gx_t;

void gx_init(gx_t *g);

/* Escritura de una palabra de 32 bits a GXFIFO (0x04000400-0x0400043F); empaquetada. Devuelve el instante hasta el que
 * hay que frenar a la CPU (>= now): una FIFO llena la detiene hasta que el motor libere sitio. */
uint64_t gx_fifo_write(gx_t *g, uint64_t now, uint32_t w);
/* Escritura a un puerto de orden (0x04000440-0x040005CC, `off` = dirección - 0x04000000). Idem. */
uint64_t gx_port_write(gx_t *g, uint64_t now, uint32_t off, uint32_t w);

/* Ejecuta los comandos que ya empezaron antes de `now`. */
void gx_advance(gx_t *g, uint64_t now);
/* Instante en que empieza el siguiente comando de la cola, o GX_NEVER (cola vacía, incompleta o parada en un SWAP). */
uint64_t gx_next_start(const gx_t *g);

/* GXSTAT completo en el instante `now` (llama a gx_advance). */
uint32_t gx_gxstat(gx_t *g, uint64_t now);
/* Escritura a GXSTAT: `mask` dice qué bytes (bit 15 borra el error; bits 30-31 son el modo de la IRQ). */
void gx_gxstat_write(gx_t *g, uint32_t v, uint32_t mask);
/* Entradas en la FIFO (sin la tubería) y condición de la IRQ 21 según el modo de GXSTAT. */
uint32_t gx_fifo_level(const gx_t *g);
int gx_irq_level(const gx_t *g);
/* RAM_COUNT (0x04000604) */
uint32_t gx_ram_count(const gx_t *g);
/* Lectura de 32 bits de un registro del motor en 0x04000600-0x040006FF (`off` = dirección - 0x04000000). */
uint32_t gx_read32(gx_t *g, uint64_t now, uint32_t off);

/* SWAP_BUFFERS: ¿hay un cambio de búfer esperando al VBlank? `gx_swap` lo hace en el instante `now` (el núcleo llama a
 * gx_advance antes), toma la foto de los registros del 3D y deja que el motor siga con lo que había detrás del
 * SWAP_BUFFERS; devuelve el cuadro entregado. */
int gx_swap_pending(const gx_t *g);
const gx_frame *gx_swap(gx_t *g, uint64_t now, const gx_regs *regs);
/* Último cuadro entregado (o NULL). */
const gx_frame *gx_front(const gx_t *g);

#endif
