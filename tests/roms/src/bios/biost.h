/*
 * SalsaDS — ROM bios: pruebas comunes del ARM9 y el ARM7 de las llamadas SWI de la BIOS (llamadas a mano, no por
 * las funciones por software de libnds). Quien la incluya define IS_ARM9 y BUF (64 KB de memoria propia).
 *
 * Los resultados esperados salen de GBATEK y de los datos que genera tools/gen-bios-vectors.py desde la
 * descripción de los formatos, no de ningún emulador.
 */
#ifndef BIOST_H
#define BIOST_H

#include <nds.h>
#include <string.h>

#include "../common/hw.h"
#include "vectors.h"

extern uint32_t probe_div(void), probe_sqrt(void), probe_waitloop(void), probe_isdebugger(void), probe_halt(void);
extern uint32_t probe_cpuset(uint32_t src, uint32_t dst, uint32_t cnt);

static uint8_t *const OUT = (uint8_t *)BUF;
static uint8_t *const HUFF_TMP = (uint8_t *)(BUF + 0x6000); /* 0x200 bytes alineados para la SWI 0x13 */
static uint8_t *const TMP = (uint8_t *)(BUF + 0x2000);

static inline uint32_t rd16le_(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static inline void wr16le_(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32le_(uint8_t *p, uint32_t v) { wr16le_(p, v); wr16le_(p + 2, v >> 16); }

#define SWI3(num, a, b, c) \
	__asm__ volatile("mov r0, %0\n\tmov r1, %1\n\tmov r2, %2\n\tswi " #num : : "r"(a), "r"(b), "r"(c) : "r0", "r1", "r2", "r3", "memory", "cc")

static uint32_t swi4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t *ret_r3, int which)
{
	register uint32_t r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c, r3 __asm__("r3") = d;

	switch (which) {
	case 0x09: __asm__ volatile("swi 0x090000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x0D: __asm__ volatile("swi 0x0D0000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x0E: __asm__ volatile("swi 0x0E0000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x10: __asm__ volatile("swi 0x100000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x11: __asm__ volatile("swi 0x110000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x12: __asm__ volatile("swi 0x120000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x13: __asm__ volatile("swi 0x130000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x14: __asm__ volatile("swi 0x140000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x15: __asm__ volatile("swi 0x150000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x16: __asm__ volatile("swi 0x160000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x17: __asm__ volatile("swi 0x170000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	case 0x18: __asm__ volatile("swi 0x180000" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "cc"); break;
	}
	if (ret_r3)
		*ret_r3 = r3;
	(void)r1;
	(void)r2;
	return r0;
}

/* Div: devuelve el cociente; resto y |cociente| por referencia */
static int32_t sdiv(int32_t n, int32_t d, int32_t *rem, uint32_t *absq)
{
	register uint32_t r0 __asm__("r0") = (uint32_t)n, r1 __asm__("r1") = (uint32_t)d, r3 __asm__("r3");

	__asm__ volatile("swi 0x090000" : "+r"(r0), "+r"(r1), "=r"(r3) : : "r2", "memory", "cc");
	*rem = (int32_t)r1;
	*absq = r3;
	return (int32_t)r0;
}

static uint32_t crc16_ref(uint32_t crc, const uint8_t *p, uint32_t n)
{
	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001u : crc >> 1;
	}
	return crc;
}

static uint32_t isqrt_ref(uint32_t x)
{
	uint32_t r = 0;

	for (int b = 15; b >= 0; b--) {
		const uint32_t t = r | (1u << b);

		if ((uint64_t)t * t <= x)
			r = t;
	}
	return r;
}

static void guard_clear(uint8_t *p, uint32_t n)
{
	memset(p, 0xCC, n + 16);
}

static uint32_t guard_ok(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < 16; i++)
		if (p[n + i] != 0xCC)
			return 0;
	return 1;
}

/* callbacks de lectura (Open, Close, Get8, Get16, Get32) para los descompresores por callback */
static uint32_t cb_open(const uint8_t *src, uint8_t *dst, uint32_t param)
{
	(void)dst;
	(void)param;
	return *(const uint32_t *)(const void *)src;
}
static uint32_t cb_close(void) { return 0; } /* GBATEK: la BIOS puede llamar a Close al terminar; con 0 saltaría a la dirección 0 */
static uint32_t cb_get8(const uint8_t *src) { return *src; }
static uint32_t cb_get16(const uint8_t *src) { return *(const uint16_t *)(const void *)src; }
static uint32_t cb_get32(const uint8_t *src) { return *(const uint32_t *)(const void *)src; }
static const uint32_t cb_table[5] = { (uint32_t)(uintptr_t)cb_open, (uint32_t)(uintptr_t)cb_close, (uint32_t)(uintptr_t)cb_get8,
				      (uint32_t)(uintptr_t)cb_get16, (uint32_t)(uintptr_t)cb_get32 };

static volatile uint32_t n_tm;
static void h_tm0(void) { n_tm++; }

static void group_math(void)
{
	static const int32_t nums[][2] = { { -1234, 10 }, { 1000, 7 }, { 7, -2 }, { -7, -2 }, { 0x7FFFFFFF, 2 },
					   { 0x7FFFFFFF, -1 }, { 0, 5 }, { 123456789, 1000 }, { -123456789, 1000 }, { 5, 5 },
					   { 3, 7 }, { INT32_MIN + 1, 1 } }; /* con INT_MIN la BIOS libre de melonDS 1.1 no termina */
	int32_t rem;
	uint32_t absq, bad = 0, bad_a = 0;

	sds_group("DIV/SQRT");
	for (unsigned i = 0; i < sizeof nums / sizeof nums[0]; i++) {
		const int32_t q = sdiv(nums[i][0], nums[i][1], &rem, &absq);

		bad += q != nums[i][0] / nums[i][1];
		/* el resto lleva el signo del numerador (GBATEK: -1234 / 10 da -4); la BIOS libre de melonDS 1.1 devuelve
		 * su valor absoluto con numerador negativo (informativo) */
		if (rem != nums[i][0] % nums[i][1])
			SDS_DIVERGE_ID(rem, nums[i][0] % nums[i][1], 7000 + i);
		bad_a += absq != (uint32_t)(q < 0 ? -(int64_t)q : (int64_t)q);
	}
	SDS_CHECK(bad, 0);
	SDS_CHECK(bad_a, 0);
	{
		static const uint32_t xs[] = { 0, 1, 2, 3, 4, 8, 9, 15, 16, 99, 100, 1000, 65535, 65536, 12345678, 0x40000000u,
					       0x7FFFFFFFu, 0xFFFE0001u, 0xFFFFFFFFu };
		uint32_t b = 0;

		for (unsigned i = 0; i < sizeof xs / 4; i++) {
			const uint32_t r = swi4(xs[i], 0, 0, 0, NULL, 0x0D);

			/* la BIOS libre de melonDS 1.1 da 0 con los números de 32 bits más grandes (informativo) */
			if (xs[i] >= 0xFFFE0000u) {
				if (r != isqrt_ref(xs[i]))
					SDS_DIVERGE_ID(r, isqrt_ref(xs[i]), 7200 + i);
			} else {
				b += r != isqrt_ref(xs[i]);
			}
		}
		SDS_CHECK(b, 0);
	}
	{
		uint32_t r3 = 0;
		const uint32_t crc = swi4(0xFFFF, (uint32_t)(uintptr_t)v_plain, 316, 0, &r3, 0x0E);

		SDS_CHECK(crc, crc16_ref(0xFFFF, v_plain, 316));
		SDS_CHECK(r3, rd16le_((const uint8_t *)v_plain + 314));
		SDS_CHECK(swi4(0x1234, (uint32_t)(uintptr_t)v_plain, 100, 0, NULL, 0x0E), crc16_ref(0x1234, v_plain, 100));
		SDS_CHECK(swi4(0xFFFF, (uint32_t)(uintptr_t)v_plain, 2, 0, NULL, 0x0E), crc16_ref(0xFFFF, v_plain, 2));
	}
	SDS_CHECK(swi4(0, 0, 0, 0, NULL, 0x0F) == 0 ? 0 : 1, 0); /* no es un equipo de desarrollo */
}

static void group_copy(void)
{
	uint32_t i, bad;
	uint16_t *h = (uint16_t *)(void *)OUT;
	uint32_t *w = (uint32_t *)(void *)OUT;
	uint32_t *src = (uint32_t *)(void *)(BUF + 0x4000);

	sds_group("CPUSET");
	for (i = 0; i < 64; i++)
		src[i] = 0x10000000u * (i & 7) + i * 0x01010101u + 0x0C0DE000u;
	/* CpuSet 16 bits: 20 medias palabras */
	guard_clear(OUT, 40);
	SWI3(0x0B0000, src, OUT, 20);
	bad = 0;
	for (i = 0; i < 20; i++)
		bad += h[i] != ((uint16_t *)(void *)src)[i];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 40), 1);
	/* CpuSet 32 bits: 13 palabras */
	guard_clear(OUT, 52);
	SWI3(0x0B0000, src, OUT, (1u << 26) | 13);
	bad = 0;
	for (i = 0; i < 13; i++)
		bad += w[i] != src[i];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 52), 1);
	/* relleno de 16 y de 32 bits */
	guard_clear(OUT, 30);
	SWI3(0x0B0000, src, OUT, (1u << 24) | 15);
	bad = 0;
	for (i = 0; i < 15; i++)
		bad += h[i] != ((uint16_t *)(void *)src)[0];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 30), 1);
	guard_clear(OUT, 40);
	SWI3(0x0B0000, src, OUT, (1u << 24) | (1u << 26) | 10);
	bad = 0;
	for (i = 0; i < 10; i++)
		bad += w[i] != src[0];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 40), 1);
	/* CpuFastSet: 24 palabras (3 bloques de 8) y 13 (en la DS cualquier cuenta) */
	guard_clear(OUT, 96);
	SWI3(0x0C0000, src, OUT, 24);
	bad = 0;
	for (i = 0; i < 24; i++)
		bad += w[i] != src[i];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 96), 1);
	guard_clear(OUT, 52);
	SWI3(0x0C0000, src, OUT, 13);
	bad = 0;
	for (i = 0; i < 13; i++)
		bad += w[i] != src[i];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 52), 1);
	guard_clear(OUT, 64);
	SWI3(0x0C0000, src, OUT, (1u << 24) | 16);
	bad = 0;
	for (i = 0; i < 16; i++)
		bad += w[i] != src[0];
	SDS_CHECK(bad, 0);
	SDS_CHECK(guard_ok(OUT, 64), 1);
}

static uint32_t same(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	return memcmp(a, b, n) == 0;
}



/* Si la BIOS no escribió nada, no implementa la función (la BIOS libre de melonDS 1.1 no tiene la SWI 0x13): se
 * anota como divergencia en vez de fallo. Si escribió, tiene que ser correcto. */
static void huff_check(const uint8_t *out, uint32_t n)
{
	if (out[0] == 0xCC && out[1] == 0xCC) {
		SDS_DIVERGE(0, 1);
		return;
	}
	SDS_CHECK(same(out, v_plain, n), 1);
	SDS_CHECK(guard_ok(out, n), 1);
}

static void group_decomp(void)
{
	static const struct {
		uint32_t unit;
		const uint8_t *src;
		uint32_t dw, sw, off, zero, nbytes;
		const uint8_t *expect;
	} bu[] = {
		{ 0, v_bu_src, 8, 2, 0x10, 0, 32, v_bu_2to8 },
		{ 0, v_bu_src, 8, 2, 0x10, 1, 32, v_bu_2to8z },
		{ 0, v_bu_src, 16, 4, 0x100, 0, 32, v_bu_4to16 },
		{ 0, v_bu_src, 4, 1, 1, 0, 32, v_bu_1to4 },
	};
	uint8_t info[8] __attribute__((aligned(4)));

	sds_group("DESCOMPRIME");
	/* LZ77 desde memoria, escritura de 8 bits */
	guard_clear(OUT, 316);
	sds_progress("SWI 0x11");
	swi4((uint32_t)(uintptr_t)v_lz77, (uint32_t)(uintptr_t)OUT, 0, 0, NULL, 0x11);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
	/* LZ77 por callbacks (escritura de 16 bits) */
	guard_clear(OUT, 316);
	sds_progress("SWI 0x12");
	swi4((uint32_t)(uintptr_t)v_lz77, (uint32_t)(uintptr_t)OUT, 0, (uint32_t)(uintptr_t)cb_table, NULL, 0x12);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
	/* RLE desde memoria y por callbacks */
	guard_clear(OUT, 316);
	sds_progress("SWI 0x14");
	swi4((uint32_t)(uintptr_t)v_rle, (uint32_t)(uintptr_t)OUT, 0, 0, NULL, 0x14);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
	guard_clear(OUT, 316);
	sds_progress("SWI 0x15");
	swi4((uint32_t)(uintptr_t)v_rle, (uint32_t)(uintptr_t)OUT, 0, (uint32_t)(uintptr_t)cb_table, NULL, 0x15);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
#ifdef BIOS_SWI13
	/* PENDIENTE (la ROM bios no la llama; se prueba aparte en bios13): en una DS real la SWI 0x13 por callbacks no volvía.
	 * Hipótesis: la BIOS usa r2 como búfer temporal de 0x200 bytes para el árbol (con r2 = 0 escribía en la
	 * dirección 0) y llama a Close (antes 0). Aquí r2 apunta a un búfer y Close es una función vacía. */
	/* Huffman de 8 y de 4 bits (por callbacks; r2 llega al callback de apertura: libnds pasa 0. GBATEK habla de un
	 * búfer temporal en r2, pero la BIOS libre de melonDS no escribe con él y libnds no lo usa) */
	guard_clear(OUT, 316);
	sds_progress("SWI 0x13");
	swi4((uint32_t)(uintptr_t)v_huff8, (uint32_t)(uintptr_t)OUT, (uint32_t)(uintptr_t)HUFF_TMP, (uint32_t)(uintptr_t)cb_table, NULL, 0x13);
	huff_check(OUT, 316);
	guard_clear(OUT, 316);
	sds_progress("SWI 0x13");
	swi4((uint32_t)(uintptr_t)v_huff4, (uint32_t)(uintptr_t)OUT, (uint32_t)(uintptr_t)HUFF_TMP, (uint32_t)(uintptr_t)cb_table, NULL, 0x13);
	huff_check(OUT, 316);
#endif
	/* BitUnPack */
	for (unsigned k = 0; k < sizeof bu / sizeof bu[0]; k++) {
		wr16le_(info, 8);
		info[2] = (uint8_t)bu[k].sw;
		info[3] = (uint8_t)bu[k].dw;
		wr32le_(info + 4, bu[k].off | (bu[k].zero << 31));
		sds_progress("SWI 0x10 BitUnPack");
		guard_clear(OUT, bu[k].nbytes);
		swi4((uint32_t)(uintptr_t)bu[k].src, (uint32_t)(uintptr_t)OUT, (uint32_t)(uintptr_t)info, 0, NULL, 0x10);
		SDS_CHECK_ID(same(OUT, bu[k].expect, bu[k].nbytes), 1, 9000 + k);
		SDS_CHECK_ID(guard_ok(OUT, bu[k].nbytes), 1, 9100 + k);
	}
#if IS_ARM9
	/* filtros de diferencias (solo ARM9) */
	guard_clear(OUT, 316);
	sds_progress("SWI 0x16");
	swi4((uint32_t)(uintptr_t)v_diff8, (uint32_t)(uintptr_t)OUT, 0, 0, NULL, 0x16);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
	guard_clear(OUT, 316);
#ifdef BIOS_SWI17
	/* La SWI 0x17 (Diff8bitUnFilterVram) es de la GBA y no figura en la BIOS de la DS: en una DS real una SWI sin
	 * implementar salta fuera de la BIOS y la ROM cae en la excepción de libnds. Solo se llama si se construye con
	 * -DBIOS_SWI17 (para comparar emuladores entre sí); las ROMs que van a la DS no la llaman. */
	sds_progress("SWI 0x17");
	swi4((uint32_t)(uintptr_t)v_diff8, (uint32_t)(uintptr_t)OUT, 0, 0, NULL, 0x17);
	SDS_DIVERGE(same(OUT, v_plain, 316), 1);
#endif
	guard_clear(OUT, 316);
	sds_progress("SWI 0x18");
	swi4((uint32_t)(uintptr_t)v_diff16, (uint32_t)(uintptr_t)OUT, 0, 0, NULL, 0x18);
	SDS_CHECK(same(OUT, v_plain, 316), 1);
	SDS_CHECK(guard_ok(OUT, 316), 1);
#endif
}

/* La BIOS real conserva r4-r11 en todas las SWI. La BIOS libre de melonDS 1.1 no lo hace en alguna (informativo). */
static void group_regs(void)
{
	uint32_t m_div, m_sqrt, m_wait, m_dbg, m_halt, m_cpu;
	uint32_t *src = (uint32_t *)(void *)(BUF + 0x4000);

	sds_group("REGISTROS");
	m_div = probe_div();
	m_sqrt = probe_sqrt();
	m_wait = probe_waitloop();
	m_dbg = probe_isdebugger();
	m_cpu = probe_cpuset((uint32_t)(uintptr_t)src, (uint32_t)(uintptr_t)OUT, (1u << 24) | (1u << 26) | 4);
	SDS_CHECK(m_div, 0);
	SDS_CHECK(m_sqrt, 0);
	SDS_CHECK(m_wait, 0);
	SDS_CHECK(m_dbg, 0);
	SDS_CHECK(m_cpu, 0);
	/* Halt: lo despierta una IRQ de temporizador (IME = 1 y manejador de libnds) */
	irqSet(IRQ_TIMER0, h_tm0);
	MEM16(R_TM_H(0)) = 0;
	MEM32(R_IF) = IRQ_TIMER0;
	irqEnable(IRQ_TIMER0);
	MEM32(R_IME) = 1;
	n_tm = 0;
	MEM16(R_TM_L(0)) = 0xF000;
	MEM16(R_TM_H(0)) = 0xC0;
	m_halt = probe_halt();
	MEM16(R_TM_H(0)) = 0;
	irqDisable(IRQ_TIMER0);
	SDS_CHECK(n_tm != 0, 1);
	SDS_CHECK(m_halt & 0xFF, 0); /* r4-r11 se conservan */
#if !IS_ARM9
	/* ARM7: la SWI del Halt conserva todos los registros (GBATEK). La BIOS libre de melonDS 1.1 destruye r0 y r2 (y libnds7
	 * llama la SWI sin declarar clobbers, así que el compilador deja valores vivos en ellos): informativo. En el ARM9
	 * (CP15) GBATEK dice que r0 se destruye y no se comprueba. */
	SDS_DIVERGE_ID(m_halt >> 8, 0, 5998);

	/* el ARM7 con IME = 0: el HALT termina por IE & IF sin entregar la IRQ */
	{
		const uint32_t ie = MEM32(R_IE);

		MEM32(R_IME) = 0;
		MEM32(R_IE) = IRQ_TIMER0;
		MEM16(R_TM_H(0)) = 0;
		MEM32(R_IF) = IRQ_TIMER0;
		MEM16(R_TM_L(0)) = 0xF000;
		MEM16(R_TM_H(0)) = 0xC0;
		m_halt = probe_halt();
		MEM16(R_TM_H(0)) = 0;
		MEM32(R_IF) = IRQ_TIMER0;
		MEM32(R_IE) = ie;
		MEM32(R_IME) = 1;
		SDS_DIVERGE_ID(m_halt >> 8, 0, 6000);
	}
#endif
}

#endif
