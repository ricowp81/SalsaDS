/*
 * SalsaDS — reglas del motor de render 3D de la DS (Fase 3.2), compartidas por la referencia (r3d_ref.c) y el
 * rasterizador por bandas de los SPE (r3d_core.c): las dos llaman a estas mismas funciones, así que dan los mismos
 * bits; lo que cambia es cómo recorren el cuadro (entero contra bandas), de dónde salen los polígonos (memoria contra
 * DMA) y los texeles (VRAM directa contra la caché de bloques decodificados).
 *
 * Fuente: GBATEK («DS 3D Rendering», «DS 3D Polygon Attributes», «DS 3D Texture Formats», «DS 3D Toon, Edge, Fog,
 * Alpha-Blending, Anti-Aliasing») y el comportamiento de melonDS ejecutándolo. Código propio. Lo que no se pudo
 * contrastar con una DS real va marcado [sin contrastar]; lo que se ajustó mirando a melonDS, [melonDS].
 *
 * Modelo de rasterizado (docs/fase3.md, 3.2):
 *   - Vértices en píxeles enteros. El polígono cubre las líneas ytop..ybot-1 (la de abajo no; uno de altura cero, su
 *     única línea). Dos cadenas de aristas desde el vértice de arriba; cuál es la izquierda sale del signo del área.
 *   - Aristas en punto fijo de 18 bits con la pendiente truncada; las «y-mayores» (|dx| <= dy) dan un píxel por
 *     línea y las «x-mayores» se evalúan en la línea siguiente, con tramos «gordos» cuando van hacia afuera: ver
 *     r3d_edge_x (ajustado a melonDS con tools/r3d-fit.py).
 *   - Atributos (color, s, t, w) interpolados con corrección por w: factor
 *         f = (pos · w0 << P) / ((len - pos) · w1 + pos · w0)
 *     con P = 9 a lo largo de las aristas y P = 8 a lo largo de la línea, y a = (a0 · (2^P - f) + a1 · f) >> P. La w es
 *     la del vértice desplazada `wshift` (16 bits). z (Z-buffer) se interpola lineal en pantalla; la del W-buffer es la
 *     w interpolada (<< wshift).
 *   - Todo en enteros: los mismos bits en el SPE, el PPE y el host.
 */
#ifndef SALSADS_GPU3D_R3D_PX_H
#define SALSADS_GPU3D_R3D_PX_H

#include <stdint.h>

#include "gpu3d/gx.h"
#include "gpu3d/r3d.h"

#define R3D_INLINE static inline __attribute__((always_inline))

#ifndef R3D_PS
#define R3D_PS 9 /* bits del factor de interpolación a lo largo de la línea (texturas y w) */
#endif
#ifndef R3D_PC
#define R3D_PC 8 /* ídem, para el color */
#endif

/* ---- registros del render (de gx_regs, una vez por cuadro) ---------------------------------------------- */

#define D3_TEXTURE (1u << 0)
#define D3_HIGHLIGHT (1u << 1)
#define D3_ALPHATEST (1u << 2)
#define D3_BLEND (1u << 3)
#define D3_AA (1u << 4)
#define D3_EDGE (1u << 5)
#define D3_FOG_ALPHA (1u << 6)
#define D3_FOG (1u << 7)
#define D3_REARBMP (1u << 14)

/* Atributos de polígono (POLYGON_ATTR) */
#define PA_MODE(a) (((a) >> 4) & 3u)
#define PA_TDEPTH (1u << 11)   /* los translúcidos escriben profundidad */
#define PA_DEQUAL (1u << 14)   /* prueba de profundidad «igual» */
#define PA_FOG (1u << 15)
#define PA_ALPHA(a) (((a) >> 16) & 31u)
#define PA_ID(a) (((a) >> 24) & 63u)

/* Búfer de atributos por píxel */
#define AT_OID(a) ((a) & 63u)          /* id del último opaco */
#define AT_TID(a) (((a) >> 6) & 63u)   /* id del último translúcido */
#define AT_TRANS (1u << 12)            /* ya hay un translúcido en el píxel */
#define AT_FOG (1u << 13)
#define AT_EDGE (1u << 14)
#define AT_STENCIL (1u << 15)
#define AT_BACK (1u << 16)             /* la profundidad del píxel es de un polígono de espalda (empate: lo frontal gana) */

typedef struct r3d_rstate {
	uint32_t d3;          /* DISP3DCNT */
	uint32_t wbuf;        /* swap_params bit 1 */
	uint32_t alpha_ref;   /* ALPHA_TEST_REF (5 bits) */
	uint32_t clear_px, clear_depth, clear_attr;
	uint32_t fog_px;      /* color y alfa de la niebla (R3D_PX) */
	uint32_t fog_offset, fog_shift;
	uint8_t fog_table[32];
	uint32_t toon[32];    /* R3D_PX (alfa 0) */
	uint32_t edge[8];     /* R3D_PX */
	uint32_t flags;       /* R3D_F_* */
} r3d_rstate;

R3D_INLINE uint32_t r3d_e6(uint32_t c5)
{
	return c5 ? (c5 << 1) + 1u : 0u;
}

R3D_INLINE uint32_t r3d_555(uint32_t c, uint32_t a)
{
	return R3D_PX(r3d_e6(c & 31u), r3d_e6((c >> 5) & 31u), r3d_e6((c >> 10) & 31u), a);
}

static inline void r3d_rstate_init(r3d_rstate *rs, const gx_frame *f, uint32_t flags)
{
	const gx_regs *g = &f->regs;
	const uint32_t cc = g->clear_color, cd = g->clear_depth & 0x7FFFu;

	rs->d3 = g->disp3dcnt;
	rs->wbuf = (f->swap_params >> 1) & 1u;
	rs->alpha_ref = g->alpha_test_ref & 31u;
	rs->clear_px = r3d_555(cc & 0x7FFFu, (cc >> 16) & 31u);
	/* CLEAR_DEPTH de 15 bits a 24: << 9, y el máximo (0x7FFF) llena los 9 de abajo [sin contrastar] */
	rs->clear_depth = (cd << 9) | (cd == 0x7FFFu ? 0x1FFu : 0u);
	rs->clear_attr = ((cc >> 24) & 63u) | ((cc & 0x8000u) ? AT_FOG : 0u);
	rs->fog_px = r3d_555(g->fog_color & 0x7FFFu, (g->fog_color >> 16) & 31u);
	rs->fog_offset = g->fog_offset & 0x7FFFu;
	rs->fog_shift = (g->disp3dcnt >> 8) & 15u;
	for (int i = 0; i < 32; i++) {
		rs->fog_table[i] = (uint8_t)(g->fog_table[i] & 127u);
		rs->toon[i] = r3d_555(g->toon_table[i] & 0x7FFFu, 0);
	}
	for (int i = 0; i < 8; i++)
		rs->edge[i] = r3d_555(g->edge_color[i] & 0x7FFFu, 31);
	rs->flags = flags;
}

/* Color de vértice de 6 bits a 9 para interpolar: 3 bits más, con la fracción llena salvo en el 0. */
R3D_INLINE int32_t r3d_c9(int32_t c6)
{
	return (c6 << 3) | (c6 ? 7 : 0);
}

/* ---- polígono preparado ------------------------------------------------------------------------- */

typedef struct r3d_v {
	int32_t x, y;
	uint32_t z, w;       /* z de 24 bits; w normalizada (>> wshift), al menos 1 */
	int32_t s, t;        /* 1.11.4 */
	int32_t r, g, b;     /* 6 bits */
} r3d_v;

/* Textura de un polígono */
typedef struct r3d_tex {
	uint32_t key;        /* identidad para la caché: TEXIMAGE_PARAM sin repetir/espejo/transformación */
	uint32_t pkey;       /* PLTT_BASE (solo formatos con paleta) */
	uint32_t fmt;        /* 1..7 */
	uint32_t sl, tl;     /* log2 del ancho y del alto (3..10) */
	uint32_t addr;       /* bytes en la memoria de texturas (512 KB) */
	uint32_t pal;        /* bytes en la memoria de paletas */
	uint32_t c0t;        /* índice 0 transparente */
	uint32_t rs, rt, fs, ft; /* repetir y espejo por eje */
} r3d_tex;

typedef struct r3d_poly {
	r3d_v v[GX_POLY_MAXV];
	int32_t n;
	int32_t ytop, ybot;  /* líneas ytop..ybot-1 (o solo ytop si son iguales) */
	int32_t top;         /* índice del vértice de arriba */
	int32_t ldir;        /* +1: la cadena izquierda avanza en el orden de los vértices; -1 al revés */
	uint32_t attr, mode, alpha, id, wshift;
	uint32_t wire, translucent_sort;
	uint32_t textured;
	uint32_t front;      /* polígono frontal (GX_POLY_FRONT) */
	r3d_tex tex;
} r3d_poly;

/* ¿El polígono va con los translúcidos al ordenar? Alfa 1..30 o textura A3I5/A5I3 (GBATEK). */
R3D_INLINE uint32_t r3d_is_translucent(uint32_t attr, uint32_t texparam)
{
	const uint32_t a = PA_ALPHA(attr), fmt = (texparam >> 26) & 7u;

	return (a != 0 && a != 31) || fmt == 1u || fmt == 6u;
}

/* Clave de orden: los opacos primero; dentro de cada grupo por la y de abajo y después la de arriba (con el bit 0 de
 * SWAP_BUFFERS los translúcidos van en el orden de envío: clave 0) [melonDS]. 18 bits; el orden es estable. */
R3D_INLINE uint32_t r3d_sort_key(const gx_poly *p, uint32_t manual)
{
	const uint32_t tr = r3d_is_translucent(p->attr, p->texparam);
	int32_t y0 = p->ymin, y1 = p->ymax;

	y0 = y0 < 0 ? 0 : y0 > 255 ? 255 : y0;
	y1 = y1 < 0 ? 0 : y1 > 255 ? 255 : y1;
	if (tr && manual)
		return 1u << 16;
	return (tr << 16) | ((uint32_t)y1 << 8) | (uint32_t)y0;
}

/* Prepara un polígono. Devuelve 0 si no dibuja nada. */
static inline int r3d_poly_setup(r3d_poly *p, const gx_poly *gp, const gx_vertex *const *vv, const r3d_rstate *rs)
{
	int64_t area = 0;
	const uint32_t tp = gp->texparam;

	p->n = gp->nverts;
	if (p->n < 3 || p->n > (int)GX_POLY_MAXV)
		return 0;
	p->attr = gp->attr;
	p->mode = PA_MODE(gp->attr);
	p->alpha = PA_ALPHA(gp->attr);
	p->front = (gp->flags & GX_POLY_FRONT) ? 1u : 0u;
	p->wire = p->alpha == 0;
	if (p->wire)
		p->alpha = 31;
	p->id = PA_ID(gp->attr);
	p->wshift = gp->wshift;
	p->ytop = 0x7FFFFFFF;
	p->ybot = -0x7FFFFFFF;
	p->top = 0;
	for (int i = 0; i < p->n; i++) {
		const gx_vertex *s = vv[i];
		r3d_v *d = &p->v[i];
		uint32_t w = (uint32_t)(s->w > 0 ? s->w : 1) >> p->wshift;

		d->x = s->sx;
		d->y = s->sy;
		d->z = s->z & 0xFFFFFFu;
		d->w = w ? w : 1u;
		d->s = s->s;
		d->t = s->t;
		/* el color se interpola con 3 bits más, con la fracción llena salvo en el 0 (medido contra melonDS con las
		 * escenas 16 a 19 de gx3d: docs/fase3.md, 3.3) */
		d->r = r3d_c9(s->r);
		d->g = r3d_c9(s->g);
		d->b = r3d_c9(s->b);
		if (d->y < p->ytop) {
			p->ytop = d->y;
			p->top = i;
		}
		if (d->y > p->ybot)
			p->ybot = d->y;
	}
	for (int i = 0; i < p->n; i++) {
		const r3d_v *a = &p->v[i], *b = &p->v[(i + 1) % p->n];

		area += (int64_t)a->x * b->y - (int64_t)b->x * a->y;
	}
	p->ldir = area < 0 ? 1 : -1;
	if (p->ybot < 0 || p->ytop > R3D_H - 1)
		return 0;
	/* textura */
	{
		const uint32_t fmt = (tp >> 26) & 7u;

		p->textured = (rs->d3 & D3_TEXTURE) && fmt != 0;
		p->translucent_sort = r3d_is_translucent(gp->attr, tp);
		if (p->textured) {
			r3d_tex *t = &p->tex;

			t->fmt = fmt;
			t->sl = 3u + ((tp >> 20) & 7u);
			t->tl = 3u + ((tp >> 23) & 7u);
			t->addr = (tp & 0xFFFFu) << 3;
			t->c0t = (tp >> 29) & 1u;
			t->rs = (tp >> 16) & 1u;
			t->rt = (tp >> 17) & 1u;
			t->fs = (tp >> 18) & 1u;
			t->ft = (tp >> 19) & 1u;
			t->pal = fmt == 2u ? (gp->pltt & 0x1FFFu) << 3 : (gp->pltt & 0x1FFFu) << 4;
			t->key = tp & 0x3FF0FFFFu;
			t->pkey = fmt == 7u ? 0u : (gp->pltt & 0x1FFFu);
		}
	}
	return 1;
}

/* ---- aristas y líneas --------------------------------------------------------------------------- */

typedef struct r3d_end {
	int32_t s, t, r, g, b;
	uint32_t w, z;
} r3d_end;

typedef struct r3d_row {
	int32_t xl, xle;     /* tramo [xl, xr); la arista izquierda ocupa [xl, xle) */
	int32_t xrs, xr;     /* la derecha [xrs, xr) */
	r3d_end l, r;        /* atributos en xl y en xr */
	int32_t den;         /* denominador de la interpolación a lo largo de la línea (xr - xl, o uno más con aristas inclinadas) */
	uint32_t alledge;    /* línea de arriba o de abajo del polígono: todo es arista */
} r3d_row;

R3D_INLINE int32_t r3d_floordiv(int32_t a, int32_t b) /* b > 0 */
{
	int32_t q = a / b;

	if ((a % b) != 0 && a < 0)
		q--;
	return q;
}

/* Factor de interpolación con corrección por w (0..2^P): piso(pos·w0·2^P / ((len - pos)·w1 + pos·w0)), o
 * piso(pos·2^P / len) si las w son iguales. Exacto y en 32 bits: un cociente aproximado en coma flotante y una corrección
 * entera (a·2^P - f·d se calcula módulo 2^32, y su valor real cabe en 32 bits con signo porque d < 2^26); el resultado no
 * depende del redondeo de la coma flotante, así que es el mismo en el SPE, el PPE y el host. Sin divisiones enteras (en el
 * SPE son de software) ni aritmética de 64 bits. tools: comprobado contra la fórmula de 64 bits en 4·10^8 casos. */
R3D_INLINE uint32_t r3d_pfac(uint32_t pos, uint32_t len, uint32_t w0, uint32_t w1, uint32_t P)
{
	uint32_t a, d;
	int32_t f, r;

	if (len == 0)
		return 0;
	if (pos >= len)
		return 1u << P;
	if (w0 == w1) {
		a = pos;
		d = len;
	} else {
		a = pos * w0;
		d = (len - pos) * w1 + a;
	}
	f = (int32_t)((float)a * (float)(1u << P) / (float)d);
	r = (int32_t)((a << P) - (uint32_t)f * d);
	if (r < 0)
		f--;
	else if ((uint32_t)r >= d)
		f++;
	return (uint32_t)f;
}

R3D_INLINE int32_t r3d_lerp(int32_t a, int32_t b, uint32_t f, uint32_t P)
{
	return (a * (int32_t)((1u << P) - f) + b * (int32_t)f) >> P;
}

/* Igual, con operandos de 16 bits (s, t y el color con signo; w sin signo): en el SPE son productos de 16 × 16 bits
 * (una instrucción) en lugar de los de 32. Mismo resultado mientras quepan (r3d_lerp lo dice arriba). */
R3D_INLINE int32_t r3d_lerp16(int32_t a, int32_t b, uint32_t f, uint32_t P)
{
	return ((int32_t)(int16_t)a * (int32_t)(int16_t)((1u << P) - f) + (int32_t)(int16_t)b * (int32_t)(int16_t)f) >> P;
}
R3D_INLINE uint32_t r3d_lerpu16(uint32_t a, uint32_t b, uint32_t f, uint32_t P)
{
	return ((uint32_t)(uint16_t)a * (uint32_t)(uint16_t)((1u << P) - f) + (uint32_t)(uint16_t)b * (uint32_t)(uint16_t)f) >> P;
}

/* a + piso((b - a)·pos / len), exacto, con |b - a|·pos < 2^31 y 0 < len (estimación en coma flotante y corrección
 * entera: no depende del redondeo de la coma flotante; sin divisiones enteras, que en el SPE son de software). */
R3D_INLINE int32_t r3d_lin(int32_t a, int32_t b, uint32_t pos, uint32_t len)
{
	const int32_t num = (b - a) * (int32_t)pos, d = (int32_t)len;
	int32_t q = (int32_t)((float)num / (float)d), r = num - q * d;

	while (r < 0) {
		q--;
		r += d;
	}
	while (r >= d) {
		q++;
		r -= d;
	}
	return a + q;
}

/* za + trunc((zb - za)·pos / len) con 0 <= pos < len, sin aritmética de 64 bits (en el SPE la división de 64 bits es de
 * software y cuesta cientos de ciclos): cociente y resto del salto entre len, piso(|zb - za|·pos / len) = q·pos + piso(r·pos / len). */
R3D_INLINE uint32_t r3d_zlerp(uint32_t za, uint32_t zb, uint32_t pos, uint32_t len)
{
	const uint32_t adz = zb >= za ? zb - za : za - zb;
	const uint32_t q = adz / len, r = adz - q * len;
	const uint32_t t = q * pos + (r * pos) / len;

	return zb >= za ? za + t : za - t;
}

static __attribute__((noinline)) void r3d_end_at(r3d_end *e, const r3d_v *a, const r3d_v *b, uint32_t pos, uint32_t len)
{
	uint32_t f;

	if (len == 0 || pos >= len) {
		const r3d_v *v = len == 0 ? a : b;

		e->s = v->s; e->t = v->t; e->r = v->r; e->g = v->g; e->b = v->b; e->w = v->w;
		e->z = len ? b->z : a->z;
		return;
	}
	if (a->w == b->w) { /* w iguales (ortográfica): interpolación lineal exacta */
		e->s = r3d_lin(a->s, b->s, pos, len);
		e->t = r3d_lin(a->t, b->t, pos, len);
		e->r = r3d_lin(a->r, b->r, pos, len);
		e->g = r3d_lin(a->g, b->g, pos, len);
		e->b = r3d_lin(a->b, b->b, pos, len);
		e->w = a->w;
		e->z = r3d_zlerp(a->z, b->z, pos, len);
		return;
	}
	f = r3d_pfac(pos, len, a->w, b->w, 9);
	e->s = r3d_lerp(a->s, b->s, f, 9);
	e->t = r3d_lerp(a->t, b->t, f, 9);
	e->r = r3d_lerp(a->r, b->r, f, 9);
	e->g = r3d_lerp(a->g, b->g, f, 9);
	e->b = r3d_lerp(a->b, b->b, f, 9);
	e->w = (uint32_t)r3d_lerp((int32_t)a->w, (int32_t)b->w, f, 9);
	if (e->w == 0)
		e->w = 1;
	/* z lineal en pantalla */
	e->z = len ? r3d_zlerp(a->z, b->z, pos, len) : a->z;
}

/* w y z de la arista en la línea `ypos` (0..len) de su recorrido en y, con la corrección por w. Para las aristas x-mayores con
 * perspectiva: la línea siguiente (ypos = y + 1 - a.y), medido contra melonDS con las escenas 28 a 33 de gx3d. */
static __attribute__((noinline)) void r3d_end_wz(r3d_end *e, const r3d_v *a, const r3d_v *b, uint32_t ypos, uint32_t len)
{
	uint32_t f;

	if (ypos >= len) {
		e->w = b->w;
		e->z = b->z;
		return;
	}
	f = r3d_pfac(ypos, len, a->w, b->w, 9);
	e->w = (uint32_t)r3d_lerp((int32_t)a->w, (int32_t)b->w, f, 9);
	if (e->w == 0)
		e->w = 1;
	e->z = r3d_zlerp(a->z, b->z, ypos, len);
}

static inline void r3d_end_of(r3d_end *e, const r3d_v *a)
{
	e->s = a->s;
	e->t = a->t;
	e->r = a->r;
	e->g = a->g;
	e->b = a->b;
	e->w = a->w;
	e->z = a->z;
}

/* Arista de una cadena que contiene la línea y (a.y <= y < b.y). 0 si no hay. */
static inline int r3d_chain_edge(const r3d_poly *p, int dir, int32_t y, const r3d_v **pa, const r3d_v **pb)
{
	int i = p->top;

	for (int k = 0; k < p->n; k++) {
		int j = i + dir;
		const r3d_v *a, *b;

		if (j < 0)
			j += p->n;
		else if (j >= p->n)
			j -= p->n;
		a = &p->v[i];
		b = &p->v[j];

		if (b->y > y && a->y <= y) {
			*pa = a;
			*pb = b;
			return 1;
		}
		if (b->y < a->y) /* empezó a subir: la cadena terminó */
			return 0;
		i = j;
	}
	return 0;
}

/* x de una arista en la línea y, en punto fijo de 18 bits: x0·2^18 + pendiente·(y - y0), con la pendiente truncada hacia
 * cero (por eso las aristas de pendiente positiva quedan un poco a la izquierda de la recta exacta y las de negativa un
 * poco a la derecha). [melonDS] */
R3D_INLINE int32_t r3d_xfix(const r3d_v *a, const r3d_v *b, int32_t y)
{
	/* en 32 bits: |dx| <= 1023 da |pendiente·(y - y0)| <= |dx|·2^18 < 2^28 */
	const int32_t slope = ((b->x - a->x) * 262144) / (b->y - a->y);

	return a->x * 262144 + slope * (y - a->y);
}

R3D_INLINE int32_t abs32(int32_t v)
{
	return v < 0 ? -v : v;
}

R3D_INLINE int32_t r3d_fx(int32_t v)
{
	return v >> 18; /* desplazamiento aritmético: piso */
}

/* ¿Arista y-mayor? Con |dx| = dy (45°) la DS la trata como x-mayor si va hacia la izquierda al bajar (dx < 0) y como y-mayor si va
 * hacia la derecha: medido contra melonDS con las escenas 0, 13, 14, 23 y 25 de gx3d (docs/fase3.md, 3.4). */
R3D_INLINE int r3d_ymajor(int32_t dx, int32_t dy)
{
	const int32_t adx = dx < 0 ? -dx : dx;

	return adx < dy || (adx == dy && dx > 0);
}

/* Bordes de la línea y con una arista: la izquierda da el primer píxel (y el final de su tramo de arista), la derecha
 * el final exclusivo (y el principio de su tramo). Modelo ajustado a melonDS con la escena 13 de la ROM gx3d
 * (tools/r3d-fit.py; docs/fase3.md, 3.2):
 *   y-mayor (|dx| < dy, o |dx| = dy hacia la derecha):  izquierda piso(X(y)) (con 1/2^18 menos si la pendiente es negativa); derecha piso(X(y))
 *   x-mayor:  la x de la línea siguiente redondeada: izquierda redondeo(X(y+1) - 1/2^18); derecha redondeo(X(y+1)) si
 *             va a la derecha, redondeo(X(y+1) - 1/2^18) si va a la izquierda; en la última línea de la arista, x1
 *   tramo de arista «gordo»: una izquierda x-mayor que va a la izquierda cubre hasta redondeo(X(y)); una derecha x-mayor
 *   que va a la derecha empieza en redondeo(X(y)). */
static inline void r3d_edge_x(const r3d_v *a, const r3d_v *b, int32_t y, int right, int32_t *xb, int32_t *xrun, r3d_end *e)
{
	const int32_t dy = b->y - a->y, dx = b->x - a->x, adx = dx < 0 ? -dx : dx;
	const int32_t xf = r3d_xfix(a, b, y);
	const int32_t half = 1 << 17;

	if (r3d_ymajor(dx, dy)) { /* y-mayor */
		*xb = r3d_fx(xf + (!right && dx < 0 ? -1 : 0));
		*xrun = right ? 0x7FFFFFFF : -0x7FFFFFFF; /* sin tramo gordo */
		r3d_end_at(e, a, b, (uint32_t)(y - a->y), (uint32_t)dy);
	} else {         /* x-mayor */
		const int last = y + 1 >= b->y;
		const int32_t xn = last ? 0 : r3d_xfix(a, b, y + 1);
		int32_t pos;

		if (!right)
			*xb = last ? b->x : r3d_fx(xn - 1 + half);
		else
			*xb = last ? b->x : r3d_fx(xn + (dx < 0 ? -1 : 0) + half);
		/* tramo de arista: hasta (izquierda) o desde (derecha) la x de esta línea, si la arista va hacia afuera */
		if (!right)
			*xrun = dx < 0 ? r3d_fx(xf - 1 + half) : -0x7FFFFFFF;
		else
			*xrun = dx > 0 ? r3d_fx(xf + half) : 0x7FFFFFFF;
		pos = *xb - a->x;
		if (pos < 0)
			pos = -pos;
		if (pos > adx)
			pos = adx;
		if (a->w == b->w) /* ortográfica: los atributos siguen la y, en el extremo de afuera del tramo de la línea [melonDS] */
			r3d_end_at(e, a, b, (uint32_t)(y - a->y + ((right ? dx > 0 : dx < 0) ? 1 : 0)), (uint32_t)dy);
		else {
			r3d_end_at(e, a, b, (uint32_t)pos, (uint32_t)adx);
			/* la w y la z de la arista siguen la y, no la x (docs/fase3.md, 3.5): al seguir la x del píxel entero saltaban de
			 * una línea a otra y dos polígonos casi coplanares (el parche de tierra sobre el suelo del mapa) se perforaban */
			r3d_end_wz(e, a, b, (uint32_t)(y - a->y + 1), (uint32_t)dy);
		}
	}
}

/* La línea y del polígono. 0 si no cubre nada. */
static inline int r3d_row_at(const r3d_poly *p, int32_t y, r3d_row *row)
{
	const r3d_v *la, *lb, *ra, *rb;
	int32_t xl, xle, xr, xrs;

	if (p->ytop == p->ybot) { /* altura cero: una línea de la x mínima a la máxima [sin contrastar] */
		int il = 0, ir = 0;

		if (y != p->ytop)
			return 0;
		for (int i = 1; i < p->n; i++) {
			if (p->v[i].x < p->v[il].x)
				il = i;
			if (p->v[i].x > p->v[ir].x)
				ir = i;
		}
		row->xl = p->v[il].x;
		row->xr = p->v[ir].x + 1;
		row->xle = row->xl + 1;
		row->xrs = row->xr - 1;
		r3d_end_of(&row->l, &p->v[il]);
		r3d_end_of(&row->r, &p->v[ir]);
		row->alledge = 1;
		row->den = row->xr - row->xl;
		return 1;
	}
	if (y < p->ytop || y >= p->ybot)
		return 0;
	if (!r3d_chain_edge(p, p->ldir, y, &la, &lb) || !r3d_chain_edge(p, -p->ldir, y, &ra, &rb))
		return 0;
	r3d_edge_x(la, lb, y, 0, &xl, &xle, &row->l);
	r3d_edge_x(ra, rb, y, 1, &xr, &xrs, &row->r);
	/* los tramos gordos de las aristas x-mayores que van hacia afuera */
	if (xle > xr)
		xr = xle;
	if (xrs < xl)
		xl = xrs;
	if (xr == xl) /* línea vacía por redondeo: un píxel */
		xr = xl + 1;
	else if (xr < xl)
		return 0;
	row->xl = xl;
	row->xle = xle > xl + 1 ? (xle < xr ? xle : xr) : xl + 1;
	row->xrs = xrs < xr - 1 ? (xrs > xl ? xrs : xl) : xr - 1;
	row->xr = xr;
	row->alledge = y == p->ytop || y == p->ybot - 1;
	/* Denominador de la interpolación a lo largo de la línea: xr - xl, y uno más cuando la arista derecha es inclinada y
	 * y-mayor (medido contra melonDS con paralelogramos, escena 22 de gx3d; con la derecha vertical o x-mayor no cambia). */
	row->den = row->xr - row->xl + ((rb->x != ra->x && abs32(rb->x - ra->x) <= rb->y - ra->y) ? 1 : 0);
	return 1;
}

/* ---- texturas -------------------------------------------------------------------------------- */

/* Coordenada de texel según repetir/espejo/recorte (GBATEK, TEXIMAGE_PARAM bits 16-19) */
R3D_INLINE uint32_t r3d_wrap(int32_t c, uint32_t l, uint32_t rep, uint32_t flip)
{
	const int32_t n = (int32_t)1 << l;

	if (!rep)
		return (uint32_t)(c < 0 ? 0 : c >= n ? n - 1 : c);
	if (flip)
		return (uint32_t)(((c >> l) & 1) ? (n - 1) - (c & (n - 1)) : (c & (n - 1)));
	return (uint32_t)c & (uint32_t)(n - 1);
}

/* Lectores de bytes de la memoria de texturas y de paletas: los da quien llama (VRAM directa o caché de líneas). */
typedef uint32_t (*r3d_rd8)(void *ctx, uint32_t addr);

R3D_INLINE uint32_t r3d_rd16(r3d_rd8 rd, void *ctx, uint32_t a)
{
	return rd(ctx, a) | (rd(ctx, a + 1) << 8);
}

/* El texel (s, t) ya acotado, como R3D_PX(r6, g6, b6, a5). Los colores de paleta y directos tienen 5 bits y se
 * extienden a 6 como los de los vértices. */
static inline uint32_t r3d_decode(const r3d_tex *tx, uint32_t s, uint32_t t, r3d_rd8 rdt, r3d_rd8 rdp, void *ctx)
{
	const uint32_t S = 1u << tx->sl, i = t * S + s;

	switch (tx->fmt) {
	case 1: { /* A3I5 */
		const uint32_t b = rdt(ctx, tx->addr + i), a3 = b >> 5;

		return r3d_555(r3d_rd16(rdp, ctx, tx->pal + (b & 31u) * 2u), (a3 << 2) + (a3 >> 1));
	}
	case 2: { /* 4 colores */
		const uint32_t idx = (rdt(ctx, tx->addr + (i >> 2)) >> ((i & 3u) * 2u)) & 3u;

		if (idx == 0 && tx->c0t)
			return 0;
		return r3d_555(r3d_rd16(rdp, ctx, tx->pal + idx * 2u), 31);
	}
	case 3: { /* 16 colores */
		const uint32_t idx = (rdt(ctx, tx->addr + (i >> 1)) >> ((i & 1u) * 4u)) & 15u;

		if (idx == 0 && tx->c0t)
			return 0;
		return r3d_555(r3d_rd16(rdp, ctx, tx->pal + idx * 2u), 31);
	}
	case 4: { /* 256 colores */
		const uint32_t idx = rdt(ctx, tx->addr + i);

		if (idx == 0 && tx->c0t)
			return 0;
		return r3d_555(r3d_rd16(rdp, ctx, tx->pal + idx * 2u), 31);
	}
	case 5: { /* comprimido 4x4 */
		const uint32_t blk = (t >> 2) * (S >> 2) + (s >> 2);
		const uint32_t da = tx->addr + blk * 4u;
		const uint32_t idx = (rdt(ctx, da + (t & 3u)) >> ((s & 3u) * 2u)) & 3u;
		/* los índices de paleta: ranura 1 (datos en la ranura 0: 0x20000 + d/2; en la 2: 0x30000 + d/2) */
		const uint32_t ia = 0x20000u + ((da & 0x1FFFFu) >> 1) + (((da >> 18) & 1u) ? 0x10000u : 0u);
		const uint32_t pi = r3d_rd16(rdt, ctx, ia), mode = pi >> 14;
		const uint32_t pa = tx->pal + (pi & 0x3FFFu) * 4u;
		uint32_t c0, c1, cr = 0;

		if (idx == 3 && mode < 2)
			return 0;
		if (mode == 0 || mode == 2 || idx < 2)
			return r3d_555(r3d_rd16(rdp, ctx, pa + idx * 2u), 31);
		c0 = r3d_rd16(rdp, ctx, pa);
		c1 = r3d_rd16(rdp, ctx, pa + 2u);
		for (uint32_t k = 0; k < 15; k += 5) {
			const uint32_t a = (c0 >> k) & 31u, b = (c1 >> k) & 31u;
			uint32_t v;

			if (mode == 1)
				v = (a + b) >> 1;
			else if (idx == 2)
				v = (a * 5u + b * 3u) >> 3;
			else
				v = (a * 3u + b * 5u) >> 3;
			cr |= v << k;
		}
		return r3d_555(cr, 31);
	}
	case 6: { /* A5I3 */
		const uint32_t b = rdt(ctx, tx->addr + i);

		return r3d_555(r3d_rd16(rdp, ctx, tx->pal + (b & 7u) * 2u), b >> 3);
	}
	default: { /* 7: directo */
		const uint32_t c = r3d_rd16(rdt, ctx, tx->addr + i * 2u);

		return r3d_555(c & 0x7FFFu, (c & 0x8000u) ? 31u : 0u);
	}
	}
}

/* Busca el texel (s, t) acotado: lo da quien llama (referencia: r3d_decode directo; SPE: caché de bloques). */
typedef uint32_t (*r3d_texfn)(void *ctx, const r3d_tex *tx, uint32_t s, uint32_t t);

/* ---- píxeles ---------------------------------------------------------------------------------------- */

R3D_INLINE uint32_t r3d_mod6(uint32_t a, uint32_t b)
{
	return ((a + 1u) * (b + 1u) - 1u) >> 6;
}

/* Color y alfa del píxel según el modo del polígono. vc = color del vértice (R3D_PX con alfa 0). */
R3D_INLINE uint32_t r3d_shade(const r3d_poly *p, const r3d_rstate *rs, uint32_t vr, uint32_t vg, uint32_t vb, uint32_t tex,
			      int textured)
{
	const uint32_t pa = p->alpha;
	uint32_t r = vr, g = vg, b = vb, a = pa;

	switch (p->mode) {
	case 1: /* decal */
		if (textured) {
			const uint32_t ta = R3D_PX_A(tex);

			if (ta == 0) /* texel transparente: el color del vértice tal cual; opaco: el del texel [melonDS] */
				return R3D_PX(vr, vg, vb, a);
			if (ta == 31u)
				return R3D_PX(R3D_PX_R(tex), R3D_PX_G(tex), R3D_PX_B(tex), a);
			r = (R3D_PX_R(tex) * ta + vr * (31u - ta)) >> 5;
			g = (R3D_PX_G(tex) * ta + vg * (31u - ta)) >> 5;
			b = (R3D_PX_B(tex) * ta + vb * (31u - ta)) >> 5;
		}
		return R3D_PX(r, g, b, a);
	case 2: { /* toon o highlight: el rojo del vértice indexa la tabla */
		const uint32_t tc = rs->toon[vr >> 1];

		if (rs->d3 & D3_HIGHLIGHT) {
			r = g = b = vr;
		} else {
			r = R3D_PX_R(tc);
			g = R3D_PX_G(tc);
			b = R3D_PX_B(tc);
		}
		if (textured) {
			r = r3d_mod6(R3D_PX_R(tex), r);
			g = r3d_mod6(R3D_PX_G(tex), g);
			b = r3d_mod6(R3D_PX_B(tex), b);
			a = ((R3D_PX_A(tex) + 1u) * (pa + 1u) - 1u) >> 5;
		}
		if (rs->d3 & D3_HIGHLIGHT) {
			r += R3D_PX_R(tc);
			g += R3D_PX_G(tc);
			b += R3D_PX_B(tc);
			r = r > 63 ? 63 : r;
			g = g > 63 ? 63 : g;
			b = b > 63 ? 63 : b;
		}
		return R3D_PX(r, g, b, a);
	}
	default: /* modulate (y sombra) */
		if (textured) {
			r = r3d_mod6(R3D_PX_R(tex), vr);
			g = r3d_mod6(R3D_PX_G(tex), vg);
			b = r3d_mod6(R3D_PX_B(tex), vb);
			a = ((R3D_PX_A(tex) + 1u) * (pa + 1u) - 1u) >> 5;
		}
		return R3D_PX(r, g, b, a);
	}
}

/* Prueba de profundidad: «menor» o «igual» (con margen: 0x200 en Z, 0xFF en W) [sin contrastar] */
R3D_INLINE int r3d_depth_ok(const r3d_poly *p, const r3d_rstate *rs, uint32_t d, uint32_t dst, uint32_t dst_att)
{
	if (p->attr & PA_DEQUAL) {
		const uint32_t m = rs->wbuf ? 0xFFu : 0x200u;

		return d + m >= dst && d <= dst + m;
	}
	/* «menor»: un polígono frontal también gana el empate sobre una profundidad de espalda [medido contra melonDS, gx3d 26 y 27] */
	if (p->front && (dst_att & AT_BACK))
		return d <= dst;
	return d < dst;
}

/* Interpolación lineal exacta de un atributo a lo largo de la línea: v(k) = a + piso((b - a)·k / L). Cociente y resto
 * por paso (sin divisiones por píxel). Se empieza en k0 por el recorte a la pantalla. */
typedef struct r3d_dda {
	int32_t v, q;
	uint32_t r, acc;
} r3d_dda;

static __attribute__((noinline)) void r3d_dda_init(r3d_dda *d, int32_t a, int32_t b, int32_t L, uint32_t k0)
{
	const int32_t Ld = L > 0 ? L : 1, q = r3d_floordiv(b - a, Ld);

	d->q = q;
	d->r = (uint32_t)((b - a) - q * Ld);
	d->v = a + q * (int32_t)k0 + (int32_t)((d->r * k0) / (uint32_t)Ld);
	d->acc = (d->r * k0) % (uint32_t)Ld;
}

R3D_INLINE int32_t r3d_dda_step(r3d_dda *d, int32_t L)
{
	const int32_t cur = d->v;

	d->v += d->q;
	d->acc += d->r;
	if (d->acc >= (uint32_t)L) {
		d->acc -= (uint32_t)L;
		d->v++;
	}
	return cur;
}

/* Dibuja la línea y del polígono en los búferes de esa línea (col, dep, att: 256 entradas cada uno). */
static inline __attribute__((always_inline)) void r3d_draw_row(const r3d_poly *p, const r3d_row *row, const r3d_rstate *rs, uint32_t *col, uint32_t *dep,
				uint32_t *att, r3d_texfn tf, void *tctx, r3d_stats *st)
{
	const int32_t L = row->den, Lpix = row->xr - row->xl; (void)Lpix;
	int32_t x0 = row->xl < 0 ? 0 : row->xl, x1 = row->xr > R3D_W ? R3D_W : row->xr;
	const int textured = (int)p->textured;
	const uint32_t shadow = p->mode == 3;
	const uint32_t wl = row->l.w, wr = row->r.w;
	const uint32_t zl = row->l.z, zr = row->r.z;
	/* z lineal, z(k) = zl + trunc((zr - zl)·k / L), exacta y en 32 bits: cociente y resto por paso */
	const uint32_t adz = zr >= zl ? zr - zl : zl - zr, zneg = zr < zl, uL = L > 0 ? (uint32_t)L : 1u;
	const uint32_t zq = adz / uL, zr_ = adz % uL;

	/* con las w iguales el factor es lineal, piso(k·2^P / L): también por cociente y resto, sin dividir por píxel */
	const uint32_t lin = wl == wr, fq = (1u << R3D_PS) / uL, fr_ = (1u << R3D_PS) % uL;
	uint32_t zu, zacc, fu, facc, written = 0;
	/* con las w iguales los atributos son lineales y exactos: a + piso((b - a)·k / L), por cociente y resto por paso */
	r3d_dda dr = { 0, 0, 0, 0 }, dg = dr, db = dr, ds = dr, dt = dr;

	if (x0 >= x1)
		return;
	{
		const uint32_t k0 = (uint32_t)(x0 - row->xl);

		if (lin) {
			r3d_dda_init(&dr, row->l.r, row->r.r, L, k0);
			r3d_dda_init(&dg, row->l.g, row->r.g, L, k0);
			r3d_dda_init(&db, row->l.b, row->r.b, L, k0);
			r3d_dda_init(&ds, row->l.s, row->r.s, L, k0);
			r3d_dda_init(&dt, row->l.t, row->r.t, L, k0);
		}

		zu = zq * k0 + (zr_ * k0) / uL;
		zacc = (zr_ * k0) % uL;
		fu = fq * k0 + (fr_ * k0) / uL;
		facc = (fr_ * k0) % uL;
	}
	st->pixels += (uint32_t)(x1 - x0);
	for (int32_t x = x0; x < x1; x++) {
		const uint32_t k = (uint32_t)(x - row->xl);
		const int edge = row->alledge || x < row->xle || x >= row->xrs;
		uint32_t f, d, a0, c;
		const uint32_t zcur = zu, fcur = fu;

		uint32_t lr = 0, lg = 0, lb = 0;
		int32_t ls = 0, lt = 0;

		if (lin) {
			lr = (uint32_t)r3d_dda_step(&dr, L);
			lg = (uint32_t)r3d_dda_step(&dg, L);
			lb = (uint32_t)r3d_dda_step(&db, L);
			if (textured) {
				ls = r3d_dda_step(&ds, L);
				lt = r3d_dda_step(&dt, L);
			}
		}
		zu += zq;
		zacc += zr_;
		if (zacc >= uL) {
			zacc -= uL;
			zu++;
		}
		fu += fq;
		facc += fr_;
		if (facc >= uL) {
			facc -= uL;
			fu++;
		}
		if (p->wire && !edge)
			continue;
		f = lin ? fcur : r3d_pfac(k, (uint32_t)L, wl, wr, R3D_PS);
		if (rs->wbuf) {
			/* w << wshift cabe en 32 bits: la w normalizada nunca pasa de la del vértice más lejano */
			const uint32_t w = r3d_lerpu16(wl, wr, f, R3D_PS) << p->wshift;

			d = w > 0xFFFFFFu ? 0xFFFFFFu : w;
		} else {
			d = zneg ? zl - zcur : zl + zcur;
		}
		a0 = att[x];
		if (shadow && p->id == 0) { /* máscara de sombra: marca donde la prueba de profundidad FALLA */
			if (!r3d_depth_ok(p, rs, d, dep[x], a0))
				att[x] = a0 | AT_STENCIL;
			continue;
		}
		if (shadow) {
			/* sombra: solo donde la máscara marcó y sobre otro id de opaco [sin contrastar] */
			if (!(a0 & AT_STENCIL))
				continue;
			att[x] = a0 &= ~AT_STENCIL;
			if (AT_OID(a0) == p->id)
				continue;
		}
		if (!r3d_depth_ok(p, rs, d, dep[x], a0))
			continue;
		{
			const uint32_t fc = f >> (R3D_PS - R3D_PC); /* en perspectiva el color con menos bits (8) [melonDS] */
			const uint32_t vr = lin ? lr >> 3 : (uint32_t)r3d_lerp16(row->l.r, row->r.r, fc, R3D_PC) >> 3;
			const uint32_t vg = lin ? lg >> 3 : (uint32_t)r3d_lerp16(row->l.g, row->r.g, fc, R3D_PC) >> 3;
			const uint32_t vb = lin ? lb >> 3 : (uint32_t)r3d_lerp16(row->l.b, row->r.b, fc, R3D_PC) >> 3;
			uint32_t tex = 0;

			if (textured) {
				const int32_t s = lin ? ls >> 4 : r3d_lerp16(row->l.s, row->r.s, f, R3D_PS) >> 4;
				const int32_t t = lin ? lt >> 4 : r3d_lerp16(row->l.t, row->r.t, f, R3D_PS) >> 4;

				tex = tf(tctx, &p->tex, r3d_wrap(s, p->tex.sl, p->tex.rs, p->tex.fs),
					 r3d_wrap(t, p->tex.tl, p->tex.rt, p->tex.ft));
			}
			c = r3d_shade(p, rs, vr, vg, vb, tex, textured);
		}
		{
			const uint32_t a = R3D_PX_A(c);

			if (a == 0)
				continue;
			if ((rs->d3 & D3_ALPHATEST) && a <= rs->alpha_ref)
				continue;
			if (a == 31 && !shadow) {
				col[x] = c;
				dep[x] = d;
				att[x] = (a0 & AT_STENCIL) | p->id | ((p->attr & PA_FOG) ? AT_FOG : 0u) | (edge ? AT_EDGE : 0u) |
					 (p->front ? 0u : AT_BACK);
			} else {
				const uint32_t dc = col[x], da = R3D_PX_A(dc);

				if ((a0 & AT_TRANS) && AT_TID(a0) == p->id)
					continue;
				if ((rs->d3 & D3_BLEND) && da != 0) {
					const uint32_t ia = 31u - a, sa = a + 1u;
					const uint32_t r = (R3D_PX_R(c) * sa + R3D_PX_R(dc) * ia) >> 5;
					const uint32_t g = (R3D_PX_G(c) * sa + R3D_PX_G(dc) * ia) >> 5;
					const uint32_t b = (R3D_PX_B(c) * sa + R3D_PX_B(dc) * ia) >> 5;

					c = R3D_PX(r, g, b, a > da ? a : da);
				}
				col[x] = c;
				if (p->attr & PA_TDEPTH)
					dep[x] = d;
				att[x] = (a0 & ~(uint32_t)(AT_FOG | (63u << 6) | ((p->attr & PA_TDEPTH) ? AT_BACK : 0u))) | AT_TRANS |
					 (p->id << 6) | (((a0 & AT_FOG) && (p->attr & PA_FOG)) ? AT_FOG : 0u) |
					 (((p->attr & PA_TDEPTH) && !p->front) ? AT_BACK : 0u);
			}
			written++;
		}
	}
	st->written += written;
}

/* ---- pasada final por línea: edge marking y niebla -------------------------------------------------- */

/* Edge marking de la línea y: `up` y `dn` son las líneas vecinas (o NULL fuera de la pantalla: cuenta el borrado). */
static inline void r3d_edge_row(const r3d_rstate *rs, uint32_t *col, const uint32_t *dep, const uint32_t *att,
				const uint32_t *dep_up, const uint32_t *att_up, const uint32_t *dep_dn, const uint32_t *att_dn)
{
	const uint32_t cid = AT_OID(rs->clear_attr), cdep = rs->clear_depth;

	for (int x = 0; x < R3D_W; x++) {
		const uint32_t a = att[x], id = AT_OID(a), d = dep[x];
		int mark = 0;

		if (!(a & AT_EDGE))
			continue;
		/* vecino con otro id y más lejos [sin contrastar] */
		{
			const uint32_t ni[4] = { x > 0 ? AT_OID(att[x - 1]) : cid, x < R3D_W - 1 ? AT_OID(att[x + 1]) : cid,
						 att_up ? AT_OID(att_up[x]) : cid, att_dn ? AT_OID(att_dn[x]) : cid };
			const uint32_t nd[4] = { x > 0 ? dep[x - 1] : cdep, x < R3D_W - 1 ? dep[x + 1] : cdep,
						 dep_up ? dep_up[x] : cdep, dep_dn ? dep_dn[x] : cdep };

			for (int k = 0; k < 4; k++)
				if (ni[k] != id && d < nd[k])
					mark = 1;
		}
		if (mark)
			col[x] = (rs->edge[id >> 3] & 0x00FFFFFFu) | (col[x] & 0xFF000000u);
	}
}

/* Densidad de niebla (0..128) para una profundidad de 24 bits */
R3D_INLINE uint32_t r3d_fog_density(const r3d_rstate *rs, uint32_t depth)
{
	const int32_t z = (int32_t)(depth >> 9), k = z - (int32_t)rs->fog_offset;
	const uint32_t sh = rs->fog_shift > 10 ? 0u : 10u - rs->fog_shift;
	uint32_t i, d;

	if (k < 0) {
		d = rs->fog_table[0];
	} else {
		i = (uint32_t)k >> sh;
		if (i >= 31) {
			d = rs->fog_table[31];
		} else {
			const int32_t t0 = rs->fog_table[i], t1 = rs->fog_table[i + 1];
			const int32_t fr = (int32_t)((uint32_t)k & ((1u << sh) - 1u));

			d = (uint32_t)(t0 + (((t1 - t0) * fr) >> sh));
		}
	}
	return d == 127 ? 128u : d;
}

static inline void r3d_fog_row(const r3d_rstate *rs, uint32_t *col, const uint32_t *dep, const uint32_t *att)
{
	const uint32_t fc = rs->fog_px, ao = (rs->d3 & D3_FOG_ALPHA) != 0;

	for (int x = 0; x < R3D_W; x++) {
		const uint32_t c = col[x];
		uint32_t d, id, r, g, b, a;

		if (!(att[x] & AT_FOG))
			continue;
		d = r3d_fog_density(rs, dep[x]);
		id = 128u - d;
		a = (R3D_PX_A(fc) * d + R3D_PX_A(c) * id) >> 7;
		if (ao) {
			col[x] = (c & 0x00FFFFFFu) | (a << 24);
			continue;
		}
		r = (R3D_PX_R(fc) * d + R3D_PX_R(c) * id) >> 7;
		g = (R3D_PX_G(fc) * d + R3D_PX_G(c) * id) >> 7;
		b = (R3D_PX_B(fc) * d + R3D_PX_B(c) * id) >> 7;
		col[x] = R3D_PX(r, g, b, a);
	}
}

#endif
