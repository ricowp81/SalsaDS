/*
 * SalsaDS — ejecución de las órdenes del motor de geometría (Fase 3.5): matrices, vértices, iluminación, recorte, viewport
 * y armado de la lista de polígonos. Es la mitad del motor de geometría que no depende del tiempo: `gx_exec` recibe órdenes
 * (código y parámetros) en el orden en que el motor las empieza y deja los resultados en `gx_exec_out` y en el cuadro de
 * salida. La otra mitad (FIFO, tubería, coste en ciclos, GXSTAT, IRQ) es gx.c.
 *
 * Código propio, escrito desde GBATEK; las decisiones están en docs/fase3.md. Portable: C99 con aritmética entera de 64 bits,
 * sin coma flotante ni dependencias del resto del núcleo; corre en el host, en el PPE big-endian y en un SPE
 * (src/spu/geom). Presupuesto de local store del SPE: ver src/spu/geom/geom.c.
 *
 * Salida. Los vértices y polígonos se escriben en las ventanas `vtx` y `poly` (índice absoluto menos `vbase`/`pbase`):
 * el PPE las apunta al cuadro entero (sin vaciados), el SPE a un búfer de su local store que `flush` copia por DMA al
 * cuadro en memoria principal.
 */
#ifndef SALSADS_GPU3D_GX_EXEC_H
#define SALSADS_GPU3D_GX_EXEC_H

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
} __attribute__((aligned(16))) gx_mat;

/* Vértice en el espacio de recorte (interno). */
typedef struct gx_cvtx {
	int32_t x, y, z, w; /* 1.19.12 */
	int32_t s, t;       /* 1.11.4 */
	int32_t r, g, b;    /* 0..63 */
	int32_t ram;        /* índice del vértice en la RAM de vértices si ya se guardó, o -1 (se comparte en las tiras) */
	int16_t sx, sy;     /* pantalla del vértice guardado (solo con ram >= 0): el cuadro de salida puede estar ya en memoria principal */
} gx_cvtx;


/* Estado de las pilas de matrices: lo comparten la ejecución (que además mueve las matrices) y el motor de tiempo (que
 * necesita el error y el nivel de GXSTAT sin ejecutar nada). Las funciones devuelven 0 si la orden da error. */
typedef struct gx_sp {
	uint32_t mode;
	uint32_t sp_proj, sp_pos, sp_tex;
} gx_sp;

int gx_sp_push(gx_sp *s);
int gx_sp_pop(gx_sp *s, uint32_t param, int32_t *idx);
int gx_sp_store(const gx_sp *s, uint32_t param, uint32_t *idx);
int gx_sp_restore(const gx_sp *s, uint32_t param, uint32_t *idx);

/* Contadores que produce la ejecución (el resto de gx_stats es del motor de tiempo). */
typedef struct gx_estats {
	uint64_t polys;         /* polígonos que llegaron a la lista */
	uint64_t culled;        /* descartados por cara */
	uint64_t clipped;       /* recortados (con vértices nuevos) */
	uint64_t rejected;      /* fuera del volumen de vista o cruzan el plano lejano sin permiso (suma de las tres siguientes) */
	uint64_t rej_outside, rej_far, rej_empty;
	uint64_t dropped_poly;  /* sin sitio en la RAM de polígonos */
	uint64_t dropped_vert;  /* sin sitio en la RAM de vértices */
} gx_estats;

/* Lo que la CPU puede leer de vuelta y lo que cuenta el cuadro. Es lo único que el SPE devuelve a memoria principal. */
typedef struct gx_exec_out {
	uint32_t npolys, nverts, dropped_polys, dropped_verts; /* del cuadro en construcción (RAM_COUNT) */
	uint32_t box_result;
	int32_t pos_result[4];
	int16_t vec_result[4];
	int32_t clip[16];   /* matriz clip y vectorial al día solo si se pidió (gx_exec_flush_mats) */
	int32_t vec[16];
	gx_estats st;
} __attribute__((aligned(16))) gx_exec_out;

typedef struct gx_exec {
	gx_sp sp;
	gx_mat proj, pos, vec, tex, clip;
	int clip_dirty;
	gx_mat stk_proj, stk_tex;
	gx_mat stk_pos[GX_STACK_POS], stk_vec[GX_STACK_POS];

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

	int32_t light_vec[4][3];    /* 1.0.9, ya transformadas */
	uint32_t light_col[4][3];   /* 5 bits */
	uint32_t dif[3], amb[3], spe[3], emi[3];
	uint32_t shin_en;
	uint8_t shin[128];

	/* salida */
	gx_vertex *vtx;             /* ventana de vértices: el índice absoluto `i` está en vtx[i - vbase] */
	gx_poly *poly;              /* ventana de polígonos */
	uint32_t vbase, pbase, vcap, pcap; /* primer índice de cada ventana y cuántos caben */
	void (*flush)(struct gx_exec *ex, void *ctx); /* vacía las ventanas (sube lo escrito) y adelanta vbase/pbase; NULL = no hay vaciado */
	void *flush_ctx;

	gx_exec_out out;
	uint64_t by_cmd[0x80];      /* órdenes ejecutadas, por código (solo para medir) */
	uint32_t prof[6];           /* con GX_PROF (SPE): ticks del decrementador en matrices, vértices, normales/luces, otras y armado de polígonos */
} gx_exec;

void gx_exec_init(gx_exec *ex);
/* Cuadro nuevo (tras un SWAP_BUFFERS): contadores a cero. */
void gx_exec_newframe(gx_exec *ex);
/* Ejecuta una orden con sus parámetros. */
void gx_exec_cmd(gx_exec *ex, uint32_t cmd, const uint32_t *p);
/* Ejecuta un registro de órdenes: palabras `cmd, p0..pn-1` seguidas (n = gx_cmd_np(cmd)). */
void gx_exec_run(gx_exec *ex, const uint32_t *log, uint32_t nwords);
/* Deja al día las matrices clip y vectorial en `out` (para lecturas de 0x04000640-0x040006A7). */
void gx_exec_flush_mats(gx_exec *ex);

/* Un trabajo del ejecutor (lo que hace el SPE de geometría y el ejecutor de prueba del host): ejecuta `nwords` palabras del
 * registro con las ventanas de salida de `vstage`/`pstage` (se vacían con put_* cuando se llenan y al terminar). `fetch`
 * trae un tramo del registro (a partir de la palabra `pos` de lo publicado); un registro que quede partido entre dos tramos
 * se vuelve a pedir entero. Con GX_JOB_RESET reinicia la ejecución; con GX_JOB_NEWFRAME pone a cero los contadores del
 * cuadro; con GX_JOB_MATS deja al día las matrices en `ex->out`. */
typedef struct gx_job_io {
	void *ctx;
	void (*fetch)(void *ctx, uint32_t pos, uint32_t *dst, uint32_t nwords);
	void (*put_verts)(void *ctx, uint32_t first, const gx_vertex *src, uint32_t n);
	void (*put_polys)(void *ctx, uint32_t first, const gx_poly *src, uint32_t n);
} gx_job_io;

#define GX_JOB_RESET 1u
#define GX_JOB_NEWFRAME 2u
#define GX_JOB_MATS 4u

void gx_job_run(gx_exec *ex, uint32_t flags, uint32_t nwords, const gx_job_io *io, gx_vertex *vstage, uint32_t vcap,
		gx_poly *pstage, uint32_t pcap, uint32_t *chunk, uint32_t chunk_words);

/* Tabla de órdenes: parámetros (0 si no lleva; GX_NP_BAD si no existe), entradas de FIFO, validez y coste en ciclos del motor
 * (depende del modo de matriz y de las luces encendidas en POLYGON_ATTR). En línea: el motor de tiempo las consulta por cada orden. */
#define GX_NP_BAD 0xFFu
extern const uint8_t gx_np_tab[0x80];
extern const uint16_t gx_cost_tab[0x80];

static inline unsigned gx_cmd_np(uint32_t cmd)
{
	return cmd < 0x80 ? gx_np_tab[cmd] : GX_NP_BAD;
}

static inline unsigned gx_cmd_entries(uint32_t cmd)
{
	const unsigned np = gx_np_tab[cmd & 0x7Fu];

	return np && np != GX_NP_BAD ? np : 1u;
}

static inline int gx_cmd_valid(uint32_t cmd)
{
	return gx_cmd_np(cmd) != GX_NP_BAD;
}

static inline unsigned gx_cmd_cost(uint32_t cmd, uint32_t mtx_mode, uint32_t polyattr)
{
	unsigned c = gx_cost_tab[cmd & 0x7Fu];

	if (mtx_mode == 2 && (cmd == 0x18 || cmd == 0x19 || cmd == 0x1A || cmd == 0x1C))
		c += 30u;
	else if (cmd == 0x21)
		c += (0x4332322132212110ull >> (4u * (polyattr & 0xFu))) & 0xFu; /* luces encendidas */
	return c;
}

#endif
