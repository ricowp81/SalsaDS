/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — declaraciones de las rutinas de helpers.S (ROM cpuarm).
 */
#ifndef CPUARM_H
#define CPUARM_H

#include <stdint.h>

/* ARM <-> Thumb */
extern uint32_t arm_ret42(void), arm_ret44(void);
extern uint32_t th_ret43(void), th_ret45(void);
extern uint32_t ldrpc_call(const void *const *pp);
extern uint32_t ldmpc_call(const void *const *pp);
extern uint32_t movpc_call(const void *target);
extern uint32_t th_poppc(const void *target);
extern uint32_t th_bx_arm(const void *target);
#ifdef ARM9
extern uint32_t th_blx_imm(void);
extern uint32_t th_blx_reg(const void *target);
#endif

/* Thumb: cargas (base, desplazamiento) */
typedef uint32_t (*th_load_fn)(const void *base, uint32_t off);
extern uint32_t th_t_ldr_i(const void *, uint32_t), th_t_ldrb_i(const void *, uint32_t), th_t_ldrh_i(const void *, uint32_t);
extern uint32_t th_t_ldr_r(const void *, uint32_t), th_t_ldrb_r(const void *, uint32_t), th_t_ldrh_r(const void *, uint32_t);
extern uint32_t th_t_ldrsb_r(const void *, uint32_t), th_t_ldrsh_r(const void *, uint32_t);
/* Thumb: escrituras (base, desplazamiento, valor) */
extern void th_t_str_r(void *, uint32_t, uint32_t), th_t_strh_r(void *, uint32_t, uint32_t), th_t_strb_r(void *, uint32_t, uint32_t);
extern void th_t_str_i(void *, uint32_t, uint32_t), th_t_strh_i(void *, uint32_t, uint32_t), th_t_strb_i(void *, uint32_t, uint32_t);

/* Thumb: registros altos, PC, SP, pila, bloques y saltos */
extern uint32_t th_t_hi_add(uint32_t, uint32_t), th_t_hi_noflags(uint32_t, uint32_t), th_t_hi_cmp(uint32_t, uint32_t);
extern uint32_t th_t_hi_cmp_lt(uint32_t, uint32_t), th_t_hi_mov(uint32_t);
extern uint32_t th_t_pcread_a(void), th_t_pcread_b(void), th_t_addpc_a(void), th_t_addpc_b(void);
extern uint32_t th_t_ldrpc_a(void), th_t_ldrpc_b(void), th_t_sp(void);
extern uint32_t th_t_pushpop(uint32_t *top), th_t_pushlr(void);
extern uint32_t th_t_stmia(uint32_t *buf), th_t_ldmia(uint32_t *buf), th_t_stmia_base(uint32_t *buf);
extern uint32_t th_t_ldmia_base_first(uint32_t *buf), th_t_ldmia_base_last(uint32_t *buf);
extern uint32_t th_t_b_far(void), th_t_b_back(void), th_t_bl_far(void), th_t_bl_lr(void);
extern const char th_t_bl_lr_next[];

/* ARM: LDM/STM con la base en la lista, listas vacías, PC, banco de usuario */
extern uint32_t arm_stm_base_notfirst(uint32_t *buf), arm_stm_base_first(uint32_t *buf);
extern uint32_t arm_ldm_base_first(uint32_t *buf), arm_ldm_base_last(uint32_t *buf), arm_ldm_base_only(uint32_t *buf);
extern uint32_t arm_stm_empty(uint32_t *buf, uint32_t *pc_of_insn), arm_ldm_empty(uint32_t *buf);
extern uint32_t arm_stm_pc(uint32_t *buf), arm_str_pc(uint32_t *buf);
extern void arm_user_bank(uint32_t *buf);

#ifdef ARM9
/* excepciones con los vectores en la ITCM */
struct exc_rec {
	uint32_t kind;   /* 1 indefinida, 2 SWI, 3 aborto de captación, 0xFF cualquier otra */
	uint32_t cpsr;   /* CPSR dentro del manejador */
	uint32_t spsr;
	uint32_t lr;
	uint32_t count;
};
extern struct exc_rec exc_rec;
extern void exc_install(uint32_t *save);
extern void exc_uninstall(const uint32_t *save);
extern void exc_call(void (*fn)(void), uint32_t *probe);
extern void arm_exc_swi(void), arm_exc_und(void), arm_exc_bkpt(void);
extern void th_exc_swi(void), th_exc_und(void), th_exc_bkpt(void);
#endif

#endif
