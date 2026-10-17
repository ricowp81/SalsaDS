/*
 * SalsaDS — ROM ipc, lado ARM9: IPCSYNC (4 bits por sentido, IRQ) y las dos FIFO (16 palabras, banderas,
 * error, vaciado, IRQ por flanco). El ARM9 dirige: manda órdenes al ARM7 por el buzón de RAM y prueba el
 * IPC desde los dos lados (las órdenes están en arm7.c).
 */
#include <nds.h>

#include "../common/hw.h"
#include "ipc.h"

static volatile uint32_t n_sync, n_send, n_recv;

static void h_sync(void) { n_sync++; }
static void h_send(void) { n_send++; }
static void h_recv(void)
{
	n_recv++;
	/* las palabras que lleguen en una prueba de IRQ se dejan en la cola; este manejador solo cuenta */
}

static int call(uint32_t cmd, uint32_t a0, uint32_t a1)
{
	return mb_call(cmd, a0, a1, 0);
}

static void group_sync(void)
{
	sds_group("SYNC");
	REG16(R_IPCSYNC) = 0;
	SDS_CHECK(call(C_SYNC_SET, 0x5, 0), 1);
	SDS_CHECK(REG16(R_IPCSYNC) & 0xF, 5); /* lo que escribe el ARM7 se lee en los bits 0-3 */
	REG16(R_IPCSYNC) = 0x0700;
	SDS_CHECK(REG16(R_IPCSYNC) & 0x0F00, 0x0700);
	SDS_CHECK(call(C_SYNC_GET, 0, 0), 1);
	SDS_CHECK(MBOX->res[0] & 0xF, 7); /* y el ARM7 lo ve en los suyos */
	REG16(R_IPCSYNC) = 0xFFFF;
	SDS_CHECK(REG16(R_IPCSYNC) & 0xFF00, 0x4F00 | 0); /* se escriben 8-11 y 14; el 13 no se lee */
	SDS_CHECK(REG16(R_IPCSYNC) & 0xB0F0, 0);
	/* IRQ del ARM7 al ARM9 */
	REG16(R_IPCSYNC) = 0;
	irqSet(IRQ_IPC_SYNC, h_sync); /* libnds (irqSet e irqEnable) también enciende el bit 14 de IPCSYNC: se apaga */
	MEM32(R_IE) |= IRQ_IPC_SYNC;
	REG16(R_IPCSYNC) = 0;
	n_sync = 0;
	SDS_CHECK(call(C_SYNC_PULSE, 1, 0), 1);
	for (volatile int i = 0; i < 3000; i++)
		;
	SDS_CHECK(n_sync, 0); /* mi bit 14 está apagado: no llega */
	REG16(R_IPCSYNC) = 0x4000;
	SDS_CHECK(call(C_SYNC_PULSE, 2, 0), 1);
	SDS_CHECK(WAIT_UNTIL(n_sync != 0, 200000), 1);
	SDS_CHECK(n_sync, 1);
	SDS_CHECK(REG16(R_IPCSYNC) & 0xF, 2); /* el pulso lleva su dato */
	REG16(R_IPCSYNC) = 0;
	/* del ARM9 al ARM7: el ARM7 habilita su bit 14 y cuenta */
	SDS_CHECK(call(C_SYNC_ARM, 1, 0), 1);
	REG16(R_IPCSYNC) = 0x2000 | 0x0300; /* pulso con dato 3 */
	for (volatile int i = 0; i < 3000; i++)
		;
	SDS_CHECK(call(C_SYNC_COUNT, 0, 0), 1);
	SDS_CHECK(MBOX->res[0], 1);
	SDS_CHECK(MBOX->res[1] & 0xF, 3);
	REG16(R_IPCSYNC) = 0;
	MEM32(R_IE) &= ~IRQ_IPC_SYNC;
}

static void group_fifo(void)
{
	uint32_t v;

	sds_group("FIFO");
	irqSet(IRQ_SEND_FIFO, h_send);
	irqSet(IRQ_RECV_FIFO, h_recv);
	REG16(R_IPCFIFOCNT) = 0x4008; /* vaciar la cola de envío y reconocer el error */
	SDS_CHECK(call(C_FIFO_CNT, 0xC008, 0), 1); /* el ARM7 igual, habilitada */
	/* FIFO del ARM9 deshabilitada: escribir se descarta */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x8000, 0);
	REG32(R_IPCFIFOSEND) = 0x11111111u;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 3, 1); /* sigue vacía */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0); /* sin error */
	/* habilitada */
	REG16(R_IPCFIFOCNT) = 0x8000;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x8303, 0x8101); /* enviar: vacía (1); recibir: vacía (0x100) */
	REG32(R_IPCFIFOSEND) = 0xA0000001u;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 3, 0); /* ya no está vacía, ni llena */
	for (uint32_t i = 2; i <= 16; i++)
		REG32(R_IPCFIFOSEND) = 0xA0000000u + i;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 3, 2); /* llena */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0);
	REG32(R_IPCFIFOSEND) = 0xBADBADBAu; /* la 17: error y se pierde */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0x4000);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x4000; /* escribir 1 reconoce el error */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0);
	/* el ARM7 lee las 16 en orden y la 17 da error */
	SDS_CHECK(call(C_FIFO_RECV, 17, 0), 1);
	SDS_CHECK(MBOX->res[0], 16); /* palabras leídas con valor (en orden) */
	/* lo que devuelve la lectura de una cola vacía: GBATEK dice la última palabra recibida; melonDS 1.1 devuelve lo que
	 * quedó en esa posición del anillo (informativo) */
	SDS_DIVERGE(MBOX->res[1], 0xA0000010u);
	SDS_CHECK(MBOX->res[2] & 0x4000, 0x4000); /* error en el ARM7 por leer vacía */
	SDS_CHECK(MBOX->res[3], 0); /* 0 = todas llegaron en orden */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 3, 1); /* mi cola quedó vacía */

	/* vaciado con el bit 3 */
	REG32(R_IPCFIFOSEND) = 0xC0000001u;
	REG32(R_IPCFIFOSEND) = 0xC0000002u;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 1, 0);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0008;
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 1, 1);
	SDS_CHECK(call(C_FIFO_RECV, 1, 0), 1);
	SDS_CHECK(MBOX->res[0], 0); /* al ARM7 no le llegó nada */

	/* del ARM7 al ARM9 */
	SDS_CHECK(call(C_FIFO_SEND, 3, 0xD0000000u), 1);
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x100, 0); /* ya no está vacía */
	v = REG32(R_IPCFIFORECV);
	SDS_CHECK(v, 0xD0000000u);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xD0000001u);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xD0000002u);
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x100, 0x100);
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0);
	v = REG32(R_IPCFIFORECV); /* vacía: error y repite lo último (informativo, ver arriba) */
	SDS_DIVERGE(v, 0xD0000002u);
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0x4000);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x4000;
	/* tras un vaciado, la lectura de vacía da cero */
	SDS_CHECK(call(C_FIFO_SEND, 2, 0xE0000000u), 1);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0008; /* el bit 3 vacía MI cola de envío, no la de recepción */
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x100, 0);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xE0000000u);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xE0000001u);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x4000;
	/* con la FIFO deshabilitada la lectura devuelve la palabra más vieja sin sacarla */
	SDS_CHECK(call(C_FIFO_SEND, 2, 0xF0000000u), 1);
	REG16(R_IPCFIFOCNT) = 0;
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xF0000000u);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xF0000000u);
	REG16(R_IPCFIFOCNT) = 0x8000;
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xF0000000u);
	SDS_CHECK(REG32(R_IPCFIFORECV), 0xF0000001u);
	SDS_CHECK(REG16(R_IPCFIFOCNT) & 0x4000, 0);
}

static void group_fifo_irq(void)
{
	sds_group("FIFO IRQ");
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x4000 | 0x0008;
	irqSet(IRQ_SEND_FIFO, h_send);
	irqSet(IRQ_RECV_FIFO, h_recv);
	irqEnable(IRQ_SEND_FIFO | IRQ_RECV_FIFO);
	/* IRQ de «recepción no vacía»: llega una palabra */
	n_recv = 0;
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0400;
	SDS_CHECK(n_recv, 0);
	SDS_CHECK(call(C_FIFO_SEND, 1, 0x70000000u), 1);
	SDS_CHECK(WAIT_UNTIL(n_recv != 0, 200000), 1);
	SDS_CHECK(n_recv >= 1, 1);
	/* la IRQ la levanta el flanco: con la palabra todavía dentro y la IRQ apagada y vuelta a encender, se levanta */
	REG16(R_IPCFIFOCNT) = 0x8000;
	REG32(R_IF) = IRQ_RECV_FIFO;
	n_recv = 0;
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0400; /* habilitar la IRQ con la condición ya cierta */
	SDS_CHECK(WAIT_UNTIL(n_recv != 0, 200000), 1);
	REG16(R_IPCFIFOCNT) = 0x8000;
	SDS_CHECK(REG32(R_IPCFIFORECV), 0x70000000u);
	REG32(R_IF) = IRQ_RECV_FIFO;
	/* IRQ de «envío vacía»: se levanta al habilitarla con la cola vacía */
	n_send = 0;
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0004;
	SDS_CHECK(WAIT_UNTIL(n_send != 0, 200000), 1);
	/* y cuando el ARM7 vacía mi cola: 3 palabras mías, el ARM7 las lee */
	REG16(R_IPCFIFOCNT) = 0x8000;
	REG32(R_IF) = IRQ_SEND_FIFO;
	REG32(R_IPCFIFOSEND) = 1;
	REG32(R_IPCFIFOSEND) = 2;
	REG32(R_IPCFIFOSEND) = 3;
	n_send = 0;
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x0004;
	for (volatile int i = 0; i < 3000; i++)
		;
	SDS_CHECK(n_send, 0); /* aún hay palabras */
	SDS_CHECK(call(C_FIFO_RECV, 3, 0), 1);
	SDS_CHECK(WAIT_UNTIL(n_send != 0, 200000), 1);
	REG16(R_IPCFIFOCNT) = 0x8000;
	irqDisable(IRQ_SEND_FIFO | IRQ_RECV_FIFO | IRQ_IPC_SYNC);
	REG16(R_IPCFIFOCNT) = 0x8000 | 0x4008;
}

int main(void)
{
	sds_nocache();
	sds_init("IPC ARM9+ARM7");
	MEM32(R_IME) = 1;
	group_sync();
	group_fifo();
	group_fifo_irq();
	{
		uint32_t spin = 0, nibble;

		MBOX->done = 0;
		MBOX->cmd = CMD_QUIT;
		for (;;) {
			uint32_t n = 100000;

			nibble = REG32(R_IPCSYNC) & 0xF;
			if (nibble == SDS_IPC_OK || nibble == SDS_IPC_FAIL || spin >= 40000u)
				break;
			__asm__ volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n) : : "cc");
			spin++;
		}
		sds_group("ARM7");
		SDS_CHECK(nibble, SDS_IPC_OK);
	}
	sds_finish();
}
