/*
 * SalsaDS — rutinas en ensamblador de la ROM cpuarm (ARM9 y ARM7). Se
 * ensambla para las dos CPUs: lo que solo existe en ARMv5TE va dentro de
 * `#ifdef ARM9`. Cada rutina devuelve un valor o escribe resultados en un
 * búfer que el código en C compara con constantes verificadas a mano (y
 * contra melonDS).
 *
 * Convención: las rutinas Thumb son `.thumb_func` y se llaman desde C por
 * puntero; no tocan r4-r11 (la ABI de ARM) y vuelven con `bx`, no con
 * `pop {pc}`, porque en ARMv4T este último no cambia de estado.
 */
	.syntax unified
	.text

/* ======================================================================
 * Cambio de estado ARM <-> Thumb
 * ====================================================================== */
	.arm
	.balign 4

	.global arm_ret42
arm_ret42:
	mov r0, #42
	bx lr

	.global arm_ret44
arm_ret44:
	mov r0, #44
	bx lr

/* uint32_t ldrpc_call(const void *const *pp): LDR pc, [r0]; el destino vuelve con bx lr */
	.global ldrpc_call
ldrpc_call:
	push {lr}
	adr lr, 1f
	ldr pc, [r0]
1:	pop {pc}

/* igual con LDM {pc} */
	.global ldmpc_call
ldmpc_call:
	push {lr}
	adr lr, 1f
	ldmia r0, {pc}
1:	pop {pc}

/* igual con MOV pc, r0: no cambia de estado en ninguna versión (el bit 0 se ignora) */
	.global movpc_call
movpc_call:
	push {lr}
	adr lr, 1f
	mov pc, r0
1:	pop {pc}

	.thumb
	.thumb_func
	.global th_ret43
th_ret43:
	movs r0, #43
	bx lr

	.thumb_func
	.global th_ret45
th_ret45:
	movs r0, #45
	bx lr

/* POP {pc}: ARMv5 cambia de estado con el bit 0 del valor; ARMv4 se queda en Thumb */
	.thumb_func
	.global th_poppc
th_poppc:
	push {r0}
	pop {pc}

/* Thumb -> ARM con BX (r0 = destino ARM, sin el bit 0) */
	.thumb_func
	.global th_bx_arm
th_bx_arm:
	bx r0

#ifdef ARM9
/* BLX con inmediato desde Thumb a una rutina ARM (solo ARMv5); LR = dirección siguiente | 1 */
	.thumb_func
	.global th_blx_imm
th_blx_imm:
	push {lr}
	blx arm_ret42
	pop {r3}
	bx r3

/* BLX por registro desde Thumb */
	.thumb_func
	.global th_blx_reg
th_blx_reg:
	push {lr}
	blx r0
	pop {r3}
	bx r3
#endif

/* ======================================================================
 * Thumb: formatos 5 a 19
 * ====================================================================== */
#define T1(name, insn...) \
	.thumb_func;       \
	.global name;      \
name:                      \
	insn;              \
	bx lr

	.thumb
	.balign 4
/* formatos 7, 8, 9, 10: carga y almacenamiento; r0 = base, r1 = desplazamiento */
T1(th_t_ldr_i, ldr r0, [r0, #12])
T1(th_t_ldrb_i, ldrb r0, [r0, #9])
T1(th_t_ldrh_i, ldrh r0, [r0, #10])
T1(th_t_ldr_r, ldr r0, [r0, r1])
T1(th_t_ldrb_r, ldrb r0, [r0, r1])
T1(th_t_ldrh_r, ldrh r0, [r0, r1])
T1(th_t_ldrsb_r, ldrsb r0, [r0, r1])
T1(th_t_ldrsh_r, ldrsh r0, [r0, r1])
/* escrituras: r0 = base, r1 = desplazamiento, r2 = valor */
T1(th_t_str_r, str r2, [r0, r1])
T1(th_t_strh_r, strh r2, [r0, r1])
T1(th_t_strb_r, strb r2, [r0, r1])
T1(th_t_str_i, str r2, [r0, #20])
T1(th_t_strh_i, strh r2, [r0, #22])
T1(th_t_strb_i, strb r2, [r0, #27])

/* formato 5: operaciones con registros altos */
T1(th_t_hi_add, mov r12, r0; add r12, r1; mov r0, r12)
	/* ADD con registros altos no toca las banderas: Z (de movs) sigue puesto */
	.thumb_func
	.global th_t_hi_noflags
th_t_hi_noflags:
	movs r2, #0
	mov r12, r1
	add r12, r0
	beq 1f
	movs r0, #0
	bx lr
1:	mov r0, r12
	bx lr
	/* CMP con registros altos sí cambia las banderas */
	.thumb_func
	.global th_t_hi_cmp
th_t_hi_cmp:
	mov r12, r0
	cmp r12, r1
	beq 1f
	movs r0, #0
	bx lr
1:	movs r0, #1
	bx lr
	.thumb_func
	.global th_t_hi_cmp_lt
th_t_hi_cmp_lt:
	mov r12, r0
	cmp r12, r1
	blt 1f
	movs r0, #0
	bx lr
1:	movs r0, #1
	bx lr
	/* MOV con registros altos: copia de r8 a r0 pasando por r12, sin banderas */
T1(th_t_hi_mov, mov r12, r0; mov r1, r12; adds r0, r1, #0)

/* el PC se lee como la dirección de la instrucción + 4 */
	.balign 4
	.thumb_func
	.global th_t_pcread_a
th_t_pcread_a:
	mov r0, pc
	bx lr
	.hword 0x46C0 /* nop */
	.thumb_func
	.global th_t_pcread_b
th_t_pcread_b:
	mov r0, pc
	bx lr

/* ADD Rd, PC, #imm: (PC & ~3) + imm con PC = A + 4 */
	.balign 4
	.thumb_func
	.global th_t_addpc_a
th_t_addpc_a:
	add r0, pc, #8
	bx lr
	.hword 0x46C0
	.thumb_func
	.global th_t_addpc_b
th_t_addpc_b:
	add r0, pc, #8
	bx lr

/* LDR Rd, [PC, #imm]: la base se alinea a palabra */
	.balign 4
	.thumb_func
	.global th_t_ldrpc_a
th_t_ldrpc_a:
	ldr r0, [pc, #4]
	bx lr
	.word 0xAAAA0001
	.word 0x12345678
	.hword 0x46C0
	.thumb_func
	.global th_t_ldrpc_b
th_t_ldrpc_b:
	ldr r0, [pc, #4]
	bx lr
	.hword 0
	.word 0x9ABCDEF0

/* formato 13: ADD/SUB SP, #imm; formato 12: ADD Rd, SP, #imm. Devuelve sp_nuevo - sp_antiguo, etc. */
	.thumb_func
	.global th_t_sp
th_t_sp:
	mov r1, sp          @ sp antes
	sub sp, #32
	mov r2, sp
	subs r0, r2, r1     @ -32
	add r3, sp, #12     @ sp + 12
	subs r3, r3, r2     @ 12
	lsls r3, r3, #8
	adds r0, r0, r3     @ -32 + 12*256
	str r1, [sp, #4]    @ [sp+4] = sp antes
	ldr r2, [sp, #4]
	subs r2, r2, r1     @ 0
	adds r0, r0, r2
	add sp, #32
	bx lr

/* formato 14: PUSH/POP. r0 = tope de una zona de pila (alineado). Devuelve sp_final - tope. */
	.thumb_func
	.global th_t_pushpop
th_t_pushpop:
	push {r4, r5, lr}
	mov r4, sp
	mov sp, r0
	movs r1, #0x11
	movs r2, #0x22
	movs r3, #0x33
	push {r1, r2, r3}   @ r0[-3] = 0x11, r0[-2] = 0x22, r0[-1] = 0x33
	movs r1, #0
	movs r2, #0
	pop {r1, r2}        @ r1 = 0x11, r2 = 0x22; sp = tope - 4
	lsls r1, r1, #8
	adds r1, r1, r2     @ 0x1122
	mov r5, sp
	subs r5, r5, r0     @ -4
	mov sp, r4
	lsls r5, r5, #16    @ el desplazamiento en los 16 altos
	adds r0, r1, r5
	pop {r4, r5}
	pop {r3}
	bx r3

/* PUSH {lr} / POP {r3}: ida y vuelta de lr; devuelve lr == valor recuperado */
	.thumb_func
	.global th_t_pushlr
th_t_pushlr:
	mov r1, lr
	push {lr}
	pop {r2}
	movs r0, #0
	cmp r1, r2
	bne 1f
	movs r0, #1
1:	bx lr

/* formato 15: STMIA/LDMIA con escritura de vuelta; r0 = búfer de 4 palabras. Devuelve la base final - r0 en
 * los 16 bajos y deja los datos en el búfer. */
	.thumb_func
	.global th_t_stmia
th_t_stmia:
	push {r4}
	adds r4, r0, #0
	movs r1, #0x11
	movs r2, #0x22
	movs r3, #0x33
	stmia r4!, {r1, r2, r3}
	subs r4, r4, r0
	adds r0, r4, #0
	pop {r4}
	bx lr

	.thumb_func
	.global th_t_ldmia
th_t_ldmia:
	push {r4}
	adds r4, r0, #0
	ldmia r4!, {r1, r2, r3}
	subs r4, r4, r0     @ 12
	lsls r4, r4, #24
	lsls r1, r1, #16
	lsls r2, r2, #8
	adds r0, r1, r2
	adds r0, r0, r3
	adds r0, r0, r4
	pop {r4}
	bx lr

/* base en la lista: STMIA r2!, {r1, r2} (la base no es la primera): ARMv4 guarda la nueva, ARMv5 la original.
 * r0 = búfer. Devuelve la base final - r0. */
	.thumb_func
	.global th_t_stmia_base
th_t_stmia_base:
	adds r2, r0, #0
	movs r1, #0x55
	.hword 0xC206           @ stmia r2!, {r1, r2} (el ensamblador no lo admite)
	subs r2, r2, r0
	adds r0, r2, #0
	bx lr

/* LDMIA r2!, {r2, r3}: la base es la primera de la lista. ARMv4: sin escritura de vuelta (queda lo cargado);
 * ARMv5: escritura de vuelta (base + 8). r0 = búfer con dos palabras. Devuelve r2 - r0 si es base + 8,
 * o el valor cargado. */
	.thumb_func
	.global th_t_ldmia_base_first
th_t_ldmia_base_first:
	adds r2, r0, #0
	.hword 0xCA0C           @ ldmia r2!, {r2, r3}
	adds r0, r2, #0
	bx lr

/* LDMIA r2!, {r1, r2}: la base es la última de la lista: queda lo cargado en las dos versiones */
	.thumb_func
	.global th_t_ldmia_base_last
th_t_ldmia_base_last:
	adds r2, r0, #0
	.hword 0xCA06           @ ldmia r2!, {r1, r2}
	adds r0, r2, #0
	bx lr

/* formatos 16 y 18: saltos. B hacia delante 1500 bytes y BL a 0x2400 bytes en los dos sentidos. */
	.thumb_func
	.global th_t_b_far
th_t_b_far:
	movs r0, #1
	b 1f
	.space 1500
	movs r0, #2
1:	bx lr

	.thumb_func
	.global th_t_b_back
th_t_b_back:
	b 2f
1:	bx lr
	.space 1500
2:	movs r0, #7
	b 1b

	.thumb_func
	.global th_t_far_back
th_t_far_back:
	movs r0, #0x10
	lsls r0, r0, #8     @ 0x1000
	bx lr
	.space 0x2400

	.thumb_func
	.global th_t_bl_far
th_t_bl_far:
	push {r4, lr}
	bl th_t_far_back
	movs r4, r0
	bl th_t_far_fwd
	adds r0, r0, r4     @ 0x1000 + 0x234
	pop {r4}
	pop {r3}
	bx r3
	.space 0x2400

	.thumb_func
	.global th_t_far_fwd
th_t_far_fwd:
	ldr r0, =0x234
	bx lr
	.pool

/* BL deja en LR la dirección de la siguiente instrucción con el bit 0: devuelve ese LR */
	.balign 4
	.thumb_func
	.global th_t_bl_lr
th_t_bl_lr:
	push {lr}
	bl th_t_getlr
	.global th_t_bl_lr_next
th_t_bl_lr_next:
	pop {r3}
	bx r3

	.thumb_func
th_t_getlr:
	mov r0, lr
	bx lr

/* ======================================================================
 * ARM: casos de LDM/STM con la base en la lista, listas vacías y STM/LDM con PC
 * ====================================================================== */
	.arm
	.balign 4

/* STMIA r4!, {r3, r4}: la base no es la primera. r0 = búfer. Devuelve la base final - r0. */
	.global arm_stm_base_notfirst
arm_stm_base_notfirst:
	push {r4}
	mov r4, r0
	mov r3, #0x77
	stmia r4!, {r3, r4}
	sub r0, r4, r0
	pop {r4}
	bx lr

/* STMIA r4!, {r4, r5}: la base es la primera */
	.global arm_stm_base_first
arm_stm_base_first:
	push {r4, r5}
	mov r4, r0
	mov r5, #0x77
	stmia r4!, {r4, r5}
	sub r0, r4, r0
	pop {r4, r5}
	bx lr

/* LDMIA r4!, {r4, r5}: la base es la primera de la lista */
	.global arm_ldm_base_first
arm_ldm_base_first:
	push {r4, r5}
	mov r4, r0
	ldmia r4!, {r4, r5}
	mov r0, r4
	pop {r4, r5}
	bx lr

/* LDMIA r4!, {r3, r4}: la base es la última */
	.global arm_ldm_base_last
arm_ldm_base_last:
	push {r3, r4}
	mov r4, r0
	ldmia r4!, {r3, r4}
	mov r0, r4
	pop {r3, r4}
	bx lr

/* LDMIA r4!, {r4}: la base es la única */
	.global arm_ldm_base_only
arm_ldm_base_only:
	push {r4}
	mov r4, r0
	ldmia r4!, {r4}
	mov r0, r4
	pop {r4}
	bx lr

/* lista vacía: STMIA r4!, {} (se escribe a mano la codificación). r0 = búfer, r1 = puntero a escribir
 * el valor del PC esperado en ARMv4. Devuelve la base final - r0. */
	.global arm_stm_empty
arm_stm_empty:
	push {r4}
	mov r4, r0
	adr r2, 1f
	str r2, [r1]
1:	.word 0xE8A40000    @ stmia r4!, {}
	sub r0, r4, r0
	pop {r4}
	bx lr

/* LDMIA r4!, {} : ARMv4 carga el PC desde [base] (se prepara para caer en 3f); ARMv5 no carga nada.
 * Devuelve 1 si siguió de largo (ARMv5), 2 si saltó (ARMv4), y la base final - r0 en los 16 altos. */
	.global arm_ldm_empty
arm_ldm_empty:
	push {r4, r5}
	mov r4, r0
	adr r2, 3f
	str r2, [r0]
	.word 0xE8B40000    @ ldmia r4!, {}
	mov r5, #1
	b 2f
3:	mov r5, #2
2:	sub r4, r4, r0
	mov r0, r5
	orr r0, r0, r4, lsl #16
	pop {r4, r5}
	bx lr

/* STMIA r0, {pc}: guarda la dirección de la instrucción + 12. Devuelve (valor guardado) - (dirección de la
 * instrucción). */
	.global arm_stm_pc
arm_stm_pc:
	adr r2, 1f
1:	stmia r0, {pc}
	ldr r1, [r0]
	sub r0, r1, r2
	bx lr

/* STR pc, [r0]: también guarda la dirección + 12 */
	.global arm_str_pc
arm_str_pc:
	adr r2, 1f
1:	str pc, [r0]
	ldr r1, [r0]
	sub r0, r1, r2
	bx lr

/* STM^ / LDM^ con el banco de usuario desde SVC. r0 = búfer de 6 palabras:
 *   buf[0], buf[1] <- r13 y r14 de usuario (puestos aquí a 0x13579BDF y 0x2468ACE0) leídos con STM^
 *   buf[2], buf[3] -> se cargan con LDM^ en r13 y r14 de usuario
 *   buf[4], buf[5] <- r13 y r14 de usuario releídos desde el modo sistema tras el LDM^ */
	.global arm_user_bank
arm_user_bank:
	push {r4-r6, lr}
	mrs r6, cpsr
	msr cpsr_c, #0xDF
	mov r4, sp
	mov r5, lr
	ldr sp, =0x13579BDF
	ldr lr, =0x2468ACE0
	msr cpsr_c, #0xD3
	stmia r0, {sp, lr}^
	add r1, r0, #8
	ldmia r1, {sp, lr}^
	nop
	msr cpsr_c, #0xDF
	str sp, [r0, #16]
	str lr, [r0, #20]
	mov sp, r4
	mov lr, r5
	msr cpsr_c, r6
	pop {r4-r6, pc}
	.ltorg

/* ======================================================================
 * Solo ARM9: excepciones con los vectores en la ITCM (CP15 con V = 0) y CP15
 * ====================================================================== */
#ifdef ARM9
	.arm
	.balign 4

/* Tabla de vectores: ocho `ldr pc, [pc, #0x18]` y debajo, a 0x20, las direcciones de los manejadores */
	.global exc_vectors
exc_vectors:
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	ldr pc, [pc, #0x18]
	.word exc_h_unexpected  @ reinicio
	.word exc_h_und
	.word exc_h_swi
	.word exc_h_pabt
	.word exc_h_unexpected  @ aborto de datos
	.word exc_h_unexpected
	.word exc_h_unexpected  @ IRQ
	.word exc_h_unexpected  @ FIQ

/* Registro que llenan los manejadores: kind, cpsr, spsr, lr, cuenta (struct exc_rec en cpuarm.h) */
exc_h_und:
	ldr sp, =exc_rec
	mov r12, #1
	b exc_h_common
exc_h_swi:
	ldr sp, =exc_rec
	mov r12, #2
	b exc_h_common
exc_h_pabt:
	ldr sp, =exc_rec
	mov r12, #3
	b exc_h_common
exc_h_unexpected:
	ldr sp, =exc_rec
	mov r12, #0xFF
exc_h_common:
	str r12, [sp, #0]
	mrs r12, cpsr
	str r12, [sp, #4]
	mrs r12, spsr
	str r12, [sp, #8]
	str lr, [sp, #12]
	ldr r12, [sp, #16]
	add r12, r12, #1
	str r12, [sp, #16]
	movs pc, lr
	.ltorg

/* void exc_install(uint32_t *save): save[0..15] <- los 64 bytes de la ITCM en 0; save[16] <- registro de control.
 * Copia la tabla de vectores a 0 y deja V = 0. (El PU y las cachés los apaga quien llama.) */
	.global exc_install
exc_install:
	push {r4, lr}
	mrc p15, 0, r4, c1, c0, 0
	str r4, [r0, #64]
	mov r1, #0
	mov r2, #16
1:	ldr r3, [r1], #4
	str r3, [r0], #4
	subs r2, r2, #1
	bne 1b
	ldr r0, =exc_vectors
	mov r1, #0
	mov r2, #16
2:	ldr r3, [r0], #4
	str r3, [r1], #4
	subs r2, r2, #1
	bne 2b
	bic r0, r4, #0x2000
	mcr p15, 0, r0, c1, c0, 0
	pop {r4, pc}
	.ltorg

/* void exc_uninstall(const uint32_t *save): lo contrario de exc_install */
	.global exc_uninstall
exc_uninstall:
	mov r2, #0
	mov r3, #16
	mov r12, r0
1:	ldr r1, [r12], #4
	str r1, [r2], #4
	subs r3, r3, #1
	bne 1b
	ldr r1, [r0, #64]
	mcr p15, 0, r1, c1, c0, 0
	bx lr

/* void exc_call(void (*fn)(void), uint32_t *probe): llama a fn (ARM o Thumb) con NZCV = 1010 y guarda el
 * CPSR de antes y de después en probe[0] y probe[1] */
	.global exc_call
exc_call:
	push {r4, lr}
	msr cpsr_f, #0xA0000000
	mrs r2, cpsr
	str r2, [r1, #0]
	adr lr, 1f
	bx r0
1:	mrs r2, cpsr
	str r2, [r1, #4]
	pop {r4, pc}

/* Casos: la instrucción que provoca la excepción está en fn + 0 */
	.global arm_exc_swi
arm_exc_swi:
	swi 0x123456
	bx lr

	.global arm_exc_und
arm_exc_und:
	.word 0xE7F000F0    @ UDF
	bx lr

	.global arm_exc_bkpt
arm_exc_bkpt:
	.word 0xE1200073    @ bkpt #0x13
	bx lr

	.thumb
	.thumb_func
	.global th_exc_swi
th_exc_swi:
	swi 0x42
	bx lr

	.thumb_func
	.global th_exc_und
th_exc_und:
	.hword 0xDE00       @ udf #0
	bx lr

	.thumb_func
	.global th_exc_bkpt
th_exc_bkpt:
	bkpt 0x12
	bx lr               @ se salta: la vuelta del aborto de captación es A + 4
	bx lr
	.arm
#endif
