/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — prueba dirigida de las rutas cortas de la FIFO de geometría (rama gx-rapido).
 *
 * Dos máquinas idénticas, sin ROM y con las dos CPUs paradas: la A con las rutas cortas (ráfagas de la DMA de modo 7 y escrituras de
 * la CPU por el bus corto, n->gx_lento = 0) y la B con la forma anterior (n->gx_lento = 1: una palabra a la vez por el bus). Reciben
 * los mismos estímulos al azar —DMA de todos los modos con fuentes y destinos de todo tipo (RAM, espejos y fronteras de región,
 * WRAM, VRAM, E/S, desalineadas, en sentido contrario, fijas), palabras de la CPU a GXFIFO y a los puertos de órdenes, el motor
 * encendido y apagado, GXSTAT con IRQ por nivel, tramos de tiempo y cuadros— y después de cada uno se comparan los ciclos del
 * ARM9, la cola del motor de geometría, sus contadores, los canales de DMA y los eventos. Tienen que ser idénticos al bit.
 *
 *   salsads-gxparidad [--semillas N] [--desde S] [--v]
 *   salsads-gxparidad --bench [--lento]     solo las palabras de DMA y de la CPU a la FIFO (órdenes baratas), tiempo del host
 *
 * Código propio (GPL-2.0-or-later).
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nds.h"
#include "core/nds_priv.h"
#include "util/endian.h"

static struct nds A, B;

static uint64_t rng_s;
static uint32_t rnd(void)
{
	rng_s = rng_s * 6364136223846793005ull + 1442695040888963407ull;
	return (uint32_t)(rng_s >> 33);
}

static uint64_t fnv(uint64_t h, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		h ^= (v >> (8 * i)) & 0xFFu;
		h *= 0x100000001B3ull;
	}
	return h;
}

/* Todo lo observable del motor de geometría, los canales de DMA del ARM9 y el tiempo. */
static uint64_t digest(struct nds *n)
{
	uint64_t h = 0xCBF29CE484222325ull;
	const gx_t *g = &n->gx;

	h = fnv(h, n->a9.cycles);
	h = fnv(h, n->a7.cycles);
	h = fnv(h, n->time);
	h = fnv(h, g->qcount);
	h = fnv(h, g->qhead);
	for (uint32_t i = 0; i < g->qcount; i++) {
		const gx_qent *e = &g->q[(g->qhead + i) & (GX_QRING - 1u)];

		h = fnv(h, e->cmd);
		h = fnv(h, e->param);
		h = fnv(h, e->t);
	}
	h = fnv(h, g->pk_word);
	h = fnv(h, g->pk_idx);
	h = fnv(h, g->pk_left);
	h = fnv(h, g->pk_cmd);
	h = fnv(h, g->cycle_free);
	h = fnv(h, g->stack_busy_until);
	h = fnv(h, g->test_busy_until);
	h = fnv(h, g->test_pending);
	h = fnv(h, g->now);
	h = fnv(h, g->nxt);
	h = fnv(h, g->err);
	h = fnv(h, g->irqmode);
	h = fnv(h, g->fe_polyattr);
	h = fnv(h, (uint64_t)g->swap_pending);
	h = fnv(h, g->st.cmds);
	h = fnv(h, g->st.cycles);
	h = fnv(h, g->st.fifo_overflow);
	h = fnv(h, g->st.unknown_cmds);
	h = fnv(h, g->st.swaps);
	h = fnv(h, g->ex.out.npolys);
	h = fnv(h, g->ex.out.nverts);
	h = fnv(h, n->gx_frames);
	h = fnv(h, n->gx_anchor);
	h = fnv(h, n->gx_irq_t);
	for (int c = 0; c < 4; c++) {
		const struct nds_dma *d = &n->io[NDS_CPU9].dma[c];

		h = fnv(h, d->sad);
		h = fnv(h, d->dad);
		h = fnv(h, d->cnt);
		h = fnv(h, d->src);
		h = fnv(h, d->dst);
		h = fnv(h, d->left);
		h = fnv(h, d->latched);
	}
	for (int e = 0; e < EV_COUNT; e++)
		h = fnv(h, n->ev[e]);
	h = fnv(h, n->io[NDS_CPU9].ifl);
	h = fnv(h, n->io[NDS_CPU7].ifl);
	return h;
}

/* La caché de la DMA del ARM9 tiene que coincidir con el recorrido de los canales. */
static int cache_ok(struct nds *n)
{
	return n->dma9_gxw == (nds_dma_waits(n, NDS_CPU9, NDS_DMA_GXFIFO) != 0);
}

static void machine(struct nds *n, int lento)
{
	n->gx_lento = (uint8_t)lento;
	n->quantum = 128;
	nds_init(n);
	n->powcnt1 |= 0x0008u; /* motor de geometría encendido */
	nds_cpu_halt(n, NDS_CPU9);
	nds_cpu_halt(n, NDS_CPU7);
}

/* ---- estímulos ------------------------------------------------------------------------------------------------------ */

#define MAXOPS 32
enum { OP_RAMFILL, OP_DMA, OP_CPUW, OP_RUN, OP_FRAME, OP_READ, OP_GXSTAT, OP_POWER, OP_DMAOFF };
struct op {
	int kind;
	uint32_t a, b, c, d;
};

/* Una palabra de órdenes empaquetada al azar con sus parámetros: escribe en `dst` y devuelve cuántas palabras. */
static unsigned cmd_stream(uint32_t *dst, unsigned maxw)
{
	static const uint8_t cmds[] = { 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x19, 0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x22,
					0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41,
					0x60, 0x70, 0x71, 0x72 };
	unsigned n = 0;

	while (n + 1u + 8u * 4u < maxw) {
		uint8_t c[4];
		uint32_t w = 0;
		unsigned np = 0;

		for (int i = 0; i < 4; i++) {
			c[i] = (rnd() % 11u == 0u) ? 0 : cmds[rnd() % sizeof cmds];
			w |= (uint32_t)c[i] << (8 * i);
			np += gx_cmd_np(c[i]) == 0xFFu ? 0u : gx_cmd_np(c[i]);
		}
		dst[n++] = w;
		/* los parámetros de cada orden, de seguido (las órdenes sin parámetros no llevan) */
		for (unsigned k = 0; k < np && n < maxw; k++)
			dst[n++] = rnd();
		if (rnd() % 9u == 0u) /* un SWAP_BUFFERS de vez en cuando (el motor espera al VBlank) */
			dst[n - 1] = (dst[n - 1] & 0x00FFFFFFu) | 0x50000000u;
	}
	return n;
}

static void gen_scenario(struct op *ops, unsigned *nops)
{
	unsigned n = 0;

	ops[n++] = (struct op){ OP_RAMFILL, rnd(), 0, 0, 0 };
	ops[n++] = (struct op){ OP_POWER, 1, 0, 0, 0 };
	/* una DMA de modo 7 normal (fuente en la RAM, destino fijo en GXFIFO) y tiempo para que el motor y la DMA avancen */
	ops[n++] = (struct op){ OP_DMA, rnd() & 3u, 0, 0, (rnd() & 0xFFFFFu) | 0x80000000u };
	ops[n++] = (struct op){ OP_RUN, 1000 + rnd() % 30000u, 0, 0, 0 };
	while (n < MAXOPS - 1) {
		struct op o = { 0, rnd(), rnd(), rnd(), rnd() };
		const unsigned k = rnd() % 16u;

		if (k < 5)
			o.kind = OP_DMA;
		else if (k < 8)
			o.kind = OP_CPUW;
		else if (k < 11)
			o.kind = OP_RUN;
		else if (k < 12)
			o.kind = rnd() % 3u ? OP_RUN : OP_FRAME;
		else if (k < 13)
			o.kind = OP_READ;
		else if (k < 14)
			o.kind = OP_GXSTAT;
		else if (k < 15)
			o.kind = OP_POWER;
		else
			o.kind = OP_DMAOFF;
		ops[n++] = o;
	}
	ops[n++] = (struct op){ OP_RUN, 40000, 0, 0, 0 };
	*nops = n;
}

static void wr(struct nds *n, uint32_t a, uint32_t v)
{
	nds_bus_write(n, NDS_CPU9, a, 4, v);
}

static void apply(struct nds *n, const struct op *o)
{
	switch (o->kind) {
	case OP_RAMFILL: {
		/* unos 24 KB de órdenes en la RAM (y los espejos) y en la WRAM compartida, y basura en la VRAM */
		static uint32_t buf[6144];
		uint64_t save = rng_s;
		unsigned w;

		rng_s = o->a | (uint64_t)o->a << 32;
		w = cmd_stream(buf, 6144);
		for (unsigned i = 0; i < w; i++) {
			wr(n, 0x02000000u + 4u * i, buf[i]);
			wr(n, 0x03000000u + 4u * (i & 0x1FFFu), buf[i]);
		}
		for (unsigned i = 0; i < 64; i++) { /* el final de los 4 MB, para cruzar el espejo */
			wr(n, 0x023FFF00u + 4u * i, buf[i % w]);
			wr(n, 0x027FFF00u + 4u * i, buf[(i * 7u) % w]);
		}
		for (unsigned i = 0; i < 256; i++)
			wr(n, 0x06800000u + 4u * i, buf[(i * 3u) % w]);
		rng_s = save;
		break;
	}
	case OP_DMA: {
		static const uint32_t srcs[] = { 0x02000000u, 0x02000100u, 0x02000401u, 0x02000402u, 0x023FFFF0u, 0x023FFFC0u, 0x027FFFE8u,
						 0x02400000u - 0x20u, 0x03000000u, 0x03000F00u, 0x06800000u, 0x04000600u, 0x02001000u,
						 0x02002000u, 0x0237FFF0u };
		static const uint32_t dsts[] = { 0x04000400u, 0x04000400u, 0x04000400u, 0x04000400u, 0x04000400u, 0x04000404u,
						 0x0400043Cu, 0x04000440u, 0x04000488u, 0x040004CCu };
		const unsigned ch = o->a & 3u;
		const uint32_t base = 0x040000B0u + 12u * ch;
		const uint32_t src = srcs[o->b % (sizeof srcs / sizeof srcs[0])] + ((o->b >> 8) & 1u ? 4u * ((o->b >> 9) & 31u) : 0u);
		const uint32_t dst = dsts[o->c % (sizeof dsts / sizeof dsts[0])];
		uint32_t cnt;
		unsigned mode = 7;
		unsigned count = 1 + (o->d % 700u);

		if (o->d & 0x80000000u) { /* la DMA normal: modo 7, 32 bits, destino fijo, fuente creciente */
			count = 100 + (o->d % 600u);
			cnt = 0x80000000u | count | (7u << 27) | 0x04000000u | (2u << 21) | ((o->d >> 12) & 1u ? 0x40000000u : 0u);
		} else {
			if (((o->d >> 20) & 15u) == 5u)
				mode = (o->d >> 23) & 7u; /* otros modos de arranque (0 = inmediato) */
			if (((o->d >> 26) & 7u) == 3u)
				count = 1 + (o->d % 8u);
			cnt = 0x80000000u | (count & 0x1FFFFFu) | (mode << 27) | 0x04000000u; /* 32 bits */
			if (((o->d >> 13) & 15u) == 7u)
				cnt &= ~0x04000000u; /* 16 bits */
			cnt |= (((o->c >> 8) & 7u) == 3u ? ((o->c >> 12) & 3u) : 2u) << 21; /* destino: fijo casi siempre */
			cnt |= (((o->b >> 16) & 7u) == 3u ? ((o->b >> 20) & 3u) : 0u) << 23; /* fuente: creciente casi siempre */
			if (((o->d >> 5) & 7u) == 1u)
				cnt |= 0x02000000u; /* repetir */
			if (((o->d >> 7) & 3u) == 0u)
				cnt |= 0x40000000u; /* IRQ al terminar */
		}
		wr(n, base + 8u, 0); /* apaga el canal antes de armarlo */
		wr(n, base + 0u, src);
		wr(n, base + 4u, dst);
		wr(n, base + 8u, cnt);
		break;
	}
	case OP_DMAOFF:
		wr(n, 0x040000B0u + 12u * (o->a & 3u) + 8u, 0);
		break;
	case OP_CPUW: {
		const unsigned k = 1 + (o->a % 40u);
		static uint32_t buf[2048];
		uint64_t save = rng_s;

		rng_s = o->b | (uint64_t)o->c << 32;
		cmd_stream(buf, 2048);
		for (unsigned i = 0; i < k; i++) {
			const unsigned sel = rnd() % 8u;
			uint32_t addr;

			if (sel < 5)
				addr = 0x04000400u + 4u * (rnd() % 16u); /* GXFIFO (cualquiera de sus 16 palabras) */
			else if (sel < 7)
				addr = 0x04000440u + 4u * (rnd() % 0x5Du); /* puertos de órdenes */
			else
				addr = 0x04000400u + (rnd() % 0x200u); /* sin alinear, o fuera de los puertos */
			/* por el bus del ARM9, como lo ven el intérprete y el camino lento del recompilador */
			if (!(addr & 3u))
				nds_bus9.write32(&n->a9, addr, buf[i]);
			else
				nds_bus9.write16(&n->a9, addr & ~1u, buf[i] & 0xFFFFu);
		}
		rng_s = save;
		break;
	}
	case OP_RUN:
		nds_run(n, 1u + (o->a % 120000u));
		break;
	case OP_FRAME:
		nds_run_frame(n);
		break;
	case OP_READ:
		(void)nds_bus9.read32(&n->a9, 0x04000600u);
		(void)nds_bus9.read32(&n->a9, 0x04000604u);
		break;
	case OP_GXSTAT:
		wr(n, 0x04000600u, ((o->a % 3u) << 30) | ((o->b & 1u) << 15));
		break;
	case OP_POWER:
		if (o->a == 1)
			n->powcnt1 |= 0x0008u;
		else
			n->powcnt1 = (uint16_t)((n->powcnt1 & ~0x0008u) | ((o->b & 3u) == 0u ? 0u : 0x0008u));
		break;
	}
}

#include <time.h>

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Mide el costo de llevar palabras a la FIFO (no el de ejecutar las órdenes: COLOR y NOP, casi gratis): 400 DMA de 700 palabras
 * cada una y 400 000 palabras de la CPU, con el motor corriendo entre ellas. Solo da una idea relativa en el host. */
static int bench(int lento)
{
	double t0, t1, t2;
	uint64_t words = 0;

	machine(&A, lento);
	for (unsigned i = 0; i < 4096; i++)
		wr(&A, 0x02000000u + 4u * i, 0x20002000u); /* COLOR, NOP, COLOR, NOP: un parámetro cada COLOR (el siguiente dato) */
	t0 = now_s();
	for (unsigned r = 0; r < 4000; r++) {
		const uint32_t base = 0x040000B0u;

		wr(&A, base + 8u, 0);
		wr(&A, base + 0u, 0x02000000u);
		wr(&A, base + 4u, 0x04000400u);
		wr(&A, base + 8u, 0x80000000u | 700u | (7u << 27) | 0x04000000u | (2u << 21));
		for (int k = 0; k < 40; k++)
			nds_run(&A, 2000); /* el motor consume y la DMA sigue cuando baja de la mitad */
		words += 700;
	}
	t1 = now_s();
	for (unsigned r = 0; r < 400000; r++) {
		nds_bus9.write32(&A.a9, 0x04000400u, 0x20002000u);
		if ((r & 127u) == 127u)
			nds_run(&A, 20000);
	}
	t2 = now_s();
	printf("bench%s: DMA %.0f ns por palabra (%" PRIu64 " palabras en %.3f s; ráfagas %" PRIu64 "), CPU %.0f ns por palabra\n",
	       lento ? " (gx-lento)" : "", (t1 - t0) * 1e9 / (double)words, words, t1 - t0, A.st.gx_dma_bursts,
	       (t2 - t1) * 1e9 / 400000.0);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned seeds = 300, from = 0;
	int verbose = 0;
	unsigned bad = 0;
	uint64_t dwords = 0, cwords = 0, bursts = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--semillas") && i + 1 < argc)
			seeds = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--desde") && i + 1 < argc)
			from = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--v"))
			verbose = 1;
		else if (!strcmp(argv[i], "--bench"))
			return bench(i + 1 < argc && !strcmp(argv[i + 1], "--lento"));
	}
	for (unsigned s = from; s < from + seeds; s++) {
		struct op ops[MAXOPS];
		unsigned nops;

		rng_s = 0x9E3779B97F4A7C15ull * (s + 1u);
		gen_scenario(ops, &nops);
		machine(&A, 0);
		machine(&B, 1);
		for (unsigned i = 0; i < nops; i++) {
			uint64_t da, db;

			apply(&A, &ops[i]);
			apply(&B, &ops[i]);
			da = digest(&A);
			db = digest(&B);
			if (da != db || !cache_ok(&A) || !cache_ok(&B)) {
				printf("FALLO semilla %u, paso %u (op %d): %016" PRIx64 " contra %016" PRIx64 "%s\n", s, i, ops[i].kind, da, db,
				       cache_ok(&A) && cache_ok(&B) ? "" : " (caché de la DMA incoherente)");
				bad++;
				break;
			}
		}
		dwords += A.st.gx_dma_words;
		cwords += A.st.gx_cpu_words;
		bursts += A.st.gx_dma_bursts;
		if (B.st.gx_dma_words || B.st.gx_cpu_words) { /* la forma anterior no usa las rutas cortas */
			printf("FALLO semilla %u: gx_lento usó una ruta corta\n", s);
			bad++;
		}
		if (verbose)
			printf("semilla %u: %" PRIu64 " comandos, %u cuadros\n", s, A.gx.st.cmds, (unsigned)A.gx_frames);
	}
	printf("gxparidad: %u semillas, %" PRIu64 " palabras de DMA en %" PRIu64 " ráfagas y %" PRIu64 " de la CPU por las rutas cortas; ", seeds, dwords,
	       bursts, cwords);
	if (bad) {
		printf("%u FALLOS\n", bad);
		return 1;
	}
	printf("0 fallos\n");
	return 0;
}
