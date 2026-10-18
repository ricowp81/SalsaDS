/*
 * SalsaDS — ROM «gfxrand»: una escena aleatoria de los motores 2D (registros, VRAM, paletas, OAM) que sale de una
 * semilla. La semilla viene de los cuatro bytes del código de juego de la cabecera (0x027FFE0C en RAM), que
 * tools/rom-seed.py cambia en una copia de la ROM: así una sola ROM da todas las escenas y se corre igual en SalsaDS y
 * en melonDS (que entrega su imagen por la captura del motor A).
 *
 * Los 12 bits altos de ese código de juego (la semilla son los 20 bajos) son una máscara para aislar diferencias: cada
 * bit apaga una familia de características DESPUÉS de sortear la escena (la misma semilla da la misma escena sin esa
 * parte):
 *   0x001 capa de OBJ      0x002 ventanas        0x004 efectos de color (BLDCNT)   0x008 mosaico de fondos
 *   0x010 OBJ afines       0x020 OBJ semitransparentes   0x040 OBJ de ventana      0x080 OBJ de mapa de bits
 *   0x100 fondos afines, extendidos y grandes (modo 0)   0x200 capa 3D (BG0)      0x400 (sin uso aquí)
 *   0x800 mosaico de OBJ
 * Además, los bytes 0x15 y 0x16 de la cabecera (reservados) acotan los OBJ que se dejan: [0x15, 0x16) si el segundo no es
 * cero (tools/rom-seed.py --keep A:B).
 *
 * La semilla fija un perfil (los 3 bits bajos) que carga la mano en una familia de funciones: texto y OBJ, ventanas y
 * mezclas, afines, extendidos y mapas de bits, el mapa de bits grande, paletas extendidas, mosaico, todo mezclado. El
 * resto de las decisiones salen del generador xorshift32.
 */
#include "gfx.h"

static uint32_t rs;

static uint32_t rnd(void)
{
	uint32_t x = rs;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rs = x;
}

static uint32_t rn(uint32_t n)
{
	return rnd() % n;
}

static int chance(uint32_t pct)
{
	return rn(100) < pct;
}

/* Palabra de datos con muchos ceros: tesela con huecos transparentes */
static uint32_t sparse(void)
{
	uint32_t w = rnd();

	if (rnd() & 1)
		w &= rnd();
	if (rnd() & 1)
		w &= rnd();
	return w;
}

static void fill(uint32_t addr, uint32_t bytes)
{
	volatile uint32_t *p = V32(addr);

	for (uint32_t i = 0; i < bytes / 4; i++)
		p[i] = sparse();
}

int main(void)
{
	const uint32_t full = *(volatile uint32_t *)0x027FFE0Cu;
	const uint32_t seed = full & 0x000FFFFFu;
	const uint32_t off = full >> 20; /* características desactivadas (ver abajo) */
	const int profile = (int)(seed & 7u);
	uint32_t dc, bgmode;
	int extpal = 0;

	rs = seed * 2654435761u + 0x9E3779B9u;
	if (!rs)
		rs = 1;
	for (int i = 0; i < 8; i++)
		rnd();
	sds_init_quiet();
	gfx_power();
	extpal = profile == 5 || (profile == 7 && chance(40));

	/* bancos: A LCDC; B, C, D fondos de A (384 KB); E, F, G OBJ de A (96 KB) o, con paletas extendidas, E fondos
	 * extendidos, F OBJ extendidos y G los OBJ */
	REG8(R_VRAMCNT(0)) = 0x80;
	REG8(R_VRAMCNT(1)) = 0x81;
	REG8(R_VRAMCNT(2)) = 0x89;
	REG8(R_VRAMCNT(3)) = 0x91;
	if (!extpal) {
		REG8(R_VRAMCNT(4)) = 0x82;
		REG8(R_VRAMCNT(5)) = 0x92;
		REG8(R_VRAMCNT(6)) = 0x9A;
	} else {
		REG8(R_VRAMCNT(4)) = 0x84; /* E: paletas extendidas de fondo */
		REG8(R_VRAMCNT(5)) = 0x85; /* F: paleta extendida de OBJ */
		REG8(R_VRAMCNT(6)) = 0x82; /* G: OBJ de A (16 KB) */
	}
	/* paletas extendidas: bancos mapeados un momento como LCDC para llenarlos */
	if (extpal) {
		REG8(R_VRAMCNT(4)) = 0x80;
		REG8(R_VRAMCNT(5)) = 0x80;
		fill(0x06880000u, 0x8000u);
		fill(0x06890000u, 0x2000u);
		REG8(R_VRAMCNT(4)) = 0x84;
		REG8(R_VRAMCNT(5)) = 0x85;
	}
	fill(0x06000000u, 0x60000u);
	fill(0x06400000u, extpal ? 0x4000u : 0x10000u);
	if (!extpal)
		fill(0x06410000u, 0x8000u);
	for (int i = 0; i < 256; i++) {
		PAL_BG(0)[i] = (uint16_t)(rnd() & 0x7FFF);
		PAL_OBJ(0)[i] = (uint16_t)(rnd() & 0x7FFF);
	}

	/* modo y capas */
	switch (profile) {
	case 0: bgmode = 0; break;
	case 1: bgmode = rn(2); break;
	case 2: bgmode = 1 + rn(2) * (rn(2) ? 3 : 1); break; /* 1, 2 o 4 */
	case 3: bgmode = 3 + rn(3); break;
	case 4: bgmode = 6; break;
	default: bgmode = rn(7); break;
	}
	dc = DC_GRAPHICS | bgmode;
	for (int b = 0; b < 4; b++)
		if (chance(75))
			dc |= DC_BG(b);
	if (chance(80))
		dc |= DC_OBJ;
	if (profile == 4 || chance(8))
		dc |= 1u << 3; /* BG0 es 3D (capa vacía) */
	if (chance(50))
		dc |= DC_OBJ1D;
	dc |= rn(2) << 20; /* frontera de OBJ 1D (solo 32 y 64 bytes: con más, los OBJ leen más allá de los 96 KB mapeados) */
	if (chance(50))
		dc |= 1u << 5;
	if (chance(50))
		dc |= 1u << 6;
	dc |= rn(2) << 24; /* base de teselas y de pantalla de DISPCNT: 0 o 64 KB (los bancos de fondos suman 384 KB) */
	dc |= rn(2) << 27;
	if (profile == 1 || profile == 7 || chance(15)) {
		if (chance(60))
			dc |= DC_WIN0;
		if (chance(60))
			dc |= DC_WIN1;
		if (chance(50))
			dc |= DC_OBJWIN;
	}
	if (extpal) {
		if (chance(85))
			dc |= DC_BGEXTPAL;
		if (chance(85))
			dc |= DC_OBJEXTPAL;
	}
	for (int b = 0; b < 4; b++) {
		uint32_t c = (uint32_t)rn(4) | (rn(9) << 2) | (rn(16) << 8) | (rn(4) << 14);

		if (chance(55))
			c |= 0x80;
		if (chance(profile == 6 ? 70 : 12))
			c |= 0x40;
		if (chance(40))
			c |= 0x2000;
		if (chance(30))
			c |= 4;
		ER16(0, E_BGCNT(b)) = (uint16_t)c;
		ER16(0, E_BGHOFS(b)) = (uint16_t)rn(512);
		ER16(0, E_BGVOFS(b)) = (uint16_t)rn(512);
	}
	for (int k = 0; k < 2; k++) {
		const uint32_t base = k ? E_BG3PA : E_BG2PA;
		const int rot = profile == 2 || profile == 7 || chance(40);

		ER16(0, base) = (uint16_t)(rot ? (int)rn(0x500) - 0x280 : 0x100 + (int)rn(0x40) - 0x20);
		ER16(0, base + 2) = (uint16_t)(rot ? (int)rn(0x300) - 0x180 : 0);
		ER16(0, base + 4) = (uint16_t)(rot ? (int)rn(0x300) - 0x180 : 0);
		ER16(0, base + 6) = (uint16_t)(rot ? (int)rn(0x500) - 0x280 : 0x100 + (int)rn(0x40) - 0x20);
		ER32(0, base + 8) = rnd() & 0x0FFFFFFFu;
		ER32(0, base + 12) = rnd() & 0x0FFFFFFFu;
		if (chance(50)) {
			ER32(0, base + 8) = rn(0x40000);
			ER32(0, base + 12) = rn(0x40000);
		}
	}
	ER16(0, E_WIN0H) = (uint16_t)rnd();
	ER16(0, E_WIN1H) = (uint16_t)rnd();
	ER16(0, E_WIN0V) = (uint16_t)rnd();
	ER16(0, E_WIN1V) = (uint16_t)rnd();
	if (chance(70)) { /* ventanas bien formadas */
		const uint32_t x1 = rn(200), y1 = rn(150);

		ER16(0, E_WIN0H) = (uint16_t)((x1 << 8) | (x1 + 10 + rn(60)));
		ER16(0, E_WIN0V) = (uint16_t)((y1 << 8) | (y1 + 10 + rn(40)));
		ER16(0, E_WIN1H) = (uint16_t)(((x1 + 30) << 8) | (x1 + 50 + rn(100)));
		ER16(0, E_WIN1V) = (uint16_t)(((y1 + 20) << 8) | (y1 + 30 + rn(60)));
	}
	ER16(0, E_WININ) = (uint16_t)(rnd() & 0x3F3F);
	ER16(0, E_WINOUT) = (uint16_t)(rnd() & 0x3F3F);
	ER16(0, E_MOSAIC) = (uint16_t)rnd();
	{
		uint32_t bld = rnd() & 0x3FFF;

		if (profile == 1 || profile == 7 || chance(30))
			bld |= (uint32_t)(1 + rn(3)) << 6;
		ER16(0, E_BLDCNT) = (uint16_t)bld;
		ER16(0, E_BLDALPHA) = (uint16_t)(rn(20) | (rn(20) << 8));
		ER16(0, E_BLDY) = (uint16_t)rn(20);
	}

	/* OAM */
	for (int i = 0; i < 128; i++) {
		uint32_t a0 = rn(256), a1 = rn(512), a2 = rnd() & 0xFFFF;
		const int sane = chance(90);

		a0 = (a0 + 216) & 255; /* y en -40..215 */
		a1 = (a1 + 472) & 511; /* x en -40..471 → envuelto a -40..255 */
		if (a1 > 300 && a1 < 460)
			a1 = rn(256);
		a0 |= (sane ? rn(3) : rn(4)) << 14;
		a1 |= rn(4) << 14;
		if (chance(profile == 2 || profile == 7 ? 55 : 20)) {
			a0 |= 0x100;
			if (chance(50))
				a0 |= 0x200;
			a1 |= rn(32) << 9;
		} else {
			if (chance(6))
				a0 |= 0x200; /* desactivado */
			a1 |= rn(4) << 12;
		}
		if (chance(12))
			a0 |= 1u << 10;
		if (chance(profile == 1 || profile == 7 ? 12 : 4))
			a0 |= 2u << 10;
		if (chance(profile == 3 || profile == 7 ? 20 : 4))
			a0 |= 3u << 10;
		if (chance(profile == 6 ? 60 : 12))
			a0 |= 1u << 12;
		if (chance(45))
			a0 |= 1u << 13;
		a2 &= 0xFFFF;
		if (((a0 >> 10) & 3) == 3) { /* OBJ de mapa de bits: solo en los primeros 64 KB, y sin mosaico (leería lejos) */
			a2 &= ~0x200u;
			a0 &= ~0x1000u;
		}
		/* Con mosaico, la fila del primer bloque vertical puede quedar fuera del OBJ y se lee la VRAM que sigue (o que
		 * precede, con vflip): las teselas van al final de lo mapeado para que esas lecturas caigan en VRAM con banco
		 * (melonDS espeja las zonas sin banco) */
		else if (a0 & 0x1000)
			a2 |= (dc & DC_OBJ1D) ? 0x200u : 0x3C0u;
		OAM(0)[i * 4] = (uint16_t)a0;
		OAM(0)[i * 4 + 1] = (uint16_t)a1;
		OAM(0)[i * 4 + 2] = (uint16_t)a2;
	}
	for (int g = 0; g < 32; g++) {
		const int rotg = chance(70);
		const int sc = 0x80 + (int)rn(0x180);

		OAM(0)[(g * 4 + 0) * 4 + 3] = (uint16_t)(rotg ? sc : 0x100);
		OAM(0)[(g * 4 + 1) * 4 + 3] = (uint16_t)(rotg ? (int)rn(0x200) - 0x100 : 0);
		OAM(0)[(g * 4 + 2) * 4 + 3] = (uint16_t)(rotg ? (int)rn(0x200) - 0x100 : 0);
		OAM(0)[(g * 4 + 3) * 4 + 3] = (uint16_t)(rotg ? sc : 0x100);
	}

	if (off & 0x001)
		dc &= ~DC_OBJ;
	if (off & 0x002)
		dc &= ~(DC_WIN0 | DC_WIN1 | DC_OBJWIN);
	if (off & 0x004)
		ER16(0, E_BLDCNT) = ER16(0, E_BLDCNT) & 0x3F3Fu;
	if (off & 0x008)
		for (int b = 0; b < 4; b++)
			ER16(0, E_BGCNT(b)) = ER16(0, E_BGCNT(b)) & (uint16_t)~0x40u;
	if (off & 0x100)
		dc &= ~7u;
	if (off & 0x200)
		dc &= ~(1u << 3);
	for (int i = 0; i < 128; i++) {
		uint16_t a0 = OAM(0)[i * 4];
		const uint16_t mode = (a0 >> 10) & 3;
		const uint8_t kfirst = *(volatile uint8_t *)0x027FFE15u, klast = *(volatile uint8_t *)0x027FFE16u;

		if (klast && (i < kfirst || i >= klast)) { /* solo los OBJ [kfirst, klast): para aislar diferencias */
			OAM(0)[i * 4] = 0x200;
			continue;
		}

		if (off & 0x010)
			a0 &= (uint16_t)~0x100u;
		if (off & 0x800)
			a0 &= (uint16_t)~0x1000u;
		if ((off & 0x020) && mode == 1)
			a0 &= (uint16_t)~0xC00u;
		if ((off & 0x040) && mode == 2)
			a0 &= (uint16_t)~0xC00u;
		if ((off & 0x080) && mode == 3)
			a0 &= (uint16_t)~0xC00u;
		OAM(0)[i * 4] = a0;
	}
	{
		const uint8_t dbg = *(volatile uint8_t *)0x027FFE17u; /* depuración (tools/rom-seed.py --dbg N) */

		if (dbg & 1)
			dc &= ~(0xFu << 8); /* sin fondos */
		if (dbg & 0x20)
			dc &= ~(3u << 20); /* frontera de OBJ 1D en 32 bytes */
		if (dbg & 0x18) /* sin hflip (8) y/o sin vflip (16) en los OBJ normales */
			for (int i = 0; i < 128; i++)
				if (!(OAM(0)[i * 4] & 0x100))
					OAM(0)[i * 4 + 1] &= (uint16_t)~(((dbg & 8) ? 0x1000u : 0) | ((dbg & 16) ? 0x2000u : 0));
		if (dbg & 2) { /* datos de OBJ todos opacos */
			for (uint32_t a = 0x06400000u; a < 0x06410000u; a += 4)
				V32(a)[0] = 0x11111111u * (1 + ((a >> 7) % 15));
		}
		if (dbg & 4) { /* datos de OBJ todos transparentes en los 4 KB después del OBJ 0 */
			for (uint32_t a = 0x06400000u + 909u * 64u; a < 0x06400000u + 909u * 64u + 8192u; a += 4)
				V32(a)[0] = 0;
		}
	}
	ER32(0, E_DISPCNT) = dc;
	gfx_finish();
}
