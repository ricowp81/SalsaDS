/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — codificación de instrucciones ARM para los programas de prueba
 * del JIT (Fase 0.7). Solo lo que usan los bucles: datos con operando
 * inmediato o registro con desplazamiento inmediato, LDR/STR/LDRB/STRB con
 * desplazamiento inmediato, B<cond> y SWI.
 */
#ifndef SALSADS_JIT_ARMENC_H
#define SALSADS_JIT_ARMENC_H

#include <stdint.h>

enum arm_cond { ARM_EQ, ARM_NE, ARM_CS, ARM_CC, ARM_MI, ARM_PL, ARM_VS, ARM_VC, ARM_HI, ARM_LS, ARM_GE, ARM_LT, ARM_GT, ARM_LE, ARM_AL };
enum arm_dp { ARM_AND, ARM_EOR, ARM_SUB, ARM_RSB, ARM_ADD, ARM_ADC, ARM_SBC, ARM_RSC, ARM_TST, ARM_TEQ, ARM_CMP, ARM_CMN, ARM_ORR, ARM_MOV, ARM_BIC, ARM_MVN };
enum arm_shift { ARM_LSL, ARM_LSR, ARM_ASR, ARM_ROR };

static inline uint32_t arm_dp_imm(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rot, unsigned imm8)
{
	return (uint32_t)ARM_AL << 28 | 1u << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | rot << 8 | imm8;
}

static inline uint32_t arm_dp_reg(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned shift,
				  unsigned amount)
{
	return (uint32_t)ARM_AL << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | amount << 7 | shift << 5 | rm;
}

/* LDR/STR con desplazamiento inmediato positivo, sin escritura de vuelta. */
static inline uint32_t arm_ldr(unsigned byte, unsigned rn, unsigned rd, unsigned off)
{
	return (uint32_t)ARM_AL << 28 | 1u << 26 | 1u << 24 | 1u << 23 | byte << 22 | 1u << 20 | rn << 16 | rd << 12 | off;
}

static inline uint32_t arm_str(unsigned byte, unsigned rn, unsigned rd, unsigned off)
{
	return (uint32_t)ARM_AL << 28 | 1u << 26 | 1u << 24 | 1u << 23 | byte << 22 | rn << 16 | rd << 12 | off;
}

/* Desplazamiento en palabras respecto de PC + 8. */
static inline uint32_t arm_b(unsigned cond, int off_words)
{
	return (uint32_t)cond << 28 | 0xAu << 24 | ((uint32_t)off_words & 0xFFFFFFu);
}

#define ARM_SWI 0xEF000000u

#endif
