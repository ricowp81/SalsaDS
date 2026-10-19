/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — registros y ayudas de hardware para las ROMs de prueba de la Fase 1.2 (ARM9 y ARM7). Solo
 * direcciones de GBATEK; no usa libnds salvo donde la ROM lo incluye aparte.
 */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#include "sds.h"

#define REG8(a) (*(volatile uint8_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define MEM8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define MEM16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* E/S (ambas CPUs salvo que se diga) */
#define R_DISPSTAT 0x04000004u
#define R_VCOUNT 0x04000006u
#define R_DMA(ch, r) (0x040000B0u + 12u * (ch) + 4u * (r)) /* r: 0 SAD, 1 DAD, 2 CNT */
#define R_DMAFILL(ch) (0x040000E0u + 4u * (ch))              /* ARM9 */
#define R_TM_L(i) (0x04000100u + 4u * (i))
#define R_TM_H(i) (0x04000102u + 4u * (i))
#define R_KEYINPUT 0x04000130u
#define R_KEYCNT 0x04000132u
#define R_EXTKEYIN 0x04000136u /* ARM7 */
#define R_RTC 0x04000138u      /* ARM7 */
#define R_IPCSYNC 0x04000180u
#define R_IPCFIFOCNT 0x04000184u
#define R_IPCFIFOSEND 0x04000188u
#define R_IPCFIFORECV 0x04100000u
#define R_AUXSPICNT 0x040001A0u
#define R_AUXSPIDATA 0x040001A2u
#define R_ROMCTRL 0x040001A4u
#define R_ROMCMD 0x040001A8u
#define R_ROMDATA 0x04100010u
#define R_SPICNT 0x040001C0u /* ARM7 */
#define R_SPIDATA 0x040001C2u
#define R_EXMEMCNT 0x04000204u
#define R_IME 0x04000208u
#define R_IE 0x04000210u
#define R_IF 0x04000214u
#define R_VRAMCNT(i) (0x04000240u + (i)) /* ARM9: A-G = 0-6; H = 8, I = 9 */
#define R_VRAMSTAT 0x04000240u            /* ARM7 */
#define R_WRAMCNT 0x04000247u             /* ARM9 */
#define R_WRAMSTAT 0x04000241u            /* ARM7 */
#define R_DIVCNT 0x04000280u
#define R_DIV_NUMER 0x04000290u
#define R_DIV_DENOM 0x04000298u
#define R_DIV_RESULT 0x040002A0u
#define R_DIVREM_RESULT 0x040002A8u
#define R_SQRTCNT 0x040002B0u
#define R_SQRT_RESULT 0x040002B4u
#define R_SQRT_PARAM 0x040002B8u
#define R_POSTFLG 0x04000300u
#define R_POWCNT1 0x04000304u
#define R_DISPCNT_A 0x04000000u
#define R_DISPCNT_B 0x04001000u

#define IRQB_VBLANK (1u << 0)
#define IRQB_HBLANK (1u << 1)
#define IRQB_VCOUNT (1u << 2)
#define IRQB_TIMER(i) (1u << (3 + (i)))
#define IRQB_DMA(i) (1u << (8 + (i)))
#define IRQB_IPCSYNC (1u << 16)
#define IRQB_IPCSEND (1u << 17)
#define IRQB_IPCRECV (1u << 18)
#define IRQB_CARDDONE (1u << 19)

#define MRC15(crn, crm, op2)                                                     \
	({                                                                       \
		uint32_t v_;                                                     \
		__asm__ volatile("mrc p15, 0, %0, " #crn ", " #crm ", " #op2 : "=r"(v_)); \
		v_;                                                              \
	})
#define MCR15(crn, crm, op2, val) __asm__ volatile("mcr p15, 0, %0, " #crn ", " #crm ", " #op2 : : "r"(val) : "memory")

#ifdef ARM9
/* Limpia la caché de datos y apaga el PU y las cachés: lo que escribe el ARM7 en la memoria principal se ve al
 * instante y las pruebas pueden tocar direcciones que el PU de libnds no deja. */
static inline void sds_nocache(void)
{
	const uint32_t ctrl = MRC15(c1, c0, 0);

	for (uint32_t seg = 0; seg < 4; seg++)
		for (uint32_t idx = 0; idx < 32; idx++)
			__asm__ volatile("mcr p15, 0, %0, c7, c14, 2" : : "r"(seg << 30 | idx << 5) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
	MCR15(c1, c0, 0, ctrl & ~(1u | 4u | 0x1000u));
}
#endif

/* Buzón entre las dos CPUs, en RAM principal (sin caché en el ARM9 tras sds_nocache). */
struct mbox {
	volatile uint32_t cmd;
	volatile uint32_t arg[6];
	volatile uint32_t res[6];
	volatile uint32_t done;
};
#define MBOX ((struct mbox *)0x02390800u)

#define CMD_QUIT 0xFFu

#ifdef ARM9
/* Pide algo al ARM7 y espera la respuesta (con tope); devuelve 1 si contestó. */
static inline int mb_call(uint32_t cmd, uint32_t a0, uint32_t a1, uint32_t a2)
{
	uint32_t spin = 0;

	MBOX->arg[0] = a0;
	MBOX->arg[1] = a1;
	MBOX->arg[2] = a2;
	MBOX->done = 0;
	MBOX->cmd = cmd;
	while (!MBOX->done && spin < 4000000u) {
		spin++;
		__asm__ volatile("" ::: "memory");
	}
	return MBOX->done != 0;
}
#endif

/* Lectura que el compilador no puede tomar por un puntero nulo (dirección 0 = BIOS del ARM7). */
static inline uint32_t rd32_raw(uint32_t addr)
{
	uint32_t v;

	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
	return v;
}

/* Espera con tope a que una condición se cumpla; evalúa a 1 si se cumplió. */
#define WAIT_UNTIL(cond, limit)                                \
	({                                                     \
		uint32_t i_ = 0;                                \
		while (!(cond) && i_ < (limit))                 \
			i_++;                                   \
		(cond) ? 1u : 0u;                               \
	})

#endif
