/*
 * SalsaDS — núcleo común de los intérpretes ARM9 (ARMv5TE) y ARM7 (ARMv4T):
 * estado, modos con banco de registros, excepciones, interrupciones y el
 * bucle de ejecución. Las instrucciones están en acore_arm.c y acore_thumb.c.
 */
#include <string.h>

#include "cpu/acore_priv.h"

/* Modo -> banco: 0 usuario/sistema, 1 FIQ, 2 IRQ, 3 SVC, 4 aborto, 5 indefinida; 0xFF = modo inválido. */
static const uint8_t bank_of_mode[32] = {
	[ACORE_MODE_USR] = 0, [ACORE_MODE_FIQ] = 1, [ACORE_MODE_IRQ] = 2, [ACORE_MODE_SVC] = 3,
	[ACORE_MODE_ABT] = 4, [ACORE_MODE_UND] = 5, [ACORE_MODE_SYS] = 0,
};

static int mode_valid(uint32_t m)
{
	return m == ACORE_MODE_USR || m == ACORE_MODE_FIQ || m == ACORE_MODE_IRQ || m == ACORE_MODE_SVC ||
	       m == ACORE_MODE_ABT || m == ACORE_MODE_UND || m == ACORE_MODE_SYS;
}

void acore_init(struct acore *c, enum acore_kind kind, const struct acore_bus *bus, void *user)
{
	acore_tables_init();
	memset(c, 0, sizeof *c);
	c->v5 = kind == ACORE_ARM9;
	c->bus = bus;
	c->user = user;
	acore_reset(c);
}

void acore_attach_tcm(struct acore *c, uint8_t *itcm, uint8_t *dtcm)
{
	c->itcm = itcm;
	c->dtcm = dtcm;
	if (c->v5)
		acore_cp15_reset(c);
}

void acore_reset(struct acore *c)
{
	memset(c->r, 0, sizeof c->r);
	memset(c->spsr_bank, 0, sizeof c->spsr_bank);
	memset(c->sp_bank, 0, sizeof c->sp_bank);
	memset(c->lr_bank, 0, sizeof c->lr_bank);
	c->bank = bank_of_mode[ACORE_MODE_SVC];
	c->cpsr = ACORE_I | ACORE_F | ACORE_MODE_SVC;
	c->halted = c->irq = c->fiq = c->attn = c->abort = 0;
	if (c->v5)
		acore_cp15_reset(c);
	else
		c->vector_base = 0;
	c->r[15] = c->vector_base + 4; /* vector de reinicio + 4 (convención de PC) */
}

/* ---- modos ----------------------------------------------------------------- */

void acore_switch_mode(struct acore *c, uint32_t mode)
{
	const uint32_t nb = bank_of_mode[mode], ob = c->bank;

	c->cpsr = (c->cpsr & ~ACORE_MODE_MASK) | mode;
	if (nb == ob)
		return;
	c->sp_bank[ob] = c->r[13];
	c->lr_bank[ob] = c->r[14];
	if (ob == 1) { /* sale de FIQ: r8-r12 vuelven a los de usuario */
		memcpy(c->fiq_r8_12, &c->r[8], sizeof c->fiq_r8_12);
		memcpy(&c->r[8], c->usr_r8_12, sizeof c->usr_r8_12);
	}
	if (nb == 1) { /* entra a FIQ */
		memcpy(c->usr_r8_12, &c->r[8], sizeof c->usr_r8_12);
		memcpy(&c->r[8], c->fiq_r8_12, sizeof c->fiq_r8_12);
	}
	c->r[13] = c->sp_bank[nb];
	c->r[14] = c->lr_bank[nb];
	c->bank = nb;
}

void acore_set_cpsr(struct acore *c, uint32_t v)
{
	uint32_t mode = v & ACORE_MODE_MASK;
	const uint32_t mask = c->v5 ? 0xF80000FFu : 0xF00000FFu;

	if (!mode_valid(mode))
		mode = c->cpsr & ACORE_MODE_MASK; /* modo inválido: impredecible; se conserva el actual */
	c->cpsr = (v & mask & ~ACORE_MODE_MASK) | (c->cpsr & ACORE_MODE_MASK);
	acore_switch_mode(c, mode);
}

uint32_t acore_get_user_reg(const struct acore *c, unsigned n)
{
	if (n >= 8 && n <= 12)
		return c->bank == 1 ? c->usr_r8_12[n - 8] : c->r[n];
	if (n == 13)
		return c->bank == 0 ? c->r[13] : c->sp_bank[0];
	if (n == 14)
		return c->bank == 0 ? c->r[14] : c->lr_bank[0];
	return c->r[n];
}

void acore_set_user_reg(struct acore *c, unsigned n, uint32_t v)
{
	if (n >= 8 && n <= 12) {
		if (c->bank == 1)
			c->usr_r8_12[n - 8] = v;
		else
			c->r[n] = v;
	} else if (n == 13) {
		if (c->bank == 0)
			c->r[13] = v;
		else
			c->sp_bank[0] = v;
	} else if (n == 14) {
		if (c->bank == 0)
			c->r[14] = v;
		else
			c->lr_bank[0] = v;
	} else {
		c->r[n] = v;
	}
}

void acore_set_pc(struct acore *c, uint32_t addr)
{
	jump(c, addr);
}

void acore_bx(struct acore *c, uint32_t addr)
{
	jump_bx(c, addr);
}

/* ---- excepciones ------------------------------------------------------------ */

void acore_exception(struct acore *c, enum acore_exc e, uint32_t lr)
{
	static const uint8_t mode_of[8] = {
		ACORE_MODE_SVC, ACORE_MODE_UND, ACORE_MODE_SVC, ACORE_MODE_ABT,
		ACORE_MODE_ABT, ACORE_MODE_SVC, ACORE_MODE_IRQ, ACORE_MODE_FIQ,
	};
	const uint32_t old = c->cpsr;

	acore_switch_mode(c, mode_of[e]);
	c->spsr_bank[c->bank] = old;
	c->r[14] = lr;
	c->cpsr = (c->cpsr & ~ACORE_T) | ACORE_I | ((e == ACORE_EXC_RESET || e == ACORE_EXC_FIQ) ? ACORE_F : 0u);
	c->r[15] = c->vector_base + 4u * (uint32_t)e + 4u;
	c->cycles += 2;
}

/* Valores de r14 según el estado, partiendo de r15 «de ejecución» (A+8 en ARM, A+4 en Thumb). */
void acore_undefined(struct acore *c)
{
	acore_exception(c, ACORE_EXC_UNDEF, c->r[15] - ((c->cpsr & ACORE_T) ? 2u : 4u));
}

void acore_swi(struct acore *c, uint32_t comment)
{
	if (c->swi_hook && c->swi_hook(c, comment))
		return;
	acore_exception(c, ACORE_EXC_SWI, c->r[15] - ((c->cpsr & ACORE_T) ? 2u : 4u));
}

/* BKPT: aborto de captación; r14 = A + 4. */
void acore_prefetch_abort(struct acore *c)
{
	acore_exception(c, ACORE_EXC_PABT, (c->cpsr & ACORE_T) ? c->r[15] : c->r[15] - 4u);
}

/* Aborto de datos: r14 = A + 8 en los dos estados. */
void acore_data_abort(struct acore *c)
{
	c->abort = 0;
	acore_exception(c, ACORE_EXC_DABT, (c->cpsr & ACORE_T) ? c->r[15] + 4u : c->r[15]);
}

/* ---- interrupciones ----------------------------------------------------------- */

static void update_attn(struct acore *c)
{
	c->attn = c->halted | c->irq | c->fiq;
}

void acore_set_irq(struct acore *c, int level)
{
	c->irq = level != 0;
	update_attn(c);
}

void acore_set_fiq(struct acore *c, int level)
{
	c->fiq = level != 0;
	update_attn(c);
}

void acore_halt(struct acore *c)
{
	c->halted = 1;
	update_attn(c);
}

/* ---- ejecución ----------------------------------------------------------------- */

ALWAYS_INLINE void step_one(struct acore *c)
{
	if (c->cpsr & ACORE_T) {
		const uint32_t a = c->r[15] - 2;
		const uint32_t insn = mem_fetch16(c, a);

		if (UNLIKELY(c->abort)) {
			c->abort = 0;
			acore_exception(c, ACORE_EXC_PABT, c->r[15] + 2u);
			return;
		}
		c->r[15] += 2;
		c->cycles++;
		c->icount++;
		acore_thumb_table[c->v5][(insn >> 6) & 0x3FF](c, insn);
	} else {
		const uint32_t a = c->r[15] - 4;
		const uint32_t insn = mem_fetch32(c, a);
		const uint32_t cond = insn >> 28;

		if (UNLIKELY(c->abort)) {
			c->abort = 0;
			acore_exception(c, ACORE_EXC_PABT, c->r[15]);
			return;
		}
		c->r[15] += 4;
		c->cycles++;
		c->icount++;
		if ((acore_cond_mask[cond] >> (c->cpsr >> 28)) & 1u)
			acore_arm_table[c->v5][((insn >> 16) & 0xFF0u) | ((insn >> 4) & 0xFu)](c, insn);
		else if (cond == 0xF && c->v5)
			acore_arm_uncond(c, insn);
	}
}

/* Atiende HALT y las líneas de interrupción. Devuelve 1 si ya consumió el paso (excepción o parada). */
static int service_attn(struct acore *c, uint64_t until)
{
	if (c->halted) {
		if (!(c->irq || c->fiq)) {
			if (c->cycles < until)
				c->cycles = until;
			return 1;
		}
		c->halted = 0;
		update_attn(c);
	}
	if (c->fiq && !(c->cpsr & ACORE_F)) {
		acore_exception(c, ACORE_EXC_FIQ, (c->cpsr & ACORE_T) ? c->r[15] + 2u : c->r[15]);
		c->cycles++;
		return 1;
	}
	if (c->irq && !(c->cpsr & ACORE_I)) {
		acore_exception(c, ACORE_EXC_IRQ, (c->cpsr & ACORE_T) ? c->r[15] + 2u : c->r[15]);
		c->cycles++;
		return 1;
	}
	return 0;
}

void acore_run(struct acore *c, uint64_t until)
{
	while (c->cycles < until) {
		if (UNLIKELY(c->attn) && service_attn(c, until))
			continue;
		step_one(c);
	}
}

void acore_step(struct acore *c)
{
	if (UNLIKELY(c->attn) && service_attn(c, c->cycles + 1))
		return;
	step_one(c);
}
