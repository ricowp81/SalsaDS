/*
 * SalsaDS — Fase 0: prueba del logger UDP.
 *
 * Envía SALSADS_UDPLOG_COUNT líneas numeradas y una línea final con el total.
 * Puerto 18195. En la Mac: tools/udplog.py --until RESULTADO y comparar el total recibido.
 */
#include <ppu-types.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "platform/ps3/log.h"

#define SALSADS_UDPLOG_COUNT 200

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	salsads_log_init("udplog");
	salsads_log("enviando %d líneas numeradas", SALSADS_UDPLOG_COUNT);

	for (int i = 0; i < SALSADS_UDPLOG_COUNT; i++) {
		salsads_log("linea %03d/%03d", i + 1, SALSADS_UDPLOG_COUNT);
		if (i % 20 == 19)
			sysUsleep(10000); /* no saturar la cola de envío */
	}

	salsads_log("RESULTADO udplog: enviadas %d líneas", SALSADS_UDPLOG_COUNT);
	salsads_log_shutdown();
	return 0;
}
