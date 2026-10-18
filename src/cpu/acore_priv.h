/*
 * SalsaDS — interno de los intérpretes de CPU (acore*.c). No lo incluye nadie más.
 */
#ifndef SALSADS_CPU_ACORE_PRIV_H
#define SALSADS_CPU_ACORE_PRIV_H

#include "cpu/acore.h"
#include "util/endian.h"

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define ALWAYS_INLINE static inline
#endif

typedef void (*acore_op_fn)(struct acore *c, uint32_t insn);

/* Tablas de despacho (acore_arm.c, acore_thumb.c): una por versión. */
extern acore_op_fn acore_arm_table[2][4096];   /* [v5][bits 27..20 y 7..4] */
extern acore_op_fn acore_thumb_table[2][1024]; /* [v5][bits 15..6] */
extern uint16_t acore_cond_mask[16];           /* bit i = condición cierta con NZCV = i */
void acore_tables_init(void);
void acore_thumb_tables_init(void);

/* Instrucciones del espacio incondicional (cond = 1111) en la ARM9. */
void acore_arm_uncond(struct acore *c, uint32_t insn);

/* CP15 (acore_cp15.c) */
void acore_cp15_reset(struct acore *c);
int acore_cp15_write(struct acore *c, uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t v);
int acore_cp15_read(struct acore *c, uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t *v);

/* ---- utilidades de bits ------------------------------------------------- */

ALWAYS_INLINE uint32_t ror32(uint32_t x, uint32_t n)
{
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

ALWAYS_INLINE uint32_t bit(uint32_t x, unsigned n)
{
	return (x >> n) & 1u;
}

/* ---- PC ----------------------------------------------------------------- */

/* Salto sin cambio de estado: alinea según T. */
ALWAYS_INLINE void jump(struct acore *c, uint32_t a)
{
	if (c->cpsr & ACORE_T)
		c->r[15] = (a & ~1u) + 2;
	else
		c->r[15] = (a & ~3u) + 4;
}

/* Salto con interworking (BX, LDR PC en v5...). */
ALWAYS_INLINE void jump_bx(struct acore *c, uint32_t a)
{
	if (a & 1) {
		c->cpsr |= ACORE_T;
		c->r[15] = (a & ~1u) + 2;
	} else {
		c->cpsr &= ~ACORE_T;
		c->r[15] = (a & ~3u) + 4;
	}
}

/* Dirección de la instrucción en ejecución. */
ALWAYS_INLINE uint32_t cur_addr(const struct acore *c)
{
	return c->r[15] - ((c->cpsr & ACORE_T) ? 4u : 8u);
}

/* ---- memoria ------------------------------------------------------------ */

#define IN_RANGE(a, base, len) ((uint64_t)(uint32_t)((a) - (base)) < (len))

ALWAYS_INLINE uint32_t mem_rd32(struct acore *c, uint32_t a)
{
	if (IN_RANGE(a, 0u, c->itcm_rd_len))
		return rd32le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)));
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_rd_len))
		return rd32le(c->dtcm + ((a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)));
	return c->bus->read32(c, a);
}

ALWAYS_INLINE uint32_t mem_rd16(struct acore *c, uint32_t a)
{
	if (IN_RANGE(a, 0u, c->itcm_rd_len))
		return rd16le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)));
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_rd_len))
		return rd16le(c->dtcm + ((a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)));
	return c->bus->read16(c, a);
}

ALWAYS_INLINE uint32_t mem_rd8(struct acore *c, uint32_t a)
{
	if (IN_RANGE(a, 0u, c->itcm_rd_len))
		return c->itcm[a & (ACORE_ITCM_SIZE - 1)];
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_rd_len))
		return c->dtcm[(a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)];
	return c->bus->read8(c, a);
}

/* En modo de carga de una TCM, la escritura va solo a la TCM. */
ALWAYS_INLINE void mem_wr32(struct acore *c, uint32_t a, uint32_t v)
{
	if (IN_RANGE(a, 0u, c->itcm_wr_len)) {
		wr32le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)), v);
		if (UNLIKELY(c->itcm_hook != 0))
			c->itcm_hook(c, a & (ACORE_ITCM_SIZE - 1), 4u);
		return;
	}
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_wr_len)) {
		wr32le(c->dtcm + ((a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)), v);
		return;
	}
	c->bus->write32(c, a, v);
}

ALWAYS_INLINE void mem_wr16(struct acore *c, uint32_t a, uint32_t v)
{
	if (IN_RANGE(a, 0u, c->itcm_wr_len)) {
		wr16le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)), (uint16_t)v);
		if (UNLIKELY(c->itcm_hook != 0))
			c->itcm_hook(c, a & (ACORE_ITCM_SIZE - 1), 2u);
		return;
	}
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_wr_len)) {
		wr16le(c->dtcm + ((a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)), (uint16_t)v);
		return;
	}
	c->bus->write16(c, a, v & 0xFFFF);
}

ALWAYS_INLINE void mem_wr8(struct acore *c, uint32_t a, uint32_t v)
{
	if (IN_RANGE(a, 0u, c->itcm_wr_len)) {
		c->itcm[a & (ACORE_ITCM_SIZE - 1)] = (uint8_t)v;
		if (UNLIKELY(c->itcm_hook != 0))
			c->itcm_hook(c, a & (ACORE_ITCM_SIZE - 1), 1u);
		return;
	}
	if (IN_RANGE(a, c->dtcm_base, c->dtcm_wr_len)) {
		c->dtcm[(a - c->dtcm_base) & (ACORE_DTCM_SIZE - 1)] = (uint8_t)v;
		return;
	}
	c->bus->write8(c, a, v & 0xFF);
}

/* Captación de instrucciones: solo la ITCM (la DTCM es de datos). */
ALWAYS_INLINE uint32_t mem_fetch32(struct acore *c, uint32_t a)
{
	if (IN_RANGE(a, 0u, c->itcm_rd_len))
		return rd32le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)));
	return c->bus->fetch32(c, a);
}

ALWAYS_INLINE uint32_t mem_fetch16(struct acore *c, uint32_t a)
{
	if (IN_RANGE(a, 0u, c->itcm_rd_len))
		return rd16le(c->itcm + (a & (ACORE_ITCM_SIZE - 1)));
	return c->bus->fetch16(c, a);
}

/* ---- accesos con las reglas de alineación de cada versión ---------------- */

/* LDR: lee la palabra alineada y la rota por los 2 bits bajos de la dirección. */
ALWAYS_INLINE uint32_t ld_word(struct acore *c, uint32_t a)
{
	return ror32(mem_rd32(c, a & ~3u), (a & 3u) * 8);
}

/* LDRH: ARMv4 rota por 8 si la dirección es impar; ARMv5 la alinea sin rotar. */
ALWAYS_INLINE uint32_t ld_half(struct acore *c, uint32_t a)
{
	uint32_t v = mem_rd16(c, a & ~1u);
	return (a & 1u) && !c->v5 ? ror32(v, 8) : v;
}

/* LDRSH: ARMv4 con dirección impar se porta como LDRSB; ARMv5 alinea. */
ALWAYS_INLINE uint32_t ld_shalf(struct acore *c, uint32_t a)
{
	if ((a & 1u) && !c->v5)
		return (uint32_t)(int32_t)(int8_t)mem_rd8(c, a);
	return (uint32_t)(int32_t)(int16_t)mem_rd16(c, a & ~1u);
}

/* ---- registros con banco ------------------------------------------------- */

void acore_switch_mode(struct acore *c, uint32_t mode);

/* r8-r14 del banco de usuario (para LDM/STM con ^). */
uint32_t acore_get_user_reg(const struct acore *c, unsigned n);
void acore_set_user_reg(struct acore *c, unsigned n, uint32_t v);

ALWAYS_INLINE int mode_has_spsr(const struct acore *c)
{
	return c->bank != 0;
}

/* CPSR <- SPSR del modo actual (salida de excepción). Sin SPSR (usuario/sistema) no hace nada. */
ALWAYS_INLINE void restore_spsr(struct acore *c)
{
	if (c->bank)
		acore_set_cpsr(c, c->spsr_bank[c->bank]);
}

/* ---- banderas ------------------------------------------------------------ */

ALWAYS_INLINE void set_nz(struct acore *c, uint32_t r)
{
	c->cpsr = (c->cpsr & ~(ACORE_N | ACORE_Z)) | (r & ACORE_N) | (r == 0 ? ACORE_Z : 0u);
}

ALWAYS_INLINE void set_nzcv(struct acore *c, uint32_t r, uint32_t carry, uint32_t overflow)
{
	c->cpsr = (c->cpsr & 0x0FFFFFFFu) | (r & ACORE_N) | (r == 0 ? ACORE_Z : 0u) | (carry ? ACORE_C : 0u) |
		  (overflow ? ACORE_V : 0u);
}

/* Suma con acarreo: r = a + b + cin; carry y overflow por referencia. */
ALWAYS_INLINE uint32_t add_cv(uint32_t a, uint32_t b, uint32_t cin, uint32_t *carry, uint32_t *ovf)
{
	uint64_t w = (uint64_t)a + b + cin;
	uint32_t r = (uint32_t)w;
	*carry = (uint32_t)(w >> 32);
	*ovf = (~(a ^ b) & (a ^ r)) >> 31;
	return r;
}

/* Resta con acarreo ARM: r = a - b - (1 - cin); C = no hubo préstamo. */
ALWAYS_INLINE uint32_t sub_cv(uint32_t a, uint32_t b, uint32_t cin, uint32_t *carry, uint32_t *ovf)
{
	uint64_t w = (uint64_t)a + (uint32_t)~b + cin;
	uint32_t r = (uint32_t)w;
	*carry = (uint32_t)(w >> 32);
	*ovf = ((a ^ b) & (a ^ r)) >> 31;
	return r;
}

/* ---- desplazador ---------------------------------------------------------- */

/* Desplazamiento por inmediato (campo de 5 bits; 0 tiene significado especial). type: 0 LSL, 1 LSR, 2 ASR, 3 ROR. */
ALWAYS_INLINE uint32_t shift_by_imm(unsigned type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *cout)
{
	switch (type) {
	case 0:
		if (amt == 0) {
			*cout = cin;
			return v;
		}
		*cout = (v >> (32 - amt)) & 1;
		return v << amt;
	case 1:
		if (amt == 0) {
			*cout = v >> 31;
			return 0;
		}
		*cout = (v >> (amt - 1)) & 1;
		return v >> amt;
	case 2:
		if (amt == 0) {
			*cout = v >> 31;
			return (uint32_t)((int32_t)v >> 31);
		}
		*cout = (v >> (amt - 1)) & 1;
		return (uint32_t)((int32_t)v >> amt);
	default:
		if (amt == 0) { /* RRX */
			*cout = v & 1;
			return (cin << 31) | (v >> 1);
		}
		*cout = (v >> (amt - 1)) & 1;
		return ror32(v, amt);
	}
}

/* Desplazamiento por registro: cuenta el byte bajo; 0 deja valor y acarreo. */
ALWAYS_INLINE uint32_t shift_by_reg(unsigned type, uint32_t v, uint32_t amt, uint32_t cin, uint32_t *cout)
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
		uint32_t n = amt & 31;
		if (n == 0) {
			*cout = v >> 31;
			return v;
		}
		*cout = (v >> (n - 1)) & 1;
		return ror32(v, n);
	}
	}
}

/* ---- ciclos y abortos ------------------------------------------------------ */

/* Los accesos pueden señalar un aborto: la instrucción termina sin efecto y se toma la excepción. */
void acore_data_abort(struct acore *c);
void acore_undefined(struct acore *c);
void acore_prefetch_abort(struct acore *c);
void acore_swi(struct acore *c, uint32_t comment);

#define CHECK_DABORT()                                                           \
	do {                                                                     \
		if (UNLIKELY(c->abort)) {                                        \
			acore_data_abort(c);                                     \
			return;                                                  \
		}                                                                \
	} while (0)

/* Ciclos de multiplicación (ARM7TDMI): 1..4 según los bytes significativos del multiplicador. */
ALWAYS_INLINE uint32_t mul_cycles(uint32_t rs, int is_signed)
{
	if (is_signed)
		rs = ((int32_t)rs < 0) ? ~rs : rs;
	if ((rs >> 8) == 0)
		return 1;
	if ((rs >> 16) == 0)
		return 2;
	if ((rs >> 24) == 0)
		return 3;
	return 4;
}

#endif
