/*
 * SalsaDS — máquina mínima para correr las ROMs de prueba de CPU en el host.
 *
 * NO es el mapa de memoria de la Fase 1.2: es lo justo para que el arranque
 * directo de una ROM de BlocksDS llegue a su `main` y deje su veredicto en
 * RAM, de modo que los intérpretes de src/cpu corran el mismo código que
 * melonDS. Memoria principal, WRAM compartida con WRAMCNT, WRAM del ARM7,
 * VRAM plana, un archivo de registros de E/S que guarda lo que se le escribe
 * (salvo IPCSYNC, VCOUNT/DISPSTAT y WRAMCNT, que tienen comportamiento) y
 * nada de IRQ, temporizadores, DMA ni gráficos. Los dos procesadores se
 * intercalan en rebanadas de tiempo (ARM9 a 67 MHz, ARM7 a 33 MHz).
 */
#ifndef SALSADS_HOST_NDSMIN_H
#define SALSADS_HOST_NDSMIN_H

#include <stddef.h>
#include <stdint.h>

#include "cpu/acore.h"

#define NDSMIN_RAM_SIZE (4u << 20)
#define NDSMIN_VRAM_SIZE 0xA4000u
#define NDSMIN_IO_SIZE 0x2000u

struct ndsmin {
	struct acore a9, a7;
	uint8_t itcm[ACORE_ITCM_SIZE], dtcm[ACORE_DTCM_SIZE];
	uint8_t ram[NDSMIN_RAM_SIZE];
	uint8_t swram[32 * 1024];
	uint8_t wram7[64 * 1024];
	uint8_t vram[NDSMIN_VRAM_SIZE];
	uint8_t pal[2048], oam[2048];
	uint8_t io[2][NDSMIN_IO_SIZE]; /* [0] ARM9, [1] ARM7 */
	uint32_t ipc_out[2];
	uint32_t wramcnt;
	int trace_io;
	uint32_t io_seen[64], io_seen_n; /* accesos a E/S sin comportamiento, para depurar */
};

void ndsmin_init(struct ndsmin *m);
/* Arranque directo: carga los dos binarios de la cabecera y arma el estado inicial. 0 = bien. */
int ndsmin_boot(struct ndsmin *m, const uint8_t *rom, size_t len);
/* Corre el ARM9 `arm9_cycles` ciclos y el ARM7 la mitad, intercalados en rebanadas cortas. */
void ndsmin_run(struct ndsmin *m, uint64_t arm9_cycles);

/* Acceso de depuración a la memoria vista por una CPU (0 = ARM9, 1 = ARM7). */
uint32_t ndsmin_peek32(struct ndsmin *m, int cpu, uint32_t addr);

#endif
