/*
 * SalsaDS — Fase 0.3: núcleos en ensamblador del PPE. Ver kernels.h.
 *
 * Convenciones de registros dentro de los bloques asm (todos declarados como
 * clobber): r14..r21 datos, r22..r25 índices, r26..r31 temporales. En los de
 * banderas: r14..r22 son r0..r8 del guest, r23..r25 el estado de banderas, el
 * resto temporales y r31 vale 1.
 */
#include "kernels.h"

#include <string.h>

#if KERNELS_AVAILABLE

#define CLOBBERS "r0", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", \
		 "r25", "r26", "r27", "r28", "r29", "r30", "r31", "ctr", "xer", "cc", "memory", \
		 "cr0", "cr1", "cr2", "cr3", "cr4", "cr5", "cr6", "cr7"

/* Índices fijos 0, 4, 8, 12 dentro de la línea. */
#define SETUP_IDX "li 22,0\n\t li 23,4\n\t li 24,8\n\t li 25,12\n\t"

#define KMEM(name, body)                                                         \
	static void name(void *base, uint64_t iters)                             \
	{                                                                        \
		__asm__ volatile(SETUP_IDX "mtctr %1\n"                          \
				 "1:\n\t" body "bdnz 1b\n"                       \
				 : : "b"(base), "r"(iters) : CLOBBERS);          \
	}

/* bswap32 de x en d (tres instrucciones) y bswap16 (dos). */
#define SW32(d, x) "rlwinm " #d "," #x ",8,0,31\n\t rlwimi " #d "," #x ",24,0,7\n\t rlwimi " #d "," #x ",24,16,23\n\t"
#define SW16(d, x) "rlwinm " #d "," #x ",8,16,23\n\t rlwimi " #d "," #x ",24,24,31\n\t"

/* Baseline: el mismo bucle sin accesos (cuatro nops de relleno). */
KMEM(k_base, "ori 0,0,0\n\t ori 0,0,0\n\t ori 0,0,0\n\t ori 0,0,0\n\t")

KMEM(k_lwz, "lwzx 14,%0,22\n\t lwzx 15,%0,23\n\t lwzx 16,%0,24\n\t lwzx 17,%0,25\n\t")
KMEM(k_lwbrx, "lwbrx 14,%0,22\n\t lwbrx 15,%0,23\n\t lwbrx 16,%0,24\n\t lwbrx 17,%0,25\n\t")
KMEM(k_lwz_rot,
     "lwzx 14,%0,22\n\t lwzx 15,%0,23\n\t lwzx 16,%0,24\n\t lwzx 17,%0,25\n\t"
     "rlwinm 18,14,8,0,31\n\t rlwinm 19,15,8,0,31\n\t rlwinm 20,16,8,0,31\n\t rlwinm 21,17,8,0,31\n\t"
     "rlwimi 18,14,24,0,7\n\t rlwimi 19,15,24,0,7\n\t rlwimi 20,16,24,0,7\n\t rlwimi 21,17,24,0,7\n\t"
     "rlwimi 18,14,24,16,23\n\t rlwimi 19,15,24,16,23\n\t rlwimi 20,16,24,16,23\n\t rlwimi 21,17,24,16,23\n\t")

KMEM(k_lhz, "lhzx 14,%0,22\n\t lhzx 15,%0,23\n\t lhzx 16,%0,24\n\t lhzx 17,%0,25\n\t")
KMEM(k_lhbrx, "lhbrx 14,%0,22\n\t lhbrx 15,%0,23\n\t lhbrx 16,%0,24\n\t lhbrx 17,%0,25\n\t")
KMEM(k_lhz_rot,
     "lhzx 14,%0,22\n\t lhzx 15,%0,23\n\t lhzx 16,%0,24\n\t lhzx 17,%0,25\n\t"
     "rlwinm 18,14,8,16,23\n\t rlwinm 19,15,8,16,23\n\t rlwinm 20,16,8,16,23\n\t rlwinm 21,17,8,16,23\n\t"
     "rlwimi 18,14,24,24,31\n\t rlwimi 19,15,24,24,31\n\t rlwimi 20,16,24,24,31\n\t rlwimi 21,17,24,24,31\n\t")

KMEM(k_stw, "stwx 14,%0,22\n\t stwx 15,%0,23\n\t stwx 16,%0,24\n\t stwx 17,%0,25\n\t")
KMEM(k_stwbrx, "stwbrx 14,%0,22\n\t stwbrx 15,%0,23\n\t stwbrx 16,%0,24\n\t stwbrx 17,%0,25\n\t")
KMEM(k_stw_rot,
     "rlwinm 18,14,8,0,31\n\t rlwinm 19,15,8,0,31\n\t rlwinm 20,16,8,0,31\n\t rlwinm 21,17,8,0,31\n\t"
     "rlwimi 18,14,24,0,7\n\t rlwimi 19,15,24,0,7\n\t rlwimi 20,16,24,0,7\n\t rlwimi 21,17,24,0,7\n\t"
     "rlwimi 18,14,24,16,23\n\t rlwimi 19,15,24,16,23\n\t rlwimi 20,16,24,16,23\n\t rlwimi 21,17,24,16,23\n\t"
     "stwx 18,%0,22\n\t stwx 19,%0,23\n\t stwx 20,%0,24\n\t stwx 21,%0,25\n\t")

KMEM(k_sth, "sthx 14,%0,22\n\t sthx 15,%0,23\n\t sthx 16,%0,24\n\t sthx 17,%0,25\n\t")
KMEM(k_sthbrx, "sthbrx 14,%0,22\n\t sthbrx 15,%0,23\n\t sthbrx 16,%0,24\n\t sthbrx 17,%0,25\n\t")
KMEM(k_sth_rot,
     "rlwinm 18,14,8,16,23\n\t rlwinm 19,15,8,16,23\n\t rlwinm 20,16,8,16,23\n\t rlwinm 21,17,8,16,23\n\t"
     "rlwimi 18,14,24,24,31\n\t rlwimi 19,15,24,24,31\n\t rlwimi 20,16,24,24,31\n\t rlwimi 21,17,24,24,31\n\t"
     "sthx 18,%0,22\n\t sthx 19,%0,23\n\t sthx 20,%0,24\n\t sthx 21,%0,25\n\t")

void kmem_base(void *base, uint64_t iters)
{
	k_base(base, iters);
}

const struct kmem_desc kmem_table[] = {
	{ "lwz  (carga normal)", KM_LOAD32, KI_PLAIN, k_lwz },
	{ "lwbrx", KM_LOAD32, KI_BR, k_lwbrx },
	{ "lwz + rotaciones", KM_LOAD32, KI_ROT, k_lwz_rot },
	{ "lhz  (carga normal)", KM_LOAD16, KI_PLAIN, k_lhz },
	{ "lhbrx", KM_LOAD16, KI_BR, k_lhbrx },
	{ "lhz + rotaciones", KM_LOAD16, KI_ROT, k_lhz_rot },
	{ "stw  (almacenamiento normal)", KM_STORE32, KI_PLAIN, k_stw },
	{ "stwbrx", KM_STORE32, KI_BR, k_stwbrx },
	{ "rotaciones + stw", KM_STORE32, KI_ROT, k_stw_rot },
	{ "sth  (almacenamiento normal)", KM_STORE16, KI_PLAIN, k_sth },
	{ "sthbrx", KM_STORE16, KI_BR, k_sthbrx },
	{ "rotaciones + sth", KM_STORE16, KI_ROT, k_sth_rot },
};
const int kmem_count = (int)(sizeof kmem_table / sizeof kmem_table[0]);

/* ---- Latencia: recorrer una permutación ---------------------------------- */

/* Un paso: in -> out. El índice se escala y enmascara (rlwinm), se carga la
 * entrada siguiente y, si hace falta, se le da la vuelta a los bytes. El
 * operando 0 del asm es el índice, el 1 la tabla y el 2 las vueltas. */
#define KCHASE(name, step)                                                       \
	static uint32_t name(const void *table, uint64_t iters)                  \
	{                                                                        \
		uint64_t idx = 0;                                                \
		__asm__ volatile("mr 14,%0\n\t mtctr %2\n"                       \
				 "1:\n\t" step(14, 16) step(16, 14) step(14, 16) step(16, 14) \
				 "bdnz 1b\n\t mr %0,14\n"                        \
				 : "+r"(idx) : "b"(table), "r"(iters) : CLOBBERS); \
		return (uint32_t)idx;                                            \
	}

#define CH32_PLAIN(in, out) "rlwinm 15," #in ",2,20,29\n\t lwzx " #out ",%1,15\n\t"
#define CH32_BR(in, out) "rlwinm 15," #in ",2,20,29\n\t lwbrx " #out ",%1,15\n\t"
#define CH32_ROT(in, out) "rlwinm 15," #in ",2,20,29\n\t lwzx 17,%1,15\n\t" SW32(out, 17)
#define CH16_PLAIN(in, out) "rlwinm 15," #in ",1,21,30\n\t lhzx " #out ",%1,15\n\t"
#define CH16_BR(in, out) "rlwinm 15," #in ",1,21,30\n\t lhbrx " #out ",%1,15\n\t"
#define CH16_ROT(in, out) "rlwinm 15," #in ",1,21,30\n\t lhzx 17,%1,15\n\t" SW16(out, 17)

KCHASE(c_lwz, CH32_PLAIN)
KCHASE(c_lwbrx, CH32_BR)
KCHASE(c_lwz_rot, CH32_ROT)
KCHASE(c_lhz, CH16_PLAIN)
KCHASE(c_lhbrx, CH16_BR)
KCHASE(c_lhz_rot, CH16_ROT)

const struct kchase_desc kchase_table[] = {
	{ "lwz  (carga normal)", 32, KI_PLAIN, c_lwz },
	{ "lwbrx", 32, KI_BR, c_lwbrx },
	{ "lwz + rotaciones", 32, KI_ROT, c_lwz_rot },
	{ "lhz  (carga normal)", 16, KI_PLAIN, c_lhz },
	{ "lhbrx", 16, KI_BR, c_lhbrx },
	{ "lhz + rotaciones", 16, KI_ROT, c_lhz_rot },
};
const int kchase_count = (int)(sizeof kchase_table / sizeof kchase_table[0]);

#else /* no PowerPC64: sin núcleos */

void kmem_base(void *base, uint64_t iters)
{
	(void)base;
	(void)iters;
}
const struct kmem_desc kmem_table[1];
const int kmem_count = 0;
const struct kchase_desc kchase_table[1];
const int kchase_count = 0;

#endif

/* ---- Comunes a todos los hosts ------------------------------------------- */

uint32_t kchase_fill(void *table, int bits, enum kmem_impl impl, uint64_t steps)
{
	uint8_t *t = table;
	uint32_t next[KCHASE_ENTRIES], idx = 0;

	/* Permutación cíclica de largo KCHASE_ENTRIES: i -> (i * 389 + 1) mod 1024
	 * (389 es impar y 389 - 1 = 388 es múltiplo de 4: ciclo completo). */
	for (uint32_t i = 0; i < KCHASE_ENTRIES; i++)
		next[i] = (i * 389u + 1u) & (KCHASE_ENTRIES - 1);
	for (uint32_t i = 0; i < KCHASE_ENTRIES; i++) {
		uint32_t v = next[i];
		int le = impl != KI_PLAIN;
		if (bits == 32) {
			for (int b = 0; b < 4; b++)
				t[i * 4 + b] = (uint8_t)(v >> (le ? 8 * b : 24 - 8 * b));
		} else {
			for (int b = 0; b < 2; b++)
				t[i * 2 + b] = (uint8_t)(v >> (le ? 8 * b : 8 - 8 * b));
		}
	}
	for (uint64_t s = 0; s < steps; s++)
		idx = next[idx];
	return idx;
}

/* ---- Banderas: referencia en C -------------------------------------------- */

#define G_R1 0x13579BDFu
#define G_R2 0x70000000u
#define G_R5 0x2AAAAAABu
#define G_R7 3u
#define G_R8 0x80000000u

void kflags_init(struct kguest *g, uint32_t iters)
{
	memset(g, 0, sizeof *g);
	g->r[1] = G_R1;
	g->r[2] = G_R2;
	g->r[4] = iters;
	g->r[5] = G_R5;
	g->r[7] = G_R7;
	g->r[8] = G_R8;
}

struct arm_flags {
	int n, z, c, v;
};

static uint32_t arm_add(uint32_t a, uint32_t b, struct arm_flags *f)
{
	uint32_t r = a + b;
	f->n = r >> 31;
	f->z = r == 0;
	f->c = r < a;
	f->v = (int)(((a ^ r) & ~(a ^ b)) >> 31);
	return r;
}

static uint32_t arm_sub(uint32_t a, uint32_t b, struct arm_flags *f)
{
	uint32_t r = a - b;
	f->n = r >> 31;
	f->z = r == 0;
	f->c = a >= b;
	f->v = (int)(((a ^ b) & (a ^ r)) >> 31);
	return r;
}

uint64_t kflags_ref(struct kguest *g)
{
	struct arm_flags f = { 0, 0, 0, 0 };
	uint32_t *r = g->r;
	uint64_t count = 0;

	do {
		r[0] = arm_add(r[0], r[1], &f);
		(void)arm_sub(r[0], r[2], &f); /* CMP */
		count += 3;
		if (!(f.n != f.v)) { /* BLT no tomado */
			r[3] = arm_add(r[3], r[5], &f);
			count++;
		}
		(void)arm_sub(r[3], r[8], &f); /* CMP */
		count += 2;
		if (!(f.c && !f.z)) { /* BHI no tomado */
			r[6] = arm_add(r[6], r[7], &f);
			count++;
		}
		r[4] = arm_sub(r[4], 1, &f);
		count += 2;
	} while (!f.z); /* BNE */
	return count;
}

#if KERNELS_AVAILABLE
#include "kflags_asm.inc"
#else
const char *const kflags_name[KF_COUNT] = { "sin banderas", "(a) completas", "(b) perezosas", "(c) cr0/cr1/cr2" };
const kflags_fn kflags_table[KF_COUNT] = { 0 };
#endif
