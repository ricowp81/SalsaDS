/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — pruebas del intérprete de CPU: instrucciones Thumb de la ARM7
 * (ARMv4T) y la ARM9 (ARMv5TE). Mismo estilo que test_acore_arm.c; la ALU se
 * compara con el modelo de la ROM cpu9.
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

static uint32_t nz(uint32_t r)
{
	return (r >> 31 ? F_N : 0) | (r == 0 ? F_Z : 0);
}

/* ---- formatos 1 a 4: desplazamientos, suma y resta, inmediatos y ALU ----------------------------------------- */

static void test_th_alu(void)
{
	/* formato 4: las 16 operaciones */
	for (unsigned op = 0; op < 16; op++)
		for (int i = 0; i < NV; i++)
			for (int j = 0; j < NV; j++)
				for (uint32_t f = 0; f < 16; f++) {
					const uint32_t a = vals[i], b = vals[j], cin = (f >> 1) & 1;
					uint32_t want = 0, wf = 0, shc;

					switch (op) {
					case 2: case 3: case 4: case 7: {
						static const int type_of[8] = { 0, 0, 0, 1, 2, 0, 0, 3 };

						want = m_shift_reg(type_of[op], a, b, cin, &shc);
						wf = nz(want) | (shc ? F_C : 0) | (f & F_V);
						break;
					}
					case 9:
						want = m_alu(3, b, 0, f, cin, &wf);
						break;
					case 13:
						want = a * b;
						wf = nz(want) | (f & (F_C | F_V));
						break;
					default:
						want = m_alu((int)op, a, b, f, cin, &wf);
						break;
					}
					if (op == 8 || op == 10 || op == 11)
						want = a;
					tm_soft(&T);
					T.c.cpsr |= ACORE_T;
					R(0) = a;
					R(1) = b;
					tm_set_flags(&T, f);
					tm_exec16(&T, t_alu(op, 0, 1));
					CHECK_EQ(R(0), want);
					CHECK_EQ(tm_flags(&T), wf);
					CHECK_EQ(acore_pc(&T.c), TM_CODE + 2);
					CHECK_EQ(R(1), b);
				}
}

static void test_th_shift_addsub(void)
{
	static const unsigned amts[] = { 0, 1, 5, 31 };

	for (unsigned type = 0; type < 3; type++)
		for (unsigned k = 0; k < 4; k++)
			for (int i = 0; i < NV; i++)
				for (uint32_t f = 0; f < 16; f += 3) {
					const uint32_t cin = (f >> 1) & 1;
					uint32_t shc, want = m_shift_imm((int)type, vals[i], amts[k], cin, &shc);

					tm_soft(&T);
					R(1) = vals[i];
					tm_set_flags(&T, f);
					tm_exec16(&T, t_shift(type, 0, 1, amts[k]));
					CHECK_EQ(R(0), want);
					CHECK_EQ(tm_flags(&T), nz(want) | (shc ? F_C : 0) | (f & F_V));
				}
	/* ADD / SUB con registro e inmediato de 3 bits */
	for (int i = 0; i < NV; i++)
		for (int j = 0; j < NV; j++)
			for (unsigned sub = 0; sub < 2; sub++) {
				uint32_t wf, want = m_alu(sub ? OP_SUB : OP_ADD, vals[i], vals[j], 0, 0, &wf);

				tm_soft(&T);
				R(1) = vals[i];
				R(2) = vals[j];
				tm_exec16(&T, t_addsub(sub, 0, 0, 1, 2));
				CHECK_EQ(R(0), want);
				CHECK_EQ(tm_flags(&T), wf);
				if (j < 8) {
					want = m_alu(sub ? OP_SUB : OP_ADD, vals[i], (uint32_t)j % 8, 0, 0, &wf);
					tm_soft(&T);
					R(1) = vals[i];
					tm_exec16(&T, t_addsub(sub, 1, 0, 1, (unsigned)j % 8));
					CHECK_EQ(R(0), want);
					CHECK_EQ(tm_flags(&T), wf);
				}
			}
	/* formato 3: MOV, CMP, ADD, SUB con inmediato de 8 bits */
	for (unsigned op = 0; op < 4; op++)
		for (int i = 0; i < NV; i++)
			for (uint32_t f = 0; f < 16; f += 5)
				for (unsigned imm = 0; imm < 256; imm += 51) {
					static const int alu_of[4] = { OP_MOV, OP_CMP, OP_ADD, OP_SUB };
					uint32_t wf, want = m_alu(alu_of[op], vals[i], imm, f, (f >> 1) & 1, &wf);

					if (op == 1)
						want = vals[i];
					tm_soft(&T);
					R(3) = vals[i];
					tm_set_flags(&T, f);
					tm_exec16(&T, t_imm8(op, 3, imm));
					CHECK_EQ(R(3), want);
					CHECK_EQ(tm_flags(&T), wf);
				}
}

/* ---- formato 5: registros altos, BX, BLX -------------------------------------------------------------------- */

static void test_th_hi(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	/* ADD y MOV con registros altos no tocan las banderas */
	tm_soft(&T);
	R(9) = 10;
	R(1) = 5;
	tm_set_flags(&T, 0xF);
	tm_exec16(&T, t_hi(0, 9, 1)); /* ADD r9, r1 */
	CHECK_EQ(R(9), 15);
	CHECK_EQ(tm_flags(&T), 0xF);
	tm_soft(&T);
	R(10) = 0x1234;
	tm_exec16(&T, t_hi(2, 2, 10)); /* MOV r2, r10 */
	CHECK_EQ(R(2), 0x1234);
	/* CMP con registros altos sí */
	tm_soft(&T);
	R(8) = 7;
	R(1) = 7;
	tm_exec16(&T, t_hi(1, 8, 1));
	CHECK_EQ(tm_flags(&T), 6u); /* Z y C */
	/* el PC se lee como A + 4 */
	tm_soft(&T);
	tm_exec16(&T, t_hi(2, 0, 15)); /* MOV r0, pc */
	CHECK_EQ(R(0), TM_CODE + 4);
	/* MOV pc, r1 y ADD pc, r1 saltan sin cambiar de estado; el destino se alinea a media palabra */
	tm_soft(&T);
	R(1) = 0x2003;
	tm_exec16(&T, t_hi(2, 15, 1));
	CHECK_EQ(acore_pc(&T.c), 0x2002);
	CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	tm_soft(&T);
	R(1) = 0x10;
	tm_exec16(&T, t_hi(0, 15, 1)); /* ADD pc, r1: A + 4 + 0x10 */
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4 + 0x10);
	/* BX a ARM y a Thumb; BX con registro alto */
	tm_soft(&T);
	R(1) = 0x4000;
	tm_exec16(&T, t_bx(1));
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(acore_pc(&T.c), 0x4000);
	tm_soft(&T);
	R(14) = 0x4005;
	tm_exec16(&T, t_bx(14));
	CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), 0x4004);
	/* BLX Rm (ARMv5): LR = dirección de la siguiente instrucción con el bit 0 */
	tm_soft(&T);
	R(1) = 0x4000;
	tm_exec16(&T, t_blx(1));
	if (v5) {
		CHECK_EQ(R(14), TM_CODE + 2 + 1);
		CHECK_EQ(T.c.cpsr & ACORE_T, 0);
		CHECK_EQ(acore_pc(&T.c), 0x4000);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
		CHECK_EQ(R(14), TM_CODE + 2);
	}
}

/* ---- cargas y almacenamientos ------------------------------------------------------------------------------------- */

static void fill_data(void)
{
	for (int i = 0; i < 64; i++)
		T.mem[TM_DATA + i] = (uint8_t)(i + 1);
	T.mem[TM_DATA + 16] = 0x80;
	T.mem[TM_DATA + 17] = 0x7F;
	T.mem[TM_DATA + 18] = 0xFF;
	T.mem[TM_DATA + 19] = 0x80;
}

static void test_th_ldst(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	fill_data();

	/* carga relativa al PC: (PC & ~3) + 4·imm con PC = A + 4 */
	tm_put32(&T, TM_CODE + 4 + 8, 0xCAFEF00Du);
	tm_soft(&T);
	tm_exec16(&T, t_ldr_pc(2, 2));
	CHECK_EQ(R(2), 0xCAFEF00Du);
	T.c.cpsr |= ACORE_T;
	tm_put32(&T, TM_CODE + 4 + 4, 0x0BADBEEFu);
	tm_soft(&T);
	T.c.cpsr |= ACORE_T;
	tm_code16(&T, 1, t_ldr_pc(2, 1)); /* en A = TM_CODE + 2: PC = A + 4 = TM_CODE + 6, alineado a TM_CODE + 4 */
	acore_set_pc(&T.c, TM_CODE + 2);
	acore_step(&T.c);
	CHECK_EQ(R(2), 0x0BADBEEFu);

	/* registro de desplazamiento: los 8 tipos */
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 5;
	tm_exec16(&T, t_ls_r(6, 0, 1, 2)); /* LDRB */
	CHECK_EQ(R(0), 6);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 16;
	tm_exec16(&T, t_ls_r(3, 0, 1, 2)); /* LDRSB */
	CHECK_EQ(R(0), 0xFFFFFF80u);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 18;
	tm_exec16(&T, t_ls_r(7, 0, 1, 2)); /* LDRSH */
	CHECK_EQ(R(0), 0xFFFF80FFu);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 17; /* LDRSH impar: ARMv4 como LDRSB, ARMv5 alineado */
	tm_exec16(&T, t_ls_r(7, 0, 1, 2));
	CHECK_EQ(R(0), v5 ? 0x7F80u : 0x7Fu);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 2;
	tm_exec16(&T, t_ls_r(5, 0, 1, 2)); /* LDRH */
	CHECK_EQ(R(0), 0x0403);
	tm_soft(&T);
	R(1) = TM_DATA;
	R(2) = 1; /* LDRH impar */
	tm_exec16(&T, t_ls_r(5, 0, 1, 2));
	CHECK_EQ(R(0), v5 ? 0x0201u : 0x01000002u);
	for (uint32_t k = 0; k < 4; k++) { /* LDR no alineado rota */
		tm_soft(&T);
		R(1) = TM_DATA;
		R(2) = k;
		tm_exec16(&T, t_ls_r(4, 0, 1, 2));
		CHECK_EQ(R(0), m_ror(0x04030201u, 8 * k));
	}
	fill_data();
	tm_soft(&T);
	R(0) = 0xA1B2C3D4u;
	R(1) = TM_DATA;
	R(2) = 6;
	tm_exec16(&T, t_ls_r(0, 0, 1, 2)); /* STR no alineado ignora los 2 bits bajos */
	CHECK_EQ(tm_get32(&T, TM_DATA + 4), 0xA1B2C3D4u);
	tm_soft(&T);
	R(0) = 0xFEDC9876u;
	R(1) = TM_DATA;
	R(2) = 13;
	tm_exec16(&T, t_ls_r(1, 0, 1, 2)); /* STRH */
	CHECK_EQ(tm_get16(&T, TM_DATA + 12), 0x9876);
	CHECK_EQ(T.log[0].addr, TM_DATA + 12);
	tm_soft(&T);
	R(0) = 0x1234ABu;
	R(1) = TM_DATA;
	R(2) = 9;
	tm_exec16(&T, t_ls_r(2, 0, 1, 2)); /* STRB */
	CHECK_EQ(T.mem[TM_DATA + 9], 0xAB);

	/* inmediato de 5 bits: palabra ×4, byte ×1, media palabra ×2 */
	fill_data();
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec16(&T, t_ls_i(1, 0, 1, 3)); /* LDR r0, [r1, #12] */
	CHECK_EQ(R(0), tm_get32(&T, TM_DATA + 12));
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec16(&T, t_ls_i(3, 0, 1, 31)); /* LDRB r0, [r1, #31] */
	CHECK_EQ(R(0), 32);
	tm_soft(&T);
	R(0) = 0x55;
	R(1) = TM_DATA;
	tm_exec16(&T, t_ls_i(2, 0, 1, 7)); /* STRB r0, [r1, #7] */
	CHECK_EQ(T.mem[TM_DATA + 7], 0x55);
	tm_soft(&T);
	R(0) = 0x12345678;
	R(1) = TM_DATA + 0x80;
	tm_exec16(&T, t_ls_i(0, 0, 1, 31)); /* STR r0, [r1, #124] */
	CHECK_EQ(tm_get32(&T, TM_DATA + 0x80 + 124), 0x12345678);
	tm_soft(&T);
	R(1) = TM_DATA;
	tm_exec16(&T, t_ls_h(1, 0, 1, 3)); /* LDRH r0, [r1, #6] */
	CHECK_EQ(R(0), tm_get16(&T, TM_DATA + 6));
	tm_soft(&T);
	R(0) = 0xBEEF;
	R(1) = TM_DATA + 0x80;
	tm_exec16(&T, t_ls_h(0, 0, 1, 31)); /* STRH r0, [r1, #62] */
	CHECK_EQ(tm_get16(&T, TM_DATA + 0x80 + 62), 0xBEEF);

	/* relativos a SP y direcciones PC/SP */
	tm_soft(&T);
	R(13) = TM_DATA;
	R(3) = 0x77777777u;
	tm_exec16(&T, t_ls_sp(0, 3, 4)); /* STR r3, [sp, #16] */
	CHECK_EQ(tm_get32(&T, TM_DATA + 16), 0x77777777u);
	tm_soft(&T);
	R(13) = TM_DATA;
	tm_exec16(&T, t_ls_sp(1, 3, 2)); /* LDR r3, [sp, #8] */
	CHECK_EQ(R(3), tm_get32(&T, TM_DATA + 8));
	tm_soft(&T);
	R(13) = 0x2000;
	tm_exec16(&T, t_add_pcsp(1, 4, 5)); /* ADD r4, sp, #20 */
	CHECK_EQ(R(4), 0x2014);
	tm_soft(&T);
	tm_exec16(&T, t_add_pcsp(0, 4, 5)); /* ADD r4, pc, #20: (A + 4) & ~3 + 20 */
	CHECK_EQ(R(4), TM_CODE + 4 + 20);
	tm_soft(&T);
	T.c.cpsr |= ACORE_T;
	tm_code16(&T, 1, t_add_pcsp(0, 4, 5));
	acore_set_pc(&T.c, TM_CODE + 2);
	acore_step(&T.c);
	CHECK_EQ(R(4), TM_CODE + 4 + 20);
	tm_soft(&T);
	R(13) = 0x2000;
	tm_exec16(&T, t_sp_adj(0, 3)); /* ADD sp, #12 */
	CHECK_EQ(R(13), 0x200C);
	tm_exec16(&T, t_sp_adj(1, 5)); /* SUB sp, #20 */
	CHECK_EQ(R(13), 0x200C - 20);

	/* aborto de datos desde Thumb: LR = A + 8 y se vuelve a ARM */
	tm_soft(&T);
	R(1) = TM_DATA;
	T.abort_lo = TM_DATA;
	T.abort_hi = TM_DATA + 64;
	tm_exec16(&T, t_ls_i(1, 0, 1, 0));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(R(14), TM_CODE + 8);
	CHECK_EQ(T.c.spsr_bank[T.c.bank] & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), 0x10);
}

/* ---- PUSH, POP, LDMIA, STMIA -------------------------------------------------------------------------------------------- */

static void test_th_block(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;
	const uint32_t S = TM_DATA + 0x200;

	tm_init(&T, kind);
	for (int i = 0; i < 24; i++)
		tm_put32(&T, S - 64 + 4 * (uint32_t)i, 0xEE000000u + (uint32_t)i);

	/* PUSH {r0, r1, lr}: sp baja 12 y se guarda de menor a mayor registro */
	tm_soft(&T);
	R(13) = S;
	R(0) = 0x11;
	R(1) = 0x22;
	R(14) = 0x33;
	tm_exec16(&T, t_push(1, 3));
	CHECK_EQ(R(13), S - 12);
	CHECK_EQ(tm_get32(&T, S - 12), 0x11);
	CHECK_EQ(tm_get32(&T, S - 8), 0x22);
	CHECK_EQ(tm_get32(&T, S - 4), 0x33);
	CHECK_EQ(T.nlog, 3);
	CHECK_EQ(T.log[0].addr, S - 12);
	/* POP {r2, r3}: sp sube 8 */
	tm_soft(&T);
	R(13) = S - 12;
	tm_exec16(&T, t_pop(0, 0xC));
	CHECK_EQ(R(2), 0x11);
	CHECK_EQ(R(3), 0x22);
	CHECK_EQ(R(13), S - 4);
	/* POP {pc}: ARMv5 cambia de estado con el bit 0; ARMv4 se queda en Thumb y alinea a 2 */
	tm_put32(&T, S, 0x6000);
	tm_soft(&T);
	R(13) = S;
	tm_exec16(&T, t_pop(1, 0));
	CHECK_EQ(R(13), S + 4);
	CHECK_EQ(acore_pc(&T.c), 0x6000);
	CHECK_EQ(T.c.cpsr & ACORE_T, v5 ? 0u : ACORE_T);
	tm_put32(&T, S, 0x6001);
	tm_soft(&T);
	R(13) = S;
	tm_exec16(&T, t_pop(1, 0));
	CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), 0x6000);
	/* aborto en un PUSH: sp intacto */
	tm_soft(&T);
	R(13) = S;
	T.abort_lo = S - 8;
	T.abort_hi = S - 4;
	tm_exec16(&T, t_push(0, 7));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	acore_set_cpsr(&T.c, T.c.spsr_bank[T.c.bank] & ~ACORE_T);
	CHECK_EQ(R(13), S);

	/* STMIA / LDMIA con writeback */
	for (int i = 0; i < 24; i++)
		tm_put32(&T, S - 64 + 4 * (uint32_t)i, 0xC0DE0000u + (uint32_t)i);
	tm_soft(&T);
	R(4) = S;
	R(0) = 0xA0;
	R(2) = 0xA2;
	tm_exec16(&T, t_block(0, 4, 0x05));
	CHECK_EQ(tm_get32(&T, S), 0xA0);
	CHECK_EQ(tm_get32(&T, S + 4), 0xA2);
	CHECK_EQ(R(4), S + 8);
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(1, 4, 0x06)); /* LDMIA r4!, {r1, r2} */
	CHECK_EQ(R(1), 0xA0);
	CHECK_EQ(R(2), 0xA2);
	CHECK_EQ(R(4), S + 8);
	/* base en la lista: STMIA guarda la vieja si es la primera (ARMv4) o siempre (ARMv5) */
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(0, 4, 0x10)); /* {r4} */
	CHECK_EQ(tm_get32(&T, S), S);
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(0, 4, 0x11)); /* {r0, r4} */
	CHECK_EQ(tm_get32(&T, S + 4), v5 ? S : S + 8);
	/* LDMIA con la base en la lista: ARMv4 no hace writeback; ARMv5 lo hace si no es el último */
	tm_put32(&T, S, 0x5A5A5A5Au);
	tm_put32(&T, S + 4, 0x6B6B6B6Bu);
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(1, 4, 0x30)); /* {r4, r5}: r4 no es el último */
	CHECK_EQ(R(4), v5 ? S + 8 : 0x5A5A5A5Au);
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(1, 4, 0x11)); /* {r0, r4}: r4 es el último */
	CHECK_EQ(R(4), 0x6B6B6B6Bu);
	/* lista vacía: la base cambia 0x40; ARMv4 transfiere el PC */
	tm_put32(&T, S, 0x7000);
	tm_soft(&T);
	R(4) = S;
	tm_exec16(&T, t_block(1, 4, 0));
	CHECK_EQ(R(4), S + 0x40);
	CHECK_EQ(acore_pc(&T.c), v5 ? TM_CODE + 2 : 0x7000);
	tm_soft(&T);
	R(4) = S + 0x100;
	tm_exec16(&T, t_block(0, 4, 0));
	CHECK_EQ(R(4), S + 0x140);
	if (v5) {
		CHECK_EQ(T.nlog, 0);
	} else {
		CHECK_EQ(T.nlog, 1);
		CHECK_EQ(tm_get32(&T, S + 0x100), TM_CODE + 6);
	}
}

/* ---- saltos -------------------------------------------------------------------------------------------------------------------- */

static void test_th_branch(enum acore_kind kind)
{
	const int v5 = kind == ACORE_ARM9;

	tm_init(&T, kind);
	/* ramas condicionales: destino = A + 4 + 2·off */
	for (unsigned cc = 0; cc < 14; cc++)
		for (uint32_t f = 0; f < 16; f++) {
			tm_soft(&T);
			tm_set_flags(&T, f);
			tm_exec16(&T, t_bcond(cc, 5));
			CHECK_EQ(acore_pc(&T.c), m_cond((int)cc, f) ? TM_CODE + 4 + 10 : TM_CODE + 2);
		}
	tm_soft(&T);
	tm_set_flags(&T, 4);
	tm_exec16(&T, t_bcond(CC_EQ, -4));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4 - 8);
	/* B: 11 bits con signo */
	tm_soft(&T);
	tm_exec16(&T, t_b(100));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4 + 200);
	tm_soft(&T);
	tm_exec16(&T, t_b(-1024));
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 4 - 2048);
	tm_soft(&T);
	tm_exec16(&T, t_b(-2)); /* salta a sí misma */
	CHECK_EQ(acore_pc(&T.c), TM_CODE);

	/* BL: dos instrucciones; LR = dirección de la siguiente | 1 */
	for (int k = 0; k < 2; k++) {
		const int32_t off = k ? 0x2468 : -0x100;
		const uint32_t hi = ((uint32_t)off >> 12) & 0x7FF, lo = ((uint32_t)off >> 1) & 0x7FF;

		tm_soft(&T);
		T.c.cpsr |= ACORE_T;
		tm_code16(&T, 0, t_bl_hi((int)hi));
		tm_code16(&T, 1, t_bl_lo(lo));
		acore_set_pc(&T.c, TM_CODE);
		acore_step(&T.c);
		acore_step(&T.c);
		CHECK_EQ(acore_pc(&T.c), (uint32_t)(TM_CODE + 4 + off));
		CHECK_EQ(R(14), (TM_CODE + 4) | 1);
		CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	}
	/* BLX con inmediato (ARMv5): salta a ARM y alinea; en ARMv4 la codificación es indefinida */
	tm_soft(&T);
	T.c.cpsr |= ACORE_T;
	tm_code16(&T, 0, t_bl_hi(2));
	tm_code16(&T, 1, t_blx_lo(0x234));
	acore_set_pc(&T.c, TM_CODE);
	acore_step(&T.c);
	acore_step(&T.c);
	if (v5) {
		CHECK_EQ(acore_pc(&T.c), (TM_CODE + 4 + 0x2468u) & ~3u);
		CHECK_EQ(R(14), (TM_CODE + 4) | 1);
		CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
		CHECK_EQ(R(14), TM_CODE + 4); /* LR = A + 2 de la segunda mitad */
	}
	/* SWI desde Thumb: LR = A + 2, vuelve a ARM, SPSR guarda T */
	tm_soft(&T);
	tm_exec16(&T, t_swi(0x42));
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_SVC);
	CHECK_EQ(T.c.cpsr & ACORE_T, 0);
	CHECK_EQ(R(14), TM_CODE + 2);
	CHECK_EQ(T.c.spsr_bank[T.c.bank] & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), 0x08);
	tm_soft(&T);
	T.swi_handle = 1;
	tm_exec16(&T, t_swi(0xAB));
	CHECK_EQ(T.swi_last, 0xAB);
	CHECK_EQ(T.c.cpsr & ACORE_T, ACORE_T);
	CHECK_EQ(acore_pc(&T.c), TM_CODE + 2);
	T.swi_handle = 0;
	/* condición 1110 indefinida; BKPT: aborto de captación en ARMv5, indefinida en ARMv4 */
	tm_soft(&T);
	tm_exec16(&T, 0xDE00u);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	CHECK_EQ(R(14), TM_CODE + 2);
	tm_soft(&T);
	tm_exec16(&T, t_bkpt(0x11));
	if (v5) {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
		CHECK_EQ(R(14), TM_CODE + 4);
	} else {
		CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	}
	/* otras codificaciones indefinidas del grupo 1011 */
	tm_soft(&T);
	tm_exec16(&T, 0xB200u);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_UND);
	/* aborto de captación en Thumb: LR = A + 4 */
	tm_soft(&T);
	T.c.cpsr |= ACORE_T;
	acore_set_pc(&T.c, TM_CODE);
	T.fetch_abort = TM_CODE;
	acore_step(&T.c);
	CHECK_EQ(T.c.cpsr & 0x1F, ACORE_MODE_ABT);
	CHECK_EQ(R(14), TM_CODE + 4);
}

void test_acore_thumb(void)
{
	tm_init(&T, ACORE_ARM7);
	test_th_alu();
	test_th_shift_addsub();
	tm_init(&T, ACORE_ARM9);
	test_th_alu();
	test_th_shift_addsub();
	for (int k = 0; k < 2; k++) {
		const enum acore_kind kind = k ? ACORE_ARM9 : ACORE_ARM7;

		test_th_hi(kind);
		test_th_ldst(kind);
		test_th_block(kind);
		test_th_branch(kind);
	}
}
