/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — funciones en Thumb (ARMv4T/ARMv5T) de las pruebas de CPU del
 * ARM9. Se llaman desde código ARM con blx: así se prueban las instrucciones
 * Thumb de proceso de datos (que no tienen msr/mrs) con las banderas puestas
 * y leídas desde ARM, y de paso el cambio de estado con bx/blx.
 *
 * Entradas: r0 = a, r1 = b. Salida: r0. Las banderas pasan por el cuerpo.
 */
	.syntax unified
	.text
	.thumb

#define OP2(name, insn...)       \
	.global name;            \
	.thumb_func;             \
	.type name, %function;   \
name:                            \
	insn;                    \
	bx lr

OP2(th_ands, ands r0, r1)
OP2(th_eors, eors r0, r1)
OP2(th_lsls, lsls r0, r1)
OP2(th_lsrs, lsrs r0, r1)
OP2(th_asrs, asrs r0, r1)
OP2(th_adcs, adcs r0, r1)
OP2(th_sbcs, sbcs r0, r1)
OP2(th_rors, rors r0, r1)
OP2(th_tst, tst r0, r1)
OP2(th_negs, negs r0, r1)
OP2(th_cmp, cmp r0, r1)
OP2(th_cmn, cmn r0, r1)
OP2(th_orrs, orrs r0, r1)
OP2(th_muls, muls r0, r1)
OP2(th_bics, bics r0, r1)
OP2(th_mvns, mvns r0, r1)
OP2(th_adds_rrr, adds r0, r0, r1)
OP2(th_subs_rrr, subs r0, r0, r1)
OP2(th_adds_i8, adds r0, #200)
OP2(th_subs_i8, subs r0, #200)
OP2(th_cmp_i8, cmp r0, #200)
OP2(th_movs_i8, movs r0, #200)
OP2(th_lsls_i5, lsls r0, r0, #5)
OP2(th_lsrs_i5, lsrs r0, r0, #5)
OP2(th_asrs_i5, asrs r0, r0, #5)
OP2(th_lsls_i0, lsls r0, r0, #0)
OP2(th_lsrs_i32, lsrs r0, r0, #32)
OP2(th_asrs_i32, asrs r0, r0, #32)

	.global th_add
	.thumb_func
	.type th_add, %function
th_add:
	adds r0, r0, r1
	bx lr

	.global th_getlr
	.thumb_func
	.type th_getlr, %function
th_getlr:
	mov r0, lr
	bx lr

/* Ramas condicionales de Thumb: devuelven 1 si se toma el salto. El salto
 * va primero para que las banderas lleguen intactas. */
#define COND(name, cc)           \
	.global name;            \
	.thumb_func;             \
	.type name, %function;   \
name:                            \
	b##cc 1f;                \
	movs r0, #0;             \
	bx lr;                   \
1:                               \
	movs r0, #1;             \
	bx lr

COND(th_beq, eq)
COND(th_bne, ne)
COND(th_bcs, cs)
COND(th_bcc, cc)
COND(th_bmi, mi)
COND(th_bpl, pl)
COND(th_bvs, vs)
COND(th_bvc, vc)
COND(th_bhi, hi)
COND(th_bls, ls)
COND(th_bge, ge)
COND(th_blt, lt)
COND(th_bgt, gt)
COND(th_ble, le)
