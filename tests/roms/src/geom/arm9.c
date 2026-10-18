/*
 * SalsaDS — ROM geom: el motor de geometría visto desde el ARM9 (Fase 3.1). Sin rasterizar nada: sólo lo que la CPU puede
 * leer de vuelta.
 *
 *  - matrices: modos 0 a 3, MTX_LOAD/MULT 4x4, 4x3 y 3x3, SCALE, TRANS e IDENTITY; la matriz clip (0x04000640) y la
 *    vectorial (0x04000680) se leen de vuelta y se comparan con un modelo propio en C (producto de 64 bits y >> 12);
 *  - pilas de proyección, posición/vectorial y textura: PUSH, POP, STORE, RESTORE, niveles de GXSTAT, error de
 *    desbordamiento y su borrado;
 *  - POS_TEST, VEC_TEST y BOX_TEST;
 *  - listas de polígonos: RAM_COUNT (polígonos y vértices) tras triángulos, cuadriláteros y tiras, con las cuatro
 *    combinaciones de cara, fuera del volumen, recortados contra los seis planos y el plano lejano con y sin permiso;
 *  - SWAP_BUFFERS (el contador vuelve a cero al VBlank), GXSTAT con la FIFO llena y vacía, la IRQ 21 por nivel.
 *
 * Todo lo leído de vuelta queda también en una tabla de 32 bits en 0x02300000 (la marca «GEOM» en la primera palabra y
 * luego `ti` valores): `salsads-cpurun --dump 0x02300000:0x1000:t.bin` y `tools/melonds-dump.py --range ...` la dan para
 * comparar entre emuladores. Las comprobaciones sin valor documentado seguro usan SDS_DIVERGE (informativas).
 */
#include <nds.h>

#include "../common/hw.h"

#define GX_CMD(c) (0x04000400u + 4u * (c))
#define R_GXSTAT 0x04000600u
#define R_RAMCOUNT 0x04000604u
#define R_POSRES 0x04000620u
#define R_VECRES 0x04000630u
#define R_CLIPMTX 0x04000640u
#define R_VECMTX 0x04000680u
#define IRQ_GX (1u << 21)

#define TAB ((volatile uint32_t *)0x02300000u)
#define TAB_MAX 1000u
static uint32_t ti;

static void rec(uint32_t v)
{
	if (ti < TAB_MAX)
		TAB[1 + ti++] = v;
	TAB[0] = 0x4D4F4547u | 0u; /* "GEOM" */
	TAB[TAB_MAX + 1] = ti;
}

/* ---- comandos por los puertos --------------------------------------------------------------------------------- */

static void cmd(uint32_t c, uint32_t p)
{
	MEM32(GX_CMD(c)) = p;
}

static uint32_t gxstat(void)
{
	return MEM32(R_GXSTAT);
}

/* Espera con tope a que el motor termine (GXSTAT bits 0 y 27 en cero). */
static void idle(void)
{
	WAIT_UNTIL((MEM32(R_GXSTAT) & ((1u << 27) | 1u)) == 0, 3000000u);
}

static void wait_vblank_edge(void)
{
	while (MEM16(R_VCOUNT) >= 192)
		;
	while (MEM16(R_VCOUNT) < 192)
		;
}

/* ---- modelo de matrices ----------------------------------------------------------------------------------- */

typedef struct {
	int32_t m[16];
} M4;

static void m_ident(M4 *a)
{
	for (int i = 0; i < 16; i++)
		a->m[i] = (i % 5 == 0) ? 0x1000 : 0;
}

static void m_mul(M4 *o, const M4 *a, const M4 *b) /* o = a x b */
{
	M4 r;

	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			int64_t s = 0;

			for (int k = 0; k < 4; k++)
				s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
			r.m[i * 4 + j] = (int32_t)(s >> 12);
		}
	*o = r;
}

static M4 g_proj, g_pos, g_vec, g_tex;
static uint32_t g_mode;

static void model_reset(void)
{
	m_ident(&g_proj);
	m_ident(&g_pos);
	m_ident(&g_vec);
	m_ident(&g_tex);
}

static void set_mode(uint32_t m)
{
	g_mode = m;
	cmd(0x10, m);
}

static void model_load(const M4 *m)
{
	switch (g_mode) {
	case 0: g_proj = *m; break;
	case 1: g_pos = *m; break;
	case 2: g_pos = *m; g_vec = *m; break;
	default: g_tex = *m; break;
	}
}

static void model_mult(const M4 *op, int is_scale)
{
	switch (g_mode) {
	case 0: m_mul(&g_proj, op, &g_proj); break;
	case 1: m_mul(&g_pos, op, &g_pos); break;
	case 2:
		m_mul(&g_pos, op, &g_pos);
		if (!is_scale)
			m_mul(&g_vec, op, &g_vec);
		break;
	default: m_mul(&g_tex, op, &g_tex); break;
	}
}

static void op_ident(void)
{
	M4 a;

	cmd(0x15, 0);
	m_ident(&a);
	model_load(&a);
}

static void op_load44(const int32_t *p)
{
	M4 a;

	for (int i = 0; i < 16; i++) {
		cmd(0x16, (uint32_t)p[i]);
		a.m[i] = p[i];
	}
	model_load(&a);
}

static void op_load43(const int32_t *p)
{
	M4 a;

	m_ident(&a);
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 3; c++)
			a.m[r * 4 + c] = p[r * 3 + c];
	for (int i = 0; i < 12; i++)
		cmd(0x17, (uint32_t)p[i]);
	model_load(&a);
}

static void op_mult44(const int32_t *p)
{
	M4 a;

	for (int i = 0; i < 16; i++) {
		cmd(0x18, (uint32_t)p[i]);
		a.m[i] = p[i];
	}
	model_mult(&a, 0);
}

static void op_mult43(const int32_t *p)
{
	M4 a;

	m_ident(&a);
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 3; c++)
			a.m[r * 4 + c] = p[r * 3 + c];
	for (int i = 0; i < 12; i++)
		cmd(0x19, (uint32_t)p[i]);
	model_mult(&a, 0);
}

static void op_mult33(const int32_t *p)
{
	M4 a;

	m_ident(&a);
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			a.m[r * 4 + c] = p[r * 3 + c];
	for (int i = 0; i < 9; i++)
		cmd(0x1A, (uint32_t)p[i]);
	model_mult(&a, 0);
}

static void op_scale(int32_t x, int32_t y, int32_t z)
{
	M4 a;

	m_ident(&a);
	a.m[0] = x;
	a.m[5] = y;
	a.m[10] = z;
	cmd(0x1B, (uint32_t)x);
	cmd(0x1B, (uint32_t)y);
	cmd(0x1B, (uint32_t)z);
	model_mult(&a, 1);
}

static void op_trans(int32_t x, int32_t y, int32_t z)
{
	M4 a;

	m_ident(&a);
	a.m[12] = x;
	a.m[13] = y;
	a.m[14] = z;
	cmd(0x1C, (uint32_t)x);
	cmd(0x1C, (uint32_t)y);
	cmd(0x1C, (uint32_t)z);
	model_mult(&a, 0);
}

/* Compara la matriz clip y la vectorial (3x3) leídas con el modelo; deja todo en la tabla. */
static void check_mats_at(uint32_t site)
{
	M4 clip;

	idle();
	m_mul(&clip, &g_pos, &g_proj);
	for (int i = 0; i < 16; i++) {
		const uint32_t v = MEM32(R_CLIPMTX + 4u * (uint32_t)i);

		SDS_CHECK_ID(v, (uint32_t)clip.m[i], site * 100u + (uint32_t)i);
	}
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			const uint32_t v = MEM32(R_VECMTX + 4u * (uint32_t)(r * 3 + c));

			SDS_CHECK_ID(v, (uint32_t)g_vec.m[r * 4 + c], site * 100u + 50u + (uint32_t)(r * 3 + c));
		}
}

#define check_mats() check_mats_at(__LINE__)

static uint32_t lcg_s = 0x1234567u;

static int32_t rnd(int32_t range) /* -range..range */
{
	lcg_s = lcg_s * 1664525u + 1013904223u;
	return (int32_t)((lcg_s >> 8) % (uint32_t)(2 * range + 1)) - range;
}

static void rnd_mat(int32_t *p, int n, int32_t range)
{
	for (int i = 0; i < n; i++)
		p[i] = rnd(range);
}

/* ---- polígonos -------------------------------------------------------------------------------------------- */

static void gxv16(int32_t x, int32_t y, int32_t z)
{
	cmd(0x23, ((uint32_t)x & 0xFFFFu) | ((uint32_t)y << 16));
	cmd(0x23, (uint32_t)z & 0xFFFFu);
}

static uint32_t ram_polys(void)
{
	return MEM32(R_RAMCOUNT) & 0xFFFu;
}

static uint32_t ram_verts(void)
{
	return (MEM32(R_RAMCOUNT) >> 16) & 0x1FFFu;
}

#define ONE 0x1000
#define ATTR_BOTH 0x001F00C0u

/* Manda `n` vértices en el modo `prim` con `attr` y devuelve (polígonos << 16 | vértices) que sumó a RAM_COUNT. */
static uint32_t scene(uint32_t attr, uint32_t prim, const int32_t (*v)[3], int n)
{
	uint32_t p0, v0, dp, dv;

	idle();
	p0 = ram_polys();
	v0 = ram_verts();
	cmd(0x29, attr);
	cmd(0x40, prim);
	for (int i = 0; i < n; i++)
		gxv16(v[i][0], v[i][1], v[i][2]);
	cmd(0x41, 0);
	idle();
	dp = ram_polys() - p0;
	dv = ram_verts() - v0;
	rec(dp);
	rec(dv);
	return (dp << 16) | dv;
}

#define SC(attr, prim, ...)                                           \
	({                                                            \
		static const int32_t vv_[][3] = { __VA_ARGS__ };      \
		scene((attr), (prim), vv_, (int)(sizeof vv_ / sizeof vv_[0])); \
	})

static void group_state(void)
{
	sds_group("estado");
	idle();
	rec(gxstat());
	SDS_CHECK((gxstat() >> 24) & 7u, 6u);
	SDS_CHECK(gxstat() & 0xFFFFu, 0u);
	SDS_CHECK(gxstat() >> 27, 0u); /* ocioso: ni ocupado ni IRQ */
	/* una escritura de 1 al bit 15 sin error no cambia nada */
	MEM32(R_GXSTAT) = 1u << 15;
	SDS_CHECK(gxstat() & 0xFFFFu, 0u);
}

static void group_mats(void)
{
	int32_t p[16];

	model_reset();
	sds_group("matrices");
	set_mode(0);
	op_ident();
	set_mode(1);
	op_ident();
	set_mode(2);
	op_ident();
	check_mats();
	/* carga y producto */
	set_mode(0);
	{
		const int32_t proj[16] = { 0x1555, 0, 0, 0, 0, 0x2000, 0, 0, 0, 0, -0x1020, -0x1000, 0, 0, -0x2010, 0 };

		op_load44(proj);
	}
	set_mode(1);
	{
		const int32_t pos[12] = { 0x1000, 0, 0, 0, 0x1000, 0, 0, 0, 0x1000, 0x800, -0x1800, -0x4000 };

		op_load43(pos);
	}
	check_mats();
	set_mode(2);
	{
		const int32_t pos[12] = { 0x0B50, 0x0B50, 0, -0x0B50, 0x0B50, 0, 0, 0, 0x1000, 0x123, 0x456, 0x789 };

		op_load43(pos);
	}
	check_mats();
	/* operaciones al azar en los tres modos de matriz (0 proyección, 1 posición, 2 posición y vectorial) */
	for (int round = 0; round < 12; round++) {
		set_mode((uint32_t)(round % 3));
		rnd_mat(p, 16, 0x1400);
		op_load44(p); /* cada ronda parte de una matriz chica para no desbordar los 32 bits */
		check_mats();
		rnd_mat(p, 16, 0x1400);
		op_mult44(p);
		check_mats();
		rnd_mat(p, 12, 0x1400);
		op_mult43(p);
		check_mats();
		rnd_mat(p, 9, 0x1400);
		op_mult33(p);
		check_mats();
		op_scale(rnd(0x1800), rnd(0x1800), rnd(0x1800));
		check_mats();
		op_trans(rnd(0x20000), rnd(0x20000), rnd(0x20000));
		check_mats();
		rnd_mat(p, 16, 0x7FFF);
		op_load44(p);
		check_mats();
		rnd_mat(p, 12, 0x7FFF);
		op_load43(p);
		check_mats();
	}
	/* el modo 3 (textura) no toca la clip ni la vectorial */
	set_mode(3);
	rnd_mat(p, 16, 0x3000);
	op_load44(p);
	rnd_mat(p, 12, 0x3000);
	op_mult43(p);
	check_mats();
	set_mode(1);
	op_ident();
}

/* ---- pilas ------------------------------------------------------------------------------------------------ */

static uint32_t level(void)
{
	return (gxstat() >> 8) & 0x1Fu;
}

/* GXSTAT tras una orden de pila: se espera a que el motor termine y se deja el valor en la tabla */
static uint32_t st_after(void)
{
	uint32_t st;

	idle();
	st = gxstat();
	rec(st);
	return st;
}

/* Lleva el puntero de la pila de posición a 0 desde cualquier estado, sin quedarse con el error. El puntero cuenta de 0 a 63
 * (GXSTAT enseña los 5 bits bajos): se empuja hasta un múltiplo de 32, se prueba con un PUSH si es 0 o 32 (desde 32 da
 * error) y se corrige con un POP. */
static void stack_zero(void)
{
	uint32_t l;

	MEM32(R_GXSTAT) = 1u << 15;
	idle();
	l = level();
	for (uint32_t i = 0; i < ((32u - l) & 31u); i++)
		cmd(0x11, 0);
	idle();
	MEM32(R_GXSTAT) = 1u << 15;
	cmd(0x11, 0);
	idle();
	if (gxstat() & 0x8000u) { /* estaba en 32: ahora 33 */
		MEM32(R_GXSTAT) = 1u << 15;
		cmd(0x12, 0x21u); /* -31: 33 + 31 = 64 = 0 */
	} else {
		cmd(0x12, 1u);
	}
	idle();
	MEM32(R_GXSTAT) = 1u << 15;
	idle();
	SDS_CHECK(level(), 0u);
	SDS_CHECK((gxstat() >> 15) & 1u, 0u);
}

static void group_stacks(void)
{
	int32_t p[16];
	uint32_t st;

	sds_group("pilas");
	/* posición y vectorial (31 entradas) */
	model_reset();
	set_mode(0);
	op_ident();
	set_mode(2);
	op_ident();
	idle();
	SDS_CHECK(level(), 0u);
	for (int i = 0; i < 30; i++) {
		cmd(0x11, 0); /* PUSH */
		op_trans(0x1000, 0x2000 * (i + 1), 0);
	}
	idle();
	SDS_CHECK(level(), 30u);
	SDS_CHECK((gxstat() >> 15) & 1u, 0u);
	cmd(0x11, 0); /* la 31.ª todavía cabe: la entrada 30 es la última */
	op_trans(0x1000, 0x2000 * 31, 0);
	st = st_after();
	SDS_CHECK(level(), 31u);
	SDS_CHECK((st >> 15) & 1u, 0u);
	check_mats();
	cmd(0x11, 0); /* la 32.ª: desbordamiento. En melonDS el puntero se pasa de 31 y GXSTAT da nivel 0 */
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 1u);
	SDS_CHECK(level(), 0u);
	check_mats(); /* la matriz actual no cambia */
	cmd(0x11, 0); /* otra más */
	st = st_after();
	rec(level());
	MEM32(R_GXSTAT) = 1u << 15; /* el error se borra escribiendo 1 */
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 0u);
	rec(level());
	cmd(0x11, 0); /* ¿sigue siendo error con el puntero pasado de 31? */
	st = st_after();
	rec(st);
	MEM32(R_GXSTAT) = 1u << 15;
	idle();
	/* POP de dos niveles desde ahí, y de 31 de una vez desde el nivel que quede */
	cmd(0x12, 2u);
	st = st_after();
	rec(level());
	stack_zero();
	{
		M4 id;

		m_ident(&id);
		set_mode(2);
		op_ident();
		g_pos = id;
		g_vec = id;
	}
	check_mats();
	/* POP de más: subdesbordamiento, y qué nivel queda */
	cmd(0x12, 1u);
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 1u);
	rec(level());
	stack_zero();
	cmd(0x12, 3u);
	st = st_after();
	rec(level());
	stack_zero();
	/* un POP con número negativo (6 bits con signo) sube el puntero */
	cmd(0x11, 0);
	cmd(0x11, 0);
	cmd(0x11, 0);
	cmd(0x12, 0x3Fu); /* -1 */
	st = st_after();
	rec(level());
	SDS_CHECK((st >> 15) & 1u, 0u);
	stack_zero();
	/* PUSH, cambio, POP: vuelve */
	rnd_mat(p, 16, 0x3000);
	op_load44(p);
	cmd(0x11, 0);
	idle();
	SDS_CHECK(level(), 1u);
	{
		const M4 saved = g_pos, savedv = g_vec;

		op_trans(0x5000, 0x6000, 0x7000);
		cmd(0x12, 1u);
		g_pos = saved;
		g_vec = savedv;
	}
	check_mats();
	SDS_CHECK(level(), 0u);
	/* STORE y RESTORE con índice absoluto: no mueven el puntero */
	{
		const M4 s7 = g_pos, v7 = g_vec;

		cmd(0x13, 7u);
		op_scale(0x2000, 0x2000, 0x2000);
		cmd(0x14, 7u);
		g_pos = s7;
		g_vec = v7;
		check_mats();
		idle();
		SDS_CHECK(level(), 0u);
		SDS_CHECK((gxstat() >> 15) & 1u, 0u);
	}
	cmd(0x13, 30u); /* la última entrada válida */
	cmd(0x14, 30u);
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 0u);
	cmd(0x13, 31u); /* índice 31: fuera de la pila */
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 1u);
	MEM32(R_GXSTAT) = 1u << 15;
	cmd(0x14, 31u);
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 1u);
	MEM32(R_GXSTAT) = 1u << 15;
	rec(level());
	check_mats();
	/* pila de proyección (1 entrada, bit 13 de GXSTAT) */
	model_reset();
	set_mode(2);
	op_ident();
	set_mode(0);
	op_ident();
	op_scale(0x2000, 0x2000, 0x2000);
	cmd(0x11, 0);
	st = st_after();
	SDS_CHECK((st >> 13) & 1u, 1u);
	SDS_CHECK((st >> 15) & 1u, 0u);
	cmd(0x11, 0); /* la segunda: desbordamiento */
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 1u);
	SDS_CHECK((st >> 13) & 1u, 0u); /* el puntero de 1 bit da la vuelta */
	MEM32(R_GXSTAT) = 1u << 15;
	/* tras varios errores seguidos el puntero de la pila de proyección queda en un estado que melonDS y este núcleo
	 * resuelven distinto (nada de eso lo usa un juego): se vuelve a un estado limpio cargando la proyección */
	set_mode(0);
	op_ident();
	model_reset();
	set_mode(2);
	op_ident();
	idle();
	/* pila de textura: sin nivel en GXSTAT; el segundo PUSH sí da error de desbordamiento (en melonDS) */
	set_mode(3);
	cmd(0x11, 0);
	st = st_after();
	SDS_CHECK((st >> 15) & 1u, 0u);
	cmd(0x11, 0);
	st = st_after();
	SDS_DIVERGE((st >> 15) & 1u, 0u);
	MEM32(R_GXSTAT) = 1u << 15;
	set_mode(1);
	op_ident();
	set_mode(0);
	op_ident();
	set_mode(2);
	op_ident();
	model_reset();
	stack_zero();
	check_mats();
}

/* ---- órdenes de prueba -------------------------------------------------------------------------------------- */

static void group_tests(void)
{
	int32_t p[16];

	sds_group("pruebas");
	model_reset();
	set_mode(0);
	{
		const int32_t proj[16] = { 0x1555, 0, 0, 0, 0, 0x2000, 0, 0, 0, 0, -0x1020, -0x1000, 0, 0, -0x2010, 0 };

		op_load44(proj);
	}
	set_mode(2);
	op_ident();
	op_trans(0, 0, -0x4000);
	idle();
	/* POS_TEST: el vértice por la matriz clip */
	{
		M4 clip;
		const int32_t vs[3][3] = { { 0x1000, 0x2000, 0x3000 }, { -0x800, 0x123, -0x2000 }, { 0, 0, 0 } };

		m_mul(&clip, &g_pos, &g_proj);
		for (int k = 0; k < 3; k++) {
			cmd(0x71, ((uint32_t)vs[k][0] & 0xFFFFu) | ((uint32_t)vs[k][1] << 16));
			cmd(0x71, (uint32_t)vs[k][2] & 0xFFFFu);
			idle();
			for (int c = 0; c < 4; c++) {
				const int64_t s = (int64_t)vs[k][0] * clip.m[c] + (int64_t)vs[k][1] * clip.m[4 + c] +
						  (int64_t)vs[k][2] * clip.m[8 + c] + ((int64_t)clip.m[12 + c] << 12);
				const uint32_t v = MEM32(R_POSRES + 4u * (uint32_t)c);

				rec(v);
				SDS_CHECK(v, (uint32_t)(int32_t)(s >> 12));
			}
		}
	}
	/* VEC_TEST: el vector (1.0.9) por la matriz vectorial, resultado 1.3.12 de 16 bits */
	rnd_mat(p, 9, 0x1000);
	op_mult33(p);
	idle();
	for (int k = 0; k < 6; k++) {
		const int32_t x = rnd(0x1FF), y = rnd(0x1FF), z = rnd(0x1FF);
		const uint32_t w = ((uint32_t)x & 0x3FFu) | (((uint32_t)y & 0x3FFu) << 10) | (((uint32_t)z & 0x3FFu) << 20);
		int64_t s[3];
		uint32_t r0, r1;

		cmd(0x72, w);
		idle();
		for (int c = 0; c < 3; c++)
			s[c] = ((int64_t)x * 8 * g_vec.m[c] + (int64_t)y * 8 * g_vec.m[4 + c] + (int64_t)z * 8 * g_vec.m[8 + c]) >> 12;
		r0 = MEM32(R_VECRES);
		r1 = MEM32(R_VECRES + 4);
		rec(r0);
		rec(r1);
		/* melonDS 1.1 devuelve 0 en las lecturas de 32 bits de VEC_RESULT: se compara con lecturas de 16 bits; y el
		 * resultado llega con el signo extendido desde el bit 12 (1.0.12), no desde el 15 */
		SDS_DIVERGE(r0 == (MEM16(R_VECRES) | ((uint32_t)MEM16(R_VECRES + 2) << 16)), 1u);
		for (int c = 0; c < 3; c++) {
			const uint32_t h = MEM16(R_VECRES + 2u * (uint32_t)c);
			const uint32_t want = (uint32_t)s[c] & 0xFFFFu;

			rec(h);
			rec((uint32_t)s[c]);
			SDS_DIVERGE(h, want);
			SDS_CHECK(h & 0xFFFu, want & 0xFFFu);
		}
	}
	/* BOX_TEST con la clip identidad: el volumen de vista es el cubo de -1 a 1 */
	model_reset();
	set_mode(0);
	op_ident();
	set_mode(2);
	op_ident();
	idle();
	{
		static const struct {
			int16_t x, y, z, w, h, d;
			uint8_t want;
		} bt[] = {
			{ -0x400, -0x400, -0x400, 0x800, 0x800, 0x800, 1 },  /* dentro */
			{ 0x2000, 0x2000, 0x2000, 0x400, 0x400, 0x400, 0 },  /* fuera por los tres ejes */
			{ 0x800, -0x400, -0x400, 0x1000, 0x800, 0x800, 1 },  /* cruza la cara derecha */
			{ 0x1400, -0x400, -0x400, 0x400, 0x800, 0x800, 0 },  /* a la derecha, sin tocar */
			{ -0x400, -0x400, -0x2000, 0x800, 0x800, 0x1800, 1 }, /* cruza el plano cercano */
			{ -0x400, -0x400, 0x1400, 0x800, 0x800, 0x400, 0 },  /* detrás del plano lejano */
			{ -0x2000, -0x2000, -0x2000, 0x4000, 0x4000, 0x4000, 0 }, /* contiene al volumen: ninguna cara dentro */
			{ -0x400, 0x1000, -0x400, 0x800, 0x400, 0x800, 1 },  /* roza la cara de arriba (y = 1,0) */
		};
		for (unsigned k = 0; k < sizeof bt / sizeof bt[0]; k++) {
			uint32_t st;

			cmd(0x70, ((uint32_t)bt[k].x & 0xFFFFu) | ((uint32_t)bt[k].y << 16));
			cmd(0x70, ((uint32_t)bt[k].z & 0xFFFFu) | ((uint32_t)bt[k].w << 16));
			cmd(0x70, ((uint32_t)bt[k].h & 0xFFFFu) | ((uint32_t)bt[k].d << 16));
			idle();
			st = gxstat();
			rec(st);
			SDS_CHECK((st >> 1) & 1u, bt[k].want);
		}
	}
	/* el bit de ocupado sube mientras la prueba está en la FIFO o se ejecuta */
	cmd(0x70, 0);
	cmd(0x70, 0);
	cmd(0x70, 0);
	rec(gxstat());
	SDS_CHECK(gxstat() & 1u, 1u);
	idle();
	SDS_CHECK(gxstat() & 1u, 0u);
}

/* ---- listas de polígonos ------------------------------------------------------------------------------------ */

#define P(n, v) (((uint32_t)(n) << 16) | (uint32_t)(v))

static void group_lists(void)
{
	uint32_t r;

	sds_group("listas");
	model_reset();
	set_mode(0);
	op_ident();
	set_mode(2);
	op_ident();
	idle();
	/* triángulo antihorario (y hacia arriba) dentro del volumen */
	r = SC(ATTR_BOTH, 0, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 });
	SDS_CHECK(r, P(1, 3));
	/* los cuatro valores de las banderas de cara con un triángulo antihorario (frontal) y uno horario (trasero) */
	r = SC(0x001F0000u | 0x40, 0, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 }); /* sólo traseras */
	SDS_CHECK(r, P(0, 0));
	r = SC(0x001F0000u | 0x80, 0, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 }); /* sólo frontales */
	SDS_CHECK(r, P(1, 3));
	r = SC(0x001F0000u | 0x40, 0, { -0x800, -0x800, 0 }, { 0, 0x800, 0 }, { 0x800, -0x800, 0 });
	SDS_CHECK(r, P(1, 3));
	r = SC(0x001F0000u | 0x80, 0, { -0x800, -0x800, 0 }, { 0, 0x800, 0 }, { 0x800, -0x800, 0 });
	SDS_CHECK(r, P(0, 0));
	r = SC(0x001F0000u, 0, { -0x800, -0x800, 0 }, { 0, 0x800, 0 }, { 0x800, -0x800, 0 }); /* ninguna cara */
	SDS_CHECK(r, P(0, 0));
	/* fuera del volumen */
	r = SC(ATTR_BOTH, 0, { 0x1400, -0x800, 0 }, { 0x1800, -0x800, 0 }, { 0x1600, 0x800, 0 });
	SDS_CHECK(r, P(0, 0));
	r = SC(ATTR_BOTH, 0, { -0x800, 0x1400, 0 }, { 0x800, 0x1400, 0 }, { 0, 0x1800, 0 });
	SDS_CHECK(r, P(0, 0));
	/* cruza el borde derecho: ¿se recorta contra x = w? */
	r = SC(ATTR_BOTH, 0, { 0x800, -0x800, 0 }, { 0x1800, -0x800, 0 }, { 0x800, 0x800, 0 });
	SDS_CHECK(r, P(1, 4));
	/* cruza el borde izquierdo y el de abajo */
	r = SC(ATTR_BOTH, 0, { -0x1800, -0x1800, 0 }, { 0x800, -0x800, 0 }, { -0x800, 0x800, 0 });
	SDS_CHECK(r, P(1, 5));
	/* un triángulo enorme que contiene todo el volumen (los seis recortes lo dejan en un cuadrilátero) */
	r = SC(ATTR_BOTH, 0, { -0x7000, -0x7000, 0 }, { 0x7000, -0x7000, 0 }, { 0, 0x7000, 0 });
	SDS_CHECK(r, P(1, 4));
	/* plano cercano: z = -w = -1,0 */
	r = SC(ATTR_BOTH, 0, { -0x800, -0x800, -0x1800 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 });
	SDS_CHECK(r, P(1, 4));
	/* plano lejano: con el bit 12 de POLYGON_ATTR apagado se descarta; encendido se recorta */
	r = SC(ATTR_BOTH, 0, { -0x800, -0x800, 0x1800 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 });
	SDS_CHECK(r, P(0, 0));
	r = SC(ATTR_BOTH | 0x1000, 0, { -0x800, -0x800, 0x1800 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 });
	SDS_CHECK(r, P(1, 4));
	/* cuadrilátero, tira de 5 vértices (3 triángulos) y tira de cuadriláteros de 6 vértices (2) */
	r = SC(ATTR_BOTH, 1, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { 0x800, 0x800, 0 }, { -0x800, 0x800, 0 });
	SDS_CHECK(r, P(1, 4));
	r = SC(ATTR_BOTH, 2, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { -0x800, 0, 0 }, { 0x800, 0, 0 }, { -0x800, 0x800, 0 });
	SDS_CHECK(r, P(3, 5)); /* los vértices de una tira se guardan una sola vez */
	r = SC(ATTR_BOTH, 3, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { -0x800, 0, 0 }, { 0x800, 0, 0 }, { -0x800, 0x800, 0 },
	       { 0x800, 0x800, 0 });
	SDS_CHECK(r, P(2, 6));
	/* tres vértices y una tira de triángulos de dos vértices: no completa polígono */
	r = SC(ATTR_BOTH, 2, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 });
	SDS_CHECK(r, P(0, 0));
	r = SC(ATTR_BOTH, 0, { -0x800, -0x800, 0 }, { 0x800, -0x800, 0 }, { 0, 0x800, 0 }, { 0, 0x800, 0 }); /* 4 vértices: sobra uno */
	SDS_CHECK(r, P(1, 3));
	/* SWAP_BUFFERS: el contador vuelve a cero en el VBlank y el motor queda ocupado hasta entonces */
	idle();
	rec(ram_polys());
	SDS_CHECK(ram_polys() != 0, 1u);
	cmd(0x50, 0);
	rec(gxstat());
	SDS_CHECK(gxstat() >> 27 & 1u, 1u);
	wait_vblank_edge();
	idle();
	rec(MEM32(R_RAMCOUNT));
	SDS_CHECK(MEM32(R_RAMCOUNT), 0u);
	SDS_CHECK(gxstat() >> 27 & 1u, 0u);
}

/* Con un SWAP_BUFFERS delante el motor no avanza hasta el VBlank: la cuenta de la FIFO sólo depende de cuántas entradas
 * se escribieron (la tubería toma las primeras). */
static void swap_fill(void)
{
	static const uint16_t marks[] = { 1, 2, 3, 4, 5, 6, 7, 8, 12, 20, 100, 127, 128, 129, 132, 133, 140, 200, 250, 255, 256, 257, 258, 259, 260 };
	uint32_t k = 0, mi = 0;

	idle();
	wait_vblank_edge();
	idle();
	cmd(0x50, 0);
	while (mi < sizeof marks / sizeof marks[0]) {
		cmd(0x10, 1);
		k++;
		if (k == marks[mi]) {
			rec(gxstat());
			mi++;
		}
	}
	/* los 260 de arriba más la SWAP_BUFFERS (2 entradas con su parámetro) caben; al VBlank todo se vacía */
	idle();
	rec(gxstat());
	rec(MEM32(R_RAMCOUNT));
}

/* ---- FIFO e IRQ --------------------------------------------------------------------------------------------- */

static void group_fifo(void)
{
	uint32_t st, lv;

	sds_group("FIFO");
	swap_fill();
	idle();
	/* ¿Hay una IRQ 21 por nivel con la FIFO vacía? */
	MEM32(R_IF) = IRQ_GX;
	MEM32(R_GXSTAT) = 2u << 30;
	SDS_CHECK(WAIT_UNTIL(MEM32(R_IF) & IRQ_GX, 400000u), 1u);
	MEM32(R_GXSTAT) = 0;
	MEM32(R_IF) = IRQ_GX;
	/* muchas órdenes largas seguidas (MTX_POP de 0 niveles en el modo de textura: 36 ciclos del motor cada una, y la CPU
	 * escribe más rápido de lo que el motor las ejecuta): la cuenta de la FIFO sube */
	set_mode(1);
	for (int i = 0; i < 100; i++)
		cmd(0x13, 5); /* MTX_STORE: 17 ciclos del motor */
	st = gxstat();
	rec(st);
	lv = (st >> 16) & 0x1FFu;
	rec(lv);
	SDS_DIVERGE(st >> 27 & 1u, 1u); /* ocupado: melonDS vacía la FIFO a medida que se escribe (su CPU escribe más lento) */
	SDS_DIVERGE(lv > 30u && lv < 100u, 1u);
	SDS_DIVERGE((st >> 25) & 3u, 1u); /* menos de la mitad y no vacía */
	for (int i = 0; i < 150; i++)
		cmd(0x13, 5);
	st = gxstat();
	rec(st);
	lv = (st >> 16) & 0x1FFu;
	rec(lv);
	SDS_DIVERGE(lv > 128u && lv <= 256u, 1u);
	SDS_DIVERGE((st >> 25) & 1u, 0u); /* ya no es menos de la mitad */
	/* con la IRQ de «menos de la mitad» no llega hasta que baja de 128 */
	MEM32(R_IF) = IRQ_GX;
	MEM32(R_GXSTAT) = 1u << 30;
	idle();
	SDS_CHECK(WAIT_UNTIL(MEM32(R_IF) & IRQ_GX, 400000u), 1u);
	MEM32(R_GXSTAT) = 0;
	MEM32(R_IF) = IRQ_GX;
	idle();
	/* la FIFO se llena (260 entradas): la escritura que sobra frena a la CPU hasta que hay sitio, sin perder nada */
	set_mode(1);
	for (int i = 0; i < 600; i++)
		cmd(0x13, 5);
	idle();
	SDS_CHECK((gxstat() >> 16) & 0x1FFu, 0u);
	SDS_CHECK(gxstat() >> 27 & 1u, 0u);
	set_mode(2);
	op_ident();
	model_reset();
	check_mats();
	/* orden empaquetada: cuatro órdenes en una palabra (MTX_MODE, MTX_IDENTITY, MTX_MODE, MTX_IDENTITY) y el parámetro de
	 * cada MTX_MODE detrás, en orden */
	MEM32(GX_CMD(0)) = 0x15101510u; /* GXFIFO: órdenes 10h, 15h, 10h, 15h (el primero es el de menor peso) */
	MEM32(GX_CMD(0)) = 0u;          /* parámetro del primer MTX_MODE: proyección */
	MEM32(GX_CMD(0)) = 1u;          /* parámetro del segundo MTX_MODE: posición */
	idle();
	check_mats();
	/* palabra empaquetada con ceros en medio: 00h se salta */
	MEM32(GX_CMD(0)) = 0x00001B00u | 0x10u << 16; /* 00h, 1Bh... (orden en el byte 1), 10h */
	MEM32(GX_CMD(0)) = 0x3000u;
	MEM32(GX_CMD(0)) = 0x3000u;
	MEM32(GX_CMD(0)) = 0x3000u;
	MEM32(GX_CMD(0)) = 2u; /* MTX_MODE = 2 */
	idle();
	{
		M4 a;

		g_mode = 1;
		m_ident(&a);
		a.m[0] = a.m[5] = a.m[10] = 0x3000;
		g_pos = a;
		g_mode = 2;
		check_mats();
	}
	set_mode(2);
	op_ident();
	check_mats();
}

int main(void)
{
	sds_nocache();
	MEM32(R_IME) = 0;
	sds_init("GEOM");
	MEM16(R_POWCNT1) |= 0x0Cu; /* geometría y render encendidos */
	for (volatile int i = 0; i < 1000; i++)
		;
	group_state();
	group_mats();
	group_stacks();
	group_tests();
	group_lists();
	group_fifo();
	sds_notef("geom: %lu valores en la tabla de 0x02300000", (unsigned long)ti);
	sds_finish();
}
