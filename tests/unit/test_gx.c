/* SalsaDS — pruebas unitarias del motor de geometría (src/gpu3d/gx.c, Fase 3.1).
 *
 * El motor es portable y sin dependencias: aquí se maneja como lo hace el núcleo (escrituras a los puertos de órdenes y a
 * GXFIFO con un reloj propio, lecturas de GXSTAT y de los registros de resultados) y se comprueban matrices contra un modelo
 * independiente, la decodificación de palabras empaquetadas, las pilas, la lista de polígonos (cara, recorte, tiras), las
 * órdenes de prueba, la cuenta de la FIFO y el cambio de búfer. Corre en little-endian y en big-endian (qemu-ppc64). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "gpu3d/gx.h"

static gx_t G;
static uint64_t T; /* reloj de la «CPU» en ciclos del ARM9 */

#define PORT(c) (0x400u + 4u * (c))

static void w(uint32_t c, uint32_t p)
{
	gx_port_write(&G, T, PORT(c), p);
	T += 8;
}

static void wf(uint32_t word) /* GXFIFO */
{
	gx_fifo_write(&G, T, word);
	T += 8;
}

static void idle(void)
{
	T += 2000000;
	gx_advance(&G, T);
}

static void reset(void)
{
	gx_init(&G);
	T = 1000;
}

/* ---- modelo independiente de matrices (fila x columna, 64 bits, >> 12) ---- */
typedef struct {
	int32_t m[16];
} M;

static void mid(M *a)
{
	memset(a, 0, sizeof *a);
	a->m[0] = a->m[5] = a->m[10] = a->m[15] = 0x1000;
}

static void mmul(M *o, const M *a, const M *b)
{
	M r;

	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			int64_t s = 0;

			for (int k = 0; k < 4; k++)
				s += (int64_t)a->m[i * 4 + k] * b->m[k * 4 + j];
			r.m[i * 4 + j] = (int32_t)(s >> 12);
		}
	*o = r;
}

static uint32_t lcg = 77;
static int32_t rnd(int32_t range)
{
	lcg = lcg * 1664525u + 1013904223u;
	return (int32_t)((lcg >> 8) % (uint32_t)(2 * range + 1)) - range;
}

static void check_clip(const M *pos, const M *proj, const M *vec)
{
	M c;

	idle();
	mmul(&c, pos, proj);
	for (int i = 0; i < 16; i++)
		CHECK_EQ(gx_read32(&G, T, 0x640u + 4u * (uint32_t)i), (uint32_t)c.m[i]);
	for (int r = 0; r < 3; r++)
		for (int k = 0; k < 3; k++)
			CHECK_EQ(gx_read32(&G, T, 0x680u + 4u * (uint32_t)(r * 3 + k)), (uint32_t)vec->m[r * 4 + k]);
}

static void test_matrices(void)
{
	M proj, pos, vec;

	reset();
	mid(&proj);
	mid(&pos);
	mid(&vec);
	check_clip(&pos, &proj, &vec);
	for (int round = 0; round < 40; round++) {
		M a;
		const uint32_t mode = (uint32_t)(round % 3);
		int32_t p[16];

		w(0x10, mode);
		for (int i = 0; i < 16; i++)
			p[i] = rnd(0x1800);
		/* MTX_MULT_4x4 */
		for (int i = 0; i < 16; i++) {
			w(0x18, (uint32_t)p[i]);
			a.m[i] = p[i];
		}
		if (mode == 0) mmul(&proj, &a, &proj);
		else { mmul(&pos, &a, &pos); if (mode == 2) mmul(&vec, &a, &vec); }
		check_clip(&pos, &proj, &vec);
		/* MTX_MULT_4x3 */
		mid(&a);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++) {
				a.m[r * 4 + c] = p[r * 3 + c];
				w(0x19, (uint32_t)p[r * 3 + c]);
			}
		if (mode == 0) mmul(&proj, &a, &proj);
		else { mmul(&pos, &a, &pos); if (mode == 2) mmul(&vec, &a, &vec); }
		check_clip(&pos, &proj, &vec);
		/* MTX_MULT_3x3 */
		mid(&a);
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) {
				a.m[r * 4 + c] = p[r * 3 + c + 1];
				w(0x1A, (uint32_t)p[r * 3 + c + 1]);
			}
		if (mode == 0) mmul(&proj, &a, &proj);
		else { mmul(&pos, &a, &pos); if (mode == 2) mmul(&vec, &a, &vec); }
		check_clip(&pos, &proj, &vec);
		/* MTX_SCALE: no toca la vectorial */
		mid(&a);
		a.m[0] = p[0]; a.m[5] = p[1]; a.m[10] = p[2];
		w(0x1B, (uint32_t)p[0]); w(0x1B, (uint32_t)p[1]); w(0x1B, (uint32_t)p[2]);
		if (mode == 0) mmul(&proj, &a, &proj);
		else mmul(&pos, &a, &pos);
		check_clip(&pos, &proj, &vec);
		/* MTX_TRANS */
		mid(&a);
		a.m[12] = p[3]; a.m[13] = p[4]; a.m[14] = p[5];
		w(0x1C, (uint32_t)p[3]); w(0x1C, (uint32_t)p[4]); w(0x1C, (uint32_t)p[5]);
		if (mode == 0) mmul(&proj, &a, &proj);
		else { mmul(&pos, &a, &pos); if (mode == 2) mmul(&vec, &a, &vec); }
		check_clip(&pos, &proj, &vec);
		/* MTX_LOAD_4x3 e IDENTITY */
		mid(&a);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++) {
				a.m[r * 4 + c] = p[r * 3 + c];
				w(0x17, (uint32_t)p[r * 3 + c]);
			}
		if (mode == 0) proj = a;
		else { pos = a; if (mode == 2) vec = a; }
		check_clip(&pos, &proj, &vec);
		w(0x15, 0);
		mid(&a);
		if (mode == 0) proj = a;
		else { pos = a; if (mode == 2) vec = a; }
		check_clip(&pos, &proj, &vec);
	}
	/* un producto que trunca hacia menos infinito: (-1 * 1) >> 12 = -1 */
	reset();
	w(0x10, 1);
	w(0x1B, 0xFFFFFFFFu); /* escala -1/4096 */
	w(0x1B, 0x1000); w(0x1B, 0x1000);
	idle();
	CHECK_EQ(gx_read32(&G, T, 0x640u), 0xFFFFFFFFu);
	w(0x1B, 0x800); /* (-1 * 0x800) >> 12 = -1 y no 0 */
	w(0x1B, 0x1000); w(0x1B, 0x1000);
	idle();
	CHECK_EQ(gx_read32(&G, T, 0x640u), 0xFFFFFFFFu);
}

static void test_packed(void)
{
	reset();
	/* cuatro órdenes empaquetadas: MTX_MODE(1) MTX_IDENTITY MTX_MODE(0) MTX_IDENTITY; los parámetros después */
	wf(0x15101510u);
	wf(0u);
	wf(1u);
	idle();
	CHECK_EQ(G.mode, 1u);
	CHECK_EQ(G.st.cmds, 4u);
	/* 00h en medio se salta; una orden con 3 parámetros (MTX_SCALE) y luego MTX_MODE */
	wf(0x00101B00u);
	wf(0x3000u); wf(0x3000u); wf(0x3000u);
	wf(2u);
	idle();
	CHECK_EQ(G.mode, 2u);
	CHECK_EQ(G.pos.m[0], 0x3000);
	CHECK_EQ(G.vec.m[0], 0x1000); /* la escala no toca la vectorial */
	/* palabra de ceros y órdenes inexistentes: nada */
	{
		const uint64_t before = G.st.cmds;

		wf(0u);
		wf(0xFFFFFFFFu);
		idle();
		CHECK_EQ(G.st.cmds, before);
		CHECK(G.st.unknown_cmds >= 1);
	}
	/* una orden sin parámetros ocupa una entrada; una con parámetros ocupa tantas como parámetros */
	reset();
	w(0x50, 0); /* SWAP_BUFFERS detiene el motor hasta el VBlank */
	idle();
	for (int i = 0; i < 10; i++)
		w(0x11, 0);
	CHECK_EQ(gx_fifo_level(&G), 10u - GX_PIPE_ENTRIES);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_BUSY, GXSTAT_BUSY);
	{
		const uint32_t st = gx_gxstat(&G, T);

		CHECK_EQ((st >> 16) & 0x1FFu, 6u);
		CHECK_EQ(st & GXSTAT_HALF, GXSTAT_HALF);
		CHECK_EQ(st & GXSTAT_EMPTY, 0u);
	}
}

static void test_fifo_levels(void)
{
	uint32_t st;

	reset();
	st = gx_gxstat(&G, T);
	CHECK_EQ((st >> 24) & 7u, 6u); /* vacía y menos de la mitad */
	CHECK_EQ(st & 0x08000000u, 0u);
	w(0x50, 0);
	idle();
	/* el motor parado: la cuenta es entradas - 4 (la tubería) y llega a 256 con 260 */
	for (uint32_t k = 1; k <= 260; k++) {
		w(0x10, 1);
		st = gx_gxstat(&G, T);
		if (k == 4)
			CHECK_EQ((st >> 16) & 0x1FFu, 0u);
		if (k == 5)
			CHECK_EQ((st >> 16) & 0x1FFu, 1u);
		if (k == 131)
			CHECK_EQ(st & GXSTAT_HALF, GXSTAT_HALF);
		if (k == 132)
			CHECK_EQ(st & GXSTAT_HALF, 0u);
	}
	CHECK_EQ((gx_gxstat(&G, T) >> 16) & 0x1FFu, 256u);
	/* una escritura más no cabe y con el motor parado se pierde (la CPU real se queda esperando el VBlank) */
	w(0x10, 1);
	CHECK_EQ(G.st.fifo_overflow, 1u);
	/* el VBlank libera el SWAP_BUFFERS y la cola se vacía */
	gx_advance(&G, T);
	CHECK(gx_swap_pending(&G));
	gx_swap(&G, T, NULL);
	idle();
	CHECK_EQ(gx_fifo_level(&G), 0u);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_BUSY, 0u);
	/* IRQ por nivel */
	G.irqmode = 1;
	CHECK(gx_irq_level(&G));
	G.irqmode = 2;
	CHECK(gx_irq_level(&G));
	w(0x50, 0);
	for (int i = 0; i < 200; i++)
		w(0x10, 1);
	G.irqmode = 1;
	CHECK(!gx_irq_level(&G));
	G.irqmode = 2;
	CHECK(!gx_irq_level(&G));
	G.irqmode = 0;
	CHECK(!gx_irq_level(&G));
}

static void test_timing(void)
{
	reset();
	/* MTX_POP en el modo 1: 36 ciclos del motor = 72 del ARM9; tras la escritura la orden empieza en seguida */
	w(0x10, 1);
	w(0x11, 0);
	w(0x12, 1);
	gx_advance(&G, T);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_BUSY, GXSTAT_STACK_BUSY);
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & (GXSTAT_STACK_BUSY | GXSTAT_BUSY), 0u);
	/* varias órdenes seguidas: cada una empieza cuando termina la anterior */
	reset();
	for (int i = 0; i < 10; i++)
		w(0x15, 0); /* MTX_IDENTITY, 19 ciclos */
	{
		const uint64_t start = T - 80;
		uint64_t end = start + 10ull * 19 * 2;

		gx_advance(&G, T);
		/* aún queda trabajo */
		CHECK(gx_gxstat(&G, T) & GXSTAT_BUSY);
		gx_advance(&G, end + 100);
		CHECK_EQ(gx_gxstat(&G, end + 100) & GXSTAT_BUSY, 0u);
	}
	/* BOX_TEST: el bit de ocupado sube con la orden en la FIFO y baja al terminar (103 ciclos) */
	reset();
	w(0x70, 0); w(0x70, 0); w(0x70, 0);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_TEST_BUSY, GXSTAT_TEST_BUSY);
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_TEST_BUSY, 0u);
}

static void test_stacks(void)
{
	reset();
	w(0x10, 2);
	for (int i = 0; i < 31; i++) {
		w(0x11, 0);
		w(0x1C, 0x1000); w(0x1C, 0); w(0x1C, 0); /* traslación para distinguir */
	}
	idle();
	CHECK_EQ((gx_gxstat(&G, T) >> 8) & 0x1Fu, 31u);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, 0u);
	w(0x11, 0); /* la 32.ª: error, y GXSTAT ve el puntero (32) con 5 bits */
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, GXSTAT_STACK_ERR);
	CHECK_EQ((gx_gxstat(&G, T) >> 8) & 0x1Fu, 0u);
	gx_gxstat_write(&G, GXSTAT_STACK_ERR, 0x8000u);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, 0u);
	/* POP de 1 desde el puntero 32: queda en 31 y es válido (31 > 30: error) */
	w(0x12, 1);
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, GXSTAT_STACK_ERR);
	gx_gxstat_write(&G, GXSTAT_STACK_ERR, 0x8000u);
	/* POP de 31: queda en 0 y se restaura la entrada 0 (traslación 0: la identidad) */
	w(0x12, 31);
	idle();
	CHECK_EQ((gx_gxstat(&G, T) >> 8) & 0x1Fu, 0u);
	CHECK_EQ(G.pos.m[12], 0);
	/* STORE y RESTORE absolutos */
	w(0x1C, 0x2000); w(0x1C, 0); w(0x1C, 0);
	w(0x13, 7);
	w(0x1C, 0x2000); w(0x1C, 0); w(0x1C, 0);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, 0u);
	w(0x14, 7);
	idle();
	CHECK_EQ(G.pos.m[12], 0x2000);
	w(0x13, 31);
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, GXSTAT_STACK_ERR);
	gx_gxstat_write(&G, GXSTAT_STACK_ERR, 0x8000u);
	/* proyección: una entrada */
	w(0x10, 0);
	w(0x11, 0);
	idle();
	CHECK_EQ((gx_gxstat(&G, T) >> 13) & 1u, 1u);
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, 0u);
	w(0x11, 0);
	idle();
	CHECK_EQ(gx_gxstat(&G, T) & GXSTAT_STACK_ERR, GXSTAT_STACK_ERR);
}

/* ---- polígonos ---- */
#define V(x, y, z) { (x), (y), (z) }

static uint32_t scene(uint32_t attr, uint32_t prim, const int32_t (*v)[3], int n)
{
	uint32_t p0, v0, dp, dv;

	idle();
	p0 = gx_ram_count(&G) & 0xFFF;
	v0 = gx_ram_count(&G) >> 16;
	w(0x29, attr);
	w(0x40, prim);
	for (int i = 0; i < n; i++) {
		w(0x23, ((uint32_t)v[i][0] & 0xFFFFu) | ((uint32_t)v[i][1] << 16));
		w(0x23, (uint32_t)v[i][2] & 0xFFFFu);
	}
	w(0x41, 0);
	idle();
	dp = (gx_ram_count(&G) & 0xFFF) - p0;
	dv = (gx_ram_count(&G) >> 16) - v0;
	return (dp << 16) | dv;
}

#define SC(attr, prim, ...)                                                      \
	({                                                                       \
		static const int32_t vv_[][3] = { __VA_ARGS__ };                 \
		scene((attr), (prim), vv_, (int)(sizeof vv_ / sizeof vv_[0]));   \
	})
#define P(n, v) (((uint32_t)(n) << 16) | (uint32_t)(v))
#define BOTH 0x001F00C0u

static void ident_all(void)
{
	w(0x10, 0);
	w(0x15, 0);
	w(0x10, 2);
	w(0x15, 0);
	idle();
}

static void test_polygons(void)
{
	reset();
	ident_all();
	/* antihorario (y hacia arriba) dentro del volumen: frontal */
	CHECK_EQ(SC(BOTH, 0, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(1, 3));
	CHECK_EQ(SC(0x001F0040u, 0, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(0, 0));
	CHECK_EQ(SC(0x001F0080u, 0, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(1, 3));
	CHECK_EQ(SC(0x001F0040u, 0, V(-0x800, -0x800, 0), V(0, 0x800, 0), V(0x800, -0x800, 0)), P(1, 3));
	CHECK_EQ(SC(0x001F0080u, 0, V(-0x800, -0x800, 0), V(0, 0x800, 0), V(0x800, -0x800, 0)), P(0, 0));
	CHECK_EQ(SC(0x001F0000u, 0, V(-0x800, -0x800, 0), V(0, 0x800, 0), V(0x800, -0x800, 0)), P(0, 0));
	/* fuera del volumen */
	CHECK_EQ(SC(BOTH, 0, V(0x1400, -0x800, 0), V(0x1800, -0x800, 0), V(0x1600, 0x800, 0)), P(0, 0));
	/* recortes: un lado, dos, enorme, cercano, lejano con y sin permiso */
	CHECK_EQ(SC(BOTH, 0, V(0x800, -0x800, 0), V(0x1800, -0x800, 0), V(0x800, 0x800, 0)), P(1, 4));
	CHECK_EQ(SC(BOTH, 0, V(-0x1800, -0x1800, 0), V(0x800, -0x800, 0), V(-0x800, 0x800, 0)), P(1, 5));
	CHECK_EQ(SC(BOTH, 0, V(-0x7000, -0x7000, 0), V(0x7000, -0x7000, 0), V(0, 0x7000, 0)), P(1, 4));
	CHECK_EQ(SC(BOTH, 0, V(-0x800, -0x800, -0x1800), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(1, 4));
	CHECK_EQ(SC(BOTH, 0, V(-0x800, -0x800, 0x1800), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(0, 0));
	CHECK_EQ(SC(BOTH | 0x1000, 0, V(-0x800, -0x800, 0x1800), V(0x800, -0x800, 0), V(0, 0x800, 0)), P(1, 4));
	/* cuadrilátero y tiras (los vértices de una tira se guardan una vez) */
	CHECK_EQ(SC(BOTH, 1, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(0x800, 0x800, 0), V(-0x800, 0x800, 0)), P(1, 4));
	CHECK_EQ(SC(BOTH, 2, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(-0x800, 0, 0), V(0x800, 0, 0), V(-0x800, 0x800, 0)), P(3, 5));
	CHECK_EQ(SC(BOTH, 3, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(-0x800, 0, 0), V(0x800, 0, 0), V(-0x800, 0x800, 0),
		    V(0x800, 0x800, 0)), P(2, 6));
	CHECK_EQ(SC(BOTH, 2, V(-0x800, -0x800, 0), V(0x800, -0x800, 0)), P(0, 0));
	CHECK_EQ(SC(BOTH, 0, V(-0x800, -0x800, 0), V(0x800, -0x800, 0), V(0, 0x800, 0), V(0, 0x800, 0)), P(1, 3));
	/* SWAP_BUFFERS: el cuadro queda entregado y el contador vuelve a 0 */
	w(0x50, 3);
	idle();
	CHECK(gx_swap_pending(&G));
	{
		const gx_frame *f = gx_swap(&G, T, NULL);

		CHECK(f != NULL);
		CHECK_EQ(f->swap_params, 3u);
		CHECK_EQ(f->npolys, 15u);
		CHECK_EQ(gx_ram_count(&G), 0u);
		CHECK(gx_front(&G) == f);
	}
}

static void test_screen_output(void)
{
	const gx_frame *f;
	const gx_poly *p;

	reset();
	ident_all();
	w(0x60, 0u | (0u << 8) | (255u << 16) | (191u << 24)); /* viewport completo */
	w(0x2A, 0);
	w(0x20, 0x7FFFu);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x22, 0u | (0x10u << 16)); /* s = 0, t = 1,0 */
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0); /* (-0,5, -0,5, 0) */
	w(0x22, 0x100u | (0x10u << 16));
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x20, 0x001Fu); /* rojo */
	w(0x22, 0x80u | (0x100u << 16));
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->npolys, 1u);
	p = &f->poly[0];
	CHECK_EQ(p->nverts, 3u);
	CHECK(p->flags & GX_POLY_FRONT);
	CHECK_EQ(p->attr, BOTH);
	/* pantalla: x = (x + w) * 256 / (2 w) = 64 y 192; y se invierte: (w - y) * 192 / (2 w) = 144 y 48; z = 0x3FFF << 9 */
	CHECK_EQ(f->vtx[p->vidx[0]].sx, 64);
	CHECK_EQ(f->vtx[p->vidx[0]].sy, 144);
	CHECK_EQ(f->vtx[p->vidx[1]].sx, 192);
	CHECK_EQ(f->vtx[p->vidx[1]].sy, 144);
	CHECK_EQ(f->vtx[p->vidx[2]].sx, 128);
	CHECK_EQ(f->vtx[p->vidx[2]].sy, 48);
	CHECK_EQ(f->vtx[p->vidx[0]].z, 0x3FFFu << 9);
	CHECK_EQ(f->vtx[p->vidx[0]].w, 0x1000);
	CHECK_EQ(p->wshift, 0u);
	/* colores de 6 bits (31 -> 63) y coordenadas de textura sin transformar */
	CHECK_EQ(f->vtx[p->vidx[0]].r, 63u);
	CHECK_EQ(f->vtx[p->vidx[0]].g, 63u);
	CHECK_EQ(f->vtx[p->vidx[2]].r, 63u);
	CHECK_EQ(f->vtx[p->vidx[2]].g, 0u);
	CHECK_EQ(f->vtx[p->vidx[1]].s, 0x100);
	CHECK_EQ(f->vtx[p->vidx[1]].t, 0x10);
	CHECK_EQ(f->vtx[p->vidx[2]].t, 0x100);
	/* viewport más chico: (64, 32) a (127, 95) = 64 x 64 */
	w(0x60, 64u | (32u << 8) | (127u << 16) | (95u << 24));
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	p = &f->poly[0];
	CHECK_EQ(f->vtx[p->vidx[0]].sx, 64 + 16);
	CHECK_EQ(f->vtx[p->vidx[1]].sx, 64 + 48);
	/* y de abajo hacia arriba: la fila superior del viewport es 191 - y2 = 96 */
	CHECK_EQ(f->vtx[p->vidx[2]].sy, 96 + 16);
	CHECK_EQ(f->vtx[p->vidx[0]].sy, 96 + 48);
}

static void test_tests(void)
{
	reset();
	ident_all();
	/* POS_TEST con la identidad: la clip da el mismo vértice con w = 1,0 */
	w(0x71, 0x1000u | (0x2000u << 16));
	w(0x71, 0x3000u);
	idle();
	CHECK_EQ(gx_read32(&G, T, 0x620), 0x1000u);
	CHECK_EQ(gx_read32(&G, T, 0x624), 0x2000u);
	CHECK_EQ(gx_read32(&G, T, 0x628), 0x3000u);
	CHECK_EQ(gx_read32(&G, T, 0x62C), 0x1000u);
	/* VEC_TEST: (1.0.9) * 8 = 1.0.12 por la identidad; el bit 12 enciende los de arriba (melonDS) */
	w(0x72, (0x100u) | (0x80u << 10) | ((uint32_t)(-0x100) & 0x3FFu) << 20);
	idle();
	CHECK_EQ(gx_read32(&G, T, 0x630), 0x0800u | (0x0400u << 16));
	CHECK_EQ(gx_read32(&G, T, 0x634), 0xF800u);
	/* BOX_TEST */
	{
		static const struct {
			int16_t x, y, z, w, h, d;
			uint8_t want;
		} bt[] = {
			{ -0x400, -0x400, -0x400, 0x800, 0x800, 0x800, 1 },
			{ 0x2000, 0x2000, 0x2000, 0x400, 0x400, 0x400, 0 },
			{ 0x800, -0x400, -0x400, 0x1000, 0x800, 0x800, 1 },
			{ 0x1400, -0x400, -0x400, 0x400, 0x800, 0x800, 0 },
			{ -0x400, -0x400, -0x2000, 0x800, 0x800, 0x1800, 1 },
			{ -0x400, -0x400, 0x1400, 0x800, 0x800, 0x400, 0 },
			{ -0x2000, -0x2000, -0x2000, 0x4000, 0x4000, 0x4000, 0 },
		};
		for (unsigned k = 0; k < sizeof bt / sizeof bt[0]; k++) {
			w(0x70, ((uint32_t)bt[k].x & 0xFFFFu) | ((uint32_t)bt[k].y << 16));
			w(0x70, ((uint32_t)bt[k].z & 0xFFFFu) | ((uint32_t)bt[k].w << 16));
			w(0x70, ((uint32_t)bt[k].h & 0xFFFFu) | ((uint32_t)bt[k].d << 16));
			idle();
			CHECK_EQ((gx_gxstat(&G, T) >> 1) & 1u, bt[k].want);
		}
	}
}

static void test_lighting(void)
{
	const gx_frame *f;

	reset();
	ident_all();
	/* una luz blanca que apunta hacia -z y un vértice con normal hacia +z: el nivel difuso es el máximo (255) y el color sale
	 * (31 * 31 * 255) >> 13 = 29 con el difuso blanco; el ambiente aporta (a * 31) >> 5 */
	w(0x30, 0x7FFFu | 0x8000u); /* difuso blanco; también es el color del vértice */
	w(0x31, 0);
	w(0x33, 0x7FFFu);                              /* luz 0 blanca */
	w(0x32, (uint32_t)(0x000u | (0x000u << 10) | ((uint32_t)(-0x1FF) & 0x3FFu) << 20)); /* luz 0 hacia -z */
	w(0x29, 0x001F00C1u);                          /* luz 0 encendida, ambas caras */
	w(0x40, 0);
	w(0x21, 0x000u | (0x000u << 10) | (0x1FFu << 20)); /* normal +z */
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->npolys, 1u);
	/* 29 de 31 -> 6 bits: (29 << 1) + ((29 + 31) >> 5) = 59 */
	CHECK_EQ(f->vtx[f->poly[0].vidx[0]].r, 59u);
	/* una luz que mira de lado no ilumina: sólo la emisión (aquí 0) */
	w(0x32, (uint32_t)(0x1FFu | (0x000u << 10) | (0x000u << 20)));
	w(0x29, 0x001F00C1u);
	w(0x40, 0);
	w(0x21, 0x000u | (0x000u << 10) | (0x1FFu << 20));
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->vtx[f->poly[0].vidx[0]].r, 0u);
	/* la emisión suma y se satura a 31 */
	w(0x31, 0x1Fu << 16 | 0x1Fu << 21 | 0x1Fu << 26);
	w(0x29, 0x001F00C0u);
	w(0x40, 0);
	w(0x21, 0);
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->vtx[f->poly[0].vidx[0]].r, 63u);
}

static void test_limits(void)
{
	reset();
	ident_all();
	/* 2050 triángulos: caben 2048 polígonos y 6144 vértices; los demás se descartan y se cuentan */
	for (int i = 0; i < 2050; i++) {
		w(0x29, BOTH);
		w(0x40, 0);
		w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
		w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
		w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
		if (i % 50 == 49)
			idle();
	}
	idle();
	CHECK_EQ(gx_ram_count(&G) & 0xFFFu, 2048u);
	CHECK_EQ(gx_ram_count(&G) >> 16, 6144u);
	CHECK_EQ(G.st.dropped_poly, 2u);
}

static void test_clip_interp(void)
{
	const gx_frame *f;

	reset();
	ident_all();
	/* un triángulo que cruza el plano derecho: sus vértices nuevos llevan color y textura interpolados */
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x20, 0u); w(0x22, 0u);
	w(0x23, 0x0000u | (0xF000u << 16)); w(0x23, 0);       /* (0, -1) color 0, s = 0 */
	w(0x20, 0x1Fu); w(0x22, 0x100u);
	w(0x23, 0x2000u | (0xF000u << 16)); w(0x23, 0);       /* (2, -1): fuera, color 31, s = 16 */
	w(0x20, 0u); w(0x22, 0u);
	w(0x23, 0x0000u | (0x1000u << 16)); w(0x23, 0);       /* (0, 1) */
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->npolys, 1u);
	CHECK(f->poly[0].flags & GX_POLY_CLIPPED);
	CHECK_EQ(f->poly[0].nverts, 4u);
	/* el vértice nuevo sobre x = w = 1,0 está a la mitad de la arista de (0,-1) a (2,-1): rojo a la mitad (63 / 2 -> 31) */
	{
		int found = 0;

		for (unsigned i = 0; i < 4; i++) {
			const gx_vertex *v = &f->vtx[f->poly[0].vidx[i]];

			if (v->sx == 256 && v->sy == 192) {
				found = 1;
				CHECK_EQ(v->r, 31u);
				CHECK_EQ(v->s, 0x80);
			}
		}
		CHECK(found);
	}
}

/* Coordenadas de textura generadas (TEXIMAGE_PARAM bits 30-31) y VTX_DIFF: las reglas medidas contra melonDS con la ROM
 * gx3d (escena 15, docs/fase3.md 3.3). Un triángulo cuyo primer vértice lleva lo que se prueba; se lee su (s, t). */
static void texmat(int32_t d0, int32_t d1, int32_t d4, int32_t d5, int32_t d8, int32_t d9, int32_t t12, int32_t t13)
{
	const int32_t m[16] = { d0, d1, 0, 0, d4, d5, 0, 0, d8, d9, 0x1000, 0, t12, t13, 0, 0x1000 };

	w(0x10, 3);
	for (int i = 0; i < 16; i++)
		w(0x16, (uint32_t)m[i]);
	w(0x10, 2);
}

static const gx_vertex *first_vertex(const gx_frame **fo)
{
	const gx_frame *f;

	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0); /* los dos vértices que completan el triángulo */
	w(0x23, 0u | (0x0800u << 16)); w(0x23, 0);
	w(0x41, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	*fo = f;
	return f->npolys ? &f->vtx[f->poly[0].vidx[0]] : NULL;
}

static void test_texgen(void)
{
	const gx_frame *f;
	const gx_vertex *v;

	/* VTX_DIFF: la diferencia de 10 bits con signo se suma tal cual (1/4096), no por 8 */
	reset();
	ident_all();
	w(0x60, 0u | (0u << 8) | (255u << 16) | (191u << 24));
	w(0x2A, 0);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	w(0x23, 0x0800u | (0xF800u << 16)); w(0x23, 0);
	w(0x28, (uint32_t)(-300 & 0x3FF) | ((uint32_t)(400 & 0x3FF) << 10)); /* (0x0800 - 300, -0x0800 + 400) */
	w(0x41, 0);
	w(0x50, 0);
	idle();
	f = gx_swap(&G, T, NULL);
	CHECK_EQ(f->npolys, 1u);
	CHECK_EQ(f->vtx[f->poly[0].vidx[2]].sx, (int16_t)(((0x800 - 300 + 0x1000) * 256) / 0x2000));
	CHECK_EQ(f->vtx[f->poly[0].vidx[2]].sy, (int16_t)(((0x1000 - (-0x800 + 400)) * 192) / 0x2000));

	/* modo 1: (s, t, 1, 1) por la matriz: s' = (s·m0 + t·m4 + m8 + m12) >> 12, sin multiplicar la traslación por 16 */
	reset();
	ident_all();
	texmat(0x2000, 0x0800, 0x1000, 0x3000, 0x0400, 0x0200, 0x5000, 0x2000);
	w(0x2A, 1u << 30);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x22, 0x0030u | (0x0050u << 16)); /* s = 3, t = 5 texeles */
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	v = first_vertex(&f);
	CHECK(v != NULL);
	CHECK_EQ(v->s, (int16_t)((0x30 * 0x2000 + 0x50 * 0x1000 + 0x0400 + 0x5000) >> 12));
	CHECK_EQ(v->t, (int16_t)((0x30 * 0x0800 + 0x50 * 0x3000 + 0x0200 + 0x2000) >> 12));

	/* modo 2: NORMAL calcula (crudas + normal x matriz >> 21, sin traslación); un TEXCOORD posterior las pisa */
	reset();
	ident_all();
	texmat(0x100000, 0x20000, 0x40000, 0x100000, 0x10000, 0x8000, 0x70000, 0x70000);
	w(0x2A, 2u << 30);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x22, 0x0030u | (0x0050u << 16));
	w(0x21, (uint32_t)(300 & 0x3FF) | ((uint32_t)(-200 & 0x3FF) << 10) | ((uint32_t)(100 & 0x3FF) << 20));
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	v = first_vertex(&f);
	CHECK_EQ(v->s, (int16_t)(0x30 + ((300LL * 0x100000 + -200LL * 0x40000 + 100LL * 0x10000) >> 21)));
	CHECK_EQ(v->t, (int16_t)(0x50 + ((300LL * 0x20000 + -200LL * 0x100000 + 100LL * 0x8000) >> 21)));
	reset();
	ident_all();
	texmat(0x100000, 0, 0, 0x100000, 0, 0, 0, 0);
	w(0x2A, 2u << 30);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x21, 300);
	w(0x22, 0x0030u | (0x0050u << 16));
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	v = first_vertex(&f);
	CHECK_EQ(v->s, 0x30);
	CHECK_EQ(v->t, 0x50);

	/* modo 3: crudas + vértice x matriz >> 24, sin traslación */
	reset();
	ident_all();
	texmat(0x100000, 0x10000, 0x20000, 0x100000, 0, 0, 0x300000, 0x300000);
	w(0x2A, 3u << 30);
	w(0x29, BOTH);
	w(0x40, 0);
	w(0x22, 0x0030u | (0x0050u << 16));
	w(0x23, 0xF800u | (0xF800u << 16)); w(0x23, 0);
	v = first_vertex(&f);
	CHECK_EQ(v->s, (int16_t)(0x30 + ((-0x800LL * 0x100000 + -0x800LL * 0x20000) >> 24)));
	CHECK_EQ(v->t, (int16_t)(0x50 + ((-0x800LL * 0x10000 + -0x800LL * 0x100000) >> 24)));
}

void test_gx(void)
{
	test_matrices();
	test_packed();
	test_fifo_levels();
	test_timing();
	test_stacks();
	test_polygons();
	test_screen_output();
	test_tests();
	test_lighting();
	test_limits();
	test_clip_interp();
	test_texgen();
}
