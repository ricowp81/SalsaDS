/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — pruebas del recompilador ARMv5TE → PowerPC64 (Fase 4, src/jit/arec*).
 *
 * Una máquina mínima (RAM de 1 MB espejada en 0x02000000, E/S de prueba en 0x04000000 por la ruta lenta, ITCM en 0 y
 * DTCM en 0x0B000000) con dos CPUs: una con el recompilador y otra con el intérprete. En ppc64 (qemu, big-endian) el
 * código generado se ejecuta: programas al azar de ARM y de Thumb (datos, multiplicaciones, cargas y almacenamientos de
 * todos los tamaños por la ruta rápida y la lenta, LDM/STM, saltos hacia adelante, condiciones) y casos dirigidos
 * (interrupción levantada por un almacenamiento, código que se modifica a sí mismo, llamadas al intérprete, BL de
 * Thumb, BX entre estados, LDM con pc) dan los mismos registros, CPSR, ciclos, instrucciones y memoria que el
 * intérprete (modo estricto). En cualquier host se prueba la traducción.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cpu/acore.h"
#include "jit/arec.h"
#include "util/endian.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

#define AM_RAM 0x100000u
#define AM_CODE 0x02000100u
#define AM_DATA 0x02080000u
#define AM_STACK 0x020C0000u
#define AM_IO 0x04000000u

struct am {
	struct acore c;
	uint8_t itcm[ACORE_ITCM_SIZE], dtcm[ACORE_DTCM_SIZE];
	uint8_t ram[AM_RAM];
	uint32_t io_last, io_writes;
	uint32_t irq_on_write; /* escribir en AM_IO + 0x100 levanta la IRQ */
	struct arec *j;
};

static struct am MJ, MI;
static uint8_t code_mem[4u << 20] __attribute__((aligned(65536)));

/* física: [itcm 32K][dtcm 16K][ram 1M] */
#define PH_ITCM 0u
#define PH_RAM (ACORE_ITCM_SIZE + ACORE_DTCM_SIZE)
#define PH_SIZE (PH_RAM + AM_RAM)

static uint32_t io_read(struct am *m, uint32_t a)
{
	(void)m;
	return a * 2654435761u; /* valor que depende de la dirección */
}

static uint32_t am_r8(struct acore *c, uint32_t a)
{
	struct am *m = c->user;

	if ((a >> 24) == 2)
		return m->ram[a & (AM_RAM - 1)];
	if ((a >> 24) == 4)
		return io_read(m, a) & 0xFF;
	return 0;
}
static uint32_t am_r16(struct acore *c, uint32_t a)
{
	struct am *m = c->user;

	if ((a >> 24) == 2)
		return rd16le(m->ram + (a & (AM_RAM - 1)));
	if ((a >> 24) == 4)
		return io_read(m, a) & 0xFFFF;
	return 0;
}
static uint32_t am_r32(struct acore *c, uint32_t a)
{
	struct am *m = c->user;

	if ((a >> 24) == 2)
		return rd32le(m->ram + (a & (AM_RAM - 1)));
	if ((a >> 24) == 4)
		return io_read(m, a);
	return 0;
}
static void am_wr(struct acore *c, uint32_t a, uint32_t v, unsigned sz)
{
	struct am *m = c->user;

	if ((a >> 24) == 2) {
		uint8_t *p = m->ram + (a & (AM_RAM - 1));

		if (sz == 4)
			wr32le(p, v);
		else if (sz == 2)
			wr16le(p, (uint16_t)v);
		else
			*p = (uint8_t)v;
		if (m->j)
			arec_code_write(m->j, PH_RAM + (a & (AM_RAM - 1)), sz);
		return;
	}
	if ((a >> 24) == 4) {
		m->io_last = m->io_last * 31u + v + a;
		m->io_writes++;
		if ((a & 0xFFF) == 0x100)
			acore_set_irq(c, v & 1);
		if ((a & 0xFFF) == 0x104)
			c->cycles += v & 0xF; /* esperas del bus */
	}
}
static void am_w8(struct acore *c, uint32_t a, uint32_t v) { am_wr(c, a, v, 1); }
static void am_w16(struct acore *c, uint32_t a, uint32_t v) { am_wr(c, a, v, 2); }
static void am_w32(struct acore *c, uint32_t a, uint32_t v) { am_wr(c, a, v, 4); }

static const struct acore_bus am_bus = { am_r8, am_r16, am_r32, am_w8, am_w16, am_w32, am_r16, am_r32 };

static const uint8_t *am_code(void *ctx, uint32_t a, uint32_t *phys, uint32_t *avail)
{
	struct am *m = ctx;

	if ((uint64_t)a < m->c.itcm_rd_len) {
		const uint32_t o = a & (ACORE_ITCM_SIZE - 1);

		*phys = PH_ITCM + o;
		*avail = ACORE_ITCM_SIZE - o;
		return m->itcm + o;
	}
	if ((a >> 24) == 2) {
		const uint32_t o = a & (AM_RAM - 1);

		*phys = PH_RAM + o;
		*avail = AM_RAM - o;
		return m->ram + o;
	}
	return NULL;
}

static void itcm_hook(struct acore *c, uint32_t off, uint32_t size)
{
	struct am *m = c->user;

	if (m->j)
		arec_code_write(m->j, PH_ITCM + off, size);
}

static void am_map(struct am *m)
{
	struct arec *j = m->j;

	arec_map_begin(j);
	for (uint32_t a = 0x02000000u; a < 0x03000000u; a += 0x4000u) {
		uint8_t *p = m->ram + (a & (AM_RAM - 1));

		arec_map_set(j, a >> 14, p, p, PH_RAM + (a & (AM_RAM - 1)));
	}
	for (uint32_t a = 0; a < m->c.itcm_rd_len; a += 0x4000u) {
		uint8_t *p = m->itcm + (a & (ACORE_ITCM_SIZE - 1));

		arec_map_set(j, a >> 14, p, p, PH_ITCM + (a & (ACORE_ITCM_SIZE - 1)));
	}
	if (m->c.dtcm_rd_len)
		arec_map_set(j, m->c.dtcm_base >> 14, m->dtcm, m->dtcm, AREC_PHYS_NONE);
	arec_map_end(j);
}

static void am_init(struct am *m, int jit, int strict)
{
	if (m->j)
		arec_destroy(m->j);
	memset(m, 0, sizeof *m);
	acore_init(&m->c, ACORE_ARM9, &am_bus, m);
	acore_attach_tcm(&m->c, m->itcm, m->dtcm);
	/* ITCM 32 KB en 0, DTCM 16 KB en 0x0B000000 */
	acore_set_tcm_state(&m->c, 0x00050078u, 0x0000000Cu, 0x0B00000Au);
	acore_set_cpsr(&m->c, ACORE_MODE_SYS);
	m->c.r[13] = AM_STACK;
	if (jit) {
		struct arec_cfg cfg;

		memset(&cfg, 0, sizeof cfg);
#if defined(__linux__)
		mprotect(code_mem, sizeof code_mem, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
		cfg.code = code_mem;
		cfg.code_bytes = sizeof code_mem;
		cfg.strict = strict;
		cfg.no_link = 0;
		cfg.code_ptr = am_code;
		cfg.ctx = m;
		cfg.phys_size = PH_SIZE;
		m->j = arec_create(&m->c, &cfg);
		if (m->j) {
			m->c.itcm_hook = itcm_hook;
			am_map(m);
		}
	}
}

static int can_run(void)
{
#if defined(__powerpc64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return 1;
#else
	return 0;
#endif
}

/* ---- generador de programas ------------------------------------------------------------------------------- */

static uint32_t rs32;

static uint32_t rnd(void)
{
	rs32 ^= rs32 << 13;
	rs32 ^= rs32 >> 17;
	rs32 ^= rs32 << 5;
	return rs32;
}

static unsigned rn(unsigned n)
{
	return rnd() % n;
}

static uint32_t arm_cond(void)
{
	return rn(4) ? 14u : rn(15);
}

/* registros: r0–r9 datos, r10 desplazamiento chico, r11 E/S, r12 datos, r13 pila */
static uint32_t gen_arm(uint32_t addr, unsigned left)
{
	const unsigned k = rn(100);
	const uint32_t cond = arm_cond() << 28;

	if (k < 5) { /* lo que el recompilador hace en línea con el intérprete de respaldo (FK_* de arec_priv.h) */
		const unsigned rd = rn(10), rm = rn(10), q = rn(13);

		switch (q) {
		case 0:
			return cond | 0x01000090u | (rn(2) << 22) | 12u << 16 | rd << 12 | rm;           /* SWP(B) rd, rm, [r12] */
		case 1:
			return cond | 0x01000090u | (rn(2) << 22) | 11u << 16 | rd << 12 | rm;           /* SWP(B) con la E/S */
		case 2:
		case 3: { /* LDRD / STRD con inmediato (a veces no alineado o sobre la E/S) */
			const unsigned base = rn(5) == 0 ? 11 : 12, pre = rn(4) != 0, up = rn(2), wb = pre ? rn(4) == 0 : 0;
			const uint32_t off = rn(4) ? rn(32) * 8 : rn(256);

			return cond | pre << 24 | up << 23 | 1u << 22 | wb << 21 | base << 16 | (rn(5) * 2) << 12 |
			       ((off >> 4) & 0xF) << 8 | (q == 2 ? 0xDu : 0xFu) << 4 | (off & 0xF);
		}
		case 4: /* LDRD / STRD con registro (r10: desplazamiento chico) */
			return cond | 1u << 24 | rn(2) << 23 | 12u << 16 | (rn(5) * 2) << 12 | (rn(2) ? 0xDu : 0xFu) << 4 | 10u;
		case 5:
			return cond | 0x0E070F3Eu | rd << 12;                                         /* MCR p15, c7, c14, 1 */
		case 6:
			return cond | 0x0E070F9Au | rd << 12;                                         /* MCR p15, c7, c10, 4 */
		case 7:
			return cond | (rn(2) ? 0x0E190F11u : 0x0E110F10u) | rd << 12;                  /* MRC c9,c1,0 / c1,c0,0 */
		case 8:
			return cond | 0x010F0000u | rn(2) << 22 | rd << 12;                           /* MRS rd, CPSR / SPSR */
		case 9: { /* MSR CPSR_c, #imm: sistema con I/F al azar, a veces IRQ (cambio de modo: el intérprete) */
			const uint32_t mode = rn(6) ? 0x1Fu : 0x12u;

			return cond | 0x0321F000u | (rnd() & 0xC0u) | mode;
		}
		case 10:
			return cond | 0x0121F000u | rm;                                               /* MSR CPSR_c, rm */
		case 11:
			if (rn(2)) /* LDR(B/H) rd, [pc, #±off]: un literal en el código (se lee al traducir) */
				return rn(3) ? (cond | 0x051F0000u | rn(2) << 23 | rn(2) << 22 | rd << 12 | (rn(4) ? rn(64) * 4 : rn(256)))
					     : (cond | 0x015F00B0u | rn(2) << 23 | rd << 12 | ((rn(32) * 2) & 0xF0u) << 4 | (rn(8) * 2));
			return cond | 0x016FF000u | rm;                                               /* MSR SPSR_fsxc, rm */
		default:
			return cond | 0x0169F000u | rm;                                               /* MSR SPSR_fc, rm */
		}
	}
	if (k < 40) { /* datos */
		const unsigned op = rn(16), s = rn(2), rd = rn(10), rnr = rn(4) ? rn(13) : 15;
		const unsigned form = rn(3);
		uint32_t w = cond | op << 21 | s << 20 | rnr << 16 | rd << 12;

		if (op >= 8 && op <= 11)
			w |= 1u << 20; /* las de prueba siempre con S */
		if (form == 0)
			w |= 1u << 25 | rn(16) << 8 | rn(256);
		else if (form == 1)
			w |= rn(32) << 7 | rn(4) << 5 | (rn(6) ? rn(13) : 15);
		else
			w |= rn(13) << 8 | rn(4) << 5 | 1u << 4 | rn(13);
		return w;
	}
	if (k < 48) { /* MUL / MLA / MULL */
		if (rn(2)) {
			const unsigned rd = rn(10);
			unsigned rm = rn(10), rs = rn(10), ra = rn(10);

			return cond | (rn(2) ? 1u << 21 : 0) | rn(2) << 20 | rd << 16 | ra << 12 | rs << 8 | 9u << 4 | rm;
		} else {
			unsigned lo = rn(10), hi = rn(10);

			if (lo == hi)
				hi = (hi + 1) % 10;
			return cond | 1u << 23 | rn(2) << 22 | rn(2) << 21 | rn(2) << 20 | hi << 16 | lo << 12 | rn(10) << 8 |
			       9u << 4 | rn(10);
		}
	}
	if (k < 50) /* CLZ */
		return cond | 0x016F0F10u | rn(10) << 12 | rn(13);
	if (k < 72) { /* LDR/STR palabra y byte */
		const unsigned load = rn(2), byte = rn(3) == 0, rd = load ? rn(10) : rn(13);
		const unsigned base = rn(5) == 0 ? 11 : 12;
		const unsigned pre = rn(4) != 0, up = rn(2), wb = pre ? rn(4) == 0 : 0;
		uint32_t off;

		if (rn(3)) {
			off = byte || rn(6) == 0 ? rn(200) : rn(50) * 4;
			return cond | 1u << 26 | pre << 24 | up << 23 | byte << 22 | wb << 21 | load << 20 | base << 16 | rd << 12 | off;
		}
		return cond | 3u << 25 | pre << 24 | up << 23 | byte << 22 | wb << 21 | load << 20 | base << 16 | rd << 12 |
		       rn(4) << 7 | 0u << 5 | 10;
	}
	if (k < 82) { /* medias palabras y con signo */
		static const uint8_t sh[3] = { 0xB, 0xD, 0xF };
		const unsigned load = rn(2), rd = load ? rn(10) : rn(13), base = rn(5) == 0 ? 11 : 12;
		const unsigned kind = load ? sh[rn(3)] : 0xB, pre = rn(4) != 0, up = rn(2), wb = pre ? rn(4) == 0 : 0;
		const uint32_t off = rn(100) * 2 + (rn(5) == 0);

		return cond | pre << 24 | up << 23 | 1u << 22 | wb << 21 | load << 20 | base << 16 | rd << 12 |
		       ((off >> 4) & 0xF) << 8 | kind << 4 | (off & 0xF);
	}
	if (k < 90) { /* LDM/STM con la pila (pares equilibrados: STMDB sp! / LDMIA sp!) */
		const unsigned list = (rnd() & 0x3FF) | (rn(4) == 0 ? 0x4000u : 0) | 1u;
		(void)addr;
		if (rn(2))
			return cond | 0x092D0000u | list; /* STMDB sp!, {…} */
		return cond | 0x08BD0000u | (list & 0x3FF);        /* LDMIA sp!, {…} */
	}
	if (k < 96 && left > 4) { /* salto hacia adelante (condicional) */
		const unsigned skip = 1 + rn(3);

		return (rn(4) ? rn(14) : 14u) << 28 | 0x0A000000u | (skip - 1);
	}
	if (k < 98) { /* vuelta condicional a lr (que apunta al final): BXcc lr o MOVcc pc, lr */
		const uint32_t c = (uint32_t)rn(14) << 28;

		return rn(2) ? (c | 0x012FFF1Eu) : (c | 0x01A0F00Eu);
	}
	/* algo que va al intérprete: MRS / MSR de banderas */
	if (rn(2))
		return cond | 0x010F0000u | rn(10) << 12;              /* MRS rd, CPSR */
	return cond | 0x0128F000u | rn(10);                          /* MSR CPSR_f, rm */
}

static uint16_t gen_thumb(unsigned left)
{
	const unsigned k = rn(100);

	if (k < 10) /* LSL/LSR/ASR inmediato */
		return (uint16_t)(rn(3) << 11 | rn(32) << 6 | rn(8) << 3 | rn(8));
	if (k < 20) /* ADD/SUB registro o inmediato */
		return (uint16_t)(0x1800u | rn(4) << 9 | rn(8) << 6 | rn(8) << 3 | rn(8));
	if (k < 32) /* MOV/CMP/ADD/SUB inmediato de 8 bits */
		return (uint16_t)(0x2000u | rn(4) << 11 | rn(8) << 8 | rn(256));
	if (k < 50) /* ALU */
		return (uint16_t)(0x4000u | rn(16) << 6 | rn(8) << 3 | rn(8));
	if (k < 56) { /* registros altos: ADD/CMP/MOV con r8-r12 */
		const unsigned op = rn(3), h1 = rn(2), h2 = rn(2);
		unsigned rd = rn(8), rs = rn(8);

		if (op != 1 && h1 && rd >= 5)
			rd = rn(5); /* no escribir r13–r15 */
		return (uint16_t)(0x4400u | op << 8 | h1 << 7 | h2 << 6 | rs << 3 | rd);
	}
	if (k < 62) /* LDR rd, [PC, #imm] */
		return (uint16_t)(0x4800u | rn(8) << 8 | rn(64));
	if (k < 72) { /* con registro: base r6 (datos), desplazamiento r7 (chico, par) */
		return (uint16_t)(0x5000u | rn(8) << 9 | 7u << 6 | 6u << 3 | rn(6));
	}
	if (k < 82) { /* inmediato: palabra/byte, media palabra, relativo a SP */
		const unsigned w = rn(3);

		if (w == 0)
			return (uint16_t)(0x6000u | rn(4) << 11 | rn(32) << 6 | 6u << 3 | rn(6));
		if (w == 1)
			return (uint16_t)(0x8000u | rn(2) << 11 | rn(32) << 6 | 6u << 3 | rn(6));
		return (uint16_t)(0x9000u | rn(2) << 11 | rn(6) << 8 | rn(32));
	}
	if (k < 86) /* PUSH / POP equilibrados no se garantizan: solo PUSH de r0-r5 y POP con la misma forma */
		return rn(2) ? (uint16_t)(0xB400u | ((rnd() & 0x3F) | 1u)) : (uint16_t)(0xBC00u | ((rnd() & 0x3F) | 1u));
	if (k < 90) /* ADD rd, SP / PC */
		return (uint16_t)(0xA000u | rn(2) << 11 | rn(6) << 8 | rn(256));
	if (k < 97 && left > 4) /* Bcond hacia adelante */
		return (uint16_t)(0xD000u | rn(14) << 8 | rn(3));
	if (k < 98) /* BX lr o MOV pc, lr (lr apunta al final) */
		return rn(2) ? (uint16_t)0x4770u : (uint16_t)0x46F7u;
	return (uint16_t)(0x4000u | rn(16) << 6 | rn(8) << 3 | rn(8));
}

/* ---- comparación ------------------------------------------------------------------------------------------ */

static int exact = 1; /* modo estricto: también ciclos e instrucciones */

static int same_state(const char *what, unsigned seed)
{
	const struct acore *a = &MJ.c, *b = &MI.c;
	int ok = 1;

	for (int k = 0; k < 16; k++)
		if (a->r[k] != b->r[k])
			ok = 0;
	if (a->cpsr != b->cpsr || (exact && (a->cycles != b->cycles || a->icount != b->icount)))
		ok = 0;
	if (memcmp(MJ.ram, MI.ram, AM_RAM) || memcmp(MJ.dtcm, MI.dtcm, sizeof MJ.dtcm) || MJ.io_last != MI.io_last)
		ok = 0;
	if (!ok) {
		printf("FALLO arec %s semilla %u: JIT pc %08x cpsr %08x ciclos %llu instr %llu | intérprete pc %08x cpsr %08x ciclos "
		       "%llu instr %llu\n",
		       what, seed, acore_pc(a), a->cpsr, (unsigned long long)a->cycles, (unsigned long long)a->icount, acore_pc(b),
		       b->cpsr, (unsigned long long)b->cycles, (unsigned long long)b->icount);
		for (int k = 0; k < 16; k++)
			if (a->r[k] != b->r[k])
				printf("  r%d %08x contra %08x\n", k, a->r[k], b->r[k]);
		if (memcmp(MJ.ram, MI.ram, AM_RAM)) {
			uint32_t o = 0;

			while (MJ.ram[o] == MI.ram[o])
				o++;
			printf("  RAM distinta desde %08x\n", 0x02000000u + o);
		}
	}
	return ok;
}

static int same_state_q(void)
{
	const struct acore *a = &MJ.c, *b = &MI.c;

	for (int k = 0; k < 16; k++)
		if (a->r[k] != b->r[k])
			return 0;
	if (a->cpsr != b->cpsr || (exact && (a->cycles != b->cycles || a->icount != b->icount)))
		return 0;
	if (memcmp(MJ.ram, MI.ram, AM_RAM) || memcmp(MJ.dtcm, MI.dtcm, sizeof MJ.dtcm) || MJ.io_last != MI.io_last)
		return 0;
	return 1;
}

static void setup_both(int strict)
{
	exact = strict;
	am_init(&MJ, 1, strict);
	am_init(&MI, 0, 0);
}

static void put_both(uint32_t a, const void *src, size_t n)
{
	memcpy(MJ.ram + (a & (AM_RAM - 1)), src, n);
	memcpy(MI.ram + (a & (AM_RAM - 1)), src, n);
	if (MJ.j)
		arec_flush(MJ.j);
}

static void regs_both(uint32_t seed)
{
	rs32 = seed * 2654435761u + 1;
	for (int k = 0; k < 10; k++) {
		const uint32_t v = rn(3) == 0 ? rnd() : rn(4) == 0 ? 0 : rn(4) == 0 ? 0x80000000u + rn(5) - 2 : rn(1000);

		MJ.c.r[k] = MI.c.r[k] = v;
	}
	MJ.c.r[10] = MI.c.r[10] = rn(64) * 2;
	MJ.c.r[11] = MI.c.r[11] = AM_IO + 0x200;
	MJ.c.r[12] = MI.c.r[12] = AM_DATA + 0x400;
	MJ.c.r[13] = MI.c.r[13] = AM_STACK;
	MJ.c.r[14] = MI.c.r[14] = 0x02000000u;
	{
		const uint32_t f = (rnd() & 0xF0000000u);

		MJ.c.cpsr = (MJ.c.cpsr & 0x0FFFFFFFu) | f;
		MI.c.cpsr = (MI.c.cpsr & 0x0FFFFFFFu) | f;
	}
	for (uint32_t o = 0; o < 0x2000; o += 4) {
		const uint32_t v = rnd();

		wr32le(MJ.ram + ((AM_DATA + o) & (AM_RAM - 1)), v);
		wr32le(MI.ram + ((AM_DATA + o) & (AM_RAM - 1)), v);
	}
}

static void run_both(uint64_t cycles)
{
	arec_run(MJ.j, MJ.c.cycles + cycles);
	acore_run(&MI.c, MI.c.cycles + cycles);
}

static int quiet;

static int fuzz_arm_n(unsigned seed, int strict, unsigned cut)
{
	static uint32_t prog[104];
	unsigned n;

	setup_both(strict);
	if (!MJ.j)
		return 1;
	regs_both(seed);
	rs32 = seed * 7919u + 17;
	n = 24 + rn(64);
	for (unsigned i = 0; i < n; i++)
		prog[i] = gen_arm(AM_CODE + 4 * i, n - i);
	if (cut < n)
		n = cut;
	for (unsigned k = 0; k < 6; k++)
		prog[n + k] = 0xEAFFFFFEu; /* b . (varios: los saltos hacia adelante caen en uno) */
	MJ.c.r[14] = MI.c.r[14] = AM_CODE + 4 * n; /* las vueltas a lr terminan el programa */
	n += 5;
	for (unsigned i = 0; i <= n; i++) {
		uint8_t b[4];

		if (getenv("AREC_LIST"))
			printf("  %08x: %08x\n", AM_CODE + 4 * i, prog[i]);
		wr32le(b, prog[i]);
		put_both(AM_CODE + 4 * i, b, 4);
	}
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(strict ? 400 : 20000);
	if (arec_stats(MJ.j)->bad_blocks)
		printf("arec: %llu bloques con error de banderas (semilla %u)\n", (unsigned long long)arec_stats(MJ.j)->bad_blocks, seed);
	if (cut < 1000 && quiet)
		return same_state_q();
	return same_state(strict ? "arm (estricto)" : "arm", seed);
}

static void fuzz_arm(unsigned seed, int strict)
{
	if (fuzz_arm_n(seed, strict, 1000)) {
		CHECK(1);
		return;
	}
	CHECK(0);
	/* el prefijo más corto que falla */
	quiet = 1;
	for (unsigned k = 1; k < 96; k++)
		if (!fuzz_arm_n(seed, strict, k)) {
			printf("  primer prefijo que falla: %u instrucciones; la última: %08x (en %08x)\n", k,
			       rd32le(MJ.ram + ((AM_CODE + 4 * (k - 1)) & (AM_RAM - 1))), AM_CODE + 4 * (k - 1));
			quiet = 0;
			fuzz_arm_n(seed, strict, k);
			break;
		}
	quiet = 0;
}

static int fuzz_thumb_n(unsigned seed, int strict, unsigned cut)
{
	static uint16_t prog[168];
	unsigned n;

	setup_both(strict);
	if (!MJ.j)
		return 1;
	regs_both(seed);
	MJ.c.r[6] = MI.c.r[6] = AM_DATA + 0x200;
	MJ.c.r[7] = MI.c.r[7] = rn(32) * 2;
	rs32 = seed * 7919u + 23;
	n = 32 + rn(96);
	for (unsigned i = 0; i < n; i++) {
		prog[i] = gen_thumb(n - i);
	}
	if (cut < n)
		n = cut;
	for (unsigned k = 0; k < 6; k++)
		prog[n + k] = 0xE7FEu; /* b . */
	MJ.c.r[14] = MI.c.r[14] = (AM_CODE + 2 * n) | 1u;
	n += 5;
	if (getenv("AREC_SEED")) {
		for (unsigned i = 0; i <= n; i++)
			printf("%08x: %04x\n", AM_CODE + 2 * i, prog[i]);
		for (int k = 0; k < 14; k++)
			printf("r%d=%08x ", k, MI.c.r[k]);
		printf("cpsr=%08x\n", MI.c.cpsr);
	}
	for (unsigned i = 0; i <= n; i++) {
		uint8_t b[2];

		wr16le(b, prog[i]);
		put_both(AM_CODE + 2 * i, b, 2);
	}
	acore_bx(&MJ.c, AM_CODE | 1);
	acore_bx(&MI.c, AM_CODE | 1);
	run_both(strict ? 500 : 20000);
	if (arec_stats(MJ.j)->bad_blocks)
		printf("arec: %llu bloques con error de banderas (semilla %u)\n", (unsigned long long)arec_stats(MJ.j)->bad_blocks, seed);
	if (cut < 1000 && quiet)
		return same_state_q();
	return same_state(strict ? "thumb (estricto)" : "thumb", seed);
}

static void fuzz_thumb(unsigned seed, int strict)
{
	if (fuzz_thumb_n(seed, strict, 1000)) {
		CHECK(1);
		return;
	}
	CHECK(0);
	quiet = 1;
	for (unsigned k = 1; k < 160; k++)
		if (!fuzz_thumb_n(seed, strict, k)) {
			printf("  primer prefijo que falla: %u instrucciones; la última: %04x (en %08x)\n", k,
			       rd16le(MJ.ram + ((AM_CODE + 2 * (k - 1)) & (AM_RAM - 1))), AM_CODE + 2 * (k - 1));
			quiet = 0;
			fuzz_thumb_n(seed, strict, k);
			break;
		}
	quiet = 0;
}

/* ---- casos dirigidos -------------------------------------------------------------------------------------- */

static void load_prog(const uint32_t *p, unsigned n, uint32_t at)
{
	for (unsigned i = 0; i < n; i++) {
		uint8_t b[4];

		wr32le(b, p[i]);
		put_both(at + 4 * i, b, 4);
	}
}

/* Una escritura a la E/S levanta la IRQ en medio del bloque: se toma en la instrucción siguiente. */
static void test_irq_mid_block(void)
{
	static const uint32_t prog[] = {
		0xE3A00001u, /* mov r0, #1 */
		0xE3A01301u, /* mov r1, #0x04000000 */
		0xE2811C01u, /* add r1, r1, #0x100 */
		0xE5810000u, /* str r0, [r1] -> IRQ */
		0xE2822001u, /* add r2, r2, #1 */
		0xE2822001u, /* add r2, r2, #1 */
		0xEAFFFFFEu, /* b . */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(prog, 7, AM_CODE);
	{
		uint8_t b[4];

		wr32le(b, 0xEAFFFFFEu); /* vector de IRQ (vectores bajos, en la ITCM): b . */
		memcpy(MJ.itcm + 0x18, b, 4);
		memcpy(MI.itcm + 0x18, b, 4);
	}
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(100);
	CHECK(same_state("IRQ a mitad de bloque", 0));
	CHECK_EQ(MJ.c.cpsr & ACORE_MODE_MASK, ACORE_MODE_IRQ);
	CHECK_EQ(MJ.c.r[2], 0);
}

/* Un literal (LDR rd, [pc, #off]) que el programa reescribe: el bloque que lo leyó al traducirse se invalida. */
static void test_literal(void)
{
	static const uint32_t prog[] = {
		0xE59F0010u, /* ldr r0, [pc, #16]  -> literal en AM_CODE + 24 */
		0xE0811000u, /* add r1, r1, r0 */
		0xE2800001u, /* add r0, r0, #1 */
		0xE28F2004u, /* add r2, pc, #4     -> AM_CODE + 24 */
		0xE5820000u, /* str r0, [r2]       -> el literal cambia */
		0xEAFFFFF9u, /* b al principio */
		0x00000005u, /* literal */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(prog, 7, AM_CODE);
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(200);
	CHECK(same_state("literal que cambia", 0));
	CHECK(MJ.c.r[1] > 5u * 6u);
	CHECK(arec_stats(MJ.j)->inval_blocks >= 1);
}

/* MSR CPSR_c en línea que habilita la IRQ con la línea ya levantada: se toma en la instrucción siguiente. */
static void test_msr_irq(void)
{
	static const uint32_t prog[] = {
		0xE321F09Fu, /* msr cpsr_c, #0x9F (I puesto) */
		0xE3A00001u, /* mov r0, #1 */
		0xE3A01301u, /* mov r1, #0x04000000 */
		0xE2811C01u, /* add r1, r1, #0x100 */
		0xE5810000u, /* str r0, [r1] -> IRQ (enmascarada) */
		0xE2822001u, /* add r2, r2, #1 */
		0xE10F3000u, /* mrs r3, cpsr */
		0xE321F01Fu, /* msr cpsr_c, #0x1F -> se toma la IRQ */
		0xE2822001u, /* add r2, r2, #1 */
		0xEAFFFFFEu, /* b . */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(prog, 10, AM_CODE);
	{
		uint8_t b[4];

		wr32le(b, 0xEAFFFFFEu);
		memcpy(MJ.itcm + 0x18, b, 4);
		memcpy(MI.itcm + 0x18, b, 4);
	}
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(100);
	CHECK(same_state("MSR que habilita la IRQ", 0));
	CHECK_EQ(MJ.c.cpsr & ACORE_MODE_MASK, ACORE_MODE_IRQ);
	CHECK_EQ(MJ.c.r[2], 1);
}

/* El programa reescribe una instrucción que viene después en el mismo bloque y en un bucle. */
static void test_smc(void)
{
	static const uint32_t prog[] = {
		0xE59F0010u, /* ldr r0, [pc, #16]  -> la palabra nueva */
		0xE28F1000u, /* add r1, pc, #0     -> dirección de la instrucción a cambiar (AM_CODE + 12) */
		0xE5810000u, /* str r0, [r1] */
		0xE3A02001u, /* mov r2, #1   (se reescribe por mov r2, #7) */
		0xE2833001u, /* add r3, r3, #1 */
		0xEAFFFFFEu, /* b . */
		0xE3A02007u, /* mov r2, #7 */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(prog, 7, AM_CODE);
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(60);
	CHECK(same_state("código que se modifica", 0));
	CHECK_EQ(MJ.c.r[2], 7);
	CHECK(arec_stats(MJ.j)->inval_blocks >= 1);
	/* y otra vez desde el principio con la caché ya llena: se vuelve a traducir */
	run_both(10);
	acore_set_pc(&MJ.c, AM_CODE + 12);
	acore_set_pc(&MI.c, AM_CODE + 12);
	run_both(30);
	CHECK(same_state("código modificado, otra vez", 0));
}

/* Thumb: BL (las dos mitades), BX a ARM, BLX de vuelta, POP {pc}, LDM con pc. */
static void test_calls(void)
{
	/* ARM en AM_CODE: entra a Thumb en AM_CODE + 0x40 */
	static const uint32_t arm[] = {
		0xE28F0039u, /* add r0, pc, #0x39  -> AM_CODE + 0x40 | 1 */
		0xE12FFF30u, /* blx r0 */
		0xE2855001u, /* add r5, r5, #1 */
		0xE92D4003u, /* push {r0, r1, lr} */
		0xE8BD8003u, /* pop {r0, r1, pc} -> lr = AM_CODE + 8: vuelve al add de arriba... */
	};
	static const uint16_t th[] = {
		0x2405, /* movs r4, #5 */
		0xF000, 0xF803, /* bl -> AM_CODE + 0x4C */
		0x3401, /* adds r4, #1 */
		0x4770, /* bx lr (vuelve a ARM en AM_CODE + 8) */
		0x46C0, /* nop */
		0xB500, /* push {lr} */
		0x3410, /* adds r4, #16 */
		0xBD00, /* pop {pc} */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(arm, 5, AM_CODE);
	{
		uint8_t b[sizeof th];

		for (unsigned i = 0; i < sizeof th / 2; i++)
			wr16le(b + 2 * i, th[i]);
		put_both(AM_CODE + 0x40, b, sizeof b);
	}
	MJ.c.r[14] = MI.c.r[14] = AM_CODE + 0x100; /* sin uso */
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	run_both(300);
	CHECK(same_state("llamadas", 0));
}

/* Ciclos de espera del bus en una escritura lenta (modo estricto: sale y el despachador sigue exacto). */
static void test_waits(void)
{
	static const uint32_t prog[] = {
		0xE3A0000Fu, /* mov r0, #15 */
		0xE3A01301u, /* mov r1, #0x04000000 */
		0xE2811C01u, /* add r1, r1, #0x100 */
		0xE5810004u, /* str r0, [r1, #4] -> 15 ciclos de espera */
		0xE2822001u, /* add r2, r2, #1 */
		0xE2822001u, /* add r2, r2, #1 */
		0xE2822001u, /* add r2, r2, #1 */
		0xEAFFFFF7u, /* b al principio */
	};

	setup_both(1);
	if (!MJ.j)
		return;
	load_prog(prog, 8, AM_CODE);
	acore_set_pc(&MJ.c, AM_CODE);
	acore_set_pc(&MI.c, AM_CODE);
	for (int k = 0; k < 40; k++) {
		run_both(7 + (uint64_t)k % 13);
		if (!same_state("esperas del bus", (unsigned)k))
			break;
	}
	CHECK(same_state("esperas del bus", 99));
}

static void test_translate_any_host(void)
{
	static struct am T;
	struct arec_cfg cfg;
	static uint32_t out[8192];
	static uint32_t buf[1u << 18];
	const uint32_t prog[] = { 0xE0911002u, 0x1AFFFFFDu, 0xE5912004u, 0xE12FFF1Eu };

	am_init(&T, 0, 0);
	for (unsigned i = 0; i < 4; i++)
		wr32le(T.ram + ((AM_CODE + 4 * i) & (AM_RAM - 1)), prog[i]);
	memset(&cfg, 0, sizeof cfg);
	cfg.code = buf;
	cfg.code_bytes = sizeof buf;
	cfg.code_ptr = am_code;
	cfg.ctx = &T;
	cfg.phys_size = PH_SIZE;
	CHECK(arec_translate_only(&T.c, &cfg, AM_CODE, out, 8192) > 10);
	CHECK(arec_translate_only(&T.c, &cfg, AM_CODE | 1, out, 8192) > 0);
	/* no se emiten formas con punto (Rc = 1) en las instrucciones X/XO de opcode 31 ni rlwinm. ni mfcr fuera de lo frío:
	 * se revisa solo el bit Rc de las de opcode 31 y 21 */
	{
		const int n = arec_translate_only(&T.c, &cfg, AM_CODE, out, 8192);
		int dot = 0;

		for (int i = 0; i < n; i++) {
			const uint32_t op = out[i] >> 26;

			if ((op == 21 || op == 20 || op == 23) && (out[i] & 1))
				dot++;
			if (op == 31 && (out[i] & 1) && ((out[i] >> 1) & 0x3FF) != 150)
				dot++;
		}
		CHECK_EQ(dot, 0);
	}
}

void test_arec(void)
{
	test_translate_any_host();
	if (!can_run()) {
		printf("arec: este host no ejecuta PowerPC64; solo traducción\n");
		return;
	}
	if (getenv("AREC_SEED")) {
		const unsigned sd = (unsigned)atoi(getenv("AREC_SEED"));
		const unsigned cut = getenv("AREC_CUT") ? (unsigned)atoi(getenv("AREC_CUT")) : 1000;

		const int st = getenv("AREC_FAST") ? 0 : 1;

		if (getenv("AREC_THUMB"))
			CHECK(fuzz_thumb_n(sd, st, cut));
		else
			CHECK(fuzz_arm_n(sd, st, cut));
		return;
	}
	test_irq_mid_block();
	test_msr_irq();
	test_literal();
	test_smc();
	test_calls();
	test_waits();
	for (unsigned s = 1; s <= 300; s++)
		fuzz_arm(s, 1);
	for (unsigned s = 1; s <= 300; s++)
		fuzz_thumb(s, 1);
	{
		const int before = check_failed;

		for (unsigned s = 1; s <= 100; s++) {
			fuzz_arm(1000 + s, 0);
			fuzz_thumb(1000 + s, 0);
		}
		(void)before;
	}
}
