/*
 * SalsaDS — pruebas del intérprete de CPU: instrucciones ARM de la ARM7
 * (ARMv4T) y la ARM9 (ARMv5TE). Cada grupo ejecuta una instrucción (o pocas)
 * sobre una máquina de pruebas (acore_tm.h) y compara registros, banderas,
 * memoria y los accesos que ve el bus. El modelo de la ALU y del desplazador
 * es el de la ROM cpu9 (tests/roms/src/cpu9/model.h), validado en melonDS.
 */
#include "../roms/src/cpu9/model.h"
#include "acore_tm.h"

static struct tm T;

#define R(n) T.c.r[n]

static const uint32_t vals[] = {
	0x00000000, 0x00000001, 0x00000002, 0x7FFFFFFF, 0x80000000, 0x80000001,
	0xFFFFFFFE, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000,
};
#define NV ((int)(sizeof vals / sizeof vals[0]))

static const struct { unsigned type, amt; } imm_shifts[] = {
	{ LSL, 0 }, { LSL, 1 }, { LSL, 5 }, { LSL, 31 }, { LSR, 0 }, { LSR, 1 }, { LSR, 7 }, { LSR, 31 },
	{ ASR, 0 }, { ASR, 1 }, { ASR, 31 }, { ROR, 0 }, { ROR, 1 }, { ROR, 13 }, { ROR, 31 },
};
#define NIS ((int)(sizeof imm_shifts / sizeof imm_shifts[0]))

static const uint32_t reg_amts[] = { 0, 1, 2, 31, 32, 33, 63, 64, 255, 0x100, 0x101, 0xFFFFFF01 };
#define NRA ((int)(sizeof reg_amts / sizeof reg_amts[0]))

/* ---- proceso de datos ---------------------------------------------------------- */

static void check_dp(unsigned op, unsigned s, uint32_t a, uint32_t op2, uint32_t shc, uint32_t fin, uint32_t insn_check_rd)
{
	/* El resultado ya está en T tras ejecutar: se compara con el modelo. */
	uint32_t wf, want = m_alu((int)op, a, op2, fin, shc, &wf);
	int has_res = !(op >= 8 && op <= 11);

	if (has_res)
		CHECK_EQ(R(insn_check_rd), want);
	else
		CHECK_EQ(R(insn_check_rd), 0xA5A5A5A5u); /* no se escribe */
	CHECK_EQ(tm_flags(&T), s ? wf : fin);
}

static void test_alu(enum acore_kind kind)
{
	tm_init(&T, kind);
	for (unsigned op = 0; op < 16; op++) {
		const int test = op >= 8 && op <= 11;

		for (unsigned s = test ? 1 : 0; s < 2; s++) {
			for (int i = 0; i < NV; i++)
				for (int j = 0; j < NV; j++)
					for (uint32_t f = 0; f < 16; f++) {
						const uint32_t a = vals[i], b = vals[j], cin = (f >> 1) & 1;
						uint32_t shc;

						/* operando 2: registro con desplazamiento inmediato */
						for (int k = 0; k < NIS; k += (i == 0 || j == 0) ? 1 : 3) {
							uint32_t op2 = m_shift_imm((int)imm_shifts[k].type, b, imm_shifts[k].amt, cin, &shc);

							tm_soft(&T);
							R(1) = a;
							R(2) = b;
							R(0) = 0xA5A5A5A5u;
							tm_set_flags(&T, f);
							tm_exec(&T, a_dp_rs(op, s, 1, 0, 2, imm_shifts[k].type, imm_shifts[k].amt));
							check_dp(op, s, a, op2, shc, f, 0);
							CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
						}
						/* operando 2: registro con desplazamiento en un registro */
						for (int k = 0; k < NRA; k += (j == 0 || i == 0) ? 1 : 4)
							for (unsigned type = 0; type < 4; type++) {
								uint32_t op2 = m_shift_reg((int)type, b, reg_amts[k], cin, &shc);

								tm_soft(&T);
								R(1) = a;
								R(2) = b;
								R(3) = reg_amts[k];
								R(0) = 0xA5A5A5A5u;
								tm_set_flags(&T, f);
								tm_exec(&T, a_dp_rr(op, s, 1, 0, 2, type, 3));
								check_dp(op, s, a, op2, shc, f, 0);
							}
					}
			/* operando 2: inmediato rotado */
			for (int i = 0; i < NV; i++)
				for (uint32_t f = 0; f < 16; f++)
					for (unsigned rot = 0; rot < 16; rot += 3)
						for (unsigned imm = 0; imm < 256; imm += 0x55) {
							const uint32_t op2 = m_ror(imm, 2 * rot), shc = rot ? op2 >> 31 : (f >> 1) & 1;

							tm_soft(&T);
							R(1) = vals[i];
							R(0) = 0xA5A5A5A5u;
							tm_set_flags(&T, f);
							tm_exec(&T, a_dp_i(op, s, 1, 0, imm, rot));
							check_dp(op, s, vals[i], op2, shc, f, 0);
						}
		}
	}
}

/* PC como operando y como destino */
static void test_dp_pc(enum acore_kind kind)
{
	tm_init(&T, kind);

	/* ADD r0, pc, #0 lee A + 8 */
	tm_soft(&T);
	tm_exec(&T, a_dp_i(OP_ADD, 0, 15, 0, 0, 0));
	CHECK_EQ(R(0), TM_CODE + 8);
	/* MOV r0, pc con desplazamiento inmediato */
	tm_soft(&T);
	tm_exec(&T, a_mov(0, 15));
	CHECK_EQ(R(0), TM_CODE + 8);
	/* con el desplazamiento en un registro, el PC se lee como A + 12 (en Rn y en Rm) */
	tm_soft(&T);
	R(2) = 0;
	tm_exec(&T, a_dp_rr(OP_ADD, 0, 15, 0, 2, LSL, 2));
	CHECK_EQ(R(0), TM_CODE + 12);
	tm_soft(&T);
	R(2) = 0;
	tm_exec(&T, a_dp_rr(OP_MOV, 0, 0, 0, 15, LSL, 2));
	CHECK_EQ(R(0), TM_CODE + 12);

	/* MOV pc, r1: salta, alineado a palabra, sin cambiar de estado (tampoco en ARMv5) */
	tm_soft(&T);
	R(1) = 0x2003;
	tm_exec(&T, a_mov(15, 1));
	CHECK_EQ(acore_pc(&T.c), 0x2000);
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	/* ADD pc, pc, #4 salta una instrucción */
	tm_soft(&T);
	tm_exec(&T, a_dp_i(OP_ADD, 0, 15, 15, 4, 0));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 12);
	/* MOVS pc, lr desde un modo con SPSR: restaura el CPSR (y el estado Thumb) */
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_SVC | ACORE_I);
	T.c.spsr_bank[T.c.bank] = ACORE_MODE_USR | ACORE_T | ACORE_C;
	R(14) = 0x3001;
	tm_exec(&T, a_dp_rs(OP_MOV, 1, 0, 15, 14, LSL, 0));
	CHECK_EQ(T.c.cpsr, ACORE_MODE_USR | ACORE_T | ACORE_C);
	CHECK_EQ(acore_pc(&T.c), 0x3000);
	/* SUBS pc, lr, #4 (vuelta de IRQ) */
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_IRQ | ACORE_I);
	T.c.spsr_bank[T.c.bank] = ACORE_MODE_SYS;
	R(14) = 0x4008;
	tm_exec(&T, a_dp_i(OP_SUB, 1, 14, 15, 4, 0));
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_SYS);
	CHECK_EQ(acore_pc(&T.c), 0x4004);
	/* MOVS pc en modo sistema (sin SPSR): solo salta */
	tm_soft(&T);
	R(1) = 0x5000;
	tm_exec(&T, a_dp_rs(OP_MOV, 1, 0, 15, 1, LSL, 0));
	CHECK_EQ(T.c.cpsr & 0xFF, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	CHECK_EQ(acore_pc(&T.c), 0x5000);
}

/* ---- condiciones -------------------------------------------------------------------- */

static void test_cond(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	for (unsigned cc = 0; cc < 14; cc++)
		for (uint32_t f = 0; f < 16; f++) {
			tm_soft(&T);
			tm_set_flags(&T, f);
			tm_exec(&T, (cc << 28) | (a_mov_i(0, 1) & 0x0FFFFFFFu));
			CHECK_EQ(R(0), m_cond((int)cc, f) ? 1 : 0);
			CHECK_EQ(acore_pc(&T.c), TM_CODE + 4); /* pase lo que pase, avanza una instrucción */
		}
	/* cond = 1111: ARMv4 «nunca»; ARMv5 es el espacio incondicional (BLX, PLD, el resto indefinido) */
	for (uint32_t f = 0; f < 16; f++) {
		tm_soft(&T);
		tm_set_flags(&T, f);
		tm_exec(&T, 0xF0000000u | (a_mov_i(0, 1) & 0x0FFFFFFFu));
		if (!v5) {
			CHECK_EQ(R(0), 0);
			CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
		} else {
			CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
			CHECK_EQ(R(14), TM_CODE + 4);
		}
	}
	/* BLX con inmediato (solo ARMv5): LR = A + 4, salto relativo a A + 8 + 4·n + 2·H, y pasa a Thumb */
	for (unsigned h = 0; h < 2; h++) {
		tm_soft(&T);
		tm_exec(&T, a_blx_i(3, h));
		if (v5) {
			CHECK_EQ(R(14), TM_CODE + 4);
			CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
			CHECK_EQ(acore_pc(&T.c), TM_CODE + 8 + 12 + 2 * h);
		} else {
			CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
		}
	}
	/* PLD [r1, #4]: no hace nada (ARMv5); en ARMv4 es cond = «nunca» */
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, 0xF5D1F004u);
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
	CHECK_EQ(T.nlog, 0);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
}

/* ---- multiplicaciones --------------------------------------------------------------------- */

static void test_mul(enum acore_kind kind)
{
	tm_init(&T, kind);
	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++)
			for (uint32_t f = 0; f < 16; f += 5) {
				const uint32_t a = vals[i], b = vals[j];
				const uint64_t u = (uint64_t)a * b;
				const int64_t sg = (int64_t)(int32_t)a * (int32_t)b;

				/* MUL / MULS: N y Z del resultado; C y V no cambian */
				for (unsigned s = 0; s < 2; s++) {
					tm_soft(&T);
					R(1) = a;
					R(2) = b;
					tm_set_flags(&T, f);
					tm_exec(&T, a_mul(s, 0, 1, 2));
					CHECK_EQ(R(0), a * b);
					CHECK_EQ(tm_flags(&T), s ? ((a * b) >> 31 ? 8u : 0u) | (a * b == 0 ? 4u : 0u) | (f & 3) : f);
				}
				/* Rd = Rm */
				tm_soft(&T);
				R(1) = a;
				R(2) = b;
				tm_exec(&T, a_mul(0, 1, 1, 2));
				CHECK_EQ(R(1), a * b);
				/* MLA */
				tm_soft(&T);
				R(1) = a;
				R(2) = b;
				R(3) = 0x1357;
				tm_set_flags(&T, f);
				tm_exec(&T, a_mla(1, 0, 1, 2, 3));
				CHECK_EQ(R(0), a * b + 0x1357);
				CHECK_EQ(tm_flags(&T), ((a * b + 0x1357) >> 31 ? 8u : 0u) | (a * b + 0x1357 == 0 ? 4u : 0u) | (f & 3));

				/* UMULL, SMULL, UMLAL, SMLAL */
				for (unsigned sgn = 0; sgn < 2; sgn++)
					for (unsigned acc = 0; acc < 2; acc++)
						for (unsigned s = 0; s < 2; s++) {
							uint64_t want = sgn ? (uint64_t)sg : u;

							tm_soft(&T);
							R(1) = a;
							R(2) = b;
							R(3) = 0xFFFFFFF0u;
							R(4) = 7;
							tm_set_flags(&T, f);
							tm_exec(&T, a_mull(sgn, acc, s, 4, 3, 1, 2));
							if (acc)
								want += ((uint64_t)7 << 32) | 0xFFFFFFF0u;
							CHECK_EQ(R(3), (uint32_t)want);
							CHECK_EQ(R(4), (uint32_t)(want >> 32));
							CHECK_EQ(tm_flags(&T), s ? ((want >> 63) ? 8u : 0u) | (want == 0 ? 4u : 0u) | (f & 3) : f);
						}
			}
}

/* ---- cargas y almacenamientos de palabra y byte -------------------------------------------------- */

static void fill_data(void)
{
	for (int i = 0; i < 64; i++)
		T.mem[TM_DATA + i] = (uint8_t)(i + 1);
}

static void test_ldst(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	fill_data();

	/* LDR no alineado: palabra alineada rotada a la derecha 8·(dir & 3); el bus ve la dirección alineada */
	for (uint32_t k = 0; k < 4; k++) {
		tm_soft(&T);
		R(1) = TM_DATA + k;
		tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 0, 0));
		CHECK_EQ(R(0), m_ror(0x04030201u, 8 * k));
		CHECK_EQ(T.nlog, 1);
		CHECK_EQ(T.log[0].addr, TM_DATA);
		CHECK_EQ(T.log[0].size, 4);
	}
	/* LDRB: un byte en la dirección exacta */
	tm_soft(&T);
	R(1) = TM_DATA + 5;
	tm_exec(&T, a_ls_i(1, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 6);
	CHECK_EQ(T.log[0].addr, TM_DATA + 5);
	CHECK_EQ(T.log[0].size, 1);
	/* STR no alineado ignora los 2 bits bajos; STRB escribe el byte bajo en la dirección exacta */
	fill_data();
	tm_soft(&T);
	R(0) = 0xA1B2C3D4u;
	R(1) = TM_DATA + 6;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(tm_get32(&T, TM_DATA + 4), 0xA1B2C3D4u);
	CHECK_EQ(tm_get32(&T, TM_DATA), 0x04030201u);
	CHECK_EQ(T.log[0].addr, TM_DATA + 4);
	tm_soft(&T);
	R(0) = 0x1234ABu;
	R(1) = TM_DATA + 9;
	tm_exec(&T, a_ls_i(0, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(T.mem[TM_DATA + 9], 0xAB);
	CHECK_EQ(T.mem[TM_DATA + 8], 0x09);
	CHECK_EQ(T.mem[TM_DATA + 10], 0x0B);
	CHECK_EQ(T.log[0].size, 1);

	/* modos de direccionamiento con inmediato */
	fill_data();
	tm_soft(&T); /* pre-indexado sin writeback */
	R(1) = TM_DATA + 8;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 0, 8));
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 16));
	CHECK_EQ(R(1), TM_DATA + 8);
	tm_soft(&T); /* pre-indexado restando, con writeback */
	R(1) = TM_DATA + 8;
	tm_exec(&T, a_ls_i(1, 0, 1, 0, 1, 1, 0, 4));
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 4));
	CHECK_EQ(R(1), TM_DATA + 4);
	tm_soft(&T); /* post-indexado: lee en la base, luego actualiza */
	R(1) = TM_DATA + 8;
	tm_exec(&T, a_ls_i(1, 0, 0, 1, 0, 1, 0, 12));
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 8));
	CHECK_EQ(R(1), TM_DATA + 20);
	tm_soft(&T); /* post-indexado restando */
	R(1) = TM_DATA + 8;
	tm_exec(&T, a_ls_i(1, 0, 0, 0, 0, 1, 0, 8));
	CHECK_EQ(R(1), TM_DATA);
	/* registro de desplazamiento con desplazamiento: [r1, r2, LSL #2], [r1, -r2, LSR #1]!, post-indexado con ASR */
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 3;
	tm_exec(&T, a_ls_r(1, 0, 1, 1, 0, 1, 0, 2, LSL, 2));
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 12));
	tm_soft(&T);
	R(1) = TM_DATA + 16;
	R(2) = 8;
	tm_exec(&T, a_ls_r(1, 0, 1, 0, 1, 1, 0, 2, LSR, 1));
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 12));
	CHECK_EQ(R(1), TM_DATA + 12);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 0x80000000u; /* ASR #32 (campo 0) = 0xFFFFFFFF = -1 */
	tm_exec(&T, a_ls_r(1, 0, 0, 1, 0, 1, 0, 2, ASR, 0));
	CHECK_EQ(R(0), 0x04030201u);
	CHECK_EQ(R(1), TM_DATA - 1);
	/* RRX en el desplazamiento del registro (ROR #0 con C puesto) */
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 8; /* RRX con C = 1: 0x80000004 */
	tm_set_flags(&T, 2);
	tm_exec(&T, a_ls_r(1, 0, 0, 1, 0, 1, 0, 2, ROR, 0));
	CHECK_EQ(R(1), TM_DATA + 0x80000004u);

	/* Rd = Rn en una carga: gana lo cargado; en un almacenamiento se guarda el valor original */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 1, 1, 1, 4));
	CHECK_EQ(R(1), tm_get32(&T, TM_DATA + 4));
	tm_soft(&T);
	R(1) = TM_DATA + 0x100;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 1, 1, 1, 4));
	CHECK_EQ(tm_get32(&T, TM_DATA + 0x104), TM_DATA + 0x100);
	CHECK_EQ(R(1), TM_DATA + 0x104);

	/* STR con Rd = PC guarda A + 12 */
	tm_soft(&T);
	R(1) = TM_DATA + 0x200;
	tm_exec(&T, a_ls_i(0, 0, 1, 1, 0, 1, 15, 0));
	CHECK_EQ(tm_get32(&T, TM_DATA + 0x200), TM_CODE + 12);
	/* LDR PC: ARMv5 pasa a Thumb si el bit 0 es 1; ARMv4 alinea y sigue en ARM */
	tm_put32(&T, TM_DATA + 0x300, 0x6001);
	tm_soft(&T);
	R(1) = TM_DATA + 0x300;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 15, 0));
	if (v5) {
		CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
		CHECK_EQ(acore_pc(&T.c), 0x6000);
	} else {
		CHECK_EQ(T.c.cpsr & ACORE_T, 0);
		CHECK_EQ(acore_pc(&T.c), 0x6000);
	}
	tm_put32(&T, TM_DATA + 0x300, 0x6002);
	tm_soft(&T);
	R(1) = TM_DATA + 0x300;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 1, 15, 0));
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(acore_pc(&T.c), 0x6000);
	/* LDR r0, [pc, #4]: el PC se lee como A + 8 */
	tm_put32(&T, TM_CODE + 12, 0xCAFEF00Du);
	tm_soft(&T);
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 0, 15, 0, 4));
	CHECK_EQ(R(0), 0xCAFEF00Du);

	/* aborto de datos: la instrucción no tiene efecto (la base queda como estaba) y entra la excepción */
	tm_soft(&T);
	R(0) = 0x11111111u;
	R(1) = TM_DATA;
	T.abort_lo = TM_DATA;
	T.abort_hi = TM_DATA + 64;
	tm_exec(&T, a_ls_i(1, 0, 1, 1, 1, 1, 0, 8));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	CHECK_EQ(T.c.spsr_bank[T.c.bank] & 0x1F, ACORE_MODE_SYS);
	CHECK_EQ(R(14), TM_CODE + 8);
	CHECK_EQ(acore_pc(&T.c), 0x10);
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(T.c.abort, 0);
	acore_set_cpsr(&T.c, T.c.spsr_bank[T.c.bank]); /* de vuelta en sistema: r0 y r1 intactos */
	CHECK_EQ(R(0), 0x11111111u);
	CHECK_EQ(R(1), TM_DATA);
	/* aborto en un almacenamiento: la memoria no cambia */
	tm_soft(&T);
	R(0) = 0x77777777u;
	R(1) = TM_DATA;
	T.abort_lo = TM_DATA;
	T.abort_hi = TM_DATA + 64;
	tm_exec(&T, a_ls_i(0, 0, 0, 1, 0, 1, 0, 4));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	CHECK(tm_get32(&T, TM_DATA) != 0x77777777u);
}

/* ---- medias palabras, con signo y dobles palabras ------------------------------------------------ */

static void test_halfword(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	fill_data();
	T.mem[TM_DATA + 16] = 0x80;
	T.mem[TM_DATA + 17] = 0x7F;
	T.mem[TM_DATA + 18] = 0xFF;
	T.mem[TM_DATA + 19] = 0x80;

	/* LDRH alineado, e impar: ARMv4 lee el halfword alineado y lo rota 8; ARMv5 lo lee sin rotar */
	tm_soft(&T);
	R(1) = TM_DATA + 2;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 0x0403);
	CHECK_EQ(T.log[0].addr, TM_DATA + 2);
	CHECK_EQ(T.log[0].size, 2);
	tm_soft(&T);
	R(1) = TM_DATA + 1;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), v5 ? 0x0201u : 0x01000002u);
	CHECK_EQ(T.log[0].addr, TM_DATA);
	CHECK_EQ(T.log[0].size, 2);
	tm_soft(&T);
	R(1) = TM_DATA + 3;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), v5 ? 0x0403u : 0x03000004u);
	/* LDRSB */
	tm_soft(&T);
	R(1) = TM_DATA + 16;
	tm_exec(&T, a_hs_i(1, 2, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 0xFFFFFF80u);
	CHECK_EQ(T.log[0].size, 1);
	tm_soft(&T);
	R(1) = TM_DATA + 17;
	tm_exec(&T, a_hs_i(1, 2, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 0x7F);
	/* LDRSH alineado; impar: ARMv4 se porta como LDRSB, ARMv5 alinea */
	tm_soft(&T);
	R(1) = TM_DATA + 16;
	tm_exec(&T, a_hs_i(1, 3, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 0x7F80u);
	tm_soft(&T);
	R(1) = TM_DATA + 18;
	tm_exec(&T, a_hs_i(1, 3, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), 0xFFFF80FFu);
	tm_soft(&T);
	R(1) = TM_DATA + 17;
	tm_exec(&T, a_hs_i(1, 3, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), v5 ? 0x7F80u : 0x7Fu);
	tm_soft(&T);
	R(1) = TM_DATA + 19;
	tm_exec(&T, a_hs_i(1, 3, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(R(0), v5 ? 0xFFFF80FFu : 0xFFFFFF80u);
	/* STRH: dirección alineada a 2, escribe 16 bits */
	fill_data();
	tm_soft(&T);
	R(0) = 0xFEDC9876u;
	R(1) = TM_DATA + 13;
	tm_exec(&T, a_hs_i(0, 1, 1, 1, 0, 1, 0, 0));
	CHECK_EQ(tm_get16(&T, TM_DATA + 12), 0x9876);
	CHECK_EQ(T.mem[TM_DATA + 14], 0x0F);
	CHECK_EQ(T.log[0].addr, TM_DATA + 12);
	CHECK_EQ(T.log[0].size, 2);
	/* direccionamiento: inmediato partido en dos nibbles, registro, pre/post y writeback */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 1, 1, 0, 0x26)); /* [r1, #0x26]! */
	CHECK_EQ(R(0), tm_get16(&T, TM_DATA + 0x26));
	CHECK_EQ(R(1), TM_DATA + 0x26);
	tm_soft(&T);
	R(1) = TM_DATA + 0x30;
	R(2) = 6;
	tm_exec(&T, a_hs_r(1, 1, 0, 0, 0, 1, 0, 2)); /* [r1], -r2 */
	CHECK_EQ(R(0), tm_get16(&T, TM_DATA + 0x30));
	CHECK_EQ(R(1), TM_DATA + 0x2A);
	tm_soft(&T);
	R(1) = TM_DATA + 0x20;
	R(2) = 4;
	tm_exec(&T, a_hs_r(1, 3, 1, 1, 0, 1, 0, 2)); /* [r1, r2] */
	CHECK_EQ(R(0), (uint32_t)(int32_t)(int16_t)tm_get16(&T, TM_DATA + 0x24));
	/* Rd = Rn en una carga con writeback: gana lo cargado */
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 1, 1, 1, 4));
	CHECK_EQ(R(1), tm_get16(&T, TM_DATA + 4));
	/* aborto: no cambia la base */
	tm_soft(&T);
	R(1) = TM_DATA;
	T.abort_lo = TM_DATA;
	T.abort_hi = TM_DATA + 64;
	tm_exec(&T, a_hs_i(1, 1, 1, 1, 1, 1, 0, 4));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	acore_set_cpsr(&T.c, T.c.spsr_bank[T.c.bank]);
	CHECK_EQ(R(1), TM_DATA);

	/* LDRD / STRD: solo ARMv5TE; en ARMv4 la codificación es indefinida */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA + 8;
	tm_exec(&T, a_hs_i(0, 2, 1, 1, 0, 1, 2, 0)); /* LDRD r2, [r1] */
	if (v5) {
		CHECK_EQ(R(2), 0x0C0B0A09u);
		CHECK_EQ(R(3), 0x100F0E0Du);
		CHECK_EQ(T.nlog, 2);
		CHECK_EQ(T.log[0].size, 4);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
		CHECK_EQ(R(14), TM_CODE + 4);
	}
	if (v5) {
		tm_soft(&T);
		R(1) = TM_DATA + 0x40 + 2; /* no alineada: el acceso va a la palabra alineada */
		R(4) = 0x11223344u;
		R(5) = 0x55667788u;
		tm_exec(&T, a_hs_i(0, 3, 1, 1, 0, 1, 4, 0)); /* STRD r4, [r1] */
		CHECK_EQ(tm_get32(&T, TM_DATA + 0x40), 0x11223344u);
		CHECK_EQ(tm_get32(&T, TM_DATA + 0x44), 0x55667788u);
		tm_soft(&T);
		R(1) = TM_DATA;
		tm_exec(&T, a_hs_i(0, 2, 1, 1, 1, 1, 3, 8)); /* LDRD con Rd impar: indefinida */
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
		tm_soft(&T);
		R(1) = TM_DATA;
		tm_exec(&T, a_hs_i(0, 2, 0, 1, 0, 1, 6, 8)); /* post-indexado con writeback */
		CHECK_EQ(R(1), TM_DATA + 8);
		CHECK_EQ(R(6), tm_get32(&T, TM_DATA));
		CHECK_EQ(R(7), tm_get32(&T, TM_DATA + 4));
	}
}

/* ---- SWP ----------------------------------------------------------------------------------------------- */

static void test_swp(enum acore_kind kind)
{
	tm_init(&T, kind);
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 0xCAFEF00Du;
	tm_exec(&T, a_swp(0, 1, 0, 2));
	CHECK_EQ(R(0), 0x04030201u);
	CHECK_EQ(tm_get32(&T, TM_DATA), 0xCAFEF00Du);
	/* no alineada: lee rotado, escribe en la palabra alineada sin rotar */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA + 6;
	R(2) = 0x12345678u;
	tm_exec(&T, a_swp(0, 1, 0, 2));
	CHECK_EQ(R(0), m_ror(0x08070605u, 16));
	CHECK_EQ(tm_get32(&T, TM_DATA + 4), 0x12345678u);
	/* SWPB */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA + 5;
	R(2) = 0x1FF;
	tm_exec(&T, a_swp(1, 1, 0, 2));
	CHECK_EQ(R(0), 6);
	CHECK_EQ(tm_get32(&T, TM_DATA + 4), 0x0807FF05u);
	/* Rd = Rm: se guarda el valor original */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 0x99999999u;
	tm_exec(&T, a_swp(0, 1, 2, 2));
	CHECK_EQ(R(2), 0x04030201u);
	CHECK_EQ(tm_get32(&T, TM_DATA), 0x99999999u);
}

/* ---- LDM / STM ----------------------------------------------------------------------------------------- */

static void fill_words(uint32_t base, uint32_t tag)
{
	for (int i = 0; i < 24; i++)
		tm_put32(&T, base + 4 * i, tag + (uint32_t)i);
}

static void test_block(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;
	const uint32_t B = TM_DATA + 0x100;

	tm_init(&T, kind);

	/* STM: las cuatro variantes de direccionamiento y el writeback */
	static const struct {
		unsigned p, u;
		int first;   /* dirección del primer registro respecto de la base */
		int final;   /* base final respecto de la base */
	} modes[4] = {
		{ 0, 1, 0, 12 },   /* IA */
		{ 1, 1, 4, 12 },   /* IB */
		{ 0, 0, -8, -12 }, /* DA */
		{ 1, 0, -12, -12 }, /* DB */
	};
	for (int m = 0; m < 4; m++) {
		fill_words(B - 64, 0xEE000000u);
		tm_soft(&T);
		R(1) = B;
		R(2) = 0x11;
		R(3) = 0x22;
		R(4) = 0x33;
		tm_exec(&T, a_block(0, modes[m].p, modes[m].u, 0, 1, 1, 0x1C));
		CHECK_EQ(tm_get32(&T, B + modes[m].first), 0x11);
		CHECK_EQ(tm_get32(&T, B + modes[m].first + 4), 0x22);
		CHECK_EQ(tm_get32(&T, B + modes[m].first + 8), 0x33);
		CHECK_EQ(R(1), B + modes[m].final);
		CHECK_EQ(T.nlog, 3);
		CHECK_EQ(T.log[0].addr, B + modes[m].first);
		CHECK_EQ(T.log[2].addr, B + modes[m].first + 8);
		/* LDM equivalente */
		fill_words(B - 64, 0xC0DE0000u);
		tm_soft(&T);
		R(1) = B;
		tm_exec(&T, a_block(1, modes[m].p, modes[m].u, 0, 1, 1, 0x1C));
		CHECK_EQ(R(2), tm_get32(&T, B + modes[m].first));
		CHECK_EQ(R(3), tm_get32(&T, B + modes[m].first + 4));
		CHECK_EQ(R(4), tm_get32(&T, B + modes[m].first + 8));
		CHECK_EQ(R(1), B + modes[m].final);
	}
	/* sin writeback la base no cambia; con la base desalineada se ignoran los 2 bits bajos y se conservan en el writeback */
	fill_words(B - 64, 0xC0DE0000u);
	tm_soft(&T);
	R(1) = B + 2;
	tm_exec(&T, A_LDMIA(1, 1, 0x1C));
	CHECK_EQ(R(2), tm_get32(&T, B));
	CHECK_EQ(R(1), B + 2 + 12);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(0, 1, 0xC));
	CHECK_EQ(R(1), B);

	/* STM con la base en la lista y writeback */
	fill_words(B - 64, 0xEE000000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_STMIA(1, 1, 0x6)); /* {r1, r2}: r1 es el primero -> base original en las dos versiones */
	CHECK_EQ(tm_get32(&T, B), B);
	fill_words(B - 64, 0xEE000000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_STMIA(1, 1, 0x3)); /* {r0, r1}: r1 no es el primero -> ARMv4 guarda la nueva, ARMv5 la original */
	CHECK_EQ(tm_get32(&T, B + 4), v5 ? B : B + 8);
	CHECK_EQ(R(1), B + 8);
	/* LDM con la base en la lista y writeback */
	fill_words(B - 64, 0xC0DE0000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(1, 1, 0x6)); /* {r1, r2}: r1 no es el último */
	CHECK_EQ(R(1), v5 ? B + 8 : 0xC0DE0010u);
	CHECK_EQ(R(2), tm_get32(&T, B + 4));
	fill_words(B - 64, 0xC0DE0000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(1, 1, 0x3)); /* {r0, r1}: r1 es el último -> gana lo cargado */
	CHECK_EQ(R(1), tm_get32(&T, B + 4));
	fill_words(B - 64, 0xC0DE0000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(1, 1, 0x2)); /* {r1}: es el único */
	CHECK_EQ(R(1), v5 ? B + 4 : tm_get32(&T, B));

	/* lista vacía: la base cambia 0x40; ARMv4 transfiere el PC (STM guarda A + 12), ARMv5 no transfiere nada */
	for (int m = 0; m < 4; m++) {
		const int first = modes[m].u ? (modes[m].p ? 4 : 0) : (modes[m].p ? -0x40 : -0x3C);

		fill_words(B - 0x80, 0xEE000000u);
		tm_soft(&T);
		R(1) = B;
		tm_exec(&T, a_block(0, modes[m].p, modes[m].u, 0, 1, 1, 0));
		CHECK_EQ(R(1), modes[m].u ? B + 0x40 : B - 0x40);
		if (v5) {
			CHECK_EQ(T.nlog, 0);
		} else {
			CHECK_EQ(T.nlog, 1);
			CHECK_EQ(T.log[0].addr, B + first);
			CHECK_EQ(tm_get32(&T, B + first), TM_CODE + 12);
		}
	}
	tm_put32(&T, B, 0x7000);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(1, 1, 0));
	CHECK_EQ(R(1), B + 0x40);
	if (v5) {
		CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
	} else {
		CHECK_EQ(acore_pc(&T.c), 0x7000);
	}

	/* PC en la lista: STM guarda A + 12; LDM salta (ARMv5 con interworking por el bit 0) */
	fill_words(B - 64, 0xEE000000u);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_STMIA(0, 1, 0x8001));
	CHECK_EQ(tm_get32(&T, B + 4), TM_CODE + 12);
	tm_put32(&T, B, 0x9001);
	tm_soft(&T);
	R(1) = B;
	tm_exec(&T, A_LDMIA(0, 1, 0x8000));
	if (v5) {
		CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	} else {
		CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	}
	CHECK_EQ(acore_pc(&T.c), 0x9000);
	/* LDM con ^ y PC en la lista: restaura el CPSR y carga el banco actual */
	tm_put32(&T, B, 0xAAAA0004u);
	tm_put32(&T, B + 4, 0xBBBB0000u);
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_SVC | ACORE_I);
	T.c.spsr_bank[T.c.bank] = ACORE_MODE_SYS | ACORE_C | ACORE_N;
	R(1) = B;
	tm_exec(&T, a_block(1, 0, 1, 1, 0, 1, 0x8004));
	CHECK_EQ(T.c.cpsr, ACORE_MODE_SYS | ACORE_C | ACORE_N);
	CHECK_EQ(R(2), 0xAAAA0004u);
	CHECK_EQ(acore_pc(&T.c), 0xBBBB0000u);

	/* LDM/STM con ^ sin PC: banco de usuario (aquí, desde FIQ) */
	tm_soft(&T);
	acore_set_cpsr(&T.c, ACORE_MODE_SYS);
	R(8) = 0x80;
	R(9) = 0x90;
	R(13) = 0xD0;
	R(14) = 0xE0;
	acore_set_cpsr(&T.c, ACORE_MODE_FIQ | ACORE_I | ACORE_F);
	R(8) = 0xF8;
	R(9) = 0xF9;
	R(13) = 0xFD;
	R(1) = B;
	fill_words(B - 64, 0xEE000000u);
	tm_exec(&T, a_block(0, 0, 1, 1, 0, 1, 0x6300)); /* STMIA r1, {r8, r9, r13, r14}^ */
	CHECK_EQ(tm_get32(&T, B), 0x80);
	CHECK_EQ(tm_get32(&T, B + 4), 0x90);
	CHECK_EQ(tm_get32(&T, B + 8), 0xD0);
	CHECK_EQ(tm_get32(&T, B + 12), 0xE0);
	CHECK_EQ(R(8), 0xF8); /* el banco de FIQ queda intacto */
	tm_put32(&T, B, 0x1111);
	tm_put32(&T, B + 4, 0x2222);
	tm_exec(&T, a_block(1, 0, 1, 1, 0, 1, 0x0300)); /* LDMIA r1, {r8, r9}^ */
	CHECK_EQ(R(8), 0xF8);
	CHECK_EQ(acore_get_user_reg(&T.c, 8), 0x1111);
	CHECK_EQ(acore_get_user_reg(&T.c, 9), 0x2222);
	acore_set_cpsr(&T.c, ACORE_MODE_SYS);
	CHECK_EQ(R(8), 0x1111);
	CHECK_EQ(R(13), 0xD0);

	/* aborto en un LDM: sin writeback */
	fill_words(B - 64, 0xC0DE0000u);
	tm_soft(&T);
	R(1) = B;
	T.abort_lo = B + 4;
	T.abort_hi = B + 8;
	tm_exec(&T, A_LDMIA(1, 1, 0x1C));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	acore_set_cpsr(&T.c, T.c.spsr_bank[T.c.bank]);
	CHECK_EQ(R(1), B);
}

/* ---- ramas -------------------------------------------------------------------------------------------------- */

static void test_branch(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	/* B hacia delante y hacia atrás: destino = A + 8 + 4·off */
	tm_soft(&T);
	tm_exec(&T, a_b(0, 5));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 8 + 20);
	tm_soft(&T);
	tm_exec(&T, a_b(0, -3));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 8 - 12);
	tm_soft(&T);
	tm_exec(&T, a_b(0, -2)); /* salto a sí misma */
	CHECK_EQ(acore_pc(&T.c), TM_CODE);
	/* BL deja la dirección de la siguiente instrucción en LR */
	tm_soft(&T);
	tm_exec(&T, a_b(1, 10));
	CHECK_EQ(R(14), TM_CODE + 4);
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 8 + 40);
	/* BX: bit 0 = 1 pasa a Thumb, y el destino se alinea */
	tm_soft(&T);
	R(3) = 0x3001;
	tm_exec(&T, a_bx(3));
	CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), 0x3000);
	tm_soft(&T);
	R(3) = 0x3002;
	tm_exec(&T, a_bx(3));
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(acore_pc(&T.c), 0x3000);
	/* BLX Rm (solo ARMv5): LR = A + 4 */
	tm_soft(&T);
	R(3) = 0x3001;
	tm_exec(&T, a_blx_r(3));
	if (v5) {
		CHECK_EQ(R(14), TM_CODE + 4);
		CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
		CHECK_EQ(acore_pc(&T.c), 0x3000);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	}
	/* con la condición falsa no salta */
	tm_soft(&T);
	tm_set_flags(&T, 0);
	tm_exec(&T, (CC_EQ << 28) | (a_b(0, 5) & 0x0FFFFFFFu));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
	/* SWI sin gancho: excepción de SVC con LR = A + 4 y SPSR = CPSR */
	tm_soft(&T);
	tm_exec(&T, a_swi(0x123456));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SVC);
	CHECK_EQ(R(14), TM_CODE + 4);
	CHECK_EQ(acore_pc(&T.c), 0x08);
	CHECK_EQ(T.c.spsr_bank[T.c.bank], ACORE_MODE_SYS | ACORE_I | ACORE_F);
	CHECK_EQ(T.c.cpsr & (ACORE_I | ACORE_T), ACORE_I);
	/* con gancho que atiende la SWI: no hay excepción */
	tm_soft(&T);
	T.swi_calls = 0;
	T.swi_handle = 1;
	tm_exec(&T, a_swi(0x7654));
	CHECK_EQ(T.swi_calls, 1);
	CHECK_EQ(T.swi_last, 0x7654);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4);
	T.swi_handle = 0;
	T.swi_calls = 0;
}

/* ---- ARMv5TE: CLZ, saturación y multiplicaciones de 16 bits ------------------------------------------------------ */

static int32_t h16(uint32_t v, unsigned top) { return top ? m_hi16(v) : m_lo16(v); }

static void test_dsp(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;
	static const uint32_t hv[] = { 0x00010002, 0x7FFF8000, 0x8000FFFF, 0xFFFFFFFF, 0x12345678, 0x80008000, 0x7FFF7FFF, 0 };

	tm_init(&T, kind);
	if (!v5) { /* en ARMv4T todo esto es indefinido */
		const uint32_t insns[] = { a_clz(0, 1), a_q(0, 0, 1, 2), a_smul(3, 0, 0, 0, 0, 2, 1), a_smul(0, 1, 1, 0, 3, 2, 1) };

		for (unsigned i = 0; i < sizeof insns / sizeof insns[0]; i++) {
			tm_soft(&T);
			R(1) = 5;
			tm_exec(&T, insns[i]);
			CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
			CHECK_EQ(R(14), TM_CODE + 4);
		}
		return;
	}
	for (int i = 0; i < 40; i++) {
		uint32_t v = i < 32 ? 1u << i : (uint32_t)i * 0x01010101u;

		tm_soft(&T);
		R(1) = v;
		tm_exec(&T, a_clz(0, 1));
		CHECK_EQ(R(0), v ? (uint32_t)__builtin_clz(v) : 32);
	}
	tm_soft(&T);
	R(1) = 0;
	tm_exec(&T, a_clz(0, 1));
	CHECK_EQ(R(0), 32);

	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++) {
			const int64_t sm = (int32_t)vals[i], sn = (int32_t)vals[j];
			uint32_t wq, want;

			for (unsigned op = 0; op < 4; op++) {
				tm_soft(&T);
				R(1) = vals[i]; /* Rm */
				R(2) = vals[j]; /* Rn */
				tm_exec(&T, a_q(op, 0, 1, 2));
				wq = 0;
				{
					int64_t n = sn;

					if (op & 2)
						n = (int32_t)m_sat(2 * sn, &wq);
					want = m_sat((op & 1) ? sm - n : sm + n, &wq);
				}
				CHECK_EQ(R(0), want);
				CHECK_EQ(!!(T.c.cpsr & ACORE_Q), wq);
			}
		}
	/* la Q es pegajosa: una operación sin saturar no la borra */
	tm_soft(&T);
	T.c.cpsr |= ACORE_Q;
	R(1) = 1;
	R(2) = 1;
	tm_exec(&T, a_q(0, 0, 1, 2));
	CHECK_EQ(R(0), 2);
	CHECK(T.c.cpsr & ACORE_Q);

	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++)
			for (unsigned x = 0; x < 2; x++)
				for (unsigned y = 0; y < 2; y++) {
					const uint32_t a = hv[i], b = hv[j];
					static const uint32_t accs[] = { 0, 0x7FFFFFFF, 0x80000000, 5 };

					/* SMULxy */
					tm_soft(&T);
					R(1) = a; /* Rm */
					R(2) = b; /* Rs */
					tm_exec(&T, a_smul(3, x, y, 0, 0, 2, 1));
					CHECK_EQ(R(0), (uint32_t)(h16(a, x) * h16(b, y)));
					CHECK_EQ(T.c.cpsr & ACORE_Q, 0);
					/* SMULWy (x = 1) y SMLAWy (x = 0) */
					for (int k = 0; k < 4; k++) {
						int64_t prod, sum;

						prod = ((int64_t)(int32_t)a * h16(b, y)) >> 16;
						tm_soft(&T);
						R(1) = a;
						R(2) = b;
						R(3) = accs[k];
						tm_exec(&T, a_smul(1, 1, y, 0, 3, 2, 1));
						CHECK_EQ(R(0), (uint32_t)prod); /* SMULWy ignora Rn */
						tm_soft(&T);
						R(1) = a;
						R(2) = b;
						R(3) = accs[k];
						tm_exec(&T, a_smul(1, 0, y, 0, 3, 2, 1));
						sum = prod + (int32_t)accs[k];
						CHECK_EQ(R(0), (uint32_t)sum);
						CHECK_EQ(!!(T.c.cpsr & ACORE_Q), sum != (int32_t)sum);
						/* SMLAxy */
						tm_soft(&T);
						R(1) = a;
						R(2) = b;
						R(3) = accs[k];
						tm_exec(&T, a_smul(0, x, y, 0, 3, 2, 1));
						sum = (int64_t)(h16(a, x) * h16(b, y)) + (int32_t)accs[k];
						CHECK_EQ(R(0), (uint32_t)sum);
						CHECK_EQ(!!(T.c.cpsr & ACORE_Q), sum != (int32_t)sum);
					}
					/* SMLALxy: acumula en 64 bits, sin Q */
					{
						int64_t acc = (int64_t)(((uint64_t)0xFFFFFFF0u << 32) | 0x90000000u);

						tm_soft(&T);
						R(1) = a;
						R(2) = b;
						R(3) = 0x90000000u; /* RdLo */
						R(4) = 0xFFFFFFF0u; /* RdHi */
						tm_exec(&T, a_smul(2, x, y, 4, 3, 2, 1));
						acc += (int64_t)(h16(a, x) * h16(b, y));
						CHECK_EQ(R(3), (uint32_t)acc);
						CHECK_EQ(R(4), (uint32_t)((uint64_t)acc >> 32));
						CHECK_EQ(T.c.cpsr & ACORE_Q, 0);
					}
				}
}

/* ---- espacio indefinido -------------------------------------------------------------------------------------------- */

static void test_undefined(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;
	/* codificaciones que son indefinidas en las dos versiones */
	static const uint32_t both[] = {
		0xE7F000F0u, /* UDF: 011 con el bit 4 */
		0xE6000010u,
		0xEC000000u, /* LDC/STC: sin coprocesadores */
		0xED900000u,
		0xEE000000u, /* CDP */
		0xE1000090u + 0x100000u * 2, /* SWP-like con bits 21:20 distintos de cero */
	};

	tm_init(&T, kind);
	for (unsigned i = 0; i < sizeof both / sizeof both[0]; i++) {
		tm_soft(&T);
		tm_exec(&T, both[i]);
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
		CHECK_EQ(R(14), TM_CODE + 4);
		CHECK_EQ(acore_pc(&T.c), 0x04);
	}
	/* MCR/MRC a un coprocesador que no es CP15: indefinido; a CP15: solo en ARM9 */
	tm_soft(&T);
	tm_exec(&T, a_mrc(14, 0, 0, 0, 0, 0));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	tm_soft(&T);
	tm_exec(&T, a_mrc(15, 0, 0, 0, 0, 0)); /* ID de CP15 */
	if (v5) {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SYS);
		CHECK_EQ(R(0), 0x41059461u);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	}
	/* BKPT: aborto de captación en ARMv5; indefinido en ARMv4 */
	tm_soft(&T);
	tm_exec(&T, 0xE1200070u);
	if (v5) {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
		CHECK_EQ(R(14), TM_CODE + 4);
		CHECK_EQ(acore_pc(&T.c), 0x0C);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	}
	/* aborto de captación */
	tm_soft(&T);
	T.fetch_abort = TM_CODE;
	tm_code(&T, 0, A_NOP);
	acore_step(&T.c);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	CHECK_EQ(R(14), TM_CODE + 4);
	CHECK_EQ(acore_pc(&T.c), 0x0C);
}

void test_acore_arm(void)
{
	for (int k = 0; k < 2; k++) {
		const enum acore_kind kind = k ? ACORE_ARM9 : ACORE_ARM7;

		test_alu(kind);
		test_dp_pc(kind);
		test_cond(kind);
		test_mul(kind);
		test_ldst(kind);
		test_halfword(kind);
		test_swp(kind);
		test_block(kind);
		test_branch(kind);
		test_dsp(kind);
		test_undefined(kind);
	}
}
