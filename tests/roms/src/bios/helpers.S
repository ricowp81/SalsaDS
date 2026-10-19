/* SPDX-License-Identifier: GPL-2.0-or-later */
@ SalsaDS — ROM bios: comprueba que las SWI de la BIOS conservan r4-r11 (la BIOS real solo toca r0-r3, r12 y lr).
@ Cada rutina pone valores distintos en r4-r11, llama a la SWI y devuelve una máscara: el bit i vale 1 si r(4+i)
@ cambió. Las SWI se llaman con el estado de r0-r3 que da el llamador en el caso del Halt.

	.arm
	.text

	.macro SENTINELS
	ldr r4, =0x44444444
	ldr r5, =0x55555555
	ldr r6, =0x66666666
	ldr r7, =0x77777777
	ldr r8, =0x88888888
	ldr r9, =0x99999999
	ldr r10, =0xAAAAAAAA
	ldr r11, =0xBBBBBBBB
	.endm

	.macro MASK
	mov r0, #0
	ldr r1, =0x44444444
	cmp r4, r1
	orrne r0, r0, #0x01
	ldr r1, =0x55555555
	cmp r5, r1
	orrne r0, r0, #0x02
	ldr r1, =0x66666666
	cmp r6, r1
	orrne r0, r0, #0x04
	ldr r1, =0x77777777
	cmp r7, r1
	orrne r0, r0, #0x08
	ldr r1, =0x88888888
	cmp r8, r1
	orrne r0, r0, #0x10
	ldr r1, =0x99999999
	cmp r9, r1
	orrne r0, r0, #0x20
	ldr r1, =0xAAAAAAAA
	cmp r10, r1
	orrne r0, r0, #0x40
	ldr r1, =0xBBBBBBBB
	cmp r11, r1
	orrne r0, r0, #0x80
	.endm

	.macro PROBE name, swinum, a0, a1, a2
	.global \name
	.type \name, %function
\name:
	push {r4-r11, lr}
	SENTINELS
	ldr r0, =\a0
	ldr r1, =\a1
	ldr r2, =\a2
	swi \swinum
	MASK
	pop {r4-r11, lr}
	bx lr
	.ltorg
	.endm

	PROBE probe_div, 0x090000, 100, 7, 0
	PROBE probe_sqrt, 0x0D0000, 1000, 0, 0
	PROBE probe_waitloop, 0x030000, 50, 0, 0
	PROBE probe_isdebugger, 0x0F0000, 0, 0, 0

@ uint32_t probe_halt(void): SWI 6 con r4-r11 (bits 0-7 de la máscara) y r0-r3 y r12 (bits 8-12) con valores conocidos; la IRQ que lo
@ despierta la prepara el llamador. GBATEK: en el ARM7 la SWI solo escribe HALTCNT y deja todos los registros como estaban.
	.global probe_halt
	.type probe_halt, %function
probe_halt:
	push {r4-r11, lr}
	SENTINELS
	ldr r0, =0x10101010
	ldr r1, =0x11111111
	ldr r2, =0x12121212
	ldr r3, =0x13131313
	ldr r12, =0x1C1C1C1C
	swi 0x060000
	push {r0-r3, r12}
	MASK
	mov r2, r0
	ldmia sp!, {r4-r8}
	ldr r1, =0x10101010
	cmp r4, r1
	orrne r2, r2, #0x100
	ldr r1, =0x11111111
	cmp r5, r1
	orrne r2, r2, #0x200
	ldr r1, =0x12121212
	cmp r6, r1
	orrne r2, r2, #0x400
	ldr r1, =0x13131313
	cmp r7, r1
	orrne r2, r2, #0x800
	ldr r1, =0x1C1C1C1C
	cmp r8, r1
	orrne r2, r2, #0x1000
	mov r0, r2
	pop {r4-r11, lr}
	bx lr
	.ltorg

@ uint32_t probe_cpuset(uint32_t src, uint32_t dst, uint32_t cnt)
	.global probe_cpuset
	.type probe_cpuset, %function
probe_cpuset:
	push {r4-r11, lr}
	mov r12, r0
	mov r3, r1
	mov lr, r2
	SENTINELS
	mov r0, r12
	mov r1, r3
	mov r2, lr
	swi 0x0B0000
	MASK
	pop {r4-r11, lr}
	bx lr
	.ltorg
