/*
 * SalsaDS — grupos nuevos de la ROM cpuarm: lo que cpu9 no cubre y lo que
 * cambia entre la ARM7 (ARMv4T) y la ARM9 (ARMv5TE). Se compila para las dos
 * CPUs; ARM9 / ARM7 (definidos por la cadena de herramientas) eligen el
 * valor esperado. Los valores esperados están calculados a mano a partir
 * del manual de ARM y de GBATEK y se comprobaron en melonDS.
 */
#ifndef CPUARM_GROUPS2_H
#define CPUARM_GROUPS2_H

#include <stdint.h>
#include <string.h>

#include "../common/sds.h"
#include "cpuarm.h"

#ifdef ARM9
#define V5 1
#else
#define V5 0
#endif
/* valor esperado según la CPU */
#define BY_CPU(v4, v5) (V5 ? (uint32_t)(v5) : (uint32_t)(v4))

/*
 * Casos de borde en que melonDS 1.1 no sigue la documentación (GBATEK, manual de ARM): se comprueban donde el
 * emulador coincide y en los demás solo se anota la divergencia (sds_diverge), que no cuenta como fallo. Los
 * intérpretes de SalsaDS siguen la documentación y lo comprueban en las pruebas unitarias.
 */
#ifdef ARM7
#define CHECK_7DIV(got, want) SDS_DIVERGE(got, want) /* en la ARM7 melonDS no lo hace como la documentación */
#define CHECK_9DIV(got, want) SDS_CHECK(got, want)
#else
#define CHECK_7DIV(got, want) SDS_CHECK(got, want)
#define CHECK_9DIV(got, want) SDS_DIVERGE(got, want)
#endif

static uint8_t tb[64] __attribute__((aligned(32)));
static uint32_t w[16] __attribute__((aligned(32)));

static void fill_tb(void)
{
	volatile uint8_t *p = tb;

	for (int i = 0; i < 64; i++)
		p[i] = (uint8_t)(i + 1);
	p[16] = 0x80;
	p[17] = 0x7F;
	p[18] = 0xFF;
	p[19] = 0x80;
}

static uint32_t tw(int byte_off)
{
	return *(volatile uint32_t *)&tb[byte_off];
}

/* ---- acceso no alineado y formas de carga/almacenamiento de Thumb ---------------------------------- */

struct ldcase {
	th_load_fn fn;
	uint32_t off, want4, want5;
};
#define LDC(fn, off, w4, w5) { fn, off, w4, w5 }
static const struct ldcase ldcases[] = {
	LDC(th_t_ldr_i, 0, 0x100F0E0Du, 0x100F0E0Du),
	LDC(th_t_ldrb_i, 0, 0x0A, 0x0A),
	LDC(th_t_ldrh_i, 0, 0x0C0B, 0x0C0B),
	LDC(th_t_ldr_r, 8, 0x0C0B0A09u, 0x0C0B0A09u),
	LDC(th_t_ldr_r, 1, 0x01040302u, 0x01040302u), /* LDR no alineado: rota 8·(dir & 3) */
	LDC(th_t_ldr_r, 2, 0x02010403u, 0x02010403u),
	LDC(th_t_ldr_r, 3, 0x03020104u, 0x03020104u),
	LDC(th_t_ldrh_r, 2, 0x0403, 0x0403),
	LDC(th_t_ldrh_r, 16, 0x7F80, 0x7F80),
	LDC(th_t_ldrh_r, 18, 0x80FF, 0x80FF),
	LDC(th_t_ldrh_r, 1, 0x01000002u, 0x0201), /* LDRH impar: ARMv4 rota 8, ARMv5 alinea */
	LDC(th_t_ldrh_r, 3, 0x03000004u, 0x0403),
	LDC(th_t_ldrsh_r, 16, 0x7F80, 0x7F80),
	LDC(th_t_ldrsh_r, 18, 0xFFFF80FFu, 0xFFFF80FFu),
	LDC(th_t_ldrsh_r, 17, 0x7F, 0x7F80), /* LDRSH impar: ARMv4 se porta como LDRSB, ARMv5 alinea */
	LDC(th_t_ldrsh_r, 19, 0xFFFFFF80u, 0xFFFF80FFu),
	LDC(th_t_ldrsb_r, 16, 0xFFFFFF80u, 0xFFFFFF80u),
	LDC(th_t_ldrsb_r, 17, 0x7F, 0x7F),
	LDC(th_t_ldrsb_r, 18, 0xFFFFFFFFu, 0xFFFFFFFFu),
	LDC(th_t_ldrsb_r, 19, 0xFFFFFF80u, 0xFFFFFF80u),
	LDC(th_t_ldrb_r, 16, 0x80, 0x80),
	LDC(th_t_ldrb_r, 17, 0x7F, 0x7F),
};

static void group_align(void)
{
	sds_group("ALINEACION");
	fill_tb();

	/* instrucciones ARM */
	SDS_CHECK(op_ldrh(tb + 2), 0x0403);
	CHECK_7DIV(op_ldrh(tb + 1), BY_CPU(0x01000002u, 0x0201));
	CHECK_7DIV(op_ldrh(tb + 3), BY_CPU(0x03000004u, 0x0403));
	SDS_CHECK(op_ldrsh(tb + 16), 0x7F80);
	CHECK_7DIV(op_ldrsh(tb + 17), BY_CPU(0x7F, 0x7F80));
	CHECK_7DIV(op_ldrsh(tb + 19), BY_CPU(0xFFFFFF80u, 0xFFFF80FFu));
	SDS_CHECK(op_ldrsh(tb + 18), 0xFFFF80FFu);
	SDS_CHECK(op_ldrsb(tb + 19), 0xFFFFFF80u);
	for (uint32_t k = 0; k < 4; k++)
		SDS_CHECK(op_ldr(tb + k), 0x04030201u >> (8 * k) | (k ? 0x04030201u << (32 - 8 * k) : 0));

	/* STRH/STR no alineados: se alinea la dirección */
	fill_tb();
	op_strh(tb + 13, 0xFEDC9876u);
	SDS_CHECK(tw(12), 0x100F9876u);
	op_str(tb + 22, 0xA1B2C3D4u);
	SDS_CHECK(tw(20), 0xA1B2C3D4u);
	SDS_CHECK(tw(16), 0x80FF7F80u);

	/* SWP no alineado: lee rotado, escribe alineado y sin rotar */
	fill_tb();
	SDS_CHECK(op_swp(tb + 6, 0x12345678u), 0x06050807u);
	SDS_CHECK(tw(4), 0x12345678u);

	/* LDM/STM con la base desalineada: se ignoran los 2 bits bajos, la base final conserva los bajos */
	fill_tb();
	{
		uint32_t *p = (uint32_t *)((uint8_t *)tb + 6), a, b, c;

		op_ldmia(&p, &a, &b, &c);
		SDS_CHECK(a, 0x08070605u);
		SDS_CHECK(b, 0x0C0B0A09u);
		SDS_CHECK(c, 0x100F0E0Du);
		SDS_CHECK((uint32_t)((uint8_t *)p - tb), 6 + 12);
		p = (uint32_t *)((uint8_t *)tb + 34);
		op_stmia(&p, 0x11, 0x22, 0x33);
		SDS_CHECK(tw(32), 0x11);
		SDS_CHECK(tw(36), 0x22);
		SDS_CHECK(tw(40), 0x33);
	}

	/* formas de Thumb */
	for (unsigned i = 0; i < sizeof ldcases / sizeof ldcases[0]; i++) {
		fill_tb();
		if (ldcases[i].want4 != ldcases[i].want5)
			CHECK_7DIV(ldcases[i].fn(tb, ldcases[i].off), BY_CPU(ldcases[i].want4, ldcases[i].want5));
		else
			SDS_CHECK(ldcases[i].fn(tb, ldcases[i].off), ldcases[i].want4);
	}
	fill_tb();
	th_t_str_r(tb, 33, 0x11223344u); /* STR no alineado */
	SDS_CHECK(tw(32), 0x11223344u);
	SDS_CHECK(tw(28), 0x201F1E1Du);
	th_t_strh_r(tb, 37, 0xABCD1234u); /* STRH impar: va a 36 */
	SDS_CHECK(tw(36), 0x28271234u);
	th_t_strb_r(tb, 41, 0x1FFu); /* STRB: un byte */
	SDS_CHECK(tw(40), 0x2C2BFF29u);
	fill_tb();
	th_t_str_i(tb, 0, 0x55667788u);
	SDS_CHECK(tw(20), 0x55667788u);
	th_t_strh_i(tb, 0, 0x9999u);
	SDS_CHECK(tw(20), 0x99997788u);
	th_t_strb_i(tb, 0, 0x77u);
	SDS_CHECK(tw(24), 0x771B1A19u);
}

/* ---- LDM / STM: la base en la lista, listas vacías, el PC y el banco de usuario ---------------------- */

static void group_ldm_edge(void)
{
	sds_group("LDM EDGE");

	/* STM con la base en la lista y escritura de vuelta */
	memset(w, 0xEE, sizeof w);
	SDS_CHECK(arm_stm_base_notfirst(w), 8);
	SDS_CHECK(w[0], 0x77);
	CHECK_7DIV(w[1], BY_CPU((uint32_t)w + 8, (uint32_t)w)); /* ARMv4: base nueva; ARMv5: la vieja */
	memset(w, 0xEE, sizeof w);
	SDS_CHECK(arm_stm_base_first(w), 8);
	SDS_CHECK(w[0], (uint32_t)w);
	SDS_CHECK(w[1], 0x77);

	/* LDM con la base en la lista y escritura de vuelta */
	w[0] = 0xCAFE0001u;
	w[1] = 0xCAFE0002u;
	SDS_CHECK(arm_ldm_base_first(w), BY_CPU(0xCAFE0001u, (uint32_t)w + 8));
	SDS_CHECK(arm_ldm_base_last(w), 0xCAFE0002u);
	SDS_CHECK(arm_ldm_base_only(w), BY_CPU(0xCAFE0001u, (uint32_t)w + 4));

	/* lista vacía: la base cambia 0x40; ARMv4 transfiere el PC, ARMv5 no transfiere nada */
	{
		uint32_t pcv = 0;

		memset(w, 0xEE, sizeof w);
		SDS_DIVERGE_ID(arm_stm_empty(w, &pcv), 0x40, 9101);
		SDS_DIVERGE_ID(w[0], BY_CPU(pcv + 12, 0xEEEEEEEEu), 9102);
		SDS_DIVERGE_ID(arm_ldm_empty(w), BY_CPU(0x00400002u, 0x00400001u), 9103);
	}

	/* STM/STR con el PC guardan la dirección de la instrucción + 12 (melonDS guarda + 8 con STM) */
	SDS_DIVERGE_ID(arm_stm_pc(w), 12, 9001);
	SDS_CHECK_ID(arm_str_pc(w), 12, 9002);

	/* LDM/STM con ^ desde SVC: el banco de usuario */
	{
		uint32_t ub[6] __attribute__((aligned(8))) = { 0, 0, 0xAAAA5555u, 0x5555AAAAu, 0, 0 };

		arm_user_bank(ub);
		SDS_CHECK(ub[0], 0x13579BDFu);
		SDS_CHECK(ub[1], 0x2468ACE0u);
		SDS_CHECK(ub[4], 0xAAAA5555u);
		SDS_CHECK(ub[5], 0x5555AAAAu);
	}
}

/* ---- modos, registros con banco, SPSR y bits del CPSR ------------------------------------------------ */

#define WR_SPLR(mode, spv, lrv)                                                  \
	do {                                                                     \
		register uint32_t s_ __asm__("r4") = (spv);                      \
		register uint32_t l_ __asm__("r5") = (lrv);                      \
		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #" #mode "\n\t mov sp, r4\n\t mov lr, r5\n\t msr cpsr_c, r3" \
				 : : "r"(s_), "r"(l_) : "r3", "cc", "memory");   \
	} while (0)
#define RD_SPLR(mode, spo, lro)                                                  \
	do {                                                                     \
		register uint32_t s_ __asm__("r4");                              \
		register uint32_t l_ __asm__("r5");                              \
		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #" #mode "\n\t mov r4, sp\n\t mov r5, lr\n\t msr cpsr_c, r3" \
				 : "=r"(s_), "=r"(l_) : : "r3", "cc", "memory"); \
		(spo) = s_;                                                      \
		(lro) = l_;                                                      \
	} while (0)
/* SPSR de un modo con SPSR: escribe y vuelve a leer dentro del modo */
#define SPSR_RW(mode, val, out)                                                  \
	do {                                                                     \
		register uint32_t v_ __asm__("r4") = (val);                      \
		register uint32_t o_ __asm__("r5");                              \
		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #" #mode "\n\t msr spsr_fc, r4\n\t mrs r5, spsr\n\t msr cpsr_c, r3" \
				 : "=r"(o_) : "r"(v_) : "r3", "cc", "memory");  \
		(out) = o_;                                                      \
	} while (0)

static void group_modes(void)
{
	uint32_t sp0, lr0, sp1, lr1, v;

	(void)lr0;

	sds_group("MODOS");
	RD_SPLR(0xDF, sp0, lr0);

	/* r13 y r14 de cada modo son propios: se escriben y se leen de vuelta, y los de sistema no cambian */
	WR_SPLR(0xD1, 0x11110011u, 0x11110022u);
	WR_SPLR(0xD2, 0x22220011u, 0x22220022u);
	WR_SPLR(0xD3, 0x33330011u, 0x33330022u);
	WR_SPLR(0xD7, 0x77770011u, 0x77770022u);
	WR_SPLR(0xDB, 0xBBBB0011u, 0xBBBB0022u);
	RD_SPLR(0xD1, sp1, lr1);
	SDS_CHECK(sp1, 0x11110011u);
	SDS_CHECK(lr1, 0x11110022u);
	RD_SPLR(0xD2, sp1, lr1);
	SDS_CHECK(sp1, 0x22220011u);
	SDS_CHECK(lr1, 0x22220022u);
	RD_SPLR(0xD3, sp1, lr1);
	SDS_CHECK(sp1, 0x33330011u);
	SDS_CHECK(lr1, 0x33330022u);
	RD_SPLR(0xD7, sp1, lr1);
	SDS_CHECK(sp1, 0x77770011u);
	SDS_CHECK(lr1, 0x77770022u);
	RD_SPLR(0xDB, sp1, lr1);
	SDS_CHECK(sp1, 0xBBBB0011u);
	SDS_CHECK(lr1, 0xBBBB0022u);
	RD_SPLR(0xDF, sp1, lr1);
	SDS_CHECK(sp1, sp0);
	/* (el lr de sistema cambia con cada llamada de C: se comprueba dentro de un solo bloque) */
	{
		register uint32_t l0 __asm__("r4"), l1 __asm__("r5");

		__asm__ volatile("mov r4, lr\n\t mrs r3, cpsr\n\t msr cpsr_c, #0xD3\n\t mov lr, #0x55\n\t"
				 "msr cpsr_c, #0xDB\n\t mov lr, #0x66\n\t msr cpsr_c, r3\n\t mov r5, lr"
				 : "=r"(l0), "=r"(l1) : : "r3", "cc", "memory");
		SDS_CHECK(l0, l1);
	}

	/* FIQ tiene además r8-r12 propios; el resto de los modos comparte los de usuario */
	{
		register uint32_t a8 __asm__("r4"), a9 __asm__("r5"), a10 __asm__("r6"), a12 __asm__("r7");

		/* en sistema se ponen valores y en FIQ otros; se lee de vuelta cada uno */
		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #0xDF\n\t"
				 "mov r8, #0x88\n\t mov r9, #0x99\n\t mov r10, #0xAA\n\t mov r12, #0xCC\n\t"
				 "msr cpsr_c, #0xD1\n\t"
				 "mov r8, #0x18\n\t mov r9, #0x19\n\t mov r10, #0x1A\n\t mov r12, #0x1C\n\t"
				 "msr cpsr_c, #0xD2\n\t"
				 "mov r4, r8\n\t mov r5, r9\n\t mov r6, r10\n\t mov r7, r12\n\t" /* los de usuario, vistos desde IRQ */
				 "msr cpsr_c, r3"
				 : "=r"(a8), "=r"(a9), "=r"(a10), "=r"(a12) : : "r3", "r8", "r9", "r10", "r12", "cc", "memory");
		SDS_CHECK(a8, 0x88);
		SDS_CHECK(a9, 0x99);
		SDS_CHECK(a10, 0xAA);
		SDS_CHECK(a12, 0xCC);
		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #0xD1\n\t"
				 "mov r4, r8\n\t mov r5, r9\n\t mov r6, r10\n\t mov r7, r12\n\t" /* los de FIQ */
				 "msr cpsr_c, r3"
				 : "=r"(a8), "=r"(a9), "=r"(a10), "=r"(a12) : : "r3", "cc", "memory");
		SDS_CHECK(a8, 0x18);
		SDS_CHECK(a9, 0x19);
		SDS_CHECK(a10, 0x1A);
		SDS_CHECK(a12, 0x1C);
	}

	/* SPSR: uno por modo con SPSR */
	SPSR_RW(0xD1, 0x50000011u, v);
	SDS_CHECK(v & 0xF00000FFu, 0x50000011u);
	SPSR_RW(0xD2, 0x60000012u, v);
	SDS_CHECK(v & 0xF00000FFu, 0x60000012u);
	SPSR_RW(0xD3, 0x90000013u, v);
	SDS_CHECK(v & 0xF00000FFu, 0x90000013u);
	SPSR_RW(0xD7, 0xA0000017u, v);
	SDS_CHECK(v & 0xF00000FFu, 0xA0000017u);
	SPSR_RW(0xDB, 0xC000001Bu, v);
	SDS_CHECK(v & 0xF00000FFu, 0xC000001Bu);
	{ /* los SPSR no se pisan entre sí: se vuelve a leer el de SVC */
		register uint32_t o_ __asm__("r5");

		__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #0xD3\n\t mrs r5, spsr\n\t msr cpsr_c, r3"
				 : "=r"(o_) : : "r3", "cc", "memory");
		SDS_CHECK(o_ & 0xF00000FFu, 0x90000013u);
	}

	/* bits del CPSR: las banderas N Z C V (y Q en ARMv5); MSR no cambia el bit T */
	__asm__ volatile("msr cpsr_f, #0xF8000000\n\t mrs %0, cpsr\n\t msr cpsr_f, #0" : "=r"(v) : : "cc");
	CHECK_7DIV(v & 0xF8000000u, BY_CPU(0xF0000000u, 0xF8000000u));
	__asm__ volatile("msr cpsr_f, #0xA0000000\n\t mrs %0, cpsr\n\t msr cpsr_f, #0" : "=r"(v) : : "cc");
	SDS_CHECK(v & 0xF8000000u, 0xA0000000u);
#ifdef MSR_T_TEST
	/* Escribir el bit T con MSR es impredecible según el manual de ARM: los emuladores lo ignoran, pero un ARM946E-S real
	 * cambia a Thumb y la ROM ejecutaba basura (excepción de instrucción indefinida en la DS). Solo con -DMSR_T_TEST. */
	__asm__ volatile("mrs r3, cpsr\n\t msr cpsr_c, #0xFF\n\t mrs %0, cpsr\n\t msr cpsr_c, r3" : "=r"(v) : : "r3", "cc");
	SDS_CHECK(v & 0xFF, 0xDF); /* 0xFF pedía T puesto y el modo 0x1F: el modo cambió, T no */
#endif
	__asm__ volatile("msr cpsr_f, #0x10000000\n\t mrs %0, cpsr\n\t msr cpsr_f, #0" : "=r"(v) : : "cc");
	SDS_CHECK(v & 0xF8000000u, 0x10000000u);
}

/* ---- cambio de estado ARM/Thumb: BX, BLX, LDR pc, LDM pc, MOV pc, POP pc ------------------------------------ */

static void group_interwork2(void)
{
	const void *tgt;

	sds_group("INTERWORK");
	/* LDR pc y LDM {pc}: ARMv5 cambia de estado con el bit 0; ARMv4 lo ignora y sigue en ARM */
#ifdef ARM9
	tgt = (const void *)th_ret43; /* la dirección de una función Thumb lleva el bit 0 puesto */
	SDS_CHECK(ldrpc_call(&tgt), 43);
	SDS_CHECK(ldmpc_call(&tgt), 43);
#else
	tgt = (const void *)((uintptr_t)arm_ret42 | 1u);
	SDS_CHECK(ldrpc_call(&tgt), 42);
	SDS_CHECK(ldmpc_call(&tgt), 42);
#endif
	/* sin el bit 0 las dos siguen en ARM */
	tgt = (const void *)arm_ret42;
	SDS_CHECK(ldrpc_call(&tgt), 42);
	SDS_CHECK(ldmpc_call(&tgt), 42);
	/* MOV pc no cambia de estado en ninguna versión: el bit 0 se ignora */
	SDS_CHECK(movpc_call((const void *)((uintptr_t)arm_ret42 | 1u)), 42);
	/* BX de Thumb a ARM */
	SDS_CHECK(th_bx_arm((const void *)arm_ret42), 42);
	/* POP {pc}: ARMv5 cambia de estado con el bit 0 (aquí a ARM); ARMv4 se queda en Thumb */
#ifdef ARM9
	SDS_CHECK(th_poppc((const void *)arm_ret44), 44);
	SDS_CHECK(th_poppc((const void *)th_ret45), 45); /* con el bit 0: sigue en Thumb */
	SDS_CHECK(th_blx_imm(), 42);
	SDS_CHECK(th_blx_reg((const void *)arm_ret44), 44);
#else
	SDS_CHECK(th_poppc((const void *)((uintptr_t)th_ret45 & ~1u)), 45);
	SDS_CHECK(th_poppc((const void *)th_ret45), 45);
#endif
}

/* ---- Thumb: formatos 5 a 19 ---------------------------------------------------------------------------------------- */

static void group_thumb2(void)
{
	static const uint32_t pairs[][2] = { { 0, 0 }, { 1, 2 }, { 0x7FFFFFFF, 1 }, { 0x80000000, 0x80000000 }, { 0xFFFFFFFF, 1 },
					     { 5, 5 }, { 0xFFFFFFFE, 0xFFFFFFFF }, { 3, 0x80000000 } };
	uint32_t stk[8] __attribute__((aligned(8)));
	uint32_t a, b;

	sds_group("THUMB2");
	for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
		a = pairs[i][0];
		b = pairs[i][1];
		SDS_CHECK(th_t_hi_add(a, b), a + b);
		SDS_CHECK(th_t_hi_noflags(a, b), a + b); /* ADD con registros altos no toca las banderas */
		SDS_CHECK(th_t_hi_cmp(a, b), a == b);
		SDS_CHECK(th_t_hi_cmp_lt(a, b), (int32_t)a < (int32_t)b);
		SDS_CHECK(th_t_hi_mov(a), a);
	}
	/* el PC se lee como la dirección + 4; ADD Rd, PC y LDR Rd, [PC] alinean la base a palabra */
	a = (uint32_t)th_t_pcread_a & ~1u;
	SDS_CHECK(th_t_pcread_a(), a + 4);
	b = (uint32_t)th_t_pcread_b & ~1u;
	SDS_CHECK((b & 3), 2);
	SDS_CHECK(th_t_pcread_b(), b + 4);
	a = (uint32_t)th_t_addpc_a & ~1u;
	SDS_CHECK(th_t_addpc_a(), ((a + 4) & ~3u) + 8);
	b = (uint32_t)th_t_addpc_b & ~1u;
	SDS_CHECK(th_t_addpc_b(), ((b + 4) & ~3u) + 8);
	SDS_CHECK(th_t_ldrpc_a(), 0x12345678u);
	SDS_CHECK(th_t_ldrpc_b(), 0x9ABCDEF0u);
	SDS_CHECK(th_t_sp(), 0x0BE0);

	/* PUSH y POP */
	memset(stk, 0, sizeof stk);
	SDS_CHECK(th_t_pushpop(&stk[6]), 0xFFFC1122u);
	SDS_CHECK(stk[3], 0x11);
	SDS_CHECK(stk[4], 0x22);
	SDS_CHECK(stk[5], 0x33);
	SDS_CHECK(th_t_pushlr(), 1);

	/* STMIA y LDMIA */
	memset(stk, 0xEE, sizeof stk);
	SDS_CHECK(th_t_stmia(stk), 12);
	SDS_CHECK(stk[0], 0x11);
	SDS_CHECK(stk[1], 0x22);
	SDS_CHECK(stk[2], 0x33);
	SDS_CHECK(stk[3], 0xEEEEEEEEu);
	SDS_CHECK(th_t_ldmia(stk), 0x0C112233u);
	/* con la base en la lista */
	memset(stk, 0xEE, sizeof stk);
	SDS_CHECK(th_t_stmia_base(stk), 8);
	SDS_CHECK(stk[0], 0x55);
	CHECK_7DIV(stk[1], BY_CPU((uint32_t)stk + 8, (uint32_t)stk));
	stk[0] = 0x7A7A0001u;
	stk[1] = 0x7B7B0002u;
	CHECK_9DIV(th_t_ldmia_base_first(stk), BY_CPU(0x7A7A0001u, (uint32_t)stk + 8));
	SDS_CHECK(th_t_ldmia_base_last(stk), 0x7B7B0002u);

	/* saltos */
	SDS_CHECK(th_t_b_far(), 1);
	SDS_CHECK(th_t_b_back(), 7);
	SDS_CHECK(th_t_bl_far(), 0x1234);
	SDS_CHECK(th_t_bl_lr(), (uint32_t)th_t_bl_lr_next | 1u);
}

#endif
