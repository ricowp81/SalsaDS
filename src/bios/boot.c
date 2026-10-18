/*
 * SalsaDS — arranque directo (Fase 1.2).
 *
 * Se cargan los binarios ARM9 y ARM7 de la cabecera del cartucho en las direcciones que ella dice y se arma
 * el estado en que un arranque con BIOS dejaría la consola. Nada sale de la BIOS de Nintendo: de dónde sale
 * cada valor:
 *
 *   - Copia de la cabecera (0x170 bytes) en 0x027FFE00: GBATEK, «DS Cartridge Header» (se carga ahí desde el
 *     cartucho al arrancar) y libnds la lee de ahí.
 *   - Datos de usuario (0x70 bytes) en 0x027FFC80: GBATEK, «DS Firmware User Settings» (se espejan en esa
 *     dirección). Se copia la copia más nueva del firmware generado por el emulador (nds_fw_make).
 *   - Identificador del chip del cartucho (dos copias) en 0x027FF800/04 y 0x027FFC00/04, CRC16 de la cabecera
 *     (campo 0x15E) y del área segura (0x06C) en +8 y +A de cada una, 5835h (el CRC16 de la BIOS del ARM7) en
 *     0x027FF850 y 0x027FFC10 e indicador de arranque normal en 0x027FFC40: convención de los cargadores
 *     directos, tomada de lo que se ve en la RAM de un emulador de referencia (no verificada en GBATEK; solo
 *     la consultan juegos que miran el chip).
 *   - Estado del ARM9: modo sistema con I y F puestos, r12 = lr = pc = punto de entrada, CP15 con la ITCM de
 *     32 KB (región virtual de 32 MB, para que el espejo en 0x01000000 que usa libnds funcione) y la DTCM de
 *     16 KB en 0x00800000, ambas habilitadas, vectores altos (GBATEK, mapa de memoria y CP15). Pilas
 *     (valores propios, elegidos donde la BIOS las suele dejar; libnds fija las suyas): SVC 0x00803FC0, IRQ
 *     0x00803FA0, sistema 0x00803EC0.
 *   - Estado del ARM7: lo mismo con pilas SVC 0x0380FFDC, IRQ 0x0380FFB0, sistema 0x0380FF00.
 *   - Registros de E/S: POSTFLG = 1 (arranque completo), WRAMCNT = 3 (toda la WRAM compartida al ARM7),
 *     EXMEMCNT = 0x6000, KEYINPUT = 0x03FF, POWCNT1 = 0x0001, POWCNT2 = 1 (valor inicial según GBATEK), IME = 0.
 *
 * Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

#define PSR_DISABLE (ACORE_I | ACORE_F)

static void cpu_state(struct nds *n, int cpu, uint32_t entry)
{
	struct acore *c = nds_core(n, cpu);

	acore_reset(c);
	if (cpu == NDS_CPU9)
		acore_set_tcm_state(c, 0x00052078u, 0x00000020u, 0x0080000Au);
	acore_set_cpsr(c, ACORE_MODE_SVC | PSR_DISABLE);
	c->r[13] = cpu ? 0x0380FFDCu : 0x00803FC0u;
	acore_set_cpsr(c, ACORE_MODE_IRQ | PSR_DISABLE);
	c->r[13] = cpu ? 0x0380FFB0u : 0x00803FA0u;
	acore_set_cpsr(c, ACORE_MODE_SYS | PSR_DISABLE);
	c->r[13] = cpu ? 0x0380FF00u : 0x00803EC0u;
	c->r[12] = c->r[14] = entry;
	acore_set_pc(c, entry);
	n->intrwait_active[cpu] = 0;
	nds_irq_update(n, cpu);
}

/* SoftReset (SWI 0): vuelve al punto de entrada que dejó la cabecera en la RAM. */
void nds_softreset(struct nds *n, int cpu)
{
	const uint32_t entry = rd32le(n->ram + ((0x027FFE24u + (cpu ? 0x10u : 0u)) & (NDS_RAM_SIZE - 1)));

	cpu_state(n, cpu, entry);
}

static void load_bin(struct nds *n, int cpu, uint32_t addr, const uint8_t *src, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++) {
		const uint32_t a = addr + i;
		const struct nds_rgn *r = &n->rgn_wr[cpu][a >> 23];

		if (r->p)
			r->p[a & r->mask] = src[i];
	}
}

/* Copia un binario de la ROM (entera o por fragmentos) a la memoria de la CPU, en trozos de 4 KB. */
static void load_bin_rom(struct nds *n, int cpu, uint32_t addr, uint32_t off, uint32_t size)
{
	uint8_t tmp[4096];

	for (uint32_t i = 0; i < size; i += sizeof tmp) {
		const uint32_t k = size - i < sizeof tmp ? size - i : (uint32_t)sizeof tmp;

		nds_cart_pread(&n->cart, off + i, tmp, k);
		load_bin(n, cpu, addr + i, tmp, k);
	}
}

int nds_boot_direct(struct nds *n)
{
	const uint8_t *rom = n->cart.rom;
	const size_t len = n->cart.size;
	uint32_t a9_off, a9_entry, a9_ram, a9_size, a7_off, a7_entry, a7_ram, a7_size;
	const uint8_t *u0 = n->fw + 0x3FE00, *u1 = n->fw + 0x3FF00;
	const uint8_t *user = ((rd16le(u0 + 0x70) + 1u) & 0x7Fu) == rd16le(u1 + 0x70) ? u1 : u0;
	uint8_t *ram = n->ram;
	const uint32_t id = nds_cart_chipid(&n->cart);

	if (!rom || len < 0x200)
		return -1;
	a9_off = rd32le(rom + 0x20);
	a9_entry = rd32le(rom + 0x24);
	a9_ram = rd32le(rom + 0x28);
	a9_size = rd32le(rom + 0x2C);
	a7_off = rd32le(rom + 0x30);
	a7_entry = rd32le(rom + 0x34);
	a7_ram = rd32le(rom + 0x38);
	a7_size = rd32le(rom + 0x3C);
	if ((uint64_t)a9_off + a9_size > len || (uint64_t)a7_off + a7_size > len)
		return -1;

	load_bin_rom(n, NDS_CPU9, a9_ram, a9_off, a9_size);
	load_bin_rom(n, NDS_CPU7, a7_ram, a7_off, a7_size);
	memcpy(ram + (0x027FFE00u & (NDS_RAM_SIZE - 1)), rom, 0x170);
	memcpy(ram + (0x027FFC80u & (NDS_RAM_SIZE - 1)), user, 0x70);
	wr32le(ram + (0x027FF800u & (NDS_RAM_SIZE - 1)), id);
	wr32le(ram + (0x027FF804u & (NDS_RAM_SIZE - 1)), id);
	wr32le(ram + (0x027FFC00u & (NDS_RAM_SIZE - 1)), id);
	wr32le(ram + (0x027FFC04u & (NDS_RAM_SIZE - 1)), id);
	wr16le(ram + (0x027FF808u & (NDS_RAM_SIZE - 1)), rd16le(rom + 0x15E));
	wr16le(ram + (0x027FF80Au & (NDS_RAM_SIZE - 1)), rd16le(rom + 0x06C));
	wr16le(ram + (0x027FFC08u & (NDS_RAM_SIZE - 1)), rd16le(rom + 0x15E));
	wr16le(ram + (0x027FFC0Au & (NDS_RAM_SIZE - 1)), rd16le(rom + 0x06C));
	wr16le(ram + (0x027FFC10u & (NDS_RAM_SIZE - 1)), 0x5835);
	wr16le(ram + (0x027FF850u & (NDS_RAM_SIZE - 1)), 0x5835);
	wr16le(ram + (0x027FFC40u & (NDS_RAM_SIZE - 1)), 1);

	n->io[NDS_CPU9].postflg = 1;
	n->io[NDS_CPU7].postflg = 1;
	nds_mem_wramcnt(n, 3);
	n->io[NDS_CPU9].exmem = 0x6000;
	n->powcnt1 = 0x0001;
	n->powcnt2 = 1;
	n->keyinput = 0x03FF;
	n->extkeyin = 0x007F;

	cpu_state(n, NDS_CPU9, a9_entry);
	cpu_state(n, NDS_CPU7, a7_entry);
	nds_sched_init(n);
	return 0;
}
