/*
 * SalsaDS — BIOS en HLE (Fase 1.2): bloques de BIOS propios y las llamadas SWI.
 *
 * No hay BIOS de Nintendo ni desensamblado suyo. El bloque de cada CPU lo genera el emulador:
 *
 *   ARM9 en 0xFFFF0000 (4 KB) y ARM7 en 0x00000000 (16 KB): tabla de vectores donde todo salta a sí mismo
 *   salvo la IRQ, y un manejador de IRQ de seis instrucciones que sigue el contrato que documenta GBATEK:
 *   guarda r0-r3, r12 y lr, llama al manejador del programa a través del puntero que el programa deja en
 *   [DTCM + 0x3FFC] (ARM9, la base de la DTCM sale de CP15) o en [0x0380FFFC] (ARM7, vía 0x04000000 - 4),
 *   restaura y vuelve con `subs pc, lr, #4`. Más una instrucción `b .` en el desplazamiento 0x100, destino de
 *   retorno de los callbacks que la HLE llama dentro de una SWI (descompresión por callback).
 *
 *   Las SWI no salen por el vector: el gancho del núcleo (swi_hook) las atiende en C. En el ARM9 solo si los
 *   vectores son los de la BIOS (los vectores altos): si el programa instaló los suyos, la SWI es una
 *   excepción de verdad. Una SWI que no está implementada se anota una vez y se ignora.
 *
 * Esperas (IntrWait, VBlankIntrWait, Halt): se ponen la CPU en HALT y se reejecuta la SWI al despertar, con
 * una bandera por CPU que recuerda que ya se descartaron los avisos viejos. Las banderas de aviso están donde
 * dice GBATEK ([DTCM+0x3FF8] en el ARM9, [0x0380FFF8] en el ARM7) y las pone el manejador del programa.
 *
 * Fuente: GBATEK (BIOS Functions, formatos de descompresión). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

/* Instrucciones generadas (ensambladas aparte y comprobadas con el objdump del ARM; ver el comentario de
 * cada una). */
static const uint32_t vec_loop = 0xEAFFFFFEu; /* b . */
static const uint32_t vec_irq = 0xEA000000u;  /* b +0x20 (la rutina de IRQ, justo después de la tabla) */
static const uint32_t irq9_code[9] = {
	0xE92D500Fu, /* push {r0-r3, r12, lr} */
	0xEE190F11u, /* mrc p15, 0, r0, c9, c1, 0     base y tamaño de la DTCM */
	0xE1A00620u, /* lsr r0, r0, #12 */
	0xE1A00600u, /* lsl r0, r0, #12               base de la DTCM */
	0xE2800901u, /* add r0, r0, #0x4000           fin de la DTCM */
	0xE28FE000u, /* add lr, pc, #0                lr = la instrucción de después de la siguiente */
	0xE510F004u, /* ldr pc, [r0, #-4]             salta a [DTCM + 0x3FFC] */
	0xE8BD500Fu, /* pop {r0-r3, r12, lr} */
	0xE25EF004u, /* subs pc, lr, #4 */
};
static const uint32_t irq7_code[7] = {
	0xE92D500Fu, /* push {r0-r3, r12, lr} */
	0xE3A00301u, /* mov r0, #0x04000000 */
	0xE28FE000u, /* add lr, pc, #0 */
	0xE510F004u, /* ldr pc, [r0, #-4]             salta a [0x03FFFFFC] = [0x0380FFFC] */
	0xE8BD500Fu, /* pop {r0-r3, r12, lr} */
	0xE25EF004u, /* subs pc, lr, #4 */
};

#define CB_RETURN7 0x100u
#define CB_RETURN9 0xFFFF0100u

void nds_bios_init(struct nds *n)
{
	memset(n->bios9, 0, sizeof n->bios9);
	memset(n->bios7, 0, sizeof n->bios7);
	for (int v = 0; v < 8; v++) {
		wr32le(n->bios9 + 4 * v, v == 6 ? vec_irq : vec_loop);
		wr32le(n->bios7 + 4 * v, v == 6 ? vec_irq : vec_loop);
	}
	for (unsigned i = 0; i < 9; i++)
		wr32le(n->bios9 + 0x20 + 4 * i, irq9_code[i]);
	for (unsigned i = 0; i < 6; i++)
		wr32le(n->bios7 + 0x20 + 4 * i, irq7_code[i]);
	wr32le(n->bios9 + 0x100, vec_loop);
	wr32le(n->bios7 + 0x100, vec_loop);
}

/* ---- ayudas ------------------------------------------------------------------------------------------ */

static void warn_once(struct nds *n, int cpu, uint32_t fn)
{
	if (fn < 32 && !n->swi_warned[cpu][fn]) {
		n->swi_warned[cpu][fn] = 1;
		nds_logf(n, "ARM%d: SWI 0x%02X sin implementar (se ignora)", cpu ? 7 : 9, fn);
	}
}

/* Llama a una función del programa (un callback) y devuelve r0; preserva los registros visibles. */
static uint32_t bios_call(struct nds *n, struct acore *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2)
{
	const uint32_t sentinel = c == &n->a7 ? CB_RETURN7 : CB_RETURN9;
	uint32_t saved[16], cpsr = c->cpsr, ret;

	memcpy(saved, c->r, sizeof saved);
	c->r[0] = a0;
	c->r[1] = a1;
	c->r[2] = a2;
	c->r[14] = sentinel;
	acore_bx(c, fn);
	for (unsigned i = 0; i < 4000000u && acore_pc(c) != sentinel; i++)
		acore_step(c);
	ret = c->r[0];
	memcpy(c->r, saved, sizeof saved);
	c->cpsr = cpsr;
	return ret;
}

/* Origen de datos de los descompresores: memoria directa o por callbacks (r3 = estructura de punteros). */
struct strm {
	struct nds *n;
	struct acore *c;
	uint32_t src;
	uint32_t cb;    /* dirección de la estructura de callbacks; 0 = lectura directa */
	uint32_t param; /* r2 de la llamada */
};

static uint32_t s_get8(struct strm *s)
{
	uint32_t v;

	if (!s->cb)
		v = acore_rd8(s->c, s->src);
	else
		v = bios_call(s->n, s->c, acore_rd32(s->c, s->cb + 8), s->src, 0, 0) & 0xFFu;
	s->src++;
	return v;
}

static uint32_t s_get32(struct strm *s)
{
	uint32_t v;

	if (!s->cb) {
		v = acore_rd32(s->c, s->src & ~3u);
	} else {
		v = bios_call(s->n, s->c, acore_rd32(s->c, s->cb + 16), s->src, 0, 0);
	}
	s->src += 4;
	return v;
}

/* Cabecera de 4 bytes (tipo y tamaño). Con callbacks, la devuelve el callback de apertura. */
static uint32_t s_open(struct strm *s, uint32_t dst)
{
	uint32_t h;

	if (!s->cb) {
		h = acore_rd32(s->c, s->src & ~3u);
	} else {
		h = bios_call(s->n, s->c, acore_rd32(s->c, s->cb), s->src, dst, s->param);
	}
	s->src += 4;
	return h;
}

static void s_close(struct strm *s)
{
	if (s->cb) {
		const uint32_t f = acore_rd32(s->c, s->cb + 4);

		if (f)
			bios_call(s->n, s->c, f, s->src, 0, 0);
	}
}

/* Salida byte a byte, en bytes o en medias palabras (para la VRAM). */
struct outp {
	struct acore *c;
	uint32_t dst;
	uint32_t n;      /* bytes escritos */
	int half;
	uint8_t pend;
};

static void o_put(struct outp *o, uint8_t b)
{
	if (!o->half) {
		acore_wr8(o->c, o->dst + o->n, b);
	} else if (o->n & 1u) {
		acore_wr16(o->c, (o->dst + o->n - 1u) & ~1u, (uint32_t)o->pend | ((uint32_t)b << 8));
	} else {
		o->pend = b;
	}
	o->n++;
}

static uint8_t o_back(struct outp *o, uint32_t k)
{
	if (o->half && (o->n & 1u) && k == o->n - 1u)
		return o->pend;
	return (uint8_t)acore_rd8(o->c, o->dst + k);
}

/* ---- descompresores ------------------------------------------------------------------------------------ */

static uint32_t unlz77(struct nds *n, struct acore *c, uint32_t src, uint32_t dst, uint32_t param, uint32_t cb, int half)
{
	struct strm s = { n, c, src, cb, param };
	struct outp o = { c, dst, 0, half, 0 };
	const uint32_t hdr = s_open(&s, dst);
	const uint32_t size = hdr >> 8;

	if ((hdr & 0xF0u) != 0x10u)
		return 0xFFFFFFFFu;
	while (o.n < size) {
		uint32_t flags = s_get8(&s);

		for (int i = 0; i < 8 && o.n < size; i++, flags <<= 1) {
			if (flags & 0x80u) {
				const uint32_t b0 = s_get8(&s), b1 = s_get8(&s);
				const uint32_t len = (b0 >> 4) + 3u, disp = (((b0 & 0xFu) << 8) | b1) + 1u;

				for (uint32_t k = 0; k < len && o.n < size; k++) {
					if (disp > o.n)
						return 0xFFFFFFFFu;
					o_put(&o, o_back(&o, o.n - disp));
				}
			} else {
				o_put(&o, (uint8_t)s_get8(&s));
			}
		}
	}
	s_close(&s);
	return size;
}

static uint32_t unrle(struct nds *n, struct acore *c, uint32_t src, uint32_t dst, uint32_t param, uint32_t cb, int half)
{
	struct strm s = { n, c, src, cb, param };
	struct outp o = { c, dst, 0, half, 0 };
	const uint32_t hdr = s_open(&s, dst);
	const uint32_t size = hdr >> 8;

	if ((hdr & 0xF0u) != 0x30u)
		return 0xFFFFFFFFu;
	while (o.n < size) {
		const uint32_t f = s_get8(&s);

		if (f & 0x80u) {
			const uint32_t cnt = (f & 0x7Fu) + 3u, b = s_get8(&s);

			for (uint32_t k = 0; k < cnt && o.n < size; k++)
				o_put(&o, (uint8_t)b);
		} else {
			const uint32_t cnt = (f & 0x7Fu) + 1u;

			for (uint32_t k = 0; k < cnt && o.n < size; k++)
				o_put(&o, (uint8_t)s_get8(&s));
		}
	}
	s_close(&s);
	return size;
}

static uint32_t unhuff(struct nds *n, struct acore *c, uint32_t src, uint32_t dst, uint32_t param, uint32_t cb)
{
	struct strm s = { n, c, src, cb, param };
	uint8_t tree[0x200];
	const uint32_t hdr = s_open(&s, dst);
	const uint32_t size = hdr >> 8, bits = hdr & 0xFu;
	uint32_t tsize, word = 0, out = 0, outbits = 0, written = 0;
	int nbits = 0;

	if ((hdr & 0xF0u) != 0x20u || (bits != 4 && bits != 8))
		return 0xFFFFFFFFu;
	tsize = (s_get8(&s) + 1u) * 2u; /* bytes de la tabla, contando el byte de tamaño (índice 0) */
	tree[0] = (uint8_t)(tsize / 2 - 1);
	for (uint32_t i = 1; i < tsize && i < sizeof tree; i++)
		tree[i] = (uint8_t)s_get8(&s);
	while (written < size) {
		uint32_t node = 1; /* índice del nodo raíz en `tree` */

		for (;;) {
			uint32_t bit, next, e;

			if (nbits == 0) {
				word = s_get32(&s);
				nbits = 32;
			}
			bit = (word >> 31) & 1u;
			word <<= 1;
			nbits--;
			e = tree[node & (sizeof tree - 1)];
			next = (node & ~1u) + (e & 0x3Fu) * 2u + 2u + bit;
			if (e & (0x80u >> bit)) { /* el hijo elegido es un dato */
				out |= (uint32_t)(tree[next & (sizeof tree - 1)] & ((1u << bits) - 1u)) << outbits;
				outbits += bits;
				if (outbits == 32) {
					acore_wr32(c, dst + written, out);
					written += 4;
					out = 0;
					outbits = 0;
				}
				break;
			}
			node = next;
		}
	}
	s_close(&s);
	return size;
}

static void bitunpack(struct nds *n, struct acore *c, uint32_t src, uint32_t dst, uint32_t info)
{
	const uint32_t len = acore_rd16(c, info & ~1u), sw = acore_rd8(c, info + 2), dw = acore_rd8(c, info + 3);
	const uint32_t off = acore_rd32(c, (info + 4) & ~3u);
	const uint32_t add = off & 0x7FFFFFFFu, zero = off >> 31;
	uint32_t out = 0, outbits = 0;

	(void)n;
	if (!sw || sw > 8 || !dw || dw > 32)
		return;
	for (uint32_t i = 0; i < len; i++) {
		const uint32_t b = acore_rd8(c, src + i);

		for (uint32_t sh = 0; sh < 8; sh += sw) {
			uint32_t v = (b >> sh) & ((1u << sw) - 1u);

			if (v || zero)
				v += add;
			out |= (dw == 32 ? v : (v & ((1u << dw) - 1u))) << outbits;
			outbits += dw;
			if (outbits >= 32) {
				acore_wr32(c, dst, out);
				dst += 4;
				out = 0;
				outbits = 0;
			}
		}
	}
}

static void unfilter(struct acore *c, uint32_t src, uint32_t dst, int width16, int half_writes)
{
	const uint32_t hdr = acore_rd32(c, src & ~3u), size = hdr >> 8;
	uint32_t acc = 0, pend = 0;

	src += 4;
	if (!width16) {
		for (uint32_t i = 0; i < size; i++) {
			acc = (acc + acore_rd8(c, src + i)) & 0xFFu;
			if (!half_writes) {
				acore_wr8(c, dst + i, acc);
			} else if (i & 1u) {
				acore_wr16(c, (dst + i - 1u) & ~1u, pend | (acc << 8));
			} else {
				pend = acc;
			}
		}
	} else {
		for (uint32_t i = 0; i + 1 < size; i += 2) {
			acc = (acc + acore_rd16(c, (src + i) & ~1u)) & 0xFFFFu;
			acore_wr16(c, (dst + i) & ~1u, acc);
		}
	}
}

/* ---- Div, Sqrt, CRC, CpuSet ------------------------------------------------------------------------------ */

static void swi_div(struct acore *c)
{
	const int32_t num = (int32_t)c->r[0], den = (int32_t)c->r[1];
	int32_t q, r;

	if (den == 0) {
		q = num < 0 ? 1 : -1;
		r = num;
	} else if (num == INT32_MIN && den == -1) {
		q = INT32_MIN;
		r = 0;
	} else {
		q = num / den;
		r = num % den;
	}
	c->r[0] = (uint32_t)q;
	c->r[1] = (uint32_t)r;
	c->r[3] = (uint32_t)(q < 0 ? -(int64_t)q : (int64_t)q);
	c->cycles += 40;
}

static void swi_sqrt(struct acore *c)
{
	uint32_t x = c->r[0], r = 0;

	for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
	}
	c->r[0] = r;
	c->cycles += 40;
}

static void swi_cpuset(struct acore *c, int cpu, int fast)
{
	uint32_t src = c->r[0], dst = c->r[1];
	const uint32_t len = c->r[2] & 0x1FFFFFu, fill = (c->r[2] >> 24) & 1u;
	const int word = fast || ((c->r[2] >> 26) & 1u);

	if (cpu == NDS_CPU7 && (src < NDS_BIOS7_SIZE || src + len * (word ? 4u : 2u) < NDS_BIOS7_SIZE))
		return; /* el ARM7 no copia desde su BIOS */
	if (word) {
		src &= ~3u;
		dst &= ~3u;
	} else {
		src &= ~1u;
		dst &= ~1u;
	}
	for (uint32_t i = 0; i < len; i++) {
		if (word) {
			acore_wr32(c, dst, acore_rd32(c, src));
			dst += 4;
			if (!fill)
				src += 4;
		} else {
			acore_wr16(c, dst, acore_rd16(c, src));
			dst += 2;
			if (!fill)
				src += 2;
		}
	}
	c->cycles += len;
}

/* ---- esperas -------------------------------------------------------------------------------------------------- */

static int bios_intrwait(struct nds *n, struct acore *c, int cpu, uint32_t discard, uint32_t mask)
{
	struct nds_cpuio *io = &n->io[cpu];
	const uint32_t addr = cpu ? 0x0380FFF8u : c->dtcm_base + 0x3FF8u;
	uint32_t flags;

	if (!n->intrwait_active[cpu]) {
		io->ime = 1;
		nds_irq_update(n, cpu);
		flags = acore_rd32(c, addr);
		if (discard) {
			acore_wr32(c, addr, flags & ~mask);
		} else if (cpu == NDS_CPU7 && (flags & mask)) { /* avisos viejos: vuelve ya */
			acore_wr32(c, addr, flags & ~mask);
			return 1;
		}
		/* en el ARM9 el caso r0 = 0 espera siempre al menos una IRQ (así es el original) */
		n->intrwait_active[cpu] = 1;
	} else {
		flags = acore_rd32(c, addr);
		if (flags & mask) {
			acore_wr32(c, addr, flags & ~mask);
			n->intrwait_active[cpu] = 0;
			return 1;
		}
	}
	c->r[15] -= (c->cpsr & ACORE_T) ? 2u : 4u; /* volver a ejecutar la SWI al despertar */
	nds_cpu_halt(n, cpu);
	return 1;
}

/* ---- gancho de SWI --------------------------------------------------------------------------------------------- */

int nds_swi_hook(struct acore *c, uint32_t comment)
{
	struct nds *n = c->user;
	const int cpu = c == &n->a7 ? NDS_CPU7 : NDS_CPU9;
	const uint32_t fn = (c->cpsr & ACORE_T) ? comment & 0xFFu : (comment >> 16) & 0xFFu;
	uint32_t r;

	if (cpu == NDS_CPU9 && c->vector_base != 0xFFFF0000u)
		return 0;
	switch (fn) {
	case 0x00:
		nds_softreset(n, cpu);
		return 1;
	case 0x03: /* WaitByLoop */
		c->cycles += (uint64_t)c->r[0] * 3u;
		return 1;
	case 0x04:
		return bios_intrwait(n, c, cpu, c->r[0], c->r[1]);
	case 0x05:
		return bios_intrwait(n, c, cpu, 1, 1);
	case 0x06:
		nds_cpu_halt(n, cpu);
		return 1;
	case 0x07:
		if (cpu == NDS_CPU7) {
			nds_cpu_halt(n, cpu);
			return 1;
		}
		break;
	case 0x08:
		if (cpu == NDS_CPU7) {
			n->io[NDS_CPU7].gen[0x504 >> 1] = c->r[0] ? 0x200 : 0;
			return 1;
		}
		break;
	case 0x09:
		swi_div(c);
		return 1;
	case 0x0B:
		swi_cpuset(c, cpu, 0);
		return 1;
	case 0x0C:
		swi_cpuset(c, cpu, 1);
		return 1;
	case 0x0D:
		swi_sqrt(c);
		return 1;
	case 0x0E: {
		uint8_t buf[64];
		uint32_t crc = c->r[0] & 0xFFFFu, left = c->r[2], a = c->r[1];

		while (left) {
			const uint32_t k = left < sizeof buf ? left : (uint32_t)sizeof buf;

			for (uint32_t i = 0; i < k; i++)
				buf[i] = (uint8_t)acore_rd8(c, a + i);
			crc = nds_crc16(crc, buf, k);
			a += k;
			left -= k;
		}
		if (c->r[2] >= 2)
			c->r[3] = acore_rd16(c, (c->r[1] + c->r[2] - 2u) & ~1u);
		c->r[0] = crc;
		c->cycles += c->r[2] * 2u;
		return 1;
	}
	case 0x0F:
		c->r[0] = 0;
		return 1;
	case 0x10:
		bitunpack(n, c, c->r[0], c->r[1], c->r[2]);
		return 1;
	case 0x11: /* LZ77 desde memoria, escritura de 8 bits */
		r = unlz77(n, c, c->r[0], c->r[1], c->r[2], 0, 0);
		c->r[0] = r;
		return 1;
	case 0x12: /* LZ77 por callbacks, escritura de 16 bits */
		r = unlz77(n, c, c->r[0], c->r[1], c->r[2], c->r[3], 1);
		c->r[0] = r;
		return 1;
	case 0x13: /* Huffman por callbacks (o desde memoria si r3 = 0) */
		r = unhuff(n, c, c->r[0], c->r[1], c->r[2], c->r[3]);
		c->r[0] = r;
		return 1;
	case 0x14: /* RLE desde memoria, escritura de 8 bits */
		r = unrle(n, c, c->r[0], c->r[1], c->r[2], 0, 0);
		c->r[0] = r;
		return 1;
	case 0x15: /* RLE por callbacks, escritura de 16 bits */
		r = unrle(n, c, c->r[0], c->r[1], c->r[2], c->r[3], 1);
		c->r[0] = r;
		return 1;
	case 0x16:
		if (cpu == NDS_CPU9) {
			unfilter(c, c->r[0], c->r[1], 0, 0);
			return 1;
		}
		break;
	case 0x17:
		if (cpu == NDS_CPU9) {
			unfilter(c, c->r[0], c->r[1], 0, 1);
			return 1;
		}
		break;
	case 0x18:
		if (cpu == NDS_CPU9) {
			unfilter(c, c->r[0], c->r[1], 1, 0);
			return 1;
		}
		break;
	case 0x1F:
		if (cpu == NDS_CPU7) {
			if (c->r[2] & 0x80u)
				nds_cpu_halt(n, cpu);
			return 1;
		}
		break;
	default:
		break;
	}
	warn_once(n, cpu, fn);
	return 1;
}
