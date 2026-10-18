/*
 * SalsaDS — motor de geometría de la DS (Fase 3.1). Ver gx.h para el diseño, los formatos y el tiempo.
 *
 * Código propio escrito desde GBATEK; las decisiones donde GBATEK no alcanza (y que se tomaron comparando con melonDS
 * ejecutándolo, nunca leyendo su código) están en docs/fase3.md. Lo que no se pudo contrastar en una DS real se marca
 * «[sin contrastar]» en los comentarios.
 */
#include "gpu3d/gx.h"

#include <string.h>

#if defined(__SIZEOF_INT128__)
typedef __int128 gx_wide;
#else
typedef int64_t gx_wide;
#endif

/* ---- orden -> parámetros y coste (en ciclos del motor, 33,5 MHz) ------------------------------------------------ */

#define NP_BAD 0xFFu

static uint8_t g_np[0x80];
static int g_np_ready;

static void np_init(void)
{
	if (g_np_ready)
		return;
	memset(g_np, NP_BAD, sizeof g_np);
	static const struct {
		uint8_t cmd, np;
	} t[] = {
		{ 0x00, 0 }, { 0x10, 1 }, { 0x11, 0 }, { 0x12, 1 }, { 0x13, 1 }, { 0x14, 1 }, { 0x15, 0 }, { 0x16, 16 },
		{ 0x17, 12 }, { 0x18, 16 }, { 0x19, 12 }, { 0x1A, 9 }, { 0x1B, 3 }, { 0x1C, 3 }, { 0x20, 1 }, { 0x21, 1 },
		{ 0x22, 1 }, { 0x23, 2 }, { 0x24, 1 }, { 0x25, 1 }, { 0x26, 1 }, { 0x27, 1 }, { 0x28, 1 }, { 0x29, 1 },
		{ 0x2A, 1 }, { 0x2B, 1 }, { 0x30, 1 }, { 0x31, 1 }, { 0x32, 1 }, { 0x33, 1 }, { 0x34, 32 }, { 0x40, 1 },
		{ 0x41, 0 }, { 0x50, 1 }, { 0x60, 1 }, { 0x70, 3 }, { 0x71, 2 }, { 0x72, 1 },
	};
	for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
		g_np[t[i].cmd] = t[i].np;
	g_np_ready = 1;
}

/* Entradas de la FIFO que ocupa una orden: sus parámetros, o una si no tiene. */
static unsigned cmd_entries(uint8_t cmd)
{
	const unsigned np = g_np[cmd & 0x7F];

	return np ? np : 1u;
}

static int cmd_valid(uint32_t cmd)
{
	return cmd < 0x80 && g_np[cmd] != NP_BAD;
}

/* ---- matrices ----------------------------------------------------------------------------------------------- */

static void mat_ident(gx_mat *m)
{
	memset(m, 0, sizeof *m);
	m->m[0] = m->m[5] = m->m[10] = m->m[15] = 0x1000;
}

/* out = a x b (a por la izquierda: v' = v x a x b con v fila). Cada elemento suma cuatro productos en 64 bits y trunca
 * con >> 12 (aritmético); el resultado se guarda en 32 bits. */
static void mat_mul(gx_mat *out, const gx_mat *a, const gx_mat *b)
{
	gx_mat r;

	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			int64_t s = 0;

			for (int k = 0; k < 4; k++)
				s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
			r.m[i * 4 + j] = (int32_t)(s >> 12);
		}
	*out = r;
}

static void clip_update(gx_t *g)
{
	if (g->clip_dirty) {
		mat_mul(&g->clip, &g->pos, &g->proj);
		g->clip_dirty = 0;
	}
}

static void mtx_load(gx_t *g, const gx_mat *m)
{
	switch (g->mode) {
	case 0:
		g->proj = *m;
		g->clip_dirty = 1;
		break;
	case 1:
		g->pos = *m;
		g->clip_dirty = 1;
		break;
	case 2:
		g->pos = *m;
		g->vec = *m;
		g->clip_dirty = 1;
		break;
	default:
		g->tex = *m;
		break;
	}
}

/* Multiplica el operando por la izquierda de la matriz actual. MTX_SCALE no toca la vectorial en el modo 2. */
static void mtx_mult(gx_t *g, const gx_mat *op, int is_scale)
{
	switch (g->mode) {
	case 0:
		mat_mul(&g->proj, op, &g->proj);
		g->clip_dirty = 1;
		break;
	case 1:
		mat_mul(&g->pos, op, &g->pos);
		g->clip_dirty = 1;
		break;
	case 2:
		mat_mul(&g->pos, op, &g->pos);
		if (!is_scale)
			mat_mul(&g->vec, op, &g->vec);
		g->clip_dirty = 1;
		break;
	default:
		mat_mul(&g->tex, op, &g->tex);
		break;
	}
}

/* Operando de MTX_LOAD/MULT desde los parámetros: 4x4 (16), 4x3 (12: 4 filas de 3, última columna 0,0,0,1) o 3x3 (9:
 * 3 filas de 3; la fila de traslación es 0,0,0 y 1). */
static void mat_from_params(gx_mat *m, const uint32_t *p, int kind)
{
	mat_ident(m);
	switch (kind) {
	case 44:
		for (int i = 0; i < 16; i++)
			m->m[i] = (int32_t)p[i];
		break;
	case 43:
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++)
				m->m[r * 4 + c] = (int32_t)p[r * 3 + c];
		break;
	default:
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				m->m[r * 4 + c] = (int32_t)p[r * 3 + c];
		break;
	}
}

static int32_t sext(uint32_t v, unsigned bits)
{
	const unsigned sh = 32u - bits;

	return (int32_t)(v << sh) >> sh;
}

/* ---- recorte ------------------------------------------------------------------------------------------------- */

/* Distancia con signo de un vértice a cada uno de los seis planos del volumen de vista; dentro si >= 0. */
static int64_t plane_dist(const gx_cvtx *v, int plane)
{
	switch (plane) {
	case 0: return (int64_t)v->z + v->w; /* cercano */
	case 1: return (int64_t)v->w - v->z; /* lejano */
	case 2: return (int64_t)v->x + v->w; /* izquierdo */
	case 3: return (int64_t)v->w - v->x; /* derecho */
	case 4: return (int64_t)v->y + v->w; /* inferior */
	default: return (int64_t)v->w - v->y; /* superior */
	}
}

static unsigned outcode(const gx_cvtx *v)
{
	unsigned c = 0;

	for (int k = 0; k < 6; k++)
		if (plane_dist(v, k) < 0)
			c |= 1u << k;
	return c;
}

static int32_t lerp_field(int32_t a, int32_t b, int64_t da, int64_t db)
{
	return (int32_t)(a + (gx_wide)((int64_t)b - a) * da / (da - db));
}

static void clip_edge(gx_cvtx *o, const gx_cvtx *a, const gx_cvtx *b, int64_t da, int64_t db)
{
	o->x = lerp_field(a->x, b->x, da, db);
	o->y = lerp_field(a->y, b->y, da, db);
	o->z = lerp_field(a->z, b->z, da, db);
	o->w = lerp_field(a->w, b->w, da, db);
	o->s = lerp_field(a->s, b->s, da, db);
	o->t = lerp_field(a->t, b->t, da, db);
	o->r = lerp_field(a->r, b->r, da, db);
	o->g = lerp_field(a->g, b->g, da, db);
	o->b = lerp_field(a->b, b->b, da, db);
}

/* Sutherland-Hodgman contra los planos de `mask` (bit k = plano k). `in` y `out` de al menos GX_POLY_MAXV + 2
 * elementos; devuelve el número de vértices (puede ser 0) y deja el resultado en `out`. */
#define CLIP_BUF (GX_POLY_MAXV + 4u)
static int clip_polygon(const gx_cvtx *in, int n, gx_cvtx *out, unsigned mask)
{
	gx_cvtx buf[2][CLIP_BUF];
	int cur = 0;

	memcpy(buf[0], in, (size_t)n * sizeof *in);
	for (int k = 0; k < 6 && n > 0; k++) {
		int on = 0;
		const gx_cvtx *a = buf[cur];
		gx_cvtx *o = buf[cur ^ 1];

		if (!(mask & (1u << k)))
			continue;
		for (int i = 0; i < n; i++) {
			const gx_cvtx *p = &a[i], *q = &a[(i + 1) % n];
			const int64_t dp = plane_dist(p, k), dq = plane_dist(q, k);

			if (dp >= 0)
				o[on++] = *p;
			if ((dp >= 0) != (dq >= 0) && on < (int)CLIP_BUF)
				clip_edge(&o[on++], p, q, dp, dq);
		}
		cur ^= 1;
		n = on;
	}
	memcpy(out, buf[cur], (size_t)n * sizeof(gx_cvtx));
	return n;
}

/* ---- viewport y pantalla -------------------------------------------------------------------------------------- */

/* Pantalla de un vértice con `sh` bits fraccionarios extra (para decidir la cara con más precisión). Con w <= 0
 * (detrás del ojo, sólo si el polígono no se recortó) devuelve el origen del viewport. */
static void screen_pos(const gx_t *g, const gx_cvtx *v, unsigned sh, int64_t *sx, int64_t *sy)
{
	const int64_t x1 = g->viewport & 0xFF, y1 = (g->viewport >> 8) & 0xFF;
	const int64_t x2 = (g->viewport >> 16) & 0xFF, y2 = (g->viewport >> 24) & 0xFF;
	const int64_t vw = x2 - x1 + 1, vh = y2 - y1 + 1; /* anchura y altura (pueden ser <= 0 si x2 < x1; entonces 0) */
	const int64_t w = v->w;

	if (w <= 0 || vw <= 0 || vh <= 0) {
		*sx = x1 << sh;
		*sy = (191 - y2) << sh;
		return;
	}
	/* y se mide desde abajo (y = 191 - y); la y de recorte positiva va hacia arriba */
	*sx = (((int64_t)v->x + w) * vw * ((int64_t)1 << sh)) / (2 * w) + (x1 << sh);
	*sy = ((w - (int64_t)v->y) * vh * ((int64_t)1 << sh)) / (2 * w) + ((191 - y2) << sh);
}

static int16_t clamp16(int64_t v)
{
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void make_out(const gx_t *g, const gx_cvtx *v, gx_vertex *o)
{
	int64_t sx, sy, z;

	screen_pos(g, v, 0, &sx, &sy);
	o->sx = clamp16(sx);
	o->sy = clamp16(sy);
	if (v->w > 0) {
		z = ((int64_t)v->z * 0x4000) / v->w + 0x3FFF;
		z = z < 0 ? 0 : z > 0x7FFF ? 0x7FFF : z;
	} else {
		z = 0;
	}
	o->z = (uint32_t)z << 9;
	o->w = v->w;
	o->s = (int16_t)v->s;
	o->t = (int16_t)v->t;
	o->r = (uint8_t)v->r;
	o->g = (uint8_t)v->g;
	o->b = (uint8_t)v->b;
	o->pad = 0;
	o->rsv[0] = o->rsv[1] = 0;
}

/* ---- ensamblado y emisión de polígonos ------------------------------------------------------------------------ */

/* Recorta, descarta por cara y guarda un polígono de `n` vértices. Cada polígono lleva sus vértices propios en la RAM de
 * vértices (sin compartir los de las tiras): 6144 = 2048 triángulos x 3; sólo cuentan los polígonos que llegan a la
 * lista (los descartados por cara o por recorte no gastan RAM). */
static void emit_poly(gx_t *g, gx_cvtx *const *src, int n)
{
	gx_frame *f = &g->frame[g->bld];
	gx_cvtx pv[CLIP_BUF];
	const uint32_t attr = g->polyattr_cur;
	unsigned oc_and = 0x3Fu, oc_or = 0;
	int m = n, clipped = 0, front;
	int64_t area2 = 0;
	gx_poly *p;
	uint32_t wmax = 0;
	int16_t xmin = 32767, xmax = -32768, ymin = 32767, ymax = -32768;

	gx_cvtx in[4];

	for (int i = 0; i < n; i++) {
		unsigned c;

		in[i] = *src[i];
		c = outcode(&in[i]);
		oc_and &= c;
		oc_or |= c;
	}
	if (oc_and) { /* entero fuera de algún plano */
		g->st.rejected++;
		g->st.rej_outside++;
		return;
	}
	/* El plano lejano: si el polígono lo cruza y POLYGON_ATTR bit 12 es 0, no se dibuja; con 1 se recorta. [sin contrastar] */
	if ((oc_or & 2u) && !(attr & 0x1000u)) {
		g->st.rejected++;
		g->st.rej_far++;
		return;
	}
	if (oc_or) {
		m = clip_polygon(in, n, pv, 0x3Fu);
		if (m < 3) {
			g->st.rejected++;
			g->st.rej_empty++;
			return;
		}
		clipped = 1;
	} else {
		memcpy(pv, in, (size_t)n * sizeof in[0]);
	}
	/* Cara: signo del área en pantalla (con 8 bits fraccionarios extra). Frontal = antihorario a la vista = área < 0
	 * con y hacia abajo. Área 0 cuenta como frontal. [sin contrastar] */
	for (int i = 0; i < m; i++) {
		int64_t ax, ay, bx, by;

		screen_pos(g, &pv[i], 8, &ax, &ay);
		screen_pos(g, &pv[(i + 1) % m], 8, &bx, &by);
		area2 += ax * by - bx * ay;
	}
	front = area2 <= 0;
	if (front ? !(attr & 0x80u) : !(attr & 0x40u)) {
		g->st.culled++;
		return;
	}
	if (f->npolys >= GX_MAX_POLYS) {
		f->dropped_polys++;
		g->st.dropped_poly++;
		return;
	}
	{
		uint32_t nnew = 0;

		for (int i = 0; i < m; i++)
			nnew += clipped || src[i]->ram < 0;
		if (f->nverts + nnew > GX_MAX_VERTS) {
			f->dropped_verts += nnew;
			g->st.dropped_vert += nnew;
			return;
		}
	}
	p = &f->poly[f->npolys++];
	memset(p, 0, sizeof *p);
	p->attr = attr;
	p->texparam = g->texparam;
	p->pltt = g->pltt & 0x1FFFu;
	p->nverts = (uint8_t)m;
	p->flags = (uint8_t)((front ? GX_POLY_FRONT : 0) | (clipped ? GX_POLY_CLIPPED : 0));
	{
		const unsigned alpha = (attr >> 16) & 0x1Fu;

		if (alpha == 0)
			p->flags |= GX_POLY_WIRE;
		else if (alpha < 31)
			p->flags |= GX_POLY_TRANSLUCENT;
	}
	for (int i = 0; i < m; i++) {
		uint32_t id;
		gx_vertex *o;

		/* un polígono sin recortar reutiliza los vértices que ya guardó una tira; el recortado trae vértices nuevos */
		if (!clipped && src[i]->ram >= 0) {
			id = (uint32_t)src[i]->ram;
		} else {
			id = f->nverts++;
			make_out(g, &pv[i], &f->vtx[id]);
			if (!clipped)
				src[i]->ram = (int32_t)id;
		}
		o = &f->vtx[id];
		p->vidx[i] = (uint16_t)id;
		if (o->sx < xmin) xmin = o->sx;
		if (o->sx > xmax) xmax = o->sx;
		if (o->sy < ymin) ymin = o->sy;
		if (o->sy > ymax) ymax = o->sy;
		if (o->w > 0 && (uint32_t)o->w > wmax)
			wmax = (uint32_t)o->w;
	}
	p->xmin = xmin; p->xmax = xmax; p->ymin = ymin; p->ymax = ymax;
	/* w se normaliza a 16 bits desplazando de 4 en 4. [sin contrastar] */
	while ((wmax >> p->wshift) > 0xFFFFu)
		p->wshift = (uint8_t)(p->wshift + 4u);
	g->st.polys++;
	if (clipped)
		g->st.clipped++;
}

/* `c` es el vértice nuevo (local de quien llama); las tiras guardan una copia en la ventana `asm_v` y la comparten con el
 * siguiente polígono, de modo que el índice de RAM que se le dio al guardarla se hereda. */
static void asm_push(gx_t *g, gx_cvtx *c)
{
	gx_cvtx *a = g->asm_v;
	gx_cvtx *t[4];

	switch (g->prim & 3u) {
	case 0: /* triángulos */
		a[g->asm_n++] = *c;
		if (g->asm_n == 3) {
			t[0] = &a[0]; t[1] = &a[1]; t[2] = &a[2];
			emit_poly(g, t, 3);
			g->asm_n = 0;
		}
		break;
	case 1: /* cuadriláteros */
		a[g->asm_n++] = *c;
		if (g->asm_n == 4) {
			t[0] = &a[0]; t[1] = &a[1]; t[2] = &a[2]; t[3] = &a[3];
			emit_poly(g, t, 4);
			g->asm_n = 0;
		}
		break;
	case 2: /* tira de triángulos: el orden se invierte en los impares para conservar la cara */
		if (g->asm_n < 2) {
			a[g->asm_n++] = *c;
			break;
		}
		if (!g->asm_par) {
			t[0] = &a[0]; t[1] = &a[1];
		} else {
			t[0] = &a[1]; t[1] = &a[0];
		}
		t[2] = c;
		emit_poly(g, t, 3);
		g->asm_par ^= 1u;
		a[0] = a[1];
		a[1] = *c;
		break;
	default: /* tira de cuadriláteros: (a, b, d, c) */
		if (g->asm_n < 3) {
			a[g->asm_n++] = *c;
			break;
		}
		t[0] = &a[0]; t[1] = &a[1]; t[2] = c; t[3] = &a[2];
		emit_poly(g, t, 4);
		a[0] = a[2];
		a[1] = *c;
		g->asm_n = 2;
		break;
	}
}

/* ---- vértices ---------------------------------------------------------------------------------------------- */

static int32_t clamp_s(int64_t v, int32_t lo, int32_t hi)
{
	return (int32_t)(v < lo ? lo : v > hi ? hi : v);
}

/* Transformación de coordenadas de textura (TEXIMAGE_PARAM bits 30-31). Con modos 2 y 3 se suma la coordenada que
 * mandó TEXCOORD. Las unidades de salida son 1.11.4. [sin contrastar fuera del modo 0] */
static void tex_transform(const gx_t *g, int32_t *so, int32_t *to)
{
	const int32_t s = g->tex_s, t = g->tex_t;
	const int32_t *m = g->tex.m;

	switch (g->texparam >> 30) {
	case 1: /* coordenada de textura x matriz: (s, t, 1.0, 1.0) con 1.0 = 0x10 en 1.11.4 */
		*so = (int32_t)(((int64_t)s * m[0] + (int64_t)t * m[4] + ((int64_t)m[8] + m[12]) * 16) >> 12);
		*to = (int32_t)(((int64_t)s * m[1] + (int64_t)t * m[5] + ((int64_t)m[9] + m[13]) * 16) >> 12);
		break;
	case 2: { /* normal (1.0.9) x matriz, más la coordenada */
		const int64_t nx = g->normal[0], ny = g->normal[1], nz = g->normal[2];

		*so = (int32_t)(s + ((nx * m[0] + ny * m[4] + nz * m[8]) >> 17) + (m[12] >> 8));
		*to = (int32_t)(t + ((nx * m[1] + ny * m[5] + nz * m[9]) >> 17) + (m[13] >> 8));
		break;
	}
	case 3: { /* vértice (1.3.12) x matriz, más la coordenada */
		const int64_t vx = g->vx, vy = g->vy, vz = g->vz;

		*so = (int32_t)(s + ((vx * m[0] + vy * m[4] + vz * m[8]) >> 20) + (m[12] >> 8));
		*to = (int32_t)(t + ((vx * m[1] + vy * m[5] + vz * m[9]) >> 20) + (m[13] >> 8));
		break;
	}
	default:
		*so = s;
		*to = t;
		break;
	}
	*so = (int16_t)*so;
	*to = (int16_t)*to;
}

static uint32_t expand6(uint32_t c5)
{
	return (c5 << 1) + ((c5 + 31u) >> 5);
}

static void vertex_submit(gx_t *g, int32_t x, int32_t y, int32_t z)
{
	const int64_t px = x, py = y, pz = z;
	gx_cvtx c;

	g->vx = x; g->vy = y; g->vz = z;
	clip_update(g);
	{
		const int32_t *m = g->clip.m;

		c.x = (int32_t)((px * m[0] + py * m[4] + pz * m[8] + ((int64_t)m[12] << 12)) >> 12);
		c.y = (int32_t)((px * m[1] + py * m[5] + pz * m[9] + ((int64_t)m[13] << 12)) >> 12);
		c.z = (int32_t)((px * m[2] + py * m[6] + pz * m[10] + ((int64_t)m[14] << 12)) >> 12);
		c.w = (int32_t)((px * m[3] + py * m[7] + pz * m[11] + ((int64_t)m[15] << 12)) >> 12);
	}
	tex_transform(g, &c.s, &c.t);
	c.r = (int32_t)expand6(g->color5[0]);
	c.g = (int32_t)expand6(g->color5[1]);
	c.b = (int32_t)expand6(g->color5[2]);
	c.ram = -1;
	asm_push(g, &c);
}

/* ---- iluminación ---------------------------------------------------------------------------------------------- */

/* Calcula el color del vértice (5 bits) con las luces habilitadas en POLYGON_ATTR bits 0-3 y la normal ya
 * transformada `n` (1.0.9). Niveles de 8 bits; productos (a * b * nivel) >> 13 y ambiente (a * b) >> 5; suma con
 * saturación a 31. [sin contrastar con una DS real: los anchos son la mejor lectura de GBATEK] */
static void light_color(gx_t *g, const int32_t n[3])
{
	uint32_t acc[3] = { g->emi[0], g->emi[1], g->emi[2] };
	const unsigned en = g->polyattr & 0xFu;

	for (unsigned i = 0; i < 4; i++) {
		const int32_t *l = g->light_vec[i];
		int32_t dot, ld, sd;
		int32_t hx, hy, hz;

		if (!(en & (1u << i)))
			continue;
		dot = -(l[0] * n[0] + l[1] * n[1] + l[2] * n[2]);
		ld = dot <= 0 ? 0 : dot >> 10;
		if (ld > 255)
			ld = 255;
		/* semivector: (luz + visual) / 2, con la visual (0, 0, -1) */
		hx = l[0] >> 1;
		hy = l[1] >> 1;
		hz = (l[2] - 0x200) >> 1;
		dot = -(hx * n[0] + hy * n[1] + hz * n[2]);
		if (dot <= 0) {
			sd = 0;
		} else {
			sd = dot >> 10;
			if (sd > 255)
				sd = 255;
			sd = ((sd * sd) >> 7) - 0x100;
			if (sd < 0)
				sd = 0;
			if (sd > 255)
				sd = 255;
		}
		if (g->shin_en)
			sd = g->shin[sd >> 1];
		for (int c = 0; c < 3; c++) {
			const uint32_t lc = g->light_col[i][c];

			acc[c] += (g->dif[c] * lc * (uint32_t)ld) >> 13;
			acc[c] += (g->spe[c] * lc * (uint32_t)sd) >> 13;
			acc[c] += (g->amb[c] * lc) >> 5;
		}
	}
	for (int c = 0; c < 3; c++)
		g->color5[c] = acc[c] > 31u ? 31u : acc[c];
}

/* ---- órdenes de prueba ------------------------------------------------------------------------------------- */

static void clip_vertex_of(const gx_t *g, int32_t x, int32_t y, int32_t z, gx_cvtx *c)
{
	const int32_t *m = g->clip.m;
	const int64_t px = x, py = y, pz = z;

	memset(c, 0, sizeof *c);
	c->x = (int32_t)((px * m[0] + py * m[4] + pz * m[8] + ((int64_t)m[12] << 12)) >> 12);
	c->y = (int32_t)((px * m[1] + py * m[5] + pz * m[9] + ((int64_t)m[13] << 12)) >> 12);
	c->z = (int32_t)((px * m[2] + py * m[6] + pz * m[10] + ((int64_t)m[14] << 12)) >> 12);
	c->w = (int32_t)((px * m[3] + py * m[7] + pz * m[11] + ((int64_t)m[15] << 12)) >> 12);
}

static int box_visible(gx_t *g, const uint32_t *p)
{
	static const uint8_t face[6][4] = {
		{ 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 },
	};
	const int32_t x0 = (int16_t)p[0], y0 = (int16_t)(p[0] >> 16), z0 = (int16_t)p[1];
	const int32_t dw = (int16_t)(p[1] >> 16), dh = (int16_t)p[2], dd = (int16_t)(p[2] >> 16);
	gx_cvtx c[8];

	clip_update(g);
	for (int i = 0; i < 8; i++)
		clip_vertex_of(g, x0 + ((i & 1) ? dw : 0), y0 + ((i & 2) ? dh : 0), z0 + ((i & 4) ? dd : 0), &c[i]);
	for (int f = 0; f < 6; f++) {
		gx_cvtx q[4], o[CLIP_BUF];
		unsigned oa = 0x3Fu, oo = 0;

		for (int i = 0; i < 4; i++) {
			q[i] = c[face[f][i]];
			{
				const unsigned oc = outcode(&q[i]);

				oa &= oc;
				oo |= oc;
			}
		}
		if (oa)
			continue;
		if (!oo)
			return 1;
		if (clip_polygon(q, 4, o, 0x3Fu) > 0)
			return 1;
	}
	return 0;
}

/* ---- ejecución de órdenes ---------------------------------------------------------------------------------- */

static void stack_error(gx_t *g)
{
	g->err = 1;
}

/* Pilas. El puntero sigue contando aunque haya error (como en melonDS: tras el desbordamiento de la pila de posición
 * GXSTAT dice nivel 0 y el de la proyección vale 0 con dos PUSH); el error activa GXSTAT bit 15 y la orden no toca
 * las matrices. [melonDS; sin contrastar en una DS real] */
static void mtx_push(gx_t *g)
{
	switch (g->mode) {
	case 0:
		if (g->sp_proj > 0)
			stack_error(g);
		else
			g->stk_proj = g->proj;
		g->sp_proj = (g->sp_proj + 1) & 3u;
		break;
	case 1:
	case 2:
		if (g->sp_pos > GX_STACK_POS - 1) {
			stack_error(g);
		} else {
			g->stk_pos[g->sp_pos] = g->pos;
			g->stk_vec[g->sp_pos] = g->vec;
		}
		g->sp_pos = (g->sp_pos + 1) & 0x3Fu;
		break;
	default:
		if (g->sp_tex > 0)
			stack_error(g);
		else
			g->stk_tex = g->tex;
		g->sp_tex = (g->sp_tex + 1) & 3u;
		break;
	}
}

static void mtx_pop(gx_t *g, uint32_t param)
{
	const int32_t n = sext(param, 6);

	switch (g->mode) {
	case 0: {
		const int32_t ns = (int32_t)g->sp_proj - n;

		g->sp_proj = (uint32_t)ns & 3u;
		if (ns != 0) {
			stack_error(g);
			return;
		}
		g->proj = g->stk_proj;
		g->clip_dirty = 1;
		break;
	}
	case 1:
	case 2: {
		const int32_t ns = (int32_t)g->sp_pos - n;

		g->sp_pos = (uint32_t)ns & 0x3Fu;
		if (ns < 0 || ns >= (int32_t)GX_STACK_POS) {
			stack_error(g);
			return;
		}
		g->pos = g->stk_pos[ns];
		g->vec = g->stk_vec[ns];
		g->clip_dirty = 1;
		break;
	}
	default: {
		const int32_t ns = (int32_t)g->sp_tex - n;

		g->sp_tex = (uint32_t)ns & 3u;
		if (ns != 0) {
			stack_error(g);
			return;
		}
		g->tex = g->stk_tex;
		break;
	}
	}
}

static void mtx_store(gx_t *g, uint32_t param)
{
	const uint32_t idx = param & 0x1Fu;

	switch (g->mode) {
	case 0:
		g->stk_proj = g->proj;
		break;
	case 1:
	case 2:
		if (idx >= GX_STACK_POS) {
			stack_error(g);
			return;
		}
		g->stk_pos[idx] = g->pos;
		g->stk_vec[idx] = g->vec;
		break;
	default:
		g->stk_tex = g->tex;
		break;
	}
}

static void mtx_restore(gx_t *g, uint32_t param)
{
	const uint32_t idx = param & 0x1Fu;

	switch (g->mode) {
	case 0:
		g->proj = g->stk_proj;
		g->clip_dirty = 1;
		break;
	case 1:
	case 2:
		if (idx >= GX_STACK_POS) {
			stack_error(g);
			return;
		}
		g->pos = g->stk_pos[idx];
		g->vec = g->stk_vec[idx];
		g->clip_dirty = 1;
		break;
	default:
		g->tex = g->stk_tex;
		break;
	}
}

static void do_normal(gx_t *g, uint32_t p)
{
	int32_t n[3], t[3];
	const int32_t *m = g->vec.m;

	n[0] = sext(p, 10);
	n[1] = sext(p >> 10, 10);
	n[2] = sext(p >> 20, 10);
	g->normal[0] = n[0]; g->normal[1] = n[1]; g->normal[2] = n[2];
	/* se transforma con la matriz vectorial (3x3) y se satura a 1.0.9 */
	t[0] = clamp_s(((int64_t)n[0] * m[0] + (int64_t)n[1] * m[4] + (int64_t)n[2] * m[8]) >> 12, -512, 511);
	t[1] = clamp_s(((int64_t)n[0] * m[1] + (int64_t)n[1] * m[5] + (int64_t)n[2] * m[9]) >> 12, -512, 511);
	t[2] = clamp_s(((int64_t)n[0] * m[2] + (int64_t)n[1] * m[6] + (int64_t)n[2] * m[10]) >> 12, -512, 511);
	light_color(g, t);
}

static unsigned light_count(const gx_t *g)
{
	unsigned c = 0, en = g->polyattr & 0xFu;

	while (en) {
		c += en & 1u;
		en >>= 1;
	}
	return c;
}

/* Ejecuta una orden con sus parámetros y devuelve su coste en ciclos del motor. */
static unsigned exec_cmd(gx_t *g, uint32_t cmd, const uint32_t *p, uint64_t start)
{
	gx_mat m;

	g->st.cmds++;
	g->st.by_cmd[cmd & 0x7Fu]++;
	switch (cmd) {
	case 0x00:
		return 0;
	case 0x10:
		g->mode = p[0] & 3u;
		return 1;
	case 0x11:
		mtx_push(g);
		return 17;
	case 0x12:
		mtx_pop(g, p[0]);
		return 36;
	case 0x13:
		mtx_store(g, p[0]);
		return 17;
	case 0x14:
		mtx_restore(g, p[0]);
		return 36;
	case 0x15:
		mat_ident(&m);
		mtx_load(g, &m);
		return 19;
	case 0x16:
		mat_from_params(&m, p, 44);
		mtx_load(g, &m);
		return 34;
	case 0x17:
		mat_from_params(&m, p, 43);
		mtx_load(g, &m);
		return 30;
	case 0x18:
		mat_from_params(&m, p, 44);
		mtx_mult(g, &m, 0);
		return 35 + (g->mode == 2 ? 30u : 0u);
	case 0x19:
		mat_from_params(&m, p, 43);
		mtx_mult(g, &m, 0);
		return 31 + (g->mode == 2 ? 30u : 0u);
	case 0x1A:
		mat_from_params(&m, p, 33);
		mtx_mult(g, &m, 0);
		return 28 + (g->mode == 2 ? 30u : 0u);
	case 0x1B:
		mat_ident(&m);
		m.m[0] = (int32_t)p[0];
		m.m[5] = (int32_t)p[1];
		m.m[10] = (int32_t)p[2];
		mtx_mult(g, &m, 1);
		return 22;
	case 0x1C:
		mat_ident(&m);
		m.m[12] = (int32_t)p[0];
		m.m[13] = (int32_t)p[1];
		m.m[14] = (int32_t)p[2];
		mtx_mult(g, &m, 0);
		return 22 + (g->mode == 2 ? 30u : 0u);
	case 0x20:
		g->color5[0] = p[0] & 31u;
		g->color5[1] = (p[0] >> 5) & 31u;
		g->color5[2] = (p[0] >> 10) & 31u;
		return 1;
	case 0x21:
		do_normal(g, p[0]);
		return 9 + light_count(g);
	case 0x22:
		g->tex_s = (int16_t)p[0];
		g->tex_t = (int16_t)(p[0] >> 16);
		return 1;
	case 0x23:
		vertex_submit(g, (int16_t)p[0], (int16_t)(p[0] >> 16), (int16_t)p[1]);
		return 9;
	case 0x24:
		vertex_submit(g, sext(p[0], 10) * 64, sext(p[0] >> 10, 10) * 64, sext(p[0] >> 20, 10) * 64);
		return 8;
	case 0x25:
		vertex_submit(g, (int16_t)p[0], (int16_t)(p[0] >> 16), g->vz);
		return 8;
	case 0x26:
		vertex_submit(g, (int16_t)p[0], g->vy, (int16_t)(p[0] >> 16));
		return 8;
	case 0x27:
		vertex_submit(g, g->vx, (int16_t)p[0], (int16_t)(p[0] >> 16));
		return 8;
	case 0x28:
		vertex_submit(g, g->vx + sext(p[0], 10) * 8, g->vy + sext(p[0] >> 10, 10) * 8, g->vz + sext(p[0] >> 20, 10) * 8);
		return 8;
	case 0x29:
		g->polyattr = p[0];
		return 1;
	case 0x2A:
		g->texparam = p[0];
		return 1;
	case 0x2B:
		g->pltt = p[0] & 0x1FFFu;
		return 1;
	case 0x30:
		g->dif[0] = p[0] & 31u; g->dif[1] = (p[0] >> 5) & 31u; g->dif[2] = (p[0] >> 10) & 31u;
		g->amb[0] = (p[0] >> 16) & 31u; g->amb[1] = (p[0] >> 21) & 31u; g->amb[2] = (p[0] >> 26) & 31u;
		if (p[0] & 0x8000u) { /* bit 15: el color del vértice pasa a ser el difuso */
			g->color5[0] = g->dif[0]; g->color5[1] = g->dif[1]; g->color5[2] = g->dif[2];
		}
		return 4;
	case 0x31:
		g->spe[0] = p[0] & 31u; g->spe[1] = (p[0] >> 5) & 31u; g->spe[2] = (p[0] >> 10) & 31u;
		g->shin_en = (p[0] >> 15) & 1u;
		g->emi[0] = (p[0] >> 16) & 31u; g->emi[1] = (p[0] >> 21) & 31u; g->emi[2] = (p[0] >> 26) & 31u;
		return 4;
	case 0x32: {
		const unsigned i = p[0] >> 30;
		const int32_t *d = g->vec.m;
		const int32_t v[3] = { sext(p[0], 10), sext(p[0] >> 10, 10), sext(p[0] >> 20, 10) };

		/* la luz se guarda ya transformada por la matriz vectorial del momento */
		g->light_vec[i][0] = clamp_s(((int64_t)v[0] * d[0] + (int64_t)v[1] * d[4] + (int64_t)v[2] * d[8]) >> 12, -512, 511);
		g->light_vec[i][1] = clamp_s(((int64_t)v[0] * d[1] + (int64_t)v[1] * d[5] + (int64_t)v[2] * d[9]) >> 12, -512, 511);
		g->light_vec[i][2] = clamp_s(((int64_t)v[0] * d[2] + (int64_t)v[1] * d[6] + (int64_t)v[2] * d[10]) >> 12, -512, 511);
		return 6;
	}
	case 0x33: {
		const unsigned i = p[0] >> 30;

		g->light_col[i][0] = p[0] & 31u;
		g->light_col[i][1] = (p[0] >> 5) & 31u;
		g->light_col[i][2] = (p[0] >> 10) & 31u;
		return 1;
	}
	case 0x34:
		for (unsigned i = 0; i < 32; i++)
			for (unsigned b = 0; b < 4; b++)
				g->shin[i * 4 + b] = (uint8_t)(p[i] >> (8 * b));
		return 32;
	case 0x40:
		g->polyattr_cur = g->polyattr;
		g->prim = p[0] & 3u;
		g->asm_n = 0;
		g->asm_par = 0;
		return 1;
	case 0x41:
		return 1;
	case 0x50:
		g->swap_pending = 1;
		g->swap_param = p[0] & 3u;
		return 392;
	case 0x60:
		g->viewport = p[0];
		return 1;
	case 0x70:
		g->box_result = (uint32_t)box_visible(g, p);
		return 103;
	case 0x71: {
		gx_cvtx c;

		g->vx = (int16_t)p[0]; g->vy = (int16_t)(p[0] >> 16); g->vz = (int16_t)p[1];
		clip_update(g);
		clip_vertex_of(g, g->vx, g->vy, g->vz, &c);
		g->pos_result[0] = c.x; g->pos_result[1] = c.y; g->pos_result[2] = c.z; g->pos_result[3] = c.w;
		return 9;
	}
	case 0x72: {
		const int32_t *d = g->vec.m;
		const int64_t v[3] = { sext(p[0], 10) * 8, sext(p[0] >> 10, 10) * 8, sext(p[0] >> 20, 10) * 8 };

		/* melonDS: si el bit 12 del resultado está encendido se le encienden también los bits 13 a 15 (13 bits con signo
		 * a medias); con el bit 12 apagado el resultado queda como salió. [sin contrastar en una DS real] */
		for (int c = 0; c < 3; c++) {
			uint16_t r = (uint16_t)((v[0] * d[c] + v[1] * d[4 + c] + v[2] * d[8 + c]) >> 12);

			if (r & 0x1000u)
				r |= 0xE000u;
			g->vec_result[c] = (int16_t)r;
		}
		return 5;
	}
	default:
		g->st.unknown_cmds++;
		(void)start;
		return 0;
	}
}

/* ---- cola, tiempo y registros ------------------------------------------------------------------------------- */

void gx_init(gx_t *g)
{
	uint64_t (*const clock)(void) = g->clock;

	np_init();
	memset(g, 0, sizeof *g);
	g->clock = clock;
	mat_ident(&g->proj);
	mat_ident(&g->pos);
	mat_ident(&g->vec);
	mat_ident(&g->tex);
	mat_ident(&g->clip);
	mat_ident(&g->stk_proj);
	mat_ident(&g->stk_tex);
	for (unsigned i = 0; i < GX_STACK_POS; i++) {
		mat_ident(&g->stk_pos[i]);
		mat_ident(&g->stk_vec[i]);
	}
	g->viewport = 0u | (0u << 8) | (255u << 16) | (191u << 24);
	g->color5[0] = g->color5[1] = g->color5[2] = 31u;
	g->pk_idx = 4;
	g->front = -1;
}

uint32_t gx_fifo_level(const gx_t *g)
{
	return g->qcount > GX_PIPE_ENTRIES ? g->qcount - GX_PIPE_ENTRIES : 0u;
}

int gx_irq_level(const gx_t *g)
{
	const uint32_t lv = gx_fifo_level(g);

	switch (g->irqmode) {
	case 1: return lv < GX_FIFO_ENTRIES / 2;
	case 2: return lv == 0;
	default: return 0;
	}
}

uint64_t gx_next_start(const gx_t *g)
{
	if (g->swap_pending || g->qcount == 0)
		return GX_NEVER;
	{
		const unsigned ne = cmd_entries(g->q[g->qhead].cmd);
		uint64_t t, cf;

		if (g->qcount < ne)
			return GX_NEVER;
		t = g->q[(g->qhead + ne - 1) & (GX_QRING - 1u)].t;
		cf = g->cycle_free;
		return t > cf ? t : cf;
	}
}

void gx_advance(gx_t *g, uint64_t now)
{
	const uint64_t c0 = g->clock ? g->clock() : 0;

	if (now > g->now)
		g->now = now;
	for (;;) {
		uint32_t p[32];
		uint64_t start;
		unsigned ne, cost, np;
		uint8_t cmd;

		start = gx_next_start(g);
		if (start == GX_NEVER || start > now)
			break;
		cmd = g->q[g->qhead].cmd;
		ne = cmd_entries(cmd);
		np = g_np[cmd];
		for (unsigned i = 0; i < np; i++)
			p[i] = g->q[(g->qhead + i) & (GX_QRING - 1u)].param;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_pending = g->test_pending >= ne ? g->test_pending - ne : 0u;
		g->qhead = (g->qhead + ne) & (GX_QRING - 1u);
		g->qcount -= ne;
		cost = exec_cmd(g, cmd, p, start);
		g->cycle_free = start + 2ull * cost;
		g->st.cycles += 2ull * cost;
		if (cmd == 0x11 || cmd == 0x12 || cmd == 0x13 || cmd == 0x14 || cmd == 0x15)
			g->stack_busy_until = g->cycle_free;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_busy_until = g->cycle_free;
	}
	if (g->clock)
		g->st.host_ticks += g->clock() - c0;
}

static uint64_t q_push(gx_t *g, uint64_t now, uint8_t cmd, uint32_t param)
{
	uint64_t t = now;

	while (g->qcount >= GX_QUEUE_MAX) {
		const uint64_t ns = gx_next_start(g);

		if (ns == GX_NEVER) { /* parado en un SWAP_BUFFERS (o con una orden incompleta): la escritura se pierde */
			g->st.fifo_overflow++;
			return t;
		}
		if (ns > t)
			t = ns;
		gx_advance(g, t);
	}
	{
		gx_qent *e = &g->q[(g->qhead + g->qcount) & (GX_QRING - 1u)];

		e->cmd = cmd;
		e->param = param;
		e->t = t;
		g->qcount++;
		if (cmd >= 0x70 && cmd <= 0x72)
			g->test_pending++;
	}
	return t;
}

/* Siguiente orden no nula de la palabra empaquetada; las que no llevan parámetros entran de una vez. */
static uint64_t pk_advance(gx_t *g, uint64_t now, uint64_t stall)
{
	while (g->pk_idx < 4) {
		const uint32_t c = (g->pk_word >> (8u * g->pk_idx)) & 0xFFu;
		const unsigned np = c < 0x80 ? g_np[c] : NP_BAD;

		if (c == 0) {
			g->pk_idx++;
			continue;
		}
		if (np == NP_BAD) { /* orden inexistente: se ignora */
			g->st.unknown_cmds++;
			g->pk_idx++;
			continue;
		}
		if (np == 0) {
			const uint64_t t = q_push(g, now > stall ? now : stall, (uint8_t)c, 0);

			if (t > stall)
				stall = t;
			g->pk_idx++;
			continue;
		}
		g->pk_cmd = (uint8_t)c;
		g->pk_left = np;
		return stall;
	}
	g->pk_left = 0;
	return stall;
}

uint64_t gx_fifo_write(gx_t *g, uint64_t now, uint32_t w)
{
	uint64_t stall = now;

	gx_advance(g, now);
	if (g->pk_left) {
		const uint64_t t = q_push(g, now, g->pk_cmd, w);

		if (t > stall)
			stall = t;
		if (--g->pk_left == 0) {
			g->pk_idx++;
			stall = pk_advance(g, stall, stall);
		}
	} else {
		g->pk_word = w;
		g->pk_idx = 0;
		stall = pk_advance(g, now, stall);
	}
	return stall;
}

uint64_t gx_port_write(gx_t *g, uint64_t now, uint32_t off, uint32_t w)
{
	const uint32_t cmd = (off - 0x400u) >> 2;

	gx_advance(g, now);
	if (off < 0x440u || off > 0x5CCu || !cmd_valid(cmd) || cmd == 0)
		return now;
	return q_push(g, now, (uint8_t)cmd, g_np[cmd] ? w : 0u);
}

uint32_t gx_gxstat(gx_t *g, uint64_t now)
{
	uint32_t v = 0;
	const uint32_t lv = gx_fifo_level(g);

	gx_advance(g, now);
	if (g->test_pending || g->test_busy_until > now)
		v |= GXSTAT_TEST_BUSY;
	if (g->box_result)
		v |= GXSTAT_BOX_RESULT;
	v |= (g->sp_pos & 0x1Fu) << 8;
	v |= (g->sp_proj & 1u) << 13;
	if (g->stack_busy_until > now)
		v |= GXSTAT_STACK_BUSY;
	if (g->err)
		v |= GXSTAT_STACK_ERR;
	v |= lv << 16;
	if (lv < GX_FIFO_ENTRIES / 2)
		v |= GXSTAT_HALF;
	if (lv == 0)
		v |= GXSTAT_EMPTY;
	if (g->qcount || g->cycle_free > now || g->swap_pending)
		v |= GXSTAT_BUSY;
	v |= g->irqmode << 30;
	return v;
}

void gx_gxstat_write(gx_t *g, uint32_t v, uint32_t mask)
{
	if ((mask & 0x8000u) && (v & 0x8000u))
		g->err = 0;
	if (mask & 0xC0000000u)
		g->irqmode = (v >> 30) & 3u;
}

uint32_t gx_ram_count(const gx_t *g)
{
	const gx_frame *f = &g->frame[g->bld];

	return (f->npolys & 0xFFFu) | ((f->nverts & 0x1FFFu) << 16);
}

uint32_t gx_read32(gx_t *g, uint64_t now, uint32_t off)
{
	gx_advance(g, now);
	if (off == 0x600u)
		return gx_gxstat(g, now);
	if (off == 0x604u)
		return gx_ram_count(g);
	if (off >= 0x620u && off <= 0x62Cu)
		return (uint32_t)g->pos_result[(off - 0x620u) >> 2];
	if (off == 0x630u)
		return (uint32_t)(uint16_t)g->vec_result[0] | ((uint32_t)(uint16_t)g->vec_result[1] << 16);
	if (off == 0x634u)
		return (uint32_t)(uint16_t)g->vec_result[2];
	if (off >= 0x640u && off <= 0x67Cu) {
		clip_update(g);
		return (uint32_t)g->clip.m[(off - 0x640u) >> 2];
	}
	if (off >= 0x680u && off <= 0x6A0u) {
		const uint32_t k = (off - 0x680u) >> 2;

		return (uint32_t)g->vec.m[(k / 3u) * 4u + k % 3u];
	}
	return 0;
}

int gx_swap_pending(const gx_t *g)
{
	return g->swap_pending;
}

const gx_frame *gx_swap(gx_t *g, uint64_t now, const gx_regs *regs)
{
	gx_frame *f = &g->frame[g->bld];

	if (now > g->now)
		g->now = now;
	if (g->cycle_free < now)
		g->cycle_free = now; /* lo que sigue al SWAP_BUFFERS arranca en el VBlank */
	g->st.frame_polys += f->npolys;
	g->st.frame_verts += f->nverts;
	if (f->npolys > g->st.max_polys)
		g->st.max_polys = f->npolys;
	if (f->nverts > g->st.max_verts)
		g->st.max_verts = f->nverts;

	f->swap_params = g->swap_param;
	if (regs)
		f->regs = *regs;
	g->swaps++;
	f->seq = g->swaps;
	g->front = g->bld;
	g->bld ^= 1;
	g->frame[g->bld].npolys = 0;
	g->frame[g->bld].nverts = 0;
	g->frame[g->bld].dropped_polys = 0;
	g->frame[g->bld].dropped_verts = 0;
	g->swap_pending = 0;
	g->st.swaps++;
	return f;
}

const gx_frame *gx_front(const gx_t *g)
{
	return g->front < 0 ? NULL : &g->frame[g->front];
}
