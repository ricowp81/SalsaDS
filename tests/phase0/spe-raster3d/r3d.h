/*
 * SalsaDS — Fase 0.4: formatos del rasterizador 3D de prueba (PPE, SPE y
 * host).
 *
 * Entrada por cuadro: una lista de triángulos ya transformados (coordenadas
 * de pantalla a 1×, z de 24 bits, w, coordenadas de textura y color de
 * vértice), en el orden de dibujo, y la VRAM de texturas y de paletas de la
 * DS en memoria principal (little-endian, como en la DS). El PPE publica UN
 * trabajo por SPE y por cuadro (struct r3d_job); el SPE trae la lista por
 * DMA en lotes de R3D_CHUNK triángulos.
 *
 * Salida: el cuadro de W×H píxeles de 32 bits (r6 | g6 << 8 | b6 << 16 |
 * a5 << 24, en el orden de bytes de la máquina) en memoria principal.
 *
 * Todo lo que va por DMA está alineado a 16 bytes (los búferes a 128) y mide
 * múltiplos de 16.
 */
#ifndef SALSADS_R3D_H
#define SALSADS_R3D_H

#include <stdint.h>

/* Pantalla de la DS y límites de la prueba. */
#define R3D_W1        256
#define R3D_H1        192
#define R3D_MAX_TRIS  2048           /* lo más que dibuja la DS por cuadro */
#define R3D_MAX_SCALE 4

/* Unidad de trabajo del SPE: una banda (todo el ancho) o un tile de
 * R3D_UNIT_PIXELS píxeles; color y profundidad viven en el local store
 * mientras se dibuja y solo el color vuelve a memoria principal. */
#define R3D_UNIT_PIXELS 4096u

/* Triángulos por lote de DMA (entrada en la pasada 1, registros de setup
 * en la 2). */
#define R3D_CHUNK 32u

/* Caché de texturas del local store: páginas de 4 KB de texeles ya
 * decodificados (32 bits cada uno). */
#define R3D_TEX_PAGE    4096u
#define R3D_TEX_PAGES   28u
#define R3D_TEX_ENTRIES 32u

/* ---- Entrada: un triángulo transformado, 64 bytes ------------------------- */

/* attr */
#define R3D_ATTR_ALPHA(a)   ((a) & 31u)     /* alfa del polígono, 0–31 */
#define R3D_ATTR_TRANS      (1u << 5)       /* se mezcla (alfa < 31) */
#define R3D_ATTR_DEPTHWRITE (1u << 6)       /* el translúcido escribe profundidad */

struct r3d_tri {
	int16_t x[3], y[3];  /* píxeles × 16 a 1× (12.4) */
	int16_t s[3], t[3];  /* texeles × 16 (1.11.4, como la DS) */
	uint32_t z[3];       /* profundidad de 24 bits */
	uint32_t w[3];       /* w en 20.12, > 0 */
	uint16_t rgb[3];     /* color de vértice BGR555 (rojo en los bits 0–4) */
	uint16_t attr;
	uint32_t texparam;   /* TEXIMAGE_PARAM de la DS */
	uint16_t pltt;       /* PLTT_BASE (unidades de 16 bytes) */
	uint16_t pad;
};

/* TEXIMAGE_PARAM (GBATEK, «DS 3D Texture Attributes»). */
#define R3D_TEX_OFFSET(p)  (((p) & 0xFFFFu) * 8u)
#define R3D_TEX_REPEAT_S   (1u << 16)
#define R3D_TEX_REPEAT_T   (1u << 17)
#define R3D_TEX_SIZE_S(p)  (8u << (((p) >> 20) & 7u))
#define R3D_TEX_SIZE_T(p)  (8u << (((p) >> 23) & 7u))
#define R3D_TEX_FORMAT(p)  (((p) >> 26) & 7u)
#define R3D_TEX_COLOR0     (1u << 29)        /* índice 0 transparente */
#define R3D_TEXPARAM(off8, ss, ts, fmt) \
	((uint32_t)(off8) | (uint32_t)(ss) << 20 | (uint32_t)(ts) << 23 | (uint32_t)(fmt) << 26)

enum {
	R3D_FMT_NONE = 0,
	R3D_FMT_PAL16 = 3,    /* 4 bits por texel */
	R3D_FMT_PAL256 = 4,   /* 8 bits por texel */
	R3D_FMT_DIRECT = 7,   /* A1BGR5 */
};

/* ---- Registro de setup (lo escribe la pasada 1, lo lee la 2), 192 bytes ---- */

/*
 * Funciones de borde E_k(x, y) = e0[k] + ea[k]·(x − xbase) + eb[k]·(y − ybase)
 * en enteros (unidades de 1/256 de píxel²), con la regla de arriba-izquierda
 * ya sumada en e0: el píxel está dentro si las tres son ≥ 0.
 *
 * Atributos (planos en flotante): a(x, y) = abase + adx·(x − xbase) +
 * ady·(y − ybase), en el centro de cada píxel. Carriles: z, q = 1/w, s·q, t·q
 * (s y t ya desplazados y escalados para el direccionamiento, ver
 * r3d_core.c), (r+1)·q, (g+1)·q, (b+1)·q. La base depende solo del
 * triángulo (no de la banda ni del tile), así que cualquier reparto da los
 * mismos bits.
 */
enum { R3D_AZ, R3D_AQ, R3D_AS, R3D_AT, R3D_AR, R3D_AG, R3D_AB, R3D_NATTR };

struct r3d_rec {
	int32_t ea[4];
	int32_t eb[4];
	int32_t e0[4];
	float inv_ea[4];       /* 1/ea (0 si ea = 0), para acotar cada línea */
	float abase[8];
	float adx[8];
	float ady[8];
	int16_t xmin, xmax, ymin, ymax;  /* caja recortada al cuadro, inclusiva */
	int16_t xbase, ybase;
	uint16_t attr;
	uint16_t pltt;
	uint32_t texparam;
	uint32_t id;           /* índice del triángulo + 1 (modo id) */
	uint32_t pad[2];
};
#define R3D_REC_SIZE 192u

/* ---- Trabajo de un SPE por cuadro (cabe en los 112 bytes de spukit) ------- */

#define R3D_KIND_FRAME    1u  /* `kind` de los trabajos de spukit */
#define R3D_KIND_SELFTEST 2u
#define R3D_FLAG_ID 1u     /* escribe el id del triángulo en vez del color */

enum { R3D_SPLIT_INTERLEAVE = 0, R3D_SPLIT_HALVES = 1 };

struct r3d_job {
	uint64_t tris_ea;      /* ntris × 64 bytes, alineado a 128 */
	uint64_t vram_tex_ea;  /* VRAM de texturas (LE), alineada a 128 */
	uint64_t vram_pal_ea;  /* VRAM de paletas (LE), alineada a 128 */
	uint64_t frame_ea;     /* salida W×H × 4 bytes, alineada a 128 */
	uint64_t scratch_ea;   /* ntris × R3D_REC_SIZE, propio de cada SPE */
	uint32_t vram_tex_size, vram_pal_size;
	uint32_t ntris;
	uint32_t scale;        /* 1, 2 o 4 */
	uint32_t unit_w;       /* ancho de la unidad: W (banda) o 256 (tile) */
	uint32_t spe_index, spe_count, split;
	uint32_t flags;
	uint32_t clear_color;
	uint32_t clear_depth;  /* 24 bits */
	uint32_t pad;
};

/* ---- Estadísticas de un SPE por cuadro (resultado de spukit) -------------- */

struct r3d_stats {
	uint32_t t_setup;      /* ticks del decrementador (79,8 MHz) */
	uint32_t t_bin;        /* armar la lista de cada unidad */
	uint32_t t_gather;     /* esperas de DMA de los registros */
	uint32_t t_tex;        /* fallos de la caché de texturas (DMA + decodificar) */
	uint32_t t_raster;     /* dibujar */
	uint32_t t_out;        /* limpiar y sacar las unidades */
	uint32_t tris_mine;    /* triángulos con setup en este SPE */
	uint32_t units;
	uint32_t unit_tris;    /* suma de triángulos por unidad */
	uint32_t rows;
	uint32_t quads;        /* grupos de 4 píxeles evaluados */
	uint32_t frags;        /* píxeles escritos */
	uint32_t tex_miss;
	uint32_t tex_bytes;    /* bytes de VRAM traídos */
	uint32_t tex_fail;     /* texturas que no caben o formato sin soporte */
	uint32_t err;
};

/* Autoprueba de las funciones vectoriales (r3d_selftest.c). */
struct r3d_selftest {
	float recip[4];
	uint32_t mul16[4];
	int32_t mul32x16[4];
	uint32_t bytes[4];
	uint32_t cmp[4];
	int32_t cvt[4];
	uint32_t bits;
	uint32_t pad[3];
};
void r3d_selftest(struct r3d_selftest *o);

_Static_assert(sizeof(struct r3d_selftest) <= 112, "r3d_selftest cabe en el resultado de spukit");
_Static_assert(sizeof(struct r3d_tri) == 64, "r3d_tri mide 64 bytes");
_Static_assert(sizeof(struct r3d_rec) == R3D_REC_SIZE, "r3d_rec mide R3D_REC_SIZE");
_Static_assert(sizeof(struct r3d_job) <= 112, "r3d_job cabe en el descriptor de spukit");
_Static_assert(sizeof(struct r3d_stats) <= 112, "r3d_stats cabe en el resultado de spukit");

/* Tamaño del cuadro y de la unidad a una escala. */
static inline uint32_t r3d_frame_w(uint32_t scale) { return R3D_W1 * scale; }
static inline uint32_t r3d_frame_h(uint32_t scale) { return R3D_H1 * scale; }

/* Color de 32 bits del cuadro. */
static inline uint32_t r3d_rgba(uint32_t r6, uint32_t g6, uint32_t b6, uint32_t a5)
{
	return r6 | g6 << 8 | b6 << 16 | a5 << 24;
}

#endif
