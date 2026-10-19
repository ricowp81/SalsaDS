/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — pruebas del intérprete de CPU: registros de estado y modos con
 * banco, excepciones, interrupciones, HALT, ganchos de HLE, CP15 y las dos
 * TCM de la ARM9.
 */
#include "../roms/src/cpu9/model.h"
#include "acore_tm.h"

static struct tm T;

#define R(n) T.c.r[n]

/* Ejecuta MSR CPSR_c, #imm8 (mantiene el estado ARM). */
static void to_mode(uint32_t cpsr_c)
{
	tm_exec(&T, a_msr_i(0, 1, cpsr_c & 0xFF, 0));
}

/* ---- MRS / MSR y registros con banco --------------------------------------------------------- */

static void test_psr(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	/* MRS lee el CPSR */
	tm_soft(&T);
	tm_set_flags(&T, 0xA);
	tm_exec(&T, a_mrs(0, 0));
	CHECK_EQ(R(0), 0xA0000000u | ACORE_MODE_SYS | ACORE_I | ACORE_F);
	/* MSR CPSR_f con inmediato: solo las banderas (en la ARM7 el bit 27 no existe) */
	tm_soft(&T);
	tm_exec(&T, a_msr_i(0, 8, 0xFF, 4)); /* 0xFF ror 8 = 0xFF000000 */
	CHECK_EQ(T.c.cpsr & 0xFF000000u, v5 ? 0xF8000000u : 0xF0000000u);
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	/* MSR con registro y máscara de control: cambia el modo y las máscaras I y F */
	tm_soft(&T);
	R(1) = ACORE_MODE_SVC;
	tm_exec(&T, a_msr_r(0, 1, 1));
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_SVC);
	/* MSR no puede cambiar el bit T */
	tm_soft(&T);
	tm_exec(&T, a_msr_i(0, 1, 0x3F, 0));
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(T.c.cpsr & 0x1F, 0x1F);
	/* un modo inválido se ignora */
	tm_soft(&T);
	tm_exec(&T, a_msr_i(0, 1, 0x00, 0));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	tm_exec(&T, a_msr_i(0, 1, 0x14, 0));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	/* los bits reservados (26..8) no se guardan */
	tm_soft(&T);
	R(1) = 0x07FFFF00u | ACORE_MODE_SYS;
	tm_exec(&T, a_msr_r(0, 15, 1));
	CHECK_EQ(T.c.cpsr & 0x07FFFF00u, 0);

	/* modo usuario: solo cambian las banderas; ni el modo ni I */
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_USR);
	R(1) = 0xF0000000u | ACORE_MODE_SVC;
	tm_exec(&T, a_msr_r(0, 9, 1));
	CHECK_EQ(T.c.cpsr & 0xF0000000u, 0xF0000000u);
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_USR);
	tm_exec(&T, a_msr_r(0, 1, 1)); /* solo el campo de control */
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_USR);
	/* MSR a un SPSR que no existe (usuario/sistema) no hace nada; MRS SPSR devuelve el CPSR */
	tm_soft(&T);
	R(1) = 0x12345678u;
	tm_exec(&T, a_msr_r(1, 15, 1));
	tm_exec(&T, a_mrs(1, 2));
	CHECK_EQ(R(2), T.c.cpsr);

	/* SPSR de un modo con SPSR: escritura y lectura, y cada modo tiene el suyo */
	tm_soft(&T);
	to_mode(ACORE_MODE_SVC);
	R(1) = 0xB0000000u | ACORE_MODE_USR | ACORE_T;
	tm_exec(&T, a_msr_r(1, 9, 1)); /* SPSR_fc */
	tm_exec(&T, a_mrs(1, 2));
	CHECK_EQ(R(2), 0xB0000000u | ACORE_MODE_USR | ACORE_T);
	to_mode(ACORE_MODE_IRQ);
	tm_exec(&T, a_mrs(1, 3));
	CHECK_EQ(R(3), 0); /* el SPSR de IRQ aún no se escribió */
	to_mode(ACORE_MODE_SVC);
	tm_exec(&T, a_mrs(1, 4));
	CHECK_EQ(R(4), 0xB0000000u | ACORE_MODE_USR | ACORE_T);
}

static void test_banks(enum acore_kind kind)
{
	tm_init(&T, kind);
	tm_soft(&T);
	R(8) = 0x88;
	R(10) = 0xAA;
	R(13) = 0x1313;
	R(14) = 0x1414;
	to_mode(ACORE_MODE_IRQ | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0);
	CHECK_EQ(R(14), 0);
	CHECK_EQ(R(8), 0x88); /* r8-r12 se comparten fuera de FIQ */
	R(13) = 0x2323;
	R(14) = 0x2424;
	to_mode(ACORE_MODE_SVC | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0);
	R(13) = 0x3333;
	to_mode(ACORE_MODE_FIQ | ACORE_I | ACORE_F);
	CHECK_EQ(R(8), 0); /* FIQ tiene r8-r14 propios */
	CHECK_EQ(R(10), 0);
	R(8) = 0xF8;
	R(10) = 0xFA;
	R(13) = 0xFD;
	to_mode(ACORE_MODE_ABT | ACORE_I | ACORE_F);
	CHECK_EQ(R(8), 0x88);
	CHECK_EQ(R(10), 0xAA);
	to_mode(ACORE_MODE_UND | ACORE_I | ACORE_F);
	to_mode(ACORE_MODE_SYS | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0x1313);
	CHECK_EQ(R(14), 0x1414);
	CHECK_EQ(R(8), 0x88);
	to_mode(ACORE_MODE_IRQ | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0x2323);
	CHECK_EQ(R(14), 0x2424);
	to_mode(ACORE_MODE_FIQ | ACORE_I | ACORE_F);
	CHECK_EQ(R(8), 0xF8);
	CHECK_EQ(R(10), 0xFA);
	CHECK_EQ(R(13), 0xFD);
	to_mode(ACORE_MODE_SVC | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0x3333);
	/* usuario y sistema comparten registros */
	to_mode(ACORE_MODE_USR | ACORE_I | ACORE_F);
	CHECK_EQ(R(13), 0x1313);
	CHECK_EQ(R(14), 0x1414);
	CHECK_EQ(acore_get_user_reg(&T.c, 13), 0x1313);
}

/* ---- excepciones ------------------------------------------------------------------------------------ */

struct exc_expect {
	uint32_t mode, lr, vec;
};

static void check_exc(const struct exc_expect *e, uint32_t old_cpsr)
{
	CHECK_EQ(T.c.cpsr & 0x1F, e->mode);
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK(T.c.cpsr & ACORE_I);
	CHECK_EQ(R(14), e->lr);
	CHECK_EQ(T.c.spsr_bank[T.c.bank], old_cpsr);
	CHECK_EQ(acore_pc(&T.c), e->vec);
}

static void test_exceptions(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;
	uint32_t old;

	tm_init(&T, kind);
	for (int thumb = 0; thumb < 2; thumb++) {
		const uint32_t t = thumb ? 2u : 4u; /* tamaño de la instrucción */
		const struct exc_expect swi = { ACORE_MODE_SVC, TM_CODE + t, 0x08 };
		const struct exc_expect und = { ACORE_MODE_UND, TM_CODE + t, 0x04 };
		const struct exc_expect pabt = { ACORE_MODE_ABT, TM_CODE + 4, 0x0C };
		const struct exc_expect dabt = { ACORE_MODE_ABT, TM_CODE + 8, 0x10 };

		/* cada excepción guarda el CPSR en su SPSR y entra en estado ARM con I puesto */
		tm_soft(&T);
		T.c.cpsr &= ~(ACORE_I | ACORE_F);
		tm_set_flags(&T, 0x5);
		old = T.c.cpsr | (thumb ? ACORE_T : 0);
		thumb ? tm_exec16(&T, t_swi(5)) : tm_exec(&T, a_swi(5));
		check_exc(&swi, old);
		CHECK_EQ(T.c.cpsr & ACORE_F, 0); /* F no se toca */

		tm_soft(&T);
		old = T.c.cpsr | (thumb ? ACORE_T : 0);
		thumb ? tm_exec16(&T, 0xDE00) : tm_exec(&T, A_UNDEF);
		check_exc(&und, old);

		if (v5) {
			tm_soft(&T);
			old = T.c.cpsr | (thumb ? ACORE_T : 0);
			thumb ? tm_exec16(&T, t_bkpt(3)) : tm_exec(&T, 0xE1200073u);
			check_exc(&pabt, old);
		}

		tm_soft(&T);
		R(1) = TM_DATA;
		T.abort_lo = TM_DATA;
		T.abort_hi = TM_DATA + 16;
		old = T.c.cpsr | (thumb ? ACORE_T : 0);
		thumb ? tm_exec16(&T, t_ls_i(1, 0, 1, 0)) : tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 0, 0));
		check_exc(&dabt, old);
	}

	/* IRQ y FIQ entre instrucciones; la tomada es la que habría ejecutado la CPU: LR = A + 4 */
	for (int thumb = 0; thumb < 2; thumb++) {
		tm_soft(&T);
		T.c.cpsr &= ~(ACORE_I | ACORE_F);
		if (thumb) {
			T.c.cpsr |= ACORE_T;
			acore_set_pc(&T.c, TM_CODE + 2);
		} else {
			acore_set_pc(&T.c, TM_CODE + 4);
		}
		old = T.c.cpsr;
		acore_set_irq(&T.c, 1);
		acore_step(&T.c);
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_IRQ);
		CHECK_EQ(T.c.cpsr & (ACORE_I | ACORE_F | ACORE_T), ACORE_I);
		CHECK_EQ(R(14), (thumb ? TM_CODE + 2 : TM_CODE + 4) + 4);
		CHECK_EQ(T.c.spsr_bank[T.c.bank], old);
		CHECK_EQ(acore_pc(&T.c), 0x18);
		CHECK_EQ(T.c.icount, 0); /* la entrada no es una instrucción del programa */

		tm_soft(&T);
		T.c.cpsr &= ~(ACORE_I | ACORE_F);
		if (thumb) {
			T.c.cpsr |= ACORE_T;
			acore_set_pc(&T.c, TM_CODE + 2);
		}
		old = T.c.cpsr;
		acore_set_fiq(&T.c, 1);
		acore_step(&T.c);
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_FIQ);
		CHECK_EQ(T.c.cpsr & (ACORE_I | ACORE_F | ACORE_T), ACORE_I | ACORE_F);
		CHECK_EQ(R(14), (thumb ? TM_CODE + 2 : TM_CODE) + 4);
		CHECK_EQ(T.c.spsr_bank[T.c.bank], old);
		CHECK_EQ(acore_pc(&T.c), 0x1C);
	}
	/* enmascaradas no se toman */
	tm_soft(&T);
	tm_code(&T, 0, a_mov_i(0, 7));
	acore_set_irq(&T.c, 1);
	acore_set_fiq(&T.c, 1);
	acore_step(&T.c);
	CHECK_EQ(R(0), 7);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	/* FIQ con I puesto y F limpio gana a IRQ */
	tm_soft(&T);
	T.c.cpsr &= ~ACORE_F;
	acore_set_irq(&T.c, 1);
	acore_set_fiq(&T.c, 1);
	acore_step(&T.c);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_FIQ);

	/* reinicio: modo SVC, ARM, I y F puestos */
	tm_soft(&T);
	acore_reset(&T.c);
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_SVC | ACORE_I | ACORE_F);
	CHECK_EQ(acore_pc(&T.c), v5 ? 0xFFFF0000u : 0u);
}

/* Un programa entero: una IRQ en medio, con su manejador y la vuelta SUBS pc, lr, #4. */
static void test_irq_flow(int thumb)
{
	tm_init(&T, ACORE_ARM7);
	tm_soft(&T);
	T.c.cpsr &= ~ACORE_I;
	tm_put32(&T, 0x18, a_b(0, (int32_t)((0x100 - 0x18 - 8) / 4)));
	tm_put32(&T, 0x100, a_dp_i(OP_ADD, 0, 12, 12, 1, 0));
	tm_put32(&T, 0x104, a_dp_i(OP_SUB, 1, 14, 15, 4, 0));
	if (thumb) {
		T.c.cpsr |= ACORE_T;
		tm_code16(&T, 0, t_imm8(0, 0, 0));
		tm_code16(&T, 1, t_imm8(2, 0, 1));
		tm_code16(&T, 2, t_imm8(2, 0, 1));
		tm_code16(&T, 3, t_imm8(2, 0, 1));
	} else {
		tm_code(&T, 0, a_mov_i(0, 0));
		tm_code(&T, 1, a_dp_i(OP_ADD, 0, 0, 0, 1, 0));
		tm_code(&T, 2, a_dp_i(OP_ADD, 0, 0, 0, 1, 0));
		tm_code(&T, 3, a_dp_i(OP_ADD, 0, 0, 0, 1, 0));
	}
	acore_set_pc(&T.c, TM_CODE);
	acore_step(&T.c); /* mov r0, #0 */
	acore_step(&T.c); /* add r0, #1 */
	acore_set_irq(&T.c, 1);
	acore_step(&T.c); /* entra en la IRQ */
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_IRQ);
	acore_set_irq(&T.c, 0); /* el manejador baja la línea */
	acore_step(&T.c);       /* b handler (el vector) */
	acore_step(&T.c);       /* add r12, r12, #1 */
	acore_step(&T.c);       /* subs pc, lr, #4 */
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	CHECK_EQ(!!(T.c.cpsr & ACORE_T), thumb);
	acore_step(&T.c);
	acore_step(&T.c);
	CHECK_EQ(R(0), 3); /* ni se perdió ni se repitió ninguna instrucción */
	CHECK_EQ(R(12), 1);
}

/* ---- HALT, tiempo y ganchos ---------------------------------------------------------------------------------- */

static void test_halt_and_time(enum acore_kind kind)
{
	tm_init(&T, kind);
	for (int i = 0; i < 16; i++)
		tm_code(&T, (unsigned)i, a_dp_i(OP_ADD, 0, 0, 0, 1, 0));
	/* en HALT el reloj avanza sin ejecutar */
	tm_soft(&T);
	acore_halt(&T.c);
	acore_run(&T.c, 100);
	CHECK_EQ(T.c.cycles, 100);
	CHECK_EQ(T.c.icount, 0);
	CHECK_EQ(R(0), 0);
	/* una IRQ enmascarada despierta pero no se toma: sigue con la instrucción siguiente */
	acore_set_irq(&T.c, 1);
	acore_run(&T.c, 101);
	CHECK_EQ(T.c.halted, 0);
	CHECK_EQ(R(0), 1);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	/* acore_wake despierta sin IRQ: la instrucción siguiente se ejecuta */
	tm_soft(&T);
	acore_halt(&T.c);
	acore_run(&T.c, 50);
	CHECK_EQ(R(0), 0);
	acore_wake(&T.c);
	acore_run(&T.c, 52);
	CHECK(R(0) >= 1);
	CHECK_EQ(T.c.halted, 0);
	/* con la IRQ desenmascarada, despierta y entra */
	tm_soft(&T);
	T.c.cpsr &= ~ACORE_I;
	acore_halt(&T.c);
	acore_set_irq(&T.c, 1);
	acore_step(&T.c);
	CHECK_EQ(T.c.halted, 0);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_IRQ);

	/* acore_run se detiene al llegar al tiempo; las instrucciones cuestan al menos 1 ciclo */
	tm_soft(&T);
	acore_run(&T.c, 10);
	CHECK(T.c.cycles >= 10 && T.c.cycles < 14);
	CHECK_EQ(T.c.icount, T.c.cycles);
	CHECK_EQ(R(0), T.c.icount);
	/* ciclos: salto 3, LDR 3, STR 2, MUL 2.., LDM n + 2 */
	tm_soft(&T);
	tm_exec(&T, a_b(0, 4));
	CHECK_EQ(T.c.cycles, 3);
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(T.c.cycles, 3);
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(T.c.cycles, 2);
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, A_LDMIA(0, 1, 0x1C));
	CHECK_EQ(T.c.cycles, 5);
	/* los callbacks del bus pueden sumar esperas */
	tm_soft(&T);
	T.c.cycles = 7;
	tm_exec(&T, a_mov_i(0, 1));
	CHECK_EQ(T.c.cycles, 8);
}

/* ---- CP15 y las TCM (ARM9) ------------------------------------------------------------------------------------------- */

static void cp15w(unsigned crn, unsigned crm, unsigned op2, uint32_t v)
{
	R(0) = v;
	tm_exec(&T, a_mcr(15, 0, 0, crn, crm, op2));
}

static uint32_t cp15r(unsigned crn, unsigned crm, unsigned op2)
{
	R(0) = 0xA5A5A5A5u;
	tm_exec(&T, a_mrc(15, 0, 0, crn, crm, op2));
	return R(0);
}

static void test_cp15(void)
{
	tm_init(&T, ACORE_ARM9);
	/* identificación */
	CHECK_EQ(cp15r(0, 0, 0), 0x41059461u);
	CHECK_EQ(cp15r(0, 0, 1), 0x0F0D2112u);
	CHECK_EQ(cp15r(0, 0, 2), 0x00140180u);
	/* registro de control: tras un reinicio, vectores altos; solo los bits 0, 2, 7, 12-19 se escriben */
	CHECK_EQ(cp15r(1, 0, 0), 0x00002078u);
	cp15w(1, 0, 0, 0xFFFFFFFFu);
	CHECK_EQ(cp15r(1, 0, 0), 0x000FF0FDu | 0x78u);
	CHECK_EQ(T.c.vector_base, 0xFFFF0000u);
	cp15w(1, 0, 0, 0);
	CHECK_EQ(cp15r(1, 0, 0), 0x78u);
	CHECK_EQ(T.c.vector_base, 0u);
	/* los vectores altos mandan las excepciones a 0xFFFF0000 + offset */
	cp15w(1, 0, 0, 0x2000);
	CHECK_EQ(T.c.vector_base, 0xFFFF0000u);
	tm_exec(&T, a_swi(1));
	CHECK_EQ(acore_pc(&T.c), 0xFFFF0008u);
	/* registros de protección y de caché: se guardan y se leen de vuelta, no hacen nada */
	tm_soft(&T);
	cp15w(2, 0, 0, 0x12);
	CHECK_EQ(cp15r(2, 0, 0), 0x12);
	cp15w(6, 3, 0, 0x04000035u);
	CHECK_EQ(cp15r(6, 3, 0), 0x04000035u);
	cp15w(5, 0, 2, 0x33333333u);
	CHECK_EQ(cp15r(5, 0, 2), 0x33333333u);
	/* operaciones de caché: sin efecto y sin romper nada */
	tm_soft(&T);
	cp15w(7, 5, 0, 0);
	cp15w(7, 10, 4, 0);
	cp15w(7, 6, 1, TM_DATA);
	CHECK_EQ(T.c.halted, 0);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	/* espera de interrupción */
	cp15w(7, 0, 4, 0);
	CHECK_EQ(T.c.halted, 1);
	/* en modo usuario, indefinido */
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_USR);
	tm_exec(&T, a_mrc(15, 0, 0, 0, 0, 0));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	/* MRC a r15 pasa los 4 bits altos a las banderas */
	tm_soft(&T);
	tm_exec(&T, a_mrc(15, 0, 15, 0, 0, 0)); /* 0x41059461: N=0 Z=1 C=0 V=0 */
	CHECK_EQ(tm_flags(&T), 4u);
}

static void test_tcm(void)
{
	tm_init(&T, ACORE_ARM9);
	T.code_base = TM_CODE + 0x100000; /* el código fuera de las regiones de las TCM (la memoria plana se espeja) */
	tm_soft(&T);
	cp15w(1, 0, 0, 0); /* vectores bajos, TCM apagadas */

	/* sin habilitar, la dirección va al bus */
	tm_soft(&T);
	R(1) = 0x100;
	R(2) = 0xAABBCCDDu;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(T.nlog, 1);
	CHECK_EQ(tm_get32(&T, 0x100), 0xAABBCCDDu);

	/* ITCM: 32 KB en 0; las lecturas y escrituras no llegan al bus; el orden es little-endian */
	cp15w(9, 1, 1, 0x0C);       /* tamaño 512 << 6 = 32 KB */
	cp15w(1, 0, 0, 1u << 18);   /* ITCM habilitada */
	memset(T.itcm, 0, sizeof T.itcm);
	tm_soft(&T);
	R(1) = 0x104;
	R(2) = 0x11223344u;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(T.nlog, 0);
	CHECK_EQ(T.itcm[0x104], 0x44);
	CHECK_EQ(T.itcm[0x107], 0x11);
	tm_exec(&T, a_ls_i(1, 1, 1, 1, 0, 1, 3, 1)); /* LDRB r3, [r1, #1] */
	CHECK_EQ(R(3), 0x33);
	CHECK_EQ(T.nlog, 0);
	/* espejo: una región de 64 KB repite los 32 KB; una de 4 KB solo usa los primeros 4 KB */
	cp15w(9, 1, 1, 0x0E);
	tm_soft(&T);
	R(1) = 0x8104;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(R(3), 0x11223344u);
	R(1) = 0x10104;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0)); /* fuera de la región: al bus */
	CHECK_EQ(T.nlog, 1);
	cp15w(9, 1, 1, 0x06);
	tm_soft(&T);
	R(1) = 0x1104;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(T.nlog, 1);
	cp15w(9, 1, 1, 0x0C);
	/* captación de instrucciones desde la ITCM (la memoria del bus tiene otra cosa en esa dirección) */
	wr32le(&T.itcm[0x200], a_mov_i(5, 0x77));
	tm_put32(&T, 0x200, a_mov_i(5, 0x11));
	tm_soft(&T);
	acore_set_pc(&T.c, 0x200);
	acore_step(&T.c);
	CHECK_EQ(R(5), 0x77);

	/* DTCM: 16 KB en 0x00800000; mirror cada 16 KB dentro de la región; la captación no la usa */
	cp15w(9, 1, 0, 0x0080000A);
	CHECK_EQ(cp15r(9, 1, 0), 0x0080000Au);
	cp15w(1, 0, 0, (1u << 18) | (1u << 16));
	memset(T.dtcm, 0, sizeof T.dtcm);
	tm_soft(&T);
	R(1) = 0x00800010;
	R(2) = 0xCAFEF00Du;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(T.nlog, 0);
	CHECK_EQ(rd32le(&T.dtcm[0x10]), 0xCAFEF00Du);
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0x14)); /* [r1, #0x14] */
	CHECK_EQ(T.nlog, 0);
	cp15w(9, 1, 0, 0x00800010 | (7u << 1)); /* región de 64 KB: espejos cada 16 KB */
	tm_soft(&T);
	R(1) = 0x00800010 + 0x4000 * 2;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(T.nlog, 0);
	CHECK_EQ(R(3), 0xCAFEF00Du);
	/* la base se alinea al tamaño de la región */
	cp15w(9, 1, 0, 0x00810000 | (7u << 1));
	CHECK_EQ(T.c.dtcm_base, 0x00810000u);
	cp15w(9, 1, 0, 0x0080A000 | (6u << 1)); /* tamaño 32 KB -> base alineada a 0x00808000 */
	CHECK_EQ(T.c.dtcm_base, 0x00808000u);
	cp15w(9, 1, 0, 0x0080000A);
	/* una instrucción en la DTCM se capta del bus */
	tm_put32(&T, 0x00800000, a_mov_i(6, 0x42));
	tm_soft(&T);
	acore_set_pc(&T.c, 0x00800000);
	acore_step(&T.c);
	CHECK_EQ(R(6), 0x42);

	/* si ITCM y DTCM se solapan, gana la ITCM */
	cp15w(9, 1, 0, 0x0000000A);
	memset(T.itcm, 0, sizeof T.itcm);
	memset(T.dtcm, 0, sizeof T.dtcm);
	tm_soft(&T);
	R(1) = 0x40;
	R(2) = 0x5555AAAAu;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(rd32le(&T.itcm[0x40]), 0x5555AAAAu);
	CHECK_EQ(rd32le(&T.dtcm[0x40]), 0);

	/* modo de carga: las lecturas van al bus y las escrituras a la TCM */
	cp15w(9, 1, 0, 0x0080000A);
	cp15w(1, 0, 0, (1u << 18) | (1u << 19) | (1u << 16) | (1u << 17));
	tm_put32(&T, 0x40, 0x0BADF00Du);
	tm_put32(&T, 0x00800020, 0x0D15EA5Eu);
	memset(T.itcm, 0, sizeof T.itcm);
	memset(T.dtcm, 0, sizeof T.dtcm);
	tm_soft(&T);
	R(1) = 0x40;
	R(2) = 0x600DC0DEu;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(T.nlog, 0);
	CHECK_EQ(rd32le(&T.itcm[0x40]), 0x600DC0DEu);
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(R(3), 0x0BADF00Du);
	CHECK_EQ(T.nlog, 1);
	R(1) = 0x00800020;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(R(3), 0x0D15EA5Eu);
	R(2) = 0x1234;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 2, 0));
	CHECK_EQ(rd32le(&T.dtcm[0x20]), 0x1234);
	/* sin la TCM habilitada en el registro de control, la región no cuenta aunque esté configurada */
	cp15w(1, 0, 0, 0);
	tm_soft(&T);
	R(1) = 0x40;
	T.nlog = 0;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 3, 0));
	CHECK_EQ(T.nlog, 1);

	/* la ARM7 no tiene CP15 ni TCM */
	tm_init(&T, ACORE_ARM7);
	CHECK_EQ(T.c.itcm_rd_len | T.c.dtcm_rd_len, 0);
}

void test_acore_sys(void)
{
	for (int k = 0; k < 2; k++) {
		const enum acore_kind kind = k ? ACORE_ARM9 : ACORE_ARM7;

		test_psr(kind);
		test_banks(kind);
		test_exceptions(kind);
		test_halt_and_time(kind);
	}
	test_irq_flow(0);
	test_irq_flow(1);
	test_cp15();
	test_tcm();
}
