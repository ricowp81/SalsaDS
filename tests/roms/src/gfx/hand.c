/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROMs «gfx*» de escenas hechas a mano (Fase 2.1): cada una monta una escena sencilla en los dos motores
 * (los mismos datos, las mismas posiciones) y deja la lista de píxeles que deben tener un color conocido, calculado
 * aquí con la geometría de la escena (sds_expect_pixel). Se comprueban con `salsads-cpurun --expect --same-ab` (la
 * lista y que A y B den la misma imagen) y, para el motor A, contra melonDS (tools/gfx-compare.py).
 *
 * SCENE: 0 texto, 1 afín, 2 extendido de 16 bits, 3 mapa de bits de 256 colores, 4 mapa de bits de color directo,
 * 5 OBJ normales, 6 OBJ afines, 7 OBJ semitransparentes, de ventana y de mapa de bits, 8 ventanas, 9 mezclas y brillo,
 * 10 cambios por línea (raster), 11 modos de pantalla.
 */
#include "gfx.h"

#ifndef SCENE
#define SCENE 0
#endif

#define RED RGB5(31, 0, 0)
#define GREEN RGB5(0, 31, 0)
#define BLUE RGB5(0, 0, 31)
#define YELLOW RGB5(31, 31, 0)
#define CYAN RGB5(0, 31, 31)
#define MAGENTA RGB5(31, 0, 31)
#define WHITE RGB5(31, 31, 31)
#define BACK RGB5(0, 0, 10)

static void expect(int e, int x, int y, uint16_t c)
{
	sds_expect_pixel(e, x, y, c);
}

/* Paleta de fondo y de OBJ con colores conocidos: 1 rojo, 2 verde, 3 azul, 4 amarillo, 5 cian, 6 magenta */
static void palettes(int e)
{
	static const uint16_t c[7] = { BACK, RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA };

	for (int i = 0; i < 7; i++) {
		PAL_BG(e)[i] = c[i];
		PAL_OBJ(e)[i] = c[i];
	}
	PAL_OBJ(e)[0] = 0;
}

static void map16(uint32_t base, int stride, int tx, int ty, uint16_t v)
{
	V16(base)[ty * stride + tx] = v;
}

/* Rellena un mapa de bytes (afín) de 16 en 16: entrada i, valor v */
__attribute__((unused)) static void map8(uint32_t base, int i, uint8_t v)
{
	uint16_t w = V16(base)[i / 2];

	w = (uint16_t)((i & 1) ? ((w & 0x00FF) | (v << 8)) : ((w & 0xFF00) | v));
	V16(base)[i / 2] = w;
}

/* ---- 0: texto ------------------------------------------------------------------------------------------ */
/* BG0 de 4 bits (256×256, prioridad 1) con teselas sólidas, con marca y volteadas, banco de paleta 1, desplazamiento
 * (4,2); BG1 de 8 bits (512×256, prioridad 0) desplazado 256 a la derecha; BG2 de 4 bits (256×512, prioridad 2)
 * desplazado 256 hacia abajo. */
__attribute__((unused)) static void scene_text(int e)
{
	const uint32_t cb = BGV(e);

	palettes(e);
	PAL_BG(e)[16 + 1] = RGB5(31, 31, 1); /* banco 1 color 1 */
	PAL_BG(e)[32 + 1] = RGB5(1, 31, 16); /* banco 2 color 1 */
	gfx_tile4(cb + 32 * 1, 1);
	gfx_tile4_mark(cb + 32 * 2, 1, 2, 3);
	gfx_tile8(cb + 0x8000 + 64 * 1, 5);
	gfx_tile8_mark(cb + 0x8000 + 64 * 2, 6, 5, 4);
	/* BG0: pantalla en 0x4000 (bloque 8), teselas en 0 */
	map16(cb + 0x4000, 32, 2, 1, 1);
	map16(cb + 0x4000, 32, 3, 1, 2);
	map16(cb + 0x4000, 32, 4, 1, 2 | 0x400);
	map16(cb + 0x4000, 32, 5, 1, 2 | 0x800);
	map16(cb + 0x4000, 32, 6, 1, 2 | 0xC00);
	map16(cb + 0x4000, 32, 2, 3, 1 | (1 << 12));
	map16(cb + 0x4000, 32, 1, 4, 1);
	map16(cb + 0x4000, 32, 10, 1, 1);
	ER16(e, E_BGCNT(0)) = (8 << 8) | 1;
	ER16(e, E_BGHOFS(0)) = 4;
	ER16(e, E_BGVOFS(0)) = 2;
	/* BG1: 8 bits, 512×256; bloque de pantalla 9 (0x4800) y 10 (0x5000); teselas en 0x8000 (base 2) */
	map16(cb + 0x5000, 32, 33 - 32, 4, 1);
	map16(cb + 0x5000, 32, 34 - 32, 4, 2);
	ER16(e, E_BGCNT(1)) = (9 << 8) | 0x80 | (2 << 2) | (1 << 14) | 0;
	ER16(e, E_BGHOFS(1)) = 256;
	/* BG2: 4 bits, 256×512; pantallas 12 (0x6000) y 13 (0x6800); desplazado 256 hacia abajo → bloque 13 */
	map16(cb + 0x6800, 32, 10, 1, 1 | (2 << 12));
	ER16(e, E_BGCNT(2)) = (12 << 8) | (2 << 14) | 2;
	ER16(e, E_BGVOFS(2)) = 256;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_BG(1) | DC_BG(2);
	/* BG0: la casilla (tx,ty) está en la pantalla en (8·tx − 4, 8·ty − 2) */
	expect(e, 12, 6, RED);
	expect(e, 19, 13, RED);
	expect(e, 11, 6, BACK);
	expect(e, 20, 6, RED);      /* marca: arriba a la izquierda color 1 */
	expect(e, 27, 6, BLUE);     /* pixel (7,0) color 3 */
	expect(e, 25, 11, GREEN);   /* resto color 2 */
	expect(e, 23, 9, RED);
	expect(e, 28, 6, BLUE);     /* espejo horizontal: el (7,0) pasa a la izquierda */
	expect(e, 35, 6, RED);
	expect(e, 32, 9, RED);
	expect(e, 31, 9, GREEN);
	expect(e, 36, 13, RED);     /* espejo vertical */
	expect(e, 43, 13, BLUE);
	expect(e, 36, 6, GREEN);
	expect(e, 51, 13, RED);     /* los dos espejos */
	expect(e, 44, 13, BLUE);
	expect(e, 12, 22, RGB5(31, 31, 1)); /* banco de paleta 1 */
	expect(e, 19, 29, RGB5(31, 31, 1));
	/* BG0 (1,4) en (4,30) y BG1 (tesela cian en (8,32)): gana BG1 (prioridad 0) donde se cruzan */
	expect(e, 5, 31, RED);
	expect(e, 9, 33, CYAN);
	expect(e, 13, 38, CYAN);
	expect(e, 16, 32, MAGENTA); /* marca de 8 bits: color 6 arriba a la izquierda */
	expect(e, 23, 32, YELLOW);  /* (7,0) color 4 */
	expect(e, 21, 37, CYAN);
	/* BG2 (prioridad 2) debajo de BG0 */
	expect(e, 79, 7, RED);      /* BG0 (10,1) en (76,6) */
	expect(e, 86, 14, RGB5(1, 31, 16)); /* BG2 sola, banco 2 */
}

/* ---- 1: afín -------------------------------------------------------------------------------------------- */
/* Modo 2: BG2 afín de 256×256 con identidad desplazada (la textura (0,0) cae en (10,5)); BG3 afín de 128×128 a mitad de
 * paso (cada texel son 2 píxeles) con envoltura. AFIN = 1: BG2 girado 90° (de 128×128, con envoltura apagada). */
#ifndef AFIN
#define AFIN 0
#endif
__attribute__((unused)) static void scene_affine(int e)
{
	const uint32_t cb = BGV(e);

	palettes(e);
	gfx_tile8(cb + 64 * 1, 1);
	gfx_tile8(cb + 64 * 2, 2);
	gfx_tile8_mark(cb + 64 * 3, 3, 4, 5);
#if AFIN == 0
	/* BG2: 256×256 (32×32 teselas), pantalla en 0x4000 */
	map8(cb + 0x4000, 0, 1);
	map8(cb + 0x4000, 1, 2);
	map8(cb + 0x4000, 32, 3);
	ER16(e, E_BGCNT(2)) = (8 << 8) | (1 << 14) | 0;
	ER16(e, E_BG2PA) = 0x100;
	ER16(e, E_BG2PB) = 0;
	ER16(e, E_BG2PC) = 0;
	ER16(e, E_BG2PD) = 0x100;
	ER32(e, E_BG2X) = (uint32_t)(-10 * 256) & 0x0FFFFFFFu;
	ER32(e, E_BG2Y) = (uint32_t)(-5 * 256) & 0x0FFFFFFFu;
	/* BG3: 128×128 (16×16 teselas), pantalla en 0x4800, envoltura; paso 0,5 y origen −8 */
	map8(cb + 0x4800, 15, 1);
	map8(cb + 0x4800, 0, 2);
	ER16(e, E_BGCNT(3)) = (9 << 8) | 0x2000 | 1;
	ER16(e, E_BG3PA) = 0x80;
	ER16(e, E_BG3PB) = 0;
	ER16(e, E_BG3PC) = 0;
	ER16(e, E_BG3PD) = 0x80;
	ER32(e, E_BG3X) = (uint32_t)(-8 * 256) & 0x0FFFFFFFu;
	ER32(e, E_BG3Y) = 0;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(2) | DC_BG(3) | 2;
	/* BG2: la textura (tx,ty) cae en la pantalla en (tx + 10, ty + 5); fuera del área es transparente */
	expect(e, 10, 5, RED);
	expect(e, 17, 12, RED);
	expect(e, 18, 5, GREEN);
	expect(e, 9, 5, RED);      /* fuera del área de BG2 se ve BG3 (la tesela 15 de BG3 cubre x 0..15, y 0..15) */
	expect(e, 10, 4, RED);
	expect(e, 40, 4, BACK);
	expect(e, 9, 20, BACK);
	expect(e, 10, 13, BLUE);   /* tesela con marca: arriba a la izquierda color 3 */
	expect(e, 17, 13, CYAN);   /* píxel (7,0) color 5 */
	expect(e, 12, 15, BLUE);
	expect(e, 15, 17, YELLOW); /* resto color 4 */
	/* BG3: x de pantalla → texel (x·0,5 − 8) envuelto en 128; la tesela 15 son los texeles 120..127, o sea las
	 * pantallas 0..15; la 0 (verde), las 16..31 */
	expect(e, 0, 2, RED);
	expect(e, 15, 2, RED);
	expect(e, 16, 2, GREEN);
	expect(e, 31, 2, GREEN);
	expect(e, 32, 2, BACK);
	expect(e, 0, 16, BACK); /* y = 16 → texel 8: la fila de teselas 1 está vacía */
#else
	/* BG2: 128×128, sin envoltura, girado 90°: (u, v) = (y, 100 − x). Pantalla en 0x4000; la tesela con marca en el mapa
	 * (0, 12), o sea los texeles u 0..7, v 96..103 */
	map8(cb + 0x4000, 12 * 16, 3);
	ER16(e, E_BGCNT(2)) = (8 << 8) | 1 * 0;
	ER16(e, E_BG2PA) = 0;
	ER16(e, E_BG2PB) = 0x100;
	ER16(e, E_BG2PC) = (uint16_t)-0x100;
	ER16(e, E_BG2PD) = 0;
	ER32(e, E_BG2X) = 0;
	ER32(e, E_BG2Y) = 100u << 8;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(2) | 2;
	expect(e, 4, 0, BLUE);    /* u = 0, v = 96: píxel (0,0) de la marca */
	expect(e, 4, 7, CYAN);    /* u = 7, v = 96: píxel (7,0) */
	expect(e, 0, 0, YELLOW);  /* u = 0, v = 100: (0,4) */
	expect(e, 3, 3, BLUE);    /* u = 3, v = 97: (3,1) */
	expect(e, 5, 0, BACK);    /* v = 95: la tesela (0, 11) está vacía */
	expect(e, 0, 8, BACK);    /* u = 8: la tesela (1, 12) está vacía */
	expect(e, 4, 3, BLUE);
#endif
}

/* ---- 2: extendido de 16 bits ---------------------------------------------------------------------------- */
/* Modo 5: BG2 extendido con mapa de 16 bits (tesela, volteos), 256×256, identidad desplazada (6,3) */
__attribute__((unused)) static void scene_ext16(int e)
{
	const uint32_t cb = BGV(e);

	palettes(e);
	gfx_tile8_mark(cb + 64 * 3, 3, 4, 5);
	map16(cb + 0x4000, 32, 0, 0, 3);
	map16(cb + 0x4000, 32, 1, 0, 3 | 0x400);
	map16(cb + 0x4000, 32, 0, 1, 3 | 0x800);
	map16(cb + 0x4000, 32, 1, 1, 3 | 0xC00);
	ER16(e, E_BGCNT(2)) = (8 << 8) | (1 << 14);
	ER16(e, E_BG2PA) = 0x100;
	ER16(e, E_BG2PD) = 0x100;
	ER16(e, E_BG2PB) = 0;
	ER16(e, E_BG2PC) = 0;
	ER32(e, E_BG2X) = (uint32_t)(-6 * 256) & 0x0FFFFFFFu;
	ER32(e, E_BG2Y) = (uint32_t)(-3 * 256) & 0x0FFFFFFFu;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(2) | 5;
	expect(e, 6, 3, BLUE);
	expect(e, 13, 3, CYAN);
	expect(e, 14, 3, CYAN);   /* espejo horizontal */
	expect(e, 21, 3, BLUE);
	expect(e, 6, 18, BLUE);   /* espejo vertical: el color 1 abajo a la izquierda */
	expect(e, 13, 18, CYAN);
	expect(e, 21, 18, BLUE);  /* los dos */
	expect(e, 14, 18, CYAN);
	expect(e, 6, 11, YELLOW);
	expect(e, 5, 3, BACK);
}

/* ---- 3: mapa de bits de 256 colores y 4: de color directo ---------------------------------------------- */
static void bmp8_put(uint32_t base, int w, int x, int y, int c)
{
	const uint32_t a = base + (uint32_t)(y * w + x);
	uint16_t v = V16(a & ~1u)[0];

	v = (uint16_t)((a & 1) ? ((v & 0x00FF) | (c << 8)) : ((v & 0xFF00) | c));
	V16(a & ~1u)[0] = v;
}

__attribute__((unused)) static void scene_bmp(int e, int direct)
{
	const uint32_t cb = BGV(e);

	palettes(e);
	/* BG3 (modo 5 → extendido), bitmap 256×256 en la base 0x8000 (campo 2) si es de 256 colores; si es de color directo ocupa
	 * 128 KB y va en 0 */
	ER16(e, E_BG3PA) = 0x100;
	ER16(e, E_BG3PD) = 0x100;
	ER16(e, E_BG3PB) = 0;
	ER16(e, E_BG3PC) = 0;
	ER32(e, E_BG3X) = (uint32_t)(-4 * 256) & 0x0FFFFFFFu;
	ER32(e, E_BG3Y) = (uint32_t)(-2 * 256) & 0x0FFFFFFFu;
	if (!direct) {
		const uint32_t bm = cb + 0x8000;

		for (int y = 10; y < 20; y++)
			for (int x = 20; x < 40; x++)
				bmp8_put(bm, 256, x, y, 1);
		bmp8_put(bm, 256, 50, 50, 2);
		for (int x = 0; x < 256; x++)
			bmp8_put(bm, 256, x, 100, 3);
		bmp8_put(bm, 256, 255, 255, 4);
		ER16(e, E_BGCNT(3)) = (2 << 8) | 0x80 | (1 << 14);
		ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(3) | 5;
		/* texel (x,y) → pantalla (x + 4, y + 2) */
		expect(e, 24, 12, RED);
		expect(e, 43, 21, RED);
		expect(e, 44, 21, BACK);
		expect(e, 24, 11, BACK);
		expect(e, 54, 52, GREEN);
		expect(e, 100, 102, BLUE);
		expect(e, 4, 102, BLUE);   /* texel 0 */
		expect(e, 3, 102, BACK);   /* texel −1: fuera del mapa de bits */
		expect(e, 100, 101, BACK);
	} else {
		/* píxeles con el bit 15 (opacos); el resto, transparentes */
		for (int y = 10; y < 20; y++)
			for (int x = 20; x < 40; x++)
				V16(cb)[y * 256 + x] = (uint16_t)(0x8000 | RED);
		V16(cb)[50 * 256 + 50] = (uint16_t)(0x8000 | GREEN);
		V16(cb)[60 * 256 + 60] = (uint16_t)BLUE;           /* sin el bit 15: transparente */
		V16(cb)[70 * 256 + 70] = (uint16_t)(0x8000 | RGB5(12, 20, 5));
		ER16(e, E_BGCNT(3)) = (0 << 8) | 0x80 | 4 | (1 << 14);
		ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(3) | 5;
		expect(e, 24, 12, RED);
		expect(e, 43, 21, RED);
		expect(e, 44, 21, BACK);
		expect(e, 54, 52, GREEN);
		expect(e, 64, 62, BACK);
		expect(e, 74, 72, RGB5(12, 20, 5));
	}
}

/* ---- 5: OBJ normales ------------------------------------------------------------------------------------ */
/* Doce formas y tamaños, cada una de un color distinto (paleta de OBJ p, color 1), con los datos en las teselas
 * consecutivas (mapeo 1D, frontera de 32 bytes); cuatro OBJ de 8×8 con la marca y los volteos; prioridades contra BG0.
 * OBJ2D = 1 usa el mapeo 2D. */
#ifndef OBJ2D
#define OBJ2D 0
#endif
static const uint8_t obj_wh[12][2] = { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 }, { 16, 8 }, { 32, 8 },
				       { 32, 16 }, { 64, 32 }, { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } };

static uint16_t ocolor(int p)
{
	return RGB5(p * 2 + 3, 31 - p * 2, (p * 5) & 31);
}

static void put_obj(int e, int i, int x, int y, int shape, int size, int tile, int prio, int pal, int flips, int c256)
{
	OAM(e)[i * 4] = (uint16_t)((y & 255) | (c256 << 13) | (shape << 14));
	OAM(e)[i * 4 + 1] = (uint16_t)((x & 511) | (flips << 12) | (size << 14));
	OAM(e)[i * 4 + 2] = (uint16_t)((tile & 1023) | (prio << 10) | (pal << 12));
}

__attribute__((unused)) static void scene_obj(int e)
{
	const uint32_t ob = OBJV(e);
	uint32_t tile = 1; /* siguiente tesela libre (de 32 bytes) */
	static const uint8_t xs[12] = { 4, 18, 40, 78, 4, 26, 64, 102, 4, 18, 32, 54 };
	static const uint8_t ys[12] = { 4, 4, 4, 4, 76, 76, 76, 76, 100, 100, 100, 100 };

	palettes(e);
	for (int p = 1; p < 16; p++)
		PAL_OBJ(e)[16 * p + 1] = ocolor(p);
	PAL_OBJ(e)[16 * 15 + 2] = WHITE;
	for (int k = 0; k < 12; k++) {
		const int w = obj_wh[k][0], h = obj_wh[k][1], shape = k / 4, size = k % 4;
		const int n = (w / 8) * (h / 8);

		for (int t = 0; t < n; t++)
			gfx_tile4(ob + 32u * (tile + (uint32_t)t), 1);
#if OBJ2D
		/* mapeo 2D: la fila de teselas r está 32 teselas más abajo: se copian las teselas a su sitio */
		(void)n;
#endif
		put_obj(e, k, xs[k], ys[k], shape, size, (int)tile, 0, k + 1, 0, 0);
		tile += (uint32_t)n;
	}
	/* marca con volteos: cuatro OBJ de 8×8 con la tesela con marca (c1 = 1, c2 = 2, c3 = 3 del banco 15 → 'ocolor(15)', blanco, ...) */
	gfx_tile4_mark(ob + 32u * tile, 1, 2, 3);
	PAL_OBJ(e)[16 * 14 + 1] = RED;
	PAL_OBJ(e)[16 * 14 + 2] = GREEN;
	PAL_OBJ(e)[16 * 14 + 3] = BLUE;
	for (int f = 0; f < 4; f++)
		put_obj(e, 12 + f, 170 + f * 12, 4, 0, 0, (int)tile, 0, 14, f, 0);
	tile++;
	/* prioridades contra BG0 (prioridad 1, amarillo): OBJ de prioridad 0, 1 y 2 sobre tres casillas del fondo */
	gfx_tile4(ob + 32u * tile, 1);
	PAL_OBJ(e)[16 * 13 + 1] = MAGENTA;
	for (int q = 0; q < 3; q++)
		put_obj(e, 16 + q, 172 + q * 24, 40, 0, 0, (int)tile, q, 13, 0, 0);
	tile++;
	/* BG0: casillas amarillas de 16×16 bajo los OBJ de prioridad (tesela 1 de color 4 en el mapa) */
	gfx_tile4(BGV(e) + 32, 4);
	for (int q = 0; q < 3; q++)
		for (int dy = 0; dy < 2; dy++)
			for (int dx = 0; dx < 2; dx++)
				map16(BGV(e) + 0x4000, 32, (170 + q * 24) / 8 + dx, 4 + dy, 1);
	ER16(e, E_BGCNT(0)) = (8 << 8) | 1;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_OBJ | DC_OBJ1D;
	for (int k = 0; k < 12; k++) {
		const int w = obj_wh[k][0], h = obj_wh[k][1], x = xs[k], y = ys[k];

		expect(e, x, y, ocolor(k + 1));
		expect(e, x + w - 1, y + h - 1, ocolor(k + 1));
		expect(e, x + w - 1, y, ocolor(k + 1));
		expect(e, x, y + h - 1, ocolor(k + 1));
		expect(e, x - 1, y, BACK);
		expect(e, x + w, y + h - 1, BACK);
		expect(e, x, y - 1, BACK);
		expect(e, x, y + h, BACK);
	}
	/* la marca: color 1 en el cuarto de arriba a la izquierda (rojo), el (7,0) azul */
	expect(e, 170, 4, RED);
	expect(e, 177, 4, BLUE);
	expect(e, 175, 9, GREEN);
	expect(e, 182 + 7, 4, RED);         /* hflip: el (7,0) azul pasa a la izquierda */
	expect(e, 182, 4, BLUE);
	expect(e, 194, 11, RED);            /* vflip: el color 1 abajo a la izquierda */
	expect(e, 201, 11, BLUE);
	expect(e, 206 + 7, 11, RED);        /* los dos */
	expect(e, 206, 11, BLUE);
	/* prioridades: el OBJ de prioridad 0 y el de 1 ganan al fondo de prioridad 1; el de 2 pierde */
	expect(e, 172, 40, MAGENTA);
	expect(e, 196, 40, MAGENTA);
	expect(e, 220, 40, YELLOW);
	expect(e, 227, 47, YELLOW);
	expect(e, 180, 40, YELLOW);   /* el fondo fuera del OBJ, dentro de la casilla de 16×16 */
}

/* ---- 6: OBJ afines -------------------------------------------------------------------------------------- */
/* Un OBJ de 16×16 hecho de cuatro teselas de colores (R, G / B, Y) con cuatro juegos de parámetros: identidad, el doble de
 * escala (de tamaño doble), girado 90° y a mitad de escala. Los píxeles se calculan con la fórmula de GBATEK:
 * (sx, sy) = ((pa·(dx − cx) + pb·(dy − cy)) >> 8) + w/2, ((pc·(dx − cx) + pd·(dy − cy)) >> 8) + h/2. */
static uint16_t quad_color(int sx, int sy)
{
	if (sx < 0 || sx >= 16 || sy < 0 || sy >= 16)
		return BACK;
	return sy < 8 ? (sx < 8 ? RED : GREEN) : (sx < 8 ? BLUE : YELLOW);
}

__attribute__((unused)) static void scene_objaff(int e)
{
	const uint32_t ob = OBJV(e);
	static const int16_t par[4][4] = { { 0x100, 0, 0, 0x100 }, { 0x80, 0, 0, 0x80 }, { 0, 0x100, -0x100, 0 }, { 0x200, 0, 0, 0x200 } };
	static const int px[4] = { 20, 60, 120, 170 }, py[4] = { 20, 20, 20, 20 };
	static const int dbl[4] = { 0, 1, 0, 0 };

	palettes(e);
	gfx_tile4(ob + 32 * 1, 1);
	gfx_tile4(ob + 32 * 2, 2);
	gfx_tile4(ob + 32 * 3, 3);
	gfx_tile4(ob + 32 * 4, 4);
	for (int k = 0; k < 4; k++) {
		/* grupo k: parámetros en el cuarto atributo de los OBJ 4k..4k+3 */
		OAM(e)[(k * 4 + 0) * 4 + 3] = (uint16_t)par[k][0];
		OAM(e)[(k * 4 + 1) * 4 + 3] = (uint16_t)par[k][1];
		OAM(e)[(k * 4 + 2) * 4 + 3] = (uint16_t)par[k][2];
		OAM(e)[(k * 4 + 3) * 4 + 3] = (uint16_t)par[k][3];
	}
	for (int k = 0; k < 4; k++) {
		const int o = 8 + k; /* los OBJ afines van aparte de las filas con los parámetros */

		OAM(e)[o * 4] = (uint16_t)(py[k] | 0x100 | (dbl[k] ? 0x200 : 0) | 0);
		OAM(e)[o * 4 + 1] = (uint16_t)(px[k] | (k << 9) | (1 << 14));
		OAM(e)[o * 4 + 2] = 1;
	}
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_OBJ | DC_OBJ1D;
	for (int k = 0; k < 4; k++) {
		const int bw = dbl[k] ? 32 : 16, cx = bw / 2;

		for (int dy = 0; dy < bw; dy += (bw == 32 ? 5 : 3))
			for (int dx = 0; dx < bw; dx += (bw == 32 ? 5 : 3)) {
				const int sx = ((par[k][0] * (dx - cx) + par[k][1] * (dy - cx)) >> 8) + 8;
				const int sy = ((par[k][2] * (dx - cx) + par[k][3] * (dy - cx)) >> 8) + 8;

				expect(e, px[k] + dx, py[k] + dy, quad_color(sx, sy));
			}
	}
}

/* ---- helpers de color (fórmulas de 6 bits de melonDS 1.1, ver src/gpu2d/gpu2d_fx.h) ------------------------------ */
static uint16_t blend5(uint16_t a, uint16_t b, int eva, int evb)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		int v = (2 * ((a >> (5 * k)) & 31) * eva + 2 * ((b >> (5 * k)) & 31) * evb + 8) >> 4;

		if (v > 63)
			v = 63;
		r |= (uint16_t)((v >> 1) << (5 * k));
	}
	return r;
}

static uint16_t bup(uint16_t a, int f)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		const int c6 = 2 * ((a >> (5 * k)) & 31);

		r |= (uint16_t)(((c6 + (((63 - c6) * f + 8) >> 4)) >> 1) << (5 * k));
	}
	return r;
}

static uint16_t bdown(uint16_t a, int f)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		const int c6 = 2 * ((a >> (5 * k)) & 31);

		r |= (uint16_t)(((c6 - ((c6 * f + 7) >> 4)) >> 1) << (5 * k));
	}
	return r;
}

/* fondo de pantalla completa de una tesela (mapa de 32×32 con la entrada v), en la pantalla de base `blk` */
static void fill_map(int e, int blk, uint16_t v, int tx0, int tx1)
{
	for (int ty = 0; ty < 24; ty++)
		for (int tx = tx0; tx < tx1; tx++)
			map16(BGV(e) + (uint32_t)blk * 0x800u, 32, tx, ty, v);
}

/* ---- 8: ventanas ---------------------------------------------------------------------------------------- */
/* Tres fondos de pantalla completa (BG0 rojo, BG1 verde, BG2 azul, prioridades 0, 1, 2), la ventana 0 deja ver solo BG1, la 1
 * solo BG2, fuera BG0 y dentro de la ventana de OBJ (un OBJ de 32×32 en modo ventana) BG1 y BG2. WINX = 1: ventanas
 * con X1 > X2 y Y1 > Y2 (sin expectativas: solo la comparación con melonDS). */
#ifndef WINX
#define WINX 0
#endif
__attribute__((unused)) static void scene_win(int e)
{
	const uint32_t cb = BGV(e), ob = OBJV(e);

	palettes(e);
	gfx_tile4(cb + 32 * 1, 1);
	gfx_tile4(cb + 32 * 2, 2);
	gfx_tile4(cb + 32 * 3, 3);
	fill_map(e, 8, 1, 0, 32);
	fill_map(e, 9, 2, 0, 32);
	fill_map(e, 10, 3, 0, 32);
	ER16(e, E_BGCNT(0)) = (8 << 8) | 0;
	ER16(e, E_BGCNT(1)) = (9 << 8) | 1;
	ER16(e, E_BGCNT(2)) = (10 << 8) | 2;
	for (int t = 1; t <= 16; t++)
		gfx_tile4(ob + 32u * (uint32_t)t, 1);
	OAM(e)[0] = (uint16_t)(100 | (2 << 10) | 0);            /* y = 100, modo ventana de OBJ */
	OAM(e)[1] = (uint16_t)(160 | (2 << 14));                 /* x = 160, 32×32 */
	OAM(e)[2] = 1;
#if WINX == 0
	ER16(e, E_WIN0H) = (20 << 8) | 100;
	ER16(e, E_WIN0V) = (10 << 8) | 60;
	ER16(e, E_WIN1H) = (60 << 8) | 140;
	ER16(e, E_WIN1V) = (40 << 8) | 100;
#else
	ER16(e, E_WIN0H) = (200 << 8) | 40;
	ER16(e, E_WIN0V) = (150 << 8) | 30;
	ER16(e, E_WIN1H) = (120 << 8) | 0;
	ER16(e, E_WIN1V) = (20 << 8) | 0;
#endif
	ER16(e, E_WININ) = 0x0402;
	ER16(e, E_WINOUT) = 0x0601;
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_BG(1) | DC_BG(2) | DC_WIN0 | DC_WIN1 | DC_OBJWIN | DC_OBJ | DC_OBJ1D;
#if WINX == 0
	expect(e, 10, 10, RED);
	expect(e, 30, 20, GREEN);
	expect(e, 120, 80, BLUE);
	expect(e, 80, 50, GREEN);    /* en las dos ventanas gana la 0 */
	expect(e, 20, 10, GREEN);
	expect(e, 19, 10, RED);
	expect(e, 20, 9, RED);
	expect(e, 99, 10, GREEN);
	expect(e, 100, 10, RED);     /* el borde derecho no cuenta */
	expect(e, 20, 59, GREEN);
	expect(e, 20, 60, RED);
	expect(e, 100, 40, BLUE);
	expect(e, 139, 99, BLUE);
	expect(e, 140, 99, RED);
	expect(e, 139, 100, RED);
	expect(e, 170, 110, GREEN);  /* dentro de la ventana de OBJ: BG1 y BG2, gana BG1 */
	expect(e, 159, 110, RED);
	expect(e, 192, 110, RED);
	expect(e, 170, 99, RED);
#endif
}

/* ---- 9: mezclas y brillo -------------------------------------------------------------------------------- */
/* BG0 (prioridad 0) rojo en la mitad izquierda; BG1 (prioridad 1) azul en toda la pantalla; el fondo, azul oscuro.
 * BLEND: 0 alfa (BG0 sobre BG1, EVA 10 y EVB 6), 1 brillo arriba de BG0 (8), 2 brillo abajo (8), 3 alfa de BG0 sobre el
 * color de fondo (sin BG1), 4 alfa solo fuera de una ventana (que apaga los efectos), 5 alfa con EVA + EVB > 16. */
#ifndef BLEND
#define BLEND 0
#endif
__attribute__((unused)) static void scene_blend(int e)
{
	const uint32_t cb = BGV(e);

	palettes(e);
	gfx_tile4(cb + 32 * 1, 1);
	gfx_tile4(cb + 32 * 3, 3);
	fill_map(e, 8, 1, 0, 16);
	fill_map(e, 9, 3, 0, 32);
	if (0)
		(void)bup(0, 0), (void)bdown(0, 0), (void)blend5(0, 0, 0, 0);
	ER16(e, E_BGCNT(0)) = (8 << 8) | 0;
	ER16(e, E_BGCNT(1)) = (9 << 8) | 1;
	{
		uint32_t dc = DC_GRAPHICS | DC_BG(0) | DC_BG(1);
		uint16_t left = RED, right = BLUE;

#if BLEND == 0
		ER16(e, E_BLDCNT) = 0x0001 | (1 << 6) | (0x0002 << 8);
		ER16(e, E_BLDALPHA) = 10 | (6 << 8);
		left = blend5(RED, BLUE, 10, 6);
#elif BLEND == 1
		ER16(e, E_BLDCNT) = 0x0001 | (2 << 6);
		ER16(e, E_BLDY) = 8;
		left = bup(RED, 8);
#elif BLEND == 2
		ER16(e, E_BLDCNT) = 0x0001 | (3 << 6);
		ER16(e, E_BLDY) = 8;
		left = bdown(RED, 8);
#elif BLEND == 3
		dc = DC_GRAPHICS | DC_BG(0);
		ER16(e, E_BLDCNT) = 0x0001 | (1 << 6) | (0x0020 << 8);
		ER16(e, E_BLDALPHA) = 12 | (4 << 8);
		left = blend5(RED, BACK, 12, 4);
		right = BACK;
#elif BLEND == 4
		dc |= DC_WIN0;
		ER16(e, E_WIN0H) = (0 << 8) | 8;     /* ventana 0 en x 0..7: x1 = 0 */
		ER16(e, E_WIN0V) = (0 << 8) | 100;   /* y 0..99 */
		ER16(e, E_WININ) = 0x0003;           /* dentro: BG0 y BG1, sin efectos */
		ER16(e, E_WINOUT) = 0x003F;          /* fuera: todo, con efectos */
		ER16(e, E_BLDCNT) = 0x0001 | (1 << 6) | (0x0002 << 8);
		ER16(e, E_BLDALPHA) = 10 | (6 << 8);
		left = blend5(RED, BLUE, 10, 6);
#else
		ER16(e, E_BLDCNT) = 0x0001 | (1 << 6) | (0x0002 << 8);
		ER16(e, E_BLDALPHA) = 16 | (16 << 8);
		left = blend5(RED, BLUE, 16, 16);
#endif
		ER32(e, E_DISPCNT) = dc;
		expect(e, 20, 20, left);
		expect(e, 140, 20, right);
		expect(e, 127, 100, left);
		expect(e, 128, 100, right);
#if BLEND == 4
		expect(e, 3, 20, RED);          /* dentro de la ventana: sin mezcla */
		expect(e, 3, 120, left);        /* debajo de la ventana: con mezcla */
		expect(e, 20, 20, left);
#endif
	}
}

/* ---- 7: OBJ semitransparentes y de mapa de bits -------------------------------------------------------- */
/* BG0 azul a la izquierda (x < 128) y vacío a la derecha. Un OBJ semitransparente rojo de 16×16 sobre cada lado (EVA 10,
 * EVB 6; segundo objetivo solo BG0) y un OBJ de mapa de bits (1D, 16×16, color directo, con algunos píxeles
 * transparentes) de alfa 7 sobre la parte azul. */
__attribute__((unused)) static void scene_objx(int e)
{
	const uint32_t cb = BGV(e), ob = OBJV(e);

	palettes(e);
	gfx_tile4(cb + 32 * 3, 3);
	fill_map(e, 8, 3, 0, 16);
	ER16(e, E_BGCNT(0)) = (8 << 8) | 1;
	for (int t = 1; t <= 4; t++)
		gfx_tile4(ob + 32u * (uint32_t)t, 1);
	/* mapa de bits 1D: la imagen 16×16 empieza en el índice 4 de 128 bytes: filas de 32 bytes */
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			const uint16_t v = (x == 5 && y == 5) ? (uint16_t)(0x8000 | GREEN) : (uint16_t)(0x8000 | RGB5(x * 2, 31 - x, y * 2));

			V16(ob + 128u * 4u)[y * 16 + x] = v;
		}
	/* OBJ 0: semitransparente sobre BG0 (20,20); OBJ 1: semitransparente sobre el fondo vacío (180,20); OBJ 2: mapa de bits
	 * con alfa 7 (60,20) */
	OAM(e)[0] = (uint16_t)(20 | (1 << 10));
	OAM(e)[1] = (uint16_t)(20 | (1 << 14));
	OAM(e)[2] = 1;
	OAM(e)[4] = (uint16_t)(20 | (1 << 10));
	OAM(e)[5] = (uint16_t)(180 | (1 << 14));
	OAM(e)[6] = 1;
	OAM(e)[8] = (uint16_t)(20 | (3 << 10) | (1 << 14) * 0);
	OAM(e)[9] = (uint16_t)(60 | (1 << 14));
	OAM(e)[10] = (uint16_t)(4 | (7 << 12));
	ER16(e, E_BLDCNT) = 0x0100;
	ER16(e, E_BLDALPHA) = 10 | (6 << 8);
	ER32(e, E_DISPCNT) = DC_GRAPHICS | DC_BG(0) | DC_OBJ | DC_OBJ1D | (1u << 6);
	expect(e, 20, 20, blend5(RED, BLUE, 10, 6));
	expect(e, 35, 35, blend5(RED, BLUE, 10, 6));
	expect(e, 180, 20, RED);           /* sobre el fondo vacío: sin mezcla (el color de fondo no es objetivo) */
	/* mapa de bits: alfa 7 → EVA = 8, EVB = 8 con lo que haya debajo (BG0 azul) */
	expect(e, 60, 20, blend5(RGB5(0, 31, 0), BLUE, 8, 8));
	expect(e, 65, 25, blend5(GREEN, BLUE, 8, 8));
	expect(e, 70, 30, blend5(RGB5(10 * 2, 31 - 10, 10 * 2), BLUE, 8, 8));
	expect(e, 76, 36, BLUE);           /* fuera del OBJ: BG0 */
}

/* ---- 13: cambios por línea ------------------------------------------------------------------------------ */
/* BG0 con una tesela de dos colores (4 + 4 píxeles: rojo y el color 2 de la paleta) y, línea a línea, HOFS = y % 8 y el color 2
 * cambiando con la línea. Las primeras 96 líneas se cambian en el HBlank de la anterior; las demás justo al empezar la propia
 * línea (en melonDS y aquí un cambio hecho antes del HBlank se ve en la propia línea). Solo el motor A. */
static uint16_t rcolor(int y)
{
	return RGB5((y * 3) & 31, 31 - (y & 31), (y * 5) & 31);
}

__attribute__((unused)) static void scene_raster(void)
{
	const uint32_t cb = BGV(0);

	palettes(0);
	for (int y = 0; y < 8; y++) {
		V16(cb + 32 + (uint32_t)y * 4)[0] = 0x1111;
		V16(cb + 32 + (uint32_t)y * 4)[1] = 0x2222;
	}
	fill_map(0, 8, 1, 0, 32);
	ER16(0, E_BGCNT(0)) = (8 << 8);
	ER32(0, E_DISPCNT) = DC_GRAPHICS | DC_BG(0);
	for (int y = 0; y < 192; y += 7)
		for (int x = 0; x < 256; x += 5)
			expect(0, x, y, (((x + (y % 8)) % 8) < 4) ? RED : rcolor(y));
	for (int frame = 0; frame < 6; frame++) {
		gfx_vsync();
		ER16(0, E_BGHOFS(0)) = 0;
		PAL_BG(0)[2] = rcolor(0);
		if (frame == 2)
			gfx_capture();
		if (frame == 5)
			break;
		for (int y = 1; y < 192; y++) {
			if (y < 96) {
				while (REG16(R_VCOUNT) != y - 1)
					;
				while (!(REG16(R_DISPSTAT) & 2))
					;
			} else {
				while (REG16(R_VCOUNT) != y)
					;
			}
			ER16(0, E_BGHOFS(0)) = (uint16_t)(y % 8);
			PAL_BG(0)[2] = rcolor(y);
		}
	}
	sds_finish_quiet(1);
}


/* ---- 11: modos de pantalla (DISPCNT bits 16-17) y MASTER_BRIGHT -------------------------------------------------- */
/* DMODE 0: pantalla apagada (blanca) en los dos motores, con brillo hacia negro (A: -8/16, B: -16/16); 2: visualización
 * de VRAM (banco B como LCDC; solo el motor A) con MASTER_BRIGHT hacia blanco; 3: memoria principal por la FIFO de
 * DISP_MMEM_FIFO con DMA en modo 4 (solo el motor A). En 2 y 3 el motor B muestra un fondo de color liso. La captura de
 * melonDS toma la fuente B (VRAM o FIFO), antes del brillo. */
#ifndef DMODE
#define DMODE 0
#endif
static uint16_t mpix(int x, int y)
{
	return RGB5(x >> 3, y / 6, (x + y) & 31);
}

/* Brillo hacia blanco de un canal de 5 bits como lo hace el hardware (aritmética de 6 bits; docs/fase2.md) */
__attribute__((unused)) static int bright_up5(int c5, int f)
{
	const int c6 = c5 * 2;

	return (c6 + (((63 - c6) * f + 8) >> 4)) >> 1;
}

__attribute__((unused)) static void scene_modes(void)
{
	const uint32_t cb = BGV(1);

	/* el motor B: un fondo de texto liso (rojo) */
	PAL_BG(1)[0] = BACK;
	PAL_BG(1)[1] = RED;
	gfx_tile4(cb + 32, 1);
	for (int i = 0; i < 32 * 32; i++)
		V16(cb + 0x4000u)[i] = 1;
	ER16(1, E_BGCNT(0)) = (8 << 8);
	ER32(1, E_DISPCNT) = DC_GRAPHICS | DC_BG(0);
#if DMODE == 0
	PAL_BG(0)[0] = RED;
	ER32(0, E_DISPCNT) = DC_BG(0) | DC_OBJ; /* modo 0: la pantalla queda blanca */
	ER16(0, E_MASTER_BRIGHT) = (2u << 14) | 8; /* hacia negro, 8/16 */
	ER32(1, E_DISPCNT) = DC_BG(0);             /* B también apagada */
	ER16(1, E_MASTER_BRIGHT) = (2u << 14) | 16; /* negro */
	for (int y = 0; y < 192; y += 11)
		for (int x = 0; x < 256; x += 13) {
			expect(0, x, y, RGB5(15, 15, 15));
			expect(1, x, y, 0);
		}
	gfx_frames(6);
	sds_finish_quiet(1);
#elif DMODE == 2
	REG8(R_VRAMCNT(1)) = 0x80; /* B: LCDC (se muestra en el motor A) */
	for (int y = 0; y < 192; y++)
		for (int x = 0; x < 256; x++)
			V16(0x06820000u)[y * 256 + x] = (uint16_t)(0x8000 | mpix(x, y));
	ER16(0, E_MASTER_BRIGHT) = (1u << 14) | 6; /* hacia blanco, 6/16 */
	ER32(0, E_DISPCNT) = (2u << 16) | (1u << 18); /* modo 2, banco B */
	for (int y = 0; y < 192; y += 11)
		for (int x = 0; x < 256; x += 13) {
			const uint16_t c = mpix(x, y);
			const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

			expect(0, x, y, RGB5(bright_up5(r, 6), bright_up5(g, 6), bright_up5(b, 6)));
			expect(1, x, y, RED);
		}
	gfx_frames(2);
	ER32(0, E_DISPCAP) = 0x80000000u | (1u << 29) | (3u << 20); /* fuente B = VRAM (banco B) */
	gfx_frames(4);
	sds_finish_quiet(1);
#elif DMODE == 3
	static uint16_t buf[256 * 192] __attribute__((aligned(32)));

	sds_nocache();
	for (int y = 0; y < 192; y++)
		for (int x = 0; x < 256; x++)
			buf[y * 256 + x] = (uint16_t)(0x8000 | mpix(x, y));
	ER32(0, E_DISPCNT) = 3u << 16; /* modo 3 */
	for (int y = 0; y < 192; y += 11)
		for (int x = 0; x < 256; x += 13) {
			expect(0, x, y, mpix(x, y));
			expect(1, x, y, RED);
		}
	for (int frame = 0; frame < 7; frame++) {
		gfx_vsync();
		/* cada cuadro vuelve a armar el DMA (la fuente avanza con cada transferencia): se apaga y se arma de nuevo, que
		 * escribir el bit de habilitación estando ya en 1 no recarga la fuente. 4 palabras por disparo */
		REG32(R_DMA(3, 2)) = 0;
		REG32(R_DMA(3, 0)) = (uint32_t)(uintptr_t)buf;
		REG32(R_DMA(3, 1)) = 0x04000068u;
		REG32(R_DMA(3, 2)) = 4u | (2u << 21) | (1u << 25) | (1u << 26) | (4u << 27) | (1u << 31);
		if (frame == 2)
			ER32(0, E_DISPCAP) = 0x80000000u | (1u << 29) | (1u << 25) | (3u << 20); /* fuente B = FIFO */
	}
	gfx_frames(1);
	REG32(R_DMA(3, 2)) = 0; /* sin DMA los últimos cuadros repiten lo último que entró por la FIFO */
	sds_finish_quiet(1);
#endif
}

/* El resto de las escenas se agrega por familias (ver docs/fase2.md) */
int main(void)
{
	sds_init_quiet();
	gfx_power();
	gfx_banks();
	gfx_clear_all();
#if SCENE == 13
	scene_raster();
#endif
#if SCENE == 11
	scene_modes();
#endif
	for (int e = 0; e < 2; e++) {
#if SCENE == 0
		scene_text(e);
#elif SCENE == 1 || SCENE == 12
		scene_affine(e);
#elif SCENE == 2
		scene_ext16(e);
#elif SCENE == 3
		scene_bmp(e, 0);
#elif SCENE == 4
		scene_bmp(e, 1);
#elif SCENE == 5
		scene_obj(e);
#elif SCENE == 6
		scene_objaff(e);
#elif SCENE == 7
		scene_objx(e);
#elif SCENE == 8
		scene_win(e);
#elif SCENE == 9
		scene_blend(e);
#endif
	}
	gfx_finish();
}
