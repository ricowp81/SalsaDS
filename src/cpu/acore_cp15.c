/*
 * SalsaDS — CP15 mínimo del ARM946E-S (ARM9 de la DS).
 *
 * Lo que hace de verdad: el registro de control (vectores altos, habilitación
 * de las TCM y su modo de carga), la región de la DTCM y de la ITCM (base y
 * tamaño, con el espejo dentro de la región), los registros de identificación
 * y la espera de interrupción (c7,c0,4). La caché y la unidad de protección no
 * se emulan: sus registros se guardan y se leen de vuelta, las operaciones de
 * caché son no-ops y no hay abortos por permisos.
 *
 * Fuente: GBATEK (CP15 de la DS) y el manual del ARM946E-S (documentación).
 */
#include <string.h>

#include "cpu/acore_priv.h"

#define CP15_ID 0x41059461u        /* ARM946E-S, ARMv5TE, revisión 1 */
#define CP15_CACHE_TYPE 0x0F0D2112u /* I-caché de 8 KB, D-caché de 4 KB, líneas de 32 B */
#define CP15_TCM_SIZE 0x00140180u  /* ITCM 32 KB, DTCM 16 KB */

#define CTRL_RESET 0x00002078u     /* vectores altos (los reinicios de la DS arrancan en 0xFFFF0000) */
#define CTRL_FIXED 0x00000078u     /* bits 3-6: siempre 1 */
#define CTRL_WRITABLE 0x000FF085u  /* bits 0, 2, 7 y 12-19 */

/* Índices de c->cp15_misc */
enum {
	M_C2_DATA = 0, M_C2_INSTR, M_C3_WBUF, M_C5_DATA, M_C5_INSTR, M_C5_EXT_DATA, M_C5_EXT_INSTR,
	M_C6 = 8,        /* 8 regiones: 8..15 */
	M_C9_DLOCK = 16, M_C9_ILOCK, M_C13_PID, M_C13_TRACE,
};

/* Recalcula los rangos de las TCM a partir del registro de control y de las regiones. */
static void update_tcm(struct acore *c)
{
	const uint32_t ctrl = c->cp15_ctrl;
	uint64_t dsize = 0, isize = 0;
	uint32_t n;

	c->vector_base = (ctrl & 0x2000u) ? 0xFFFF0000u : 0u;

	n = (c->cp15_dtcm_reg >> 1) & 31u;
	if (n >= 3)
		dsize = (uint64_t)512 << (n > 23 ? 23 : n);
	n = (c->cp15_itcm_reg >> 1) & 31u;
	if (n >= 3)
		isize = (uint64_t)512 << (n > 23 ? 23 : n);

	c->dtcm_base = dsize ? (c->cp15_dtcm_reg & 0xFFFFF000u & ~(uint32_t)(dsize - 1)) : 0;
	c->dtcm_wr_len = (ctrl & (1u << 16)) ? dsize : 0;
	c->dtcm_rd_len = ((ctrl & (1u << 16)) && !(ctrl & (1u << 17))) ? dsize : 0;
	c->itcm_wr_len = (ctrl & (1u << 18)) ? isize : 0;
	c->itcm_rd_len = ((ctrl & (1u << 18)) && !(ctrl & (1u << 19))) ? isize : 0;
	/* Sin memoria conectada no hay TCM. */
	if (!c->dtcm) {
		c->dtcm_wr_len = c->dtcm_rd_len = 0;
	}
	if (!c->itcm) {
		c->itcm_wr_len = c->itcm_rd_len = 0;
	}
	if (c->tcm_hook)
		c->tcm_hook(c);
}

void acore_set_tcm_state(struct acore *c, uint32_t ctrl, uint32_t itcm_reg, uint32_t dtcm_reg)
{
	c->cp15_ctrl = (ctrl & CTRL_WRITABLE) | CTRL_FIXED | (ctrl & 0x2000u);
	c->cp15_itcm_reg = itcm_reg;
	c->cp15_dtcm_reg = dtcm_reg;
	update_tcm(c);
}

void acore_cp15_reset(struct acore *c)
{
	memset(c->cp15_misc, 0, sizeof c->cp15_misc);
	c->cp15_ctrl = CTRL_RESET;
	c->cp15_dtcm_reg = 0;
	c->cp15_itcm_reg = 0;
	update_tcm(c);
}

int acore_cp15_write(struct acore *c, uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t v)
{
	if (op1 != 0 && crn != 15)
		return -1;
	switch (crn) {
	case 1:
		if (crm == 0 && op2 == 0) {
			c->cp15_ctrl = (v & CTRL_WRITABLE) | CTRL_FIXED;
			update_tcm(c);
		}
		return 0;
	case 2:
		if (crm == 0 && op2 < 2)
			c->cp15_misc[M_C2_DATA + op2] = v & 0xFF;
		return 0;
	case 3:
		if (crm == 0 && op2 == 0)
			c->cp15_misc[M_C3_WBUF] = v & 0xFF;
		return 0;
	case 5:
		if (crm == 0 && op2 < 4)
			c->cp15_misc[M_C5_DATA + op2] = (op2 < 2) ? (v & 0xFFFF) : v;
		return 0;
	case 6:
		if (crm < 8 && op2 == 0)
			c->cp15_misc[M_C6 + crm] = v;
		return 0;
	case 7:
		/* c7,c0,4 y c7,c8,2: espera de interrupción. El resto (invalidar, limpiar, vaciar el búfer de
		 * escritura, precargar) son no-ops sin caché. */
		if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2))
			acore_halt(c);
		return 0;
	case 9:
		if (crm == 0 && op2 < 2) {
			c->cp15_misc[M_C9_DLOCK + op2] = v;
		} else if (crm == 1 && op2 == 0) {
			c->cp15_dtcm_reg = v;
			update_tcm(c);
		} else if (crm == 1 && op2 == 1) {
			c->cp15_itcm_reg = v;
			update_tcm(c);
		}
		return 0;
	case 13:
		if (crm == 0 && op2 == 1)
			c->cp15_misc[M_C13_PID] = v;
		return 0;
	default:
		return 0; /* c15 (pruebas de fábrica), c8 (TLB, no hay) y demás: se ignoran */
	}
}

int acore_cp15_read(struct acore *c, uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t *v)
{
	*v = 0;
	if (op1 != 0 && crn != 15)
		return -1;
	switch (crn) {
	case 0:
		*v = op2 == 1 ? CP15_CACHE_TYPE : op2 == 2 ? CP15_TCM_SIZE : CP15_ID;
		return 0;
	case 1:
		if (crm == 0 && op2 == 0)
			*v = c->cp15_ctrl;
		return 0;
	case 2:
		if (crm == 0 && op2 < 2)
			*v = c->cp15_misc[M_C2_DATA + op2];
		return 0;
	case 3:
		if (crm == 0 && op2 == 0)
			*v = c->cp15_misc[M_C3_WBUF];
		return 0;
	case 5:
		if (crm == 0 && op2 < 4)
			*v = c->cp15_misc[M_C5_DATA + op2];
		return 0;
	case 6:
		if (crm < 8 && op2 == 0)
			*v = c->cp15_misc[M_C6 + crm];
		return 0;
	case 9:
		if (crm == 0 && op2 < 2)
			*v = c->cp15_misc[M_C9_DLOCK + op2];
		else if (crm == 1 && op2 == 0)
			*v = c->cp15_dtcm_reg & 0xFFFFF03Eu;
		else if (crm == 1 && op2 == 1)
			*v = c->cp15_itcm_reg & 0x0000003Eu; /* la base de la ITCM es fija en 0 */
		return 0;
	case 13:
		if (crm == 0 && op2 == 1)
			*v = c->cp15_misc[M_C13_PID];
		return 0;
	default:
		return 0;
	}
}
