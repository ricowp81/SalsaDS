/*
 * SalsaDS — Fase 0.1: programa vacío de PS3.
 *
 * Solo comprueba la cadena completa (toolchain, enlace, .self y .pkg) y que
 * el programa arranca: escribe una línea de RESULTADO y sale.
 */
#include <ppu-types.h>
#include <sys/process.h>

#include "platform/ps3/log.h"

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	salsads_log_init("empty");
	salsads_log("RESULTADO empty: OK");
	salsads_log_shutdown();
	return 0;
}
