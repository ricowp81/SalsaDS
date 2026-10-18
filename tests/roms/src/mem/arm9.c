/*
 * SalsaDS — ROM mem, lado ARM9: mapa de memoria de la Fase 1.2.
 *
 * Grupos: RAM principal y sus espejos, WRAM compartida en los cuatro modos de WRAMCNT (vista del ARM9 y,
 * por el buzón, la del ARM7), los nueve bancos de VRAM en LCDC y en cada una de sus asignaciones (se escribe
 * por la ventana y se lee de vuelta como LCDC), bancos solapados (lectura con OR), espejos de las ventanas,
 * VRAM del ARM7 (bancos C y D), paleta y OAM (los accesos de 8 bits se ignoran), E/S sin definir y BIOS.
 *
 * El banco A es el framebuffer de sds.c: sus pruebas solo tocan la cola (desplazamientos 0x1F000-0x1FFFF) y
 * lo restauran como LCDC antes de volver a escribir en la pantalla.
 */
#include <stdio.h>
#include "../common/hw.h"

#define LCDC 0x06800000u
static const uint32_t bank_lcdc[9] = { 0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000, 0x06890000,
				       0x06894000, 0x06898000, 0x068A0000 };
static const uint32_t bank_size[9] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
static const uint32_t bank_reg[9] = { 0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244, 0x04000245,
				      0x04000246, 0x04000248, 0x04000249 };

static void vcfg(unsigned bank, uint32_t v)
{
	REG8(bank_reg[bank]) = (uint8_t)v;
}

static void all_lcdc(void)
{
	for (unsigned b = 0; b < 9; b++)
		vcfg(b, 0x80);
}

/* Pone el banco en (mst, ofs), escribe dos marcas por la ventana, lo devuelve al LCDC y lee las marcas.
 * Devuelve 1 si las dos coinciden. `win` es la dirección de la ventana donde debería caer el primer byte. */
static int probe(unsigned bank, uint32_t mst, uint32_t ofs, uint32_t win, uint32_t id)
{
	const uint32_t m1 = 0xBE000000u | (id << 8) | 1, m2 = 0xEF000000u | (id << 8) | 2;
	const uint32_t first = bank == 0 ? 0x1F000u : 0, last = bank_size[bank] - 4;
	uint32_t g1, g2;

	all_lcdc();
	vcfg(bank, 0x80 | mst | (ofs << 3));
	MEM32(win + first) = m1;
	MEM32(win + last) = m2;
	vcfg(bank, 0x80);
	g1 = MEM32(bank_lcdc[bank] + first);
	g2 = MEM32(bank_lcdc[bank] + last);
	SDS_CHECK_ID(g1, m1, id * 2);
	SDS_CHECK_ID(g2, m2, id * 2 + 1);
	return g1 == m1 && g2 == m2;
}

static void group_ram(void)
{
	sds_group("RAM");
	MEM32(0x02200000) = 0x11223344u;
	SDS_CHECK(MEM32(0x02600000), 0x11223344u); /* espejo cada 4 MB */
	SDS_CHECK(MEM32(0x02A00000), 0x11223344u);
	SDS_CHECK(MEM32(0x02E00000), 0x11223344u);
	SDS_CHECK(MEM8(0x02200000), 0x44);
	SDS_CHECK(MEM8(0x02200003), 0x11);
	SDS_CHECK(MEM16(0x02200002), 0x1122);
	MEM32(0x02E00004) = 0xA5A5A5A5u;
	SDS_CHECK(MEM32(0x02200004), 0xA5A5A5A5u);
	MEM8(0x02200005) = 0x5A;
	SDS_CHECK(MEM32(0x02200004), 0xA5A55AA5u);
	MEM16(0x02200006) = 0x1234;
	SDS_CHECK(MEM32(0x02200004), 0x12345AA5u);
}

static uint32_t pattern(uint32_t off)
{
	return 0xC0DE0000u + off / 4;
}

static void group_wram(void)
{
	static const uint32_t probes[] = { 0, 4, 0x3FFC, 0x4000, 0x4004, 0x7FFC };

	sds_group("WRAM");
	REG8(R_WRAMCNT) = 3;
	SDS_CHECK(REG8(R_WRAMCNT), 3);
	SDS_CHECK(mb_call(1, 0, 0, 0), 1); /* el ARM7 llena los 32 KB con la marca */

	/* el ARM9 con cada modo */
	REG8(R_WRAMCNT) = 0; /* 32 KB para el ARM9 */
	for (unsigned i = 0; i < sizeof probes / 4; i++) {
		SDS_CHECK(MEM32(0x03000000 + probes[i]), pattern(probes[i]));
		SDS_CHECK(MEM32(0x03008000 + probes[i]), pattern(probes[i])); /* espejo cada 32 KB */
		SDS_CHECK(MEM32(0x03FF8000 + probes[i]), pattern(probes[i]));
	}
	REG8(R_WRAMCNT) = 1; /* la segunda mitad para el ARM9 */
	SDS_CHECK(REG8(R_WRAMCNT), 1);
	SDS_CHECK(MEM32(0x03000000), pattern(0x4000));
	SDS_CHECK(MEM32(0x03000004), pattern(0x4004));
	SDS_CHECK(MEM32(0x03004000), pattern(0x4000)); /* 16 KB que se repiten */
	SDS_CHECK(MEM32(0x03FFC008), pattern(0x4008));
	REG8(R_WRAMCNT) = 2; /* la primera mitad */
	SDS_CHECK(MEM32(0x03000000), pattern(0));
	SDS_CHECK(MEM32(0x03000008), pattern(8));
	SDS_CHECK(MEM32(0x03004008), pattern(8));
	REG8(R_WRAMCNT) = 3; /* nada para el ARM9: vacío */
	SDS_CHECK(MEM32(0x03000000), 0);
	MEM32(0x03000000) = 0x12345678u;
	SDS_CHECK(MEM32(0x03000000), 0);

	/* el ARM7 con cada modo (vea mb_call 2 en arm7.c) */
	for (uint32_t m = 0; m < 4; m++) {
		REG8(R_WRAMCNT) = (uint8_t)m;
		SDS_CHECK(mb_call(2, m, 0, 0), 1);
		SDS_CHECK(MBOX->res[0], 1); /* 1 = el ARM7 vio lo esperado */
	}
	/* lo que el ARM7 escribe en su mitad se ve cuando esa mitad pasa al ARM9: modo 1 = el ARM7 tiene la
	 * primera mitad; modo 2 = la tiene el ARM9 */
	REG8(R_WRAMCNT) = 1;
	SDS_CHECK(mb_call(3, 0, 0, 0), 1);
	REG8(R_WRAMCNT) = 2;
	SDS_CHECK(MEM32(0x03000010), 0x600DF00Du);
	REG8(R_WRAMCNT) = 3;
}

static void group_lcdc2(void)
{
	sds_group("LCDC2");
	all_lcdc();
	for (unsigned b = 1; b < 9; b++) {
		MEM32(bank_lcdc[b]) = 0x10000000u | b;
		MEM32(bank_lcdc[b] + bank_size[b] - 4) = 0x20000000u | b;
	}
	for (unsigned b = 1; b < 9; b++) {
		SDS_CHECK(MEM32(bank_lcdc[b]), 0x10000000u | b);
		SDS_CHECK(MEM32(bank_lcdc[b] + bank_size[b] - 4), 0x20000000u | b);
		SDS_CHECK(MEM16(bank_lcdc[b]), (0x10000000u | b) & 0xFFFF);
		SDS_CHECK(MEM16(bank_lcdc[b] + 2), (0x10000000u | b) >> 16);
	}
	/* 8 bits: ignorados en el LCDC */
	MEM32(bank_lcdc[1] + 8) = 0x55AA55AAu;
	MEM8(bank_lcdc[1] + 8) = 0x11;
	MEM8(bank_lcdc[1] + 9) = 0x22;
	SDS_CHECK(MEM32(bank_lcdc[1] + 8), 0x55AA55AAu);
	/* lecturas de 8 y 16 bits, little-endian */
	MEM32(bank_lcdc[2] + 8) = 0x44332211u;
	SDS_CHECK(MEM8(bank_lcdc[2] + 8), 0x11);
	SDS_CHECK(MEM8(bank_lcdc[2] + 11), 0x44);
	SDS_CHECK(MEM16(bank_lcdc[2] + 10), 0x4433);
	/* lo que cae fuera de los 656 KB del LCDC no existe */
	SDS_CHECK(MEM32(LCDC + 0xA4000), 0);
	MEM32(LCDC + 0xA4000) = 0x77777777u;
	SDS_CHECK(MEM32(LCDC + 0xA4000), 0);
}

static void group_map(void)
{
	uint32_t id = 1, ok = 0, total = 0;

	sds_group("VRAM MAP");
	/* A y B: fondo A (MST 1, 4 posiciones) y OBJ A (MST 2, 2 posiciones) */
	for (unsigned b = 0; b < 2; b++) {
		for (uint32_t ofs = 0; ofs < 4; ofs++, id++) {
			ok += (uint32_t)probe(b, 1, ofs, 0x06000000u + 0x20000u * ofs, id);
			total++;
		}
		for (uint32_t ofs = 0; ofs < 2; ofs++, id++) {
			ok += (uint32_t)probe(b, 2, ofs, 0x06400000u + 0x20000u * ofs, id);
			total++;
		}
	}
	/* C y D: fondo A y, C como fondo B (MST 4), D como OBJ B (MST 4) */
	for (unsigned b = 2; b < 4; b++) {
		for (uint32_t ofs = 0; ofs < 4; ofs++, id++) {
			ok += (uint32_t)probe(b, 1, ofs, 0x06000000u + 0x20000u * ofs, id);
			total++;
		}
		ok += (uint32_t)probe(b, 4, 0, b == 2 ? 0x06200000u : 0x06600000u, id++);
		total++;
	}
	/* E (64 KB): fondo A y OBJ A */
	ok += (uint32_t)probe(4, 1, 0, 0x06000000u, id++);
	ok += (uint32_t)probe(4, 2, 0, 0x06400000u, id++);
	total += 2;
	/* F y G (16 KB): fondo A y OBJ A, cuatro posiciones: 0x4000 × OFS.0 + 0x10000 × OFS.1 */
	for (unsigned b = 5; b < 7; b++) {
		for (uint32_t ofs = 0; ofs < 4; ofs++, id++) {
			const uint32_t off = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);

			ok += (uint32_t)probe(b, 1, ofs, 0x06000000u + off, id);
			total++;
		}
		for (uint32_t ofs = 0; ofs < 4; ofs++, id++) {
			const uint32_t off = 0x4000u * (ofs & 1) + 0x10000u * (ofs >> 1);

			ok += (uint32_t)probe(b, 2, ofs, 0x06400000u + off, id);
			total++;
		}
	}
	/* H (32 KB) fondo B; I (16 KB) fondo B a +0x8000 y OBJ B */
	ok += (uint32_t)probe(7, 1, 0, 0x06200000u, id++);
	ok += (uint32_t)probe(8, 1, 0, 0x06208000u, id++);
	ok += (uint32_t)probe(8, 2, 0, 0x06600000u, id++);
	total += 3;
	all_lcdc();
	SDS_CHECK(ok, total);
}

static void group_overlap(void)
{
	sds_group("VRAM SOLAPE");
	all_lcdc();
	/* B y C con contenidos distintos, los dos como fondo A en la misma posición: la lectura es el OR y la
	 * escritura va a los dos */
	MEM32(bank_lcdc[1] + 0x100) = 0x0000FF00u;
	MEM32(bank_lcdc[2] + 0x100) = 0x00FF0000u;
	MEM32(bank_lcdc[1] + 0x104) = 0;
	MEM32(bank_lcdc[2] + 0x104) = 0;
	vcfg(1, 0x80 | 1);
	vcfg(2, 0x80 | 1);
	SDS_CHECK(MEM32(0x06000100), 0x00FFFF00u);
	MEM32(0x06000104) = 0xCAFE0001u;
	vcfg(1, 0x80);
	vcfg(2, 0x80);
	SDS_CHECK(MEM32(bank_lcdc[1] + 0x104), 0xCAFE0001u);
	SDS_CHECK(MEM32(bank_lcdc[2] + 0x104), 0xCAFE0001u);
	/* uno solo habilitado en la ventana: se lee solo ese */
	vcfg(1, 0x80 | 1);
	SDS_CHECK(MEM32(0x06000100), 0x0000FF00u);
	vcfg(1, 0x80);
	/* deshabilitado (bit 7 = 0) no se ve ni se escribe */
	vcfg(2, 0x01);
	SDS_CHECK(MEM32(0x06000100), 0);
	MEM32(0x06000108) = 0xFEEDBEEFu;
	vcfg(2, 0x80);
	SDS_CHECK(MEM32(bank_lcdc[2] + 0x108) == 0xFEEDBEEFu, 0);
	all_lcdc();
}

static void group_mirror(void)
{
	sds_group("VRAM ESPEJO");
	all_lcdc();
	vcfg(1, 0x80 | 1); /* B como fondo A en 0x06000000 (128 KB de una ventana de 512 KB) */
	MEM32(0x06000010) = 0x0BADCAFEu;
	SDS_CHECK(MEM32(0x06000010), 0x0BADCAFEu);
	SDS_CHECK(MEM32(0x06080010), 0x0BADCAFEu); /* la ventana se repite cada 512 KB */
	SDS_CHECK(MEM32(0x06100010), 0x0BADCAFEu);
	SDS_CHECK(MEM32(0x06180010), 0x0BADCAFEu);
	SDS_CHECK(MEM32(0x06020010), 0); /* lo no asignado dentro de la ventana */
	SDS_CHECK(MEM32(0x06040010), 0);
	vcfg(1, 0x80);
	/* OBJ A (256 KB) con E (64 KB): se repite cada 256 KB */
	vcfg(4, 0x80 | 2);
	MEM32(0x06400020) = 0x13572468u;
	SDS_CHECK(MEM32(0x06440020), 0x13572468u); /* se repite cada 256 KB */
	SDS_CHECK(MEM32(0x06410020), 0);            /* lo no asignado de la ventana */
	vcfg(4, 0x80);
	all_lcdc();
	/* fondo B (128 KB): H en 0x06200000; se repite cada 128 KB */
	vcfg(7, 0x80 | 1);
	MEM32(0x06200030) = 0x2468ACE0u;
	SDS_CHECK(MEM32(0x06220030), 0x2468ACE0u);
	SDS_CHECK(MEM32(0x06208030), 0); /* H mide 32 KB: lo demás de la ventana está vacío */
	vcfg(7, 0x80);
	all_lcdc();
}

static void group_pal_oam(void)
{
	volatile uint16_t *pal = (volatile uint16_t *)0x05000100;
	volatile uint16_t *oam = (volatile uint16_t *)0x07000100;
	const uint16_t sp0 = pal[0], sp1 = pal[1], so0 = oam[0], so1 = oam[1];

	sds_group("PAL/OAM");
	pal[0] = 0x1234;
	pal[1] = 0xABCD;
	SDS_CHECK(pal[0], 0x1234);
	SDS_CHECK(MEM32(0x05000100), 0xABCD1234u);
	SDS_CHECK(MEM8(0x05000100), 0x34);
	SDS_CHECK(MEM8(0x05000101), 0x12);
	MEM8(0x05000100) = 0xFF; /* 8 bits: se ignoran */
	MEM8(0x05000101) = 0xEE;
	SDS_CHECK(pal[0], 0x1234);
	SDS_CHECK(MEM16(0x05000900), 0x1234); /* espejo cada 2 KB */
	SDS_CHECK(MEM16(0x05FFF900), 0x1234);
	MEM32(0x05000104) = 0x76543210u;
	SDS_CHECK(pal[2], 0x3210);
	SDS_CHECK(pal[3], 0x7654);

	oam[0] = 0x0F0F;
	oam[1] = 0x1E1E;
	SDS_CHECK(oam[0], 0x0F0F);
	SDS_CHECK(MEM32(0x07000100), 0x1E1E0F0Fu);
	MEM8(0x07000100) = 0x77;
	SDS_CHECK(oam[0], 0x0F0F);
	SDS_CHECK(MEM16(0x07000900), 0x0F0F);
	SDS_CHECK(MEM16(0x07FFF900), 0x0F0F);
	pal[0] = sp0;
	pal[1] = sp1;
	oam[0] = so0;
	oam[1] = so1;
}

static void group_misc(void)
{
	sds_group("VARIOS");
	SDS_CHECK(MEM32(0x040003F0), 0); /* puertos sin definir leen cero */
	SDS_CHECK(MEM32(0x04004000), 0);
	SDS_CHECK(MEM32(0xFFFF0000) != 0, 1); /* la BIOS tiene vectores */
	{
		const uint32_t v = MEM32(0xFFFF0000);

		MEM32(0xFFFF0000) = 0;
		SDS_CHECK(MEM32(0xFFFF0000), v); /* y no se escribe */
	}
	/* la ranura de GBA vacía: lo que lee el ARM9 (informativo; GBATEK no lo dice) */
	/* ¿hay algo en la ranura de GBA? La cabecera de un cartucho de GBA tiene 0x96 en 0x080000B2. Se anota en el log y en la
	 * pantalla: lo que lee la ranura depende de si hay un cartucho (o una flashcard de GBA) o está vacía. */
	{
		const uint32_t fixed = MEM8(0x080000B2);
		const char *msg = fixed == 0x96 ? "GBA SLOT: CART" : "GBA SLOT: VACIA?";

		sds_notef("ranura de GBA: %s (0x080000B2 = %02lX, 0x08000000 = %04lX)", fixed == 0x96 ? "hay un cartucho/flashcard" : "sin cabecera GBA (vacia?)",
			  (unsigned long)fixed, (unsigned long)MEM16(0x08000000));
		sds_print_row(0, msg);
	}
	/* los primeros 16 bytes de la ROM (0x08000000) y de la SRAM (0x0A000000) de la ranura, para comparar corridas con y sin
	 * cartucho o flashcard (la cabecera 0x96 no los distingue) */
	{
		char rom[40], ram[40];

		for (int i = 0; i < 16; i++) {
			snprintf(rom + 2 * i, 3, "%02X", (unsigned)MEM8(0x08000000 + i));
			snprintf(ram + 2 * i, 3, "%02X", (unsigned)MEM8(0x0A000000 + i));
		}
		sds_notef("ranura de GBA, 16 bytes de 0x08000000: %s", rom);
		sds_notef("ranura de GBA, 16 bytes de 0x0A000000: %s", ram);
	}
	/* la ranura de GBA: lo que lee el ARM9 (informativo; GBATEK no lo dice). Con una flashcard de GBA insertada leyó 0 en
	 * 0x08000000 (16 bits) y 0x0A000000 (8 bits); falta medir con la ranura vacía. SalsaDS lee unos y melonDS 0xFE08 */
	SDS_DIVERGE_M(MEM16(0x08000000), 0xFFFF, 0xFE08);
	SDS_DIVERGE_M(MEM8(0x0A000000), 0xFF, 0xFF);
}

int main(void)
{
	sds_nocache();
	sds_init("MEM ARM9+ARM7");
	group_ram();
	group_wram();
	group_lcdc2();
	group_map();
	group_overlap();
	group_mirror();
	group_pal_oam();
	group_misc();
	/* VRAM del ARM7: C y D como memoria de trabajo del ARM7 (mb_call 4 y 5 en arm7.c) */
	sds_group("VRAM ARM7");
	all_lcdc();
	MEM32(bank_lcdc[2] + 0x40) = 0xC0C0C0C0u;
	MEM32(bank_lcdc[3] + 0x40) = 0xD0D0D0D0u;
	MEM32(bank_lcdc[2] + 0x44) = 0;
	MEM32(bank_lcdc[3] + 0x44) = 0;
	vcfg(2, 0x80 | 2); /* C al ARM7, OFS 0 */
	SDS_CHECK(mb_call(4, 0, 0, 0), 1);
	SDS_CHECK(MBOX->res[0], 0xC0C0C0C0u); /* el ARM7 lo lee en 0x06000040 */
	SDS_CHECK(MBOX->res[1], 1);           /* VRAMSTAT: C asignado a ARM7, D no */
	vcfg(3, 0x80 | 2 | (1 << 3)); /* D al ARM7 con OFS 1: 0x06020000 */
	SDS_CHECK(mb_call(5, 0, 0, 0), 1);
	SDS_CHECK(MBOX->res[0], 0xD0D0D0D0u); /* en 0x06020040 */
	SDS_CHECK(MBOX->res[1], 3);           /* VRAMSTAT = 3 */
	SDS_CHECK(MBOX->res[2], 0xC0C0C0C0u); /* y C sigue en 0x06000040 */
	vcfg(2, 0x80);
	vcfg(3, 0x80);
	SDS_CHECK(MEM32(bank_lcdc[2] + 0x44), 0xABCD0000u); /* lo que escribió el ARM7 por bytes y por palabras */
	SDS_CHECK(MEM32(bank_lcdc[3] + 0x44), 0x11223344u);
	all_lcdc();
	MBOX->done = 0;
	MBOX->cmd = CMD_QUIT;
	{
		uint32_t spin = 0, nibble;

		for (;;) {
			uint32_t n = 100000;

			nibble = REG32(R_IPCSYNC) & 0xF;
			if (nibble == SDS_IPC_OK || nibble == SDS_IPC_FAIL || spin >= 20000u)
				break;
			__asm__ volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n) : : "cc");
			spin++;
		}
		sds_group("ARM7");
		SDS_CHECK(nibble, SDS_IPC_OK);
	}
	sds_finish();
}
