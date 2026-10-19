/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — grupos de la ROM cpuarm que solo tienen sentido en la ARM9:
 * excepciones con los vectores en la ITCM (CP15 con V = 0) y CP15 con las TCM.
 */
#ifndef CPUARM_GROUPS9_H
#define CPUARM_GROUPS9_H

#include <stdint.h>
#include <string.h>

#include "../common/sds.h"
#include "cpuarm.h"

struct exc_rec exc_rec;

#define MRC15(crn, crm, op2)                                                     \
	({                                                                       \
		uint32_t v_;                                                     \
		__asm__ volatile("mrc p15, 0, %0, " #crn ", " #crm ", " #op2 : "=r"(v_)); \
		v_;                                                              \
	})
#define MCR15(crn, crm, op2, val) __asm__ volatile("mcr p15, 0, %0, " #crn ", " #crm ", " #op2 : : "r"(val) : "memory")

/* limpia e invalida toda la caché de datos (4 KB, 4 vías, líneas de 32 bytes: 4 segmentos de 32 conjuntos) */
static void dc_flush_all(void)
{
	for (uint32_t seg = 0; seg < 4; seg++)
		for (uint32_t idx = 0; idx < 32; idx++)
			__asm__ volatile("mcr p15, 0, %0, c7, c14, 2" : : "r"(seg << 30 | idx << 5) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

/* El PU de libnds deja sin acceso la página 0 y no cubre los espejos de las TCM: las pruebas que tocan esas
 * direcciones apagan el PU y las cachés (limpiando antes la de datos) y las restauran al terminar. */
static uint32_t pu_off(void)
{
	const uint32_t ctrl = MRC15(c1, c0, 0);

	dc_flush_all();
	MCR15(c1, c0, 0, ctrl & ~(1u | 4u | 0x1000u));
	return ctrl;
}

/* informative: si es verdadero, las comprobaciones son divergencias y no fallos (BKPT: melonDS 1.1 lo trata como
 * instrucción indefinida; el manual de ARM y GBATEK dicen aborto de captación) */
static void run_exc(void (*fn)(void), uint32_t kind, uint32_t mode, uint32_t lr_delta, int thumb, int informative)
{
	uint32_t probe[2] = { 0, 0 };
	const uint32_t base = (uint32_t)fn & ~1u;

	memset(&exc_rec, 0, sizeof exc_rec);
	exc_call(fn, probe);
#define EXC_CHECK(got, want) (informative ? SDS_DIVERGE(got, want) : SDS_CHECK(got, want))
	SDS_CHECK(exc_rec.count, 1);
	EXC_CHECK(exc_rec.kind, kind);
	/* el CPSR de antes (con T si la instrucción estaba en Thumb) queda en el SPSR del modo nuevo */
	EXC_CHECK(exc_rec.spsr, probe[0] | (thumb ? 0x20u : 0u));
	/* dentro del manejador: modo nuevo, I puesto, T apagado, F como estaba y las banderas intactas */
	EXC_CHECK(exc_rec.cpsr, (probe[0] & 0xF8000040u) | 0x80u | mode);
	EXC_CHECK(exc_rec.lr, base + lr_delta);
	SDS_CHECK(probe[1], probe[0]); /* tras volver, mismo modo, banderas y estado */
#undef EXC_CHECK
}

static void group_exceptions(void)
{
	uint32_t save[17], ctrl;

	sds_group("EXCEPCION");
	ctrl = pu_off();
	exc_install(save);
	run_exc(arm_exc_swi, 2, 0x13, 4, 0, 0);
	run_exc(arm_exc_und, 1, 0x1B, 4, 0, 0);
	run_exc(arm_exc_bkpt, 3, 0x17, 4, 0, 1);
	run_exc(th_exc_swi, 2, 0x13, 2, 1, 0);
	run_exc(th_exc_und, 1, 0x1B, 2, 1, 0);
	run_exc(th_exc_bkpt, 3, 0x17, 4, 1, 1);
	exc_uninstall(save);
	MCR15(c1, c0, 0, ctrl);
}

static void group_cp15(void)
{
	volatile uint32_t *p;
	uint32_t saved_d, saved_word, ctrl, ctrl_off;

	sds_group("CP15 TCM");
	SDS_CHECK(MRC15(c0, c0, 0), 0x41059461u);
	SDS_CHECK(MRC15(c0, c0, 1), 0x0F0D2112u);
	SDS_CHECK(MRC15(c0, c0, 2), 0x00140180u);
	ctrl = MRC15(c1, c0, 0);
	SDS_CHECK(ctrl & 0x78, 0x78); /* los bits 3-6 siempre valen 1 */

	/* Las pruebas siguientes usan direcciones que el PU de libnds no deja tocar: se apagan el PU y las cachés
	 * (limpiando antes la de datos) y se dejan las TCM como están. */
	(void)pu_off();
	ctrl_off = ctrl & ~(1u | 4u | 0x1000u);

	/* ITCM: 32 KB que se repiten a lo largo de su región (libnds la deja de 32 MB desde 0) */
	p = (volatile uint32_t *)0x00004100;
	saved_word = *p;
	*p = 0xA5A5C3C3u;
	SDS_CHECK(*(volatile uint32_t *)0x0000C100, 0xA5A5C3C3u);
	SDS_CHECK(*(volatile uint32_t *)0x01004100, 0xA5A5C3C3u);
	*(volatile uint32_t *)0x01004100 = 0x1234ABCDu;
	SDS_CHECK(*p, 0x1234ABCDu);
	*(volatile uint8_t *)0x00004102 = 0x66; /* un acceso de 8 bits en medio de la palabra, little-endian */
	SDS_CHECK(*p, 0x1266ABCDu);
	*(volatile uint16_t *)0x00004100 = 0x7777;
	SDS_CHECK(*p, 0x12667777u);
	*p = saved_word;

	/* DTCM: se mueve a la memoria principal, con una región de 64 KB que repite los 16 KB cuatro veces */
	saved_d = MRC15(c9, c1, 0);
	{
		volatile uint32_t *main_ram = (volatile uint32_t *)0x02200000;

		main_ram[4] = 0x11111111u;      /* el contenido de la memoria principal */
		main_ram[0x4000 / 4 + 4] = 0x22222222u;
		MCR15(c9, c1, 0, 0x0220000Eu);  /* base 0x02200000, tamaño 512 << 7 = 64 KB */
		SDS_CHECK(MRC15(c9, c1, 0), 0x0220000Eu);
		main_ram[4] = 0x12345678u;      /* ahora va a la DTCM */
		SDS_CHECK(main_ram[4], 0x12345678u);
		SDS_CHECK(main_ram[0x4000 / 4 + 4], 0x12345678u);   /* espejo cada 16 KB */
		SDS_CHECK(main_ram[0x8000 / 4 + 4], 0x12345678u);
		SDS_CHECK(main_ram[0xC000 / 4 + 4], 0x12345678u);
		main_ram[0xC000 / 4 + 4] = 0xCAFEBABEu;
		SDS_CHECK(main_ram[4], 0xCAFEBABEu);

		/* modo de carga (bit 17 del registro de control): según GBATEK las lecturas van a la memoria y las
		 * escrituras a la TCM. melonDS 1.1 no lo hace: sigue leyendo de la TCM (divergencia informativa) */
		MCR15(c1, c0, 0, ctrl_off | (1u << 17));
		SDS_DIVERGE(main_ram[4], 0x11111111u);
		main_ram[4] = 0x33333333u;
		SDS_DIVERGE(main_ram[4], 0x11111111u);
		MCR15(c1, c0, 0, ctrl_off);
		SDS_CHECK(main_ram[4], 0x33333333u);

		MCR15(c9, c1, 0, saved_d);      /* la DTCM de vuelta */
		SDS_CHECK(MRC15(c9, c1, 0), saved_d);
		SDS_CHECK(main_ram[4], 0x11111111u);
		SDS_CHECK(main_ram[0x4000 / 4 + 4], 0x22222222u);
	}
	MCR15(c1, c0, 0, ctrl);
}

#endif
