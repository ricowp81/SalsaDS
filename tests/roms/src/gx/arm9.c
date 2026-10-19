/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM gx: la FIFO de comandos del motor de geometría vista desde el ARM9, sin interpretar los comandos
 * (la geometría es de la Fase 3; en melonDS la IRQ 21 se pide por nivel): GXSTAT con la FIFO vacía, escrituras de comandos NOP por CPU y por DMA en modo 7
 * (FIFO de geometría), y la IRQ 21 en los modos «menos de la mitad» y «vacía». New Super Mario Bros. se quedaba
 * esperando que GXSTAT dijera «menos de la mitad» y que la IRQ 21 se repitiera mientras mandaba su lista.
 */
#include <nds.h>

#include "../common/hw.h"

#define R_GXFIFO 0x04000400u
#define R_GXSTAT 0x04000600u
#define BUFW ((volatile uint32_t *)0x02200000u)
#define IRQ_GX (1u << 21)

static void delay(uint32_t n)
{
	for (volatile uint32_t i = 0; i < n; i++)
		;
}

/* Espera con tope a que IF tenga el bit; devuelve 1 si llegó. */
static uint32_t wait_if(uint32_t bit)
{
	return WAIT_UNTIL(MEM32(R_IF) & bit, 400000u);
}

int main(void)
{
	sds_init("GX FIFO");
	sds_nocache();
	MEM32(R_IME) = 0;
	MEM16(R_POWCNT1) |= 0x0Cu;  /* motor de geometría y de render encendidos (sin eso el hardware ignora la FIFO) */

	sds_group("GXSTAT");
	delay(2000);
	SDS_CHECK((MEM32(R_GXSTAT) >> 24) & 7u, 6u);  /* bit 25 (menos de la mitad) y bit 26 (vacía); bit 24 y 27 en cero */
	SDS_CHECK(MEM32(R_GXSTAT) & 0xFFFFu, 0u);

	sds_group("NOP por CPU");
	for (uint32_t i = 0; i < 100; i++)
		MEM32(R_GXFIFO) = 0;  /* cuatro NOP por palabra */
	delay(20000);
	SDS_CHECK((MEM32(R_GXSTAT) >> 24) & 7u, 6u);

	sds_group("DMA modo 7");
	for (uint32_t i = 0; i < 300; i++)
		BUFW[i] = 0;
	MEM32(R_IF) = 1u << 8;
	MEM32(R_DMA(0, 0)) = 0x02200000u;
	MEM32(R_DMA(0, 1)) = R_GXFIFO;
	MEM32(R_DMA(0, 2)) = 0x80000000u | (1u << 30) | (7u << 27) | (2u << 21) | 300u;  /* IRQ, modo 7, destino fijo, 32 bits */
	SDS_CHECK(wait_if(1u << 8), 1u);
	SDS_CHECK(MEM32(R_DMA(0, 2)) >> 31, 0u);  /* sin repetición: se apagó */
	delay(30000);
	SDS_CHECK((MEM32(R_GXSTAT) >> 24) & 7u, 6u);

	sds_group("IRQ 21 modo 1");
	MEM32(R_IF) = IRQ_GX;
	MEM32(R_GXSTAT) = 1u << 30;  /* IRQ: menos de la mitad */
	SDS_CHECK(wait_if(IRQ_GX), 1u);
	MEM32(R_IF) = IRQ_GX;
	SDS_CHECK(wait_if(IRQ_GX), 1u);  /* se vuelve a pedir mientras la FIFO siga bajo la mitad (por nivel) */
	MEM32(R_GXFIFO) = 0;
	MEM32(R_IF) = IRQ_GX;
	SDS_CHECK(wait_if(IRQ_GX), 1u);

	sds_group("IRQ 21 modo 2");
	MEM32(R_GXSTAT) = 0;
	MEM32(R_IF) = IRQ_GX;
	MEM32(R_GXSTAT) = 2u << 30;  /* IRQ: vacía */
	SDS_CHECK(wait_if(IRQ_GX), 1u);
	MEM32(R_IF) = IRQ_GX;
	MEM32(R_GXSTAT) = 0;
	MEM32(R_GXFIFO) = 0;
	delay(20000);
	SDS_CHECK((MEM32(R_IF) & IRQ_GX) != 0, 0u);  /* con la IRQ apagada no hay aviso */
	sds_finish();
}
