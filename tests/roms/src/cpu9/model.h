/*
 * SalsaDS — modelo en C de la semántica de ARM usado por las pruebas de
 * CPU (cpu9). Se escribió desde el manual de ARM (ARMv5TE) y se valida
 * corriendo las pruebas en un emulador de referencia: si el modelo y el
 * emulador discrepan, hay que mirar quién tiene razón antes de tocar nada.
 *
 * Banderas en 4 bits: N = 8, Z = 4, C = 2, V = 1.
 */
#ifndef CPU9_MODEL_H
#define CPU9_MODEL_H

#include <stdint.h>

#define F_N 8u
#define F_Z 4u
#define F_C 2u
#define F_V 1u

static inline uint32_t m_ror(uint32_t x, uint32_t n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

/* type: 0 LSL, 1 LSR, 2 ASR, 3 ROR. Cantidad desde un registro (solo cuenta el byte bajo). */
static inline uint32_t m_shift_reg(int type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *cout)
{
	amt &= 0xFF;
	if (amt == 0) {
		*cout = cin;
		return v;
	}
	switch (type) {
	case 0:
		if (amt < 32) {
			*cout = (v >> (32 - amt)) & 1;
			return v << amt;
		}
		*cout = amt == 32 ? v & 1 : 0;
		return 0;
	case 1:
		if (amt < 32) {
			*cout = (v >> (amt - 1)) & 1;
			return v >> amt;
		}
		*cout = amt == 32 ? v >> 31 : 0;
		return 0;
	case 2:
		if (amt < 32) {
			*cout = (v >> (amt - 1)) & 1;
			return (uint32_t)((int32_t)v >> amt);
		}
		*cout = v >> 31;
		return (uint32_t)((int32_t)v >> 31);
	default: {
		uint32_t r = amt & 31;
		if (r == 0) {
			*cout = v >> 31;
			return v;
		}
		*cout = (v >> (r - 1)) & 1;
		return m_ror(v, r);
	}
	}
}

/* Cantidad inmediata de 0 a 31: 0 significa LSL #0, LSR #32, ASR #32 o RRX. */
static inline uint32_t m_shift_imm(int type, uint32_t v, uint32_t imm, uint32_t cin, uint32_t *cout)
{
	if (imm == 0) {
		switch (type) {
		case 0:
			*cout = cin;
			return v;
		case 1:
			*cout = v >> 31;
			return 0;
		case 2:
			*cout = v >> 31;
			return (uint32_t)((int32_t)v >> 31);
		default:
			*cout = v & 1;
			return (cin << 31) | (v >> 1);
		}
	}
	return m_shift_reg(type, v, imm, cin, cout);
}

static inline uint32_t m_add(uint32_t a, uint32_t b, uint32_t cin, uint32_t *f)
{
	uint64_t w = (uint64_t)a + b + cin;
	uint32_t r = (uint32_t)w;

	*f = (r >> 31 ? F_N : 0) | (r == 0 ? F_Z : 0) | ((w >> 32) ? F_C : 0) |
	     (((~(a ^ b) & (a ^ r)) >> 31) ? F_V : 0);
	return r;
}

/* Operaciones de proceso de datos con S puesto. op = campo opcode (0..15).
 * `shc` es el acarreo que sale del desplazador (para las lógicas). */
static inline uint32_t m_alu(int op, uint32_t a, uint32_t b, uint32_t fin, uint32_t shc, uint32_t *fout)
{
	uint32_t cin = (fin >> 1) & 1, r, f = 0;
	int logical = 0;

	switch (op) {
	case 0: case 8: r = a & b; logical = 1; break;
	case 1: case 9: r = a ^ b; logical = 1; break;
	case 2: case 10: r = m_add(a, ~b, 1, &f); break;
	case 3: r = m_add(b, ~a, 1, &f); break;
	case 4: case 11: r = m_add(a, b, 0, &f); break;
	case 5: r = m_add(a, b, cin, &f); break;
	case 6: r = m_add(a, ~b, cin, &f); break;
	case 7: r = m_add(b, ~a, cin, &f); break;
	case 12: r = a | b; logical = 1; break;
	case 13: r = b; logical = 1; break;
	case 14: r = a & ~b; logical = 1; break;
	default: r = ~b; logical = 1; break;
	}
	if (logical)
		f = (r >> 31 ? F_N : 0) | (r == 0 ? F_Z : 0) | (shc ? F_C : 0) | (fin & F_V);
	*fout = f;
	return r;
}

static inline int m_cond(int cc, uint32_t f)
{
	int n = !!(f & F_N), z = !!(f & F_Z), c = !!(f & F_C), v = !!(f & F_V);

	switch (cc) {
	case 0: return z;
	case 1: return !z;
	case 2: return c;
	case 3: return !c;
	case 4: return n;
	case 5: return !n;
	case 6: return v;
	case 7: return !v;
	case 8: return c && !z;
	case 9: return !c || z;
	case 10: return n == v;
	case 11: return n != v;
	case 12: return !z && n == v;
	case 13: return z || n != v;
	default: return 1;
	}
}

/* Aritmética saturada de ARMv5TE; *q se pone a 1 si saturó. */
static inline uint32_t m_sat(int64_t v, uint32_t *q)
{
	if (v > 0x7FFFFFFFLL) {
		*q = 1;
		return 0x7FFFFFFFu;
	}
	if (v < -0x80000000LL) {
		*q = 1;
		return 0x80000000u;
	}
	return (uint32_t)v;
}

static inline int32_t m_lo16(uint32_t x) { return (int16_t)x; }
static inline int32_t m_hi16(uint32_t x) { return (int16_t)(x >> 16); }

#endif
