/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Caminos rápidos de la E/S lenta del ARM9 (sched.c dma_block, math.c por palabra y división perezosa, cart.c ROMCTRL
 * directo, mem.c vaciado del ARM7 solo si cambia su mapa): cada uno comparado con la forma anterior en dos máquinas, una con
 * las banderas `dma_lento`/`es_lenta`/`vram7_vaciar_siempre` (lo de antes) y otra sin ellas. El estado tiene que quedar igual
 * byte a byte: memorias, registro de cambios del vídeo, páginas sucias, motor de geometría y lo que lee la CPU.
 */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "core/nds_priv.h"

static struct nds *machine(int old)
{
	struct nds *n = NULL;

	CHECK_EQ(posix_memalign((void **)&n, 128, sizeof *n), 0);
	if (!n)
		return NULL;
	memset(n, 0, sizeof *n);
	n->dma_lento = n->es_lenta = n->vram7_vaciar_siempre = (uint8_t)old;
	nds_init(n);
	return n;
}

static uint32_t rnd_state = 0x1234567u;
static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1664525u + 1013904223u;
	return rnd_state;
}

/* Rellena las memorias de las dos máquinas con lo mismo. */
static void seed_mem(struct nds *a, struct nds *b)
{
	for (size_t i = 0; i < NDS_RAM_SIZE; i += 4)
		wr32le(a->ram + i, rnd());
	for (size_t i = 0; i < NDS_VRAM_SIZE; i += 4)
		wr32le(a->vram + i, rnd());
	for (size_t i = 0; i < NDS_SWRAM_SIZE; i += 4)
		wr32le(a->swram + i, rnd());
	memcpy(b->ram, a->ram, NDS_RAM_SIZE);
	memcpy(b->vram, a->vram, NDS_VRAM_SIZE);
	memcpy(b->swram, a->swram, NDS_SWRAM_SIZE);
}

/* Una DMA inmediata del canal `ch` de `cpu`. */
static void dma(struct nds *n, int cpu, int ch, uint32_t sad, uint32_t dad, uint32_t cnt)
{
	const uint32_t base = 0x040000B0u + 12u * (uint32_t)ch;

	nds_bus_write(n, cpu, base, 4, sad);
	nds_bus_write(n, cpu, base + 4, 4, dad);
	nds_bus_write(n, cpu, base + 8, 4, cnt);
}

static int same_state(const struct nds *a, const struct nds *b, const char *what)
{
	const struct {
		const char *name;
		int same;
	} parts[] = {
		{ "RAM", memcmp(a->ram, b->ram, NDS_RAM_SIZE) == 0 },
		{ "VRAM", memcmp(a->vram, b->vram, NDS_VRAM_SIZE) == 0 },
		{ "WRAM", memcmp(a->swram, b->swram, NDS_SWRAM_SIZE) == 0 },
		{ "paleta", memcmp(a->pal, b->pal, NDS_PAL_SIZE) == 0 },
		{ "OAM", memcmp(a->oam, b->oam, NDS_OAM_SIZE) == 0 },
		{ "DMA ARM9", memcmp(&a->io[0].dma, &b->io[0].dma, sizeof a->io[0].dma) == 0 },
		{ "DMA ARM7", memcmp(&a->io[1].dma, &b->io[1].dma, sizeof a->io[1].dma) == 0 },
		{ "ciclos", a->a9.cycles == b->a9.cycles && a->a7.cycles == b->a7.cycles },
		{ "VRAM sucia", memcmp(a->vid.dirty, b->vid.dirty, sizeof a->vid.dirty) == 0 && a->vid.identity_dirty == b->vid.identity_dirty },
		{ "geometría", gx_fifo_level(&a->gx) == gx_fifo_level(&b->gx) && a->gx.pk_word == b->gx.pk_word &&
				   a->gx.pk_idx == b->gx.pk_idx && a->gx.pk_left == b->gx.pk_left && a->gx.pk_cmd == b->gx.pk_cmd &&
				   memcmp(gx_stats_get((gx_t *)&a->gx), gx_stats_get((gx_t *)&b->gx), sizeof(gx_stats)) == 0 &&
				   a->ev[EV_GX] == b->ev[EV_GX] },
		{ "IF", a->io[0].ifl == b->io[0].ifl },
	};
	int ok = 1;

	for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
		if (!parts[i].same) {
			printf("FALLO test_dma: %s distinta tras %s\n", parts[i].name, what);
			ok = 0;
		}
	CHECK(ok);
	return ok;
}

/* El registro de cambios del vídeo (paleta y OAM) tiene que ser el mismo: la cola del juego de grabación. */
static int same_video_log(const struct nds *a, const struct nds *b, const char *what)
{
	const struct nds_vset *sa = &a->vid.set[a->vid.cur], *sb = &b->vid.set[b->vid.cur];
	int ok = sa->dropped == sb->dropped;

	for (int e = 0; e < 2 && ok; e++) {
		const uint32_t na = sa->frame.eng[e].nchg, nb = sb->frame.eng[e].nchg;

		ok = na == nb && memcmp(sa->chg[e], sb->chg[e], na * sizeof sa->chg[e][0]) == 0;
	}

	if (!ok)
		printf("FALLO test_dma: registro de vídeo distinto tras %s\n", what);
	CHECK(ok);
	return ok;
}

static void test_dma_copies(void)
{
	struct nds *o = machine(1), *n = machine(0);
/* DMAxCNT: habilitar (bit 31), unidades de 32 bits (bit 26), fuente fija o decreciente (bits 23-24), destino decreciente (21-22), IRQ (30) */
#define ctl32 0x84000000u
#define ctl16 0x80000000u
#define fixsrc (2u << 23)
#define decsrc (1u << 23)
#define decdst (1u << 21)
#define fixdst (2u << 21)
#define irq (1u << 30)

	if (!o || !n)
		return;
	seed_mem(o, n);
	for (int k = 0; k < 2; k++) {
		struct nds *m = k ? n : o;

		/* bancos: A y B fondos del motor A (0x06000000 y 0x06020000), C y D al ARM7 (0x06000000 y 0x06020000), E, F y G en
		 * LCDC (0x06880000-0x06897FFF), H fondos del motor B, I OBJ del motor B */
		nds_bus_write(m, NDS_CPU9, 0x04000240u, 4, 0x8A828981u);
		nds_bus_write(m, NDS_CPU9, 0x04000244u, 4, 0x03808080u);
		nds_bus_write(m, NDS_CPU9, 0x04000248u, 2, 0x8281u);
		nds_bus_write(m, NDS_CPU9, 0x04000304u, 2, 0x820Fu); /* POWCNT1: motor de geometría encendido */
		nds_bus_write(m, NDS_CPU9, 0x040000E0u, 4, 0xA5A55A5Au); /* DMA0FILL */
		nds_bus_write(m, NDS_CPU9, 0x040000ECu, 4, 0x11223344u); /* DMA3FILL */
		m->vid.recording = 1;
	}
	static const struct {
		int cpu, ch;
		uint32_t sad, dad, cnt;
		const char *what;
	} steps[] = {
		{ NDS_CPU9, 3, 0x02000100u, 0x02100000u, ctl32 | 300u, "RAM → RAM, palabras" },
		{ NDS_CPU9, 3, 0x02000102u, 0x02200001u, ctl16 | 333u, "RAM → RAM, medias palabras desalineadas" },
		{ NDS_CPU9, 3, 0x023FFF00u, 0x02000000u, ctl32 | 200u, "fuente da la vuelta en el espejo de 4 MB" },
		{ NDS_CPU9, 3, 0x02010000u, 0x027FFE00u, ctl32 | 400u, "destino da la vuelta en el espejo" },
		{ NDS_CPU9, 3, 0x02020000u, 0x02020010u, ctl32 | 64u, "solapados (destino encima de la fuente)" },
		{ NDS_CPU9, 3, 0x02030010u, 0x02030000u, ctl32 | 64u, "solapados (fuente encima del destino)" },
		{ NDS_CPU9, 3, 0x02040000u, 0x02050000u, ctl32 | decsrc | 64u, "fuente decreciente: por unidad" },
		{ NDS_CPU9, 3, 0x02040000u, 0x02051000u, ctl32 | decdst | 64u, "destino decreciente: por unidad" },
		{ NDS_CPU9, 3, 0x040000ECu, 0x02060000u, ctl32 | fixsrc | 256u, "DMA3FILL → RAM (relleno)" },
		{ NDS_CPU9, 0, 0x040000E0u, 0x02061000u, ctl16 | fixsrc | 100u, "DMA0FILL → RAM, medias palabras" },
		{ NDS_CPU9, 3, 0x02070000u, 0x02071000u, ctl32 | fixsrc | 50u, "fuente fija en RAM" },
		{ NDS_CPU9, 3, 0x02070000u, 0x0206FFFCu, ctl32 | fixsrc | 50u, "fuente fija pisada por el destino: por unidad" },
		{ NDS_CPU9, 3, 0x02080000u, 0x06880000u, ctl32 | 5000u, "RAM → LCDC (banco E), cruza páginas" },
		{ NDS_CPU9, 3, 0x02090000u, 0x06893F00u, ctl32 | 200u, "LCDC: de F a G (seguidos en la VRAM)" },
		{ NDS_CPU9, 3, 0x02090000u, 0x06897F00u, ctl32 | 200u, "LCDC: sale de G hacia H (H no es LCDC: se pierde)" },
		{ NDS_CPU9, 3, 0x020A0000u, 0x06000000u, ctl32 | 5000u, "RAM → fondos A (banco A), varias páginas" },
		{ NDS_CPU9, 3, 0x020B0000u, 0x0601FF00u, ctl32 | 200u, "fondos A: de A a B (cambio de banco)" },
		{ NDS_CPU9, 3, 0x06000000u, 0x020C0000u, ctl32 | 1000u, "VRAM → RAM" },
		{ NDS_CPU9, 3, 0x06880000u, 0x06000100u, ctl32 | 1000u, "LCDC → fondos A" },
		{ NDS_CPU9, 3, 0x020D0000u, 0x06200000u, ctl32 | 100u, "fondos B (banco H)" },
		{ NDS_CPU9, 3, 0x020D0000u, 0x06400000u, ctl32 | 100u, "OBJ A: sin banco → por unidad (se pierde)" },
		{ NDS_CPU9, 3, 0x020E0000u, 0x07000000u, ctl32 | 256u, "RAM → OAM entera" },
		{ NDS_CPU9, 3, 0x020E0400u, 0x07000700u, ctl16 | 300u, "OAM, medias palabras, da la vuelta" },
		{ NDS_CPU9, 3, 0x020F0000u, 0x05000000u, ctl16 | 1024u, "RAM → paleta entera" },
		{ NDS_CPU9, 3, 0x020F0800u, 0x05000600u, ctl32 | 200u, "paleta, palabras, da la vuelta" },
		{ NDS_CPU9, 3, 0x040000ECu, 0x07000000u, ctl32 | fixsrc | 64u, "relleno de la OAM" },
		{ NDS_CPU9, 3, 0x02100000u, 0x08000000u, ctl32 | 10u, "ranura de GBA: por unidad (se pierde)" },
		{ NDS_CPU9, 3, 0x02100000u, 0x04000400u, ctl32 | fixdst | 100u, "GXFIFO inmediato (destino fijo)" },
		{ NDS_CPU9, 3, 0x02100000u, 0x04000400u, ctl32 | fixdst | irq | 20u, "GXFIFO inmediato con IRQ al terminar" },
		{ NDS_CPU9, 3, 0x023FFF80u, 0x04000400u, ctl32 | fixdst | 64u, "GXFIFO con la fuente dando la vuelta" },
		{ NDS_CPU9, 3, 0x02100000u, 0x04000440u, ctl32 | fixdst | 8u, "puerto de órdenes: por unidad" },
		{ NDS_CPU7, 3, 0x02110000u, 0x03800000u, ctl32 | 512u, "ARM7: RAM → WRAM propia" },
		{ NDS_CPU7, 1, 0x03800000u, 0x06000000u, ctl32 | 512u, "ARM7: WRAM → su VRAM (banco C)" },
		{ NDS_CPU7, 2, 0x06000000u, 0x0603FF00u, ctl32 | 128u, "ARM7: VRAM C → D cruzando" },
		{ NDS_CPU7, 3, 0x02110000u, 0x05000000u, ctl32 | 64u, "ARM7 a la paleta: se ignora" },
	};

	for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		dma(o, steps[i].cpu, steps[i].ch, steps[i].sad, steps[i].dad, steps[i].cnt);
		dma(n, steps[i].cpu, steps[i].ch, steps[i].sad, steps[i].dad, steps[i].cnt);
		if (!same_state(o, n, steps[i].what) || !same_video_log(o, n, steps[i].what))
			break;
	}
	CHECK(n->st.dma_block_units > 0);
	CHECK_EQ(n->st.dma_gxi_words, 184u);
	CHECK(o->st.dma_block_units == 0);
	CHECK(o->st.dma_gxi_words == 0);
	free(o);
	free(n);
}

static void test_math(void)
{
	struct nds *o = machine(1), *n = machine(0);
	static const uint64_t specials[] = { 0, 1, (uint64_t)-1, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x8000000000000000ull,
					     0x7FFFFFFFFFFFFFFFull, 0x100000000ull, 12345, (uint64_t)-98765 };

	if (!o || !n)
		return;
	for (unsigned it = 0; it < 4000; it++) {
		const unsigned mode = rnd() & 3u;
		const uint64_t num = (it & 1u) ? specials[rnd() % (sizeof specials / sizeof specials[0])] : (uint64_t)rnd() << 32 | rnd();
		const uint64_t den = (it & 2u) ? specials[rnd() % (sizeof specials / sizeof specials[0])] : (uint64_t)rnd() << 32 | rnd();
		const uint64_t sq = (it & 4u) ? specials[rnd() % (sizeof specials / sizeof specials[0])] : (uint64_t)rnd() << 32 | rnd();
		const unsigned order = rnd() & 7u;

		for (int k = 0; k < 2; k++) {
			struct nds *m = k ? n : o;

			m->a9.cycles += 7;
			nds_bus_write(m, NDS_CPU9, 0x04000280u, 2, mode);
			if (order & 1u) {
				nds_bus_write(m, NDS_CPU9, 0x04000290u, 4, (uint32_t)num);
				nds_bus_write(m, NDS_CPU9, 0x04000294u, 4, (uint32_t)(num >> 32));
			} else {
				nds_bus_write(m, NDS_CPU9, 0x04000294u, 4, (uint32_t)(num >> 32));
				nds_bus_write(m, NDS_CPU9, 0x04000290u, 4, (uint32_t)num);
			}
			nds_bus_write(m, NDS_CPU9, 0x04000298u, 4, (uint32_t)den);
			nds_bus_write(m, NDS_CPU9, 0x0400029Cu, 4, (uint32_t)(den >> 32));
			nds_bus_write(m, NDS_CPU9, 0x040002B0u, 2, (order >> 1) & 1u);
			nds_bus_write(m, NDS_CPU9, 0x040002B8u, 4, (uint32_t)sq);
			nds_bus_write(m, NDS_CPU9, 0x040002BCu, 4, (uint32_t)(sq >> 32));
			if (order & 4u)
				m->a9.cycles += 100; /* la bandera de ocupado ya bajó */
		}
		/* todas las lecturas posibles: por bytes, medias palabras y palabras */
		for (uint32_t off = 0x280u; off < 0x2C0u; off += 4) {
			CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x04000000u + off, 4), nds_bus_read(o, NDS_CPU9, 0x04000000u + off, 4));
			CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x04000000u + off, 2), nds_bus_read(o, NDS_CPU9, 0x04000000u + off, 2));
			CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x04000002u + off, 2), nds_bus_read(o, NDS_CPU9, 0x04000002u + off, 2));
			for (uint32_t b = 0; b < 4; b++)
				CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x04000000u + off + b, 1), nds_bus_read(o, NDS_CPU9, 0x04000000u + off + b, 1));
		}
		/* una lectura que cruza grupos (solo por depuración) también coincide */
		CHECK_EQ(nds_math_read(n, 0x286u, 4), nds_math_read(o, 0x286u, 4));
	}
	/* la división pendiente: escribir sin leer no la calcula; leer el cociente la resuelve una vez */
	nds_bus_write(n, NDS_CPU9, 0x04000290u, 4, 100);
	nds_bus_write(n, NDS_CPU9, 0x04000298u, 4, 7);
	CHECK_EQ(n->math.div_pending, 1);
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040002A0u, 4), 14);
	CHECK_EQ(n->math.div_pending, 0);
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040002A8u, 4), 2);
	CHECK_EQ(o->math.div_pending, 0);
	free(o);
	free(n);
}

static void test_romctrl(void)
{
	struct nds *o = machine(1), *n = machine(0);

	if (!o || !n)
		return;
	for (int k = 0; k < 2; k++) {
		struct nds *m = k ? n : o;

		m->cart.romctl = 0x12345678u | (1u << 31) | (1u << 23);
		m->cart.busy = 0;
		m->cart.drq = 1;
		m->cart.auxcnt = 0xE043u;
	}
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A4u, 4), nds_bus_read(o, NDS_CPU9, 0x040001A4u, 4));
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A4u, 4), 0x12b45678u);
	for (int k = 0; k < 2; k++) {
		struct nds *m = k ? n : o;

		m->cart.busy = 1;
		m->cart.drq = 0;
	}
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A4u, 4), nds_bus_read(o, NDS_CPU9, 0x040001A4u, 4));
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A4u, 4), 0x92345678u);
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A0u, 4), nds_bus_read(o, NDS_CPU9, 0x040001A0u, 4));
	CHECK_EQ(nds_bus_read(n, NDS_CPU9, 0x040001A6u, 2), nds_bus_read(o, NDS_CPU9, 0x040001A6u, 2));
	free(o);
	free(n);
}

void test_dma(void)
{
	test_dma_copies();
	test_math();
	test_romctrl();
}
