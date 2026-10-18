/*
 * SalsaDS — ROM «gx3d» (Fase 3.2): escenas conocidas del motor de render 3D. Solo registros de hardware (GBATEK).
 *
 * Una sola ROM, una escena por semilla (el código de juego de la cabecera, tools/rom-seed.py, como gfxrand): monta la
 * escena, la manda al motor de geometría, hace SWAP_BUFFERS y deja pasar unos cuadros; después captura el motor A al
 * banco A (DISPCAPCNT). Se compara la imagen de SalsaDS con la de melonDS con tools/gfx-compare.py (la captura de
 * melonDS por su stub de GDB) y, en SalsaDS, los píxeles de prueba con `salsads-cpurun --expect`.
 *
 * Píxeles de prueba: interiores de color conocido y algunos de borde según el modelo de rasterizado de SalsaDS
 * (docs/fase3.md, 3.2). Si la captura existe (melonDS, una DS real: DISPCAPCNT bit 31 vuelve a 0), la ROM los lee del
 * banco A y anota con SDS_DIVERGE los que no coinciden: así se ve, en cualquier emulador o en la consola, dónde se
 * separa del modelo. SalsaDS no emula la captura: ahí la lista la comprueba el corredor.
 *
 * Escenas (semilla): 0 cobertura (formas, aristas x/y-mayores, alambre, altura cero, recorte), 1 Gouraud, 2 textura en
 * perspectiva, 3 los 7 formatos de textura y repetir/espejo/recorte, 4 translúcidos (ids, orden, profundidad), 5 Z-buffer,
 * 6 W-buffer, 7 decal y toon, 8 highlight, 9 alpha test, niebla y edge marking, 10 sombras, 11 3D sobre 2D (mezcla con
 * BG1), 12 prueba de profundidad «igual», 13 y 14 triángulos al azar (cobertura y Gouraud) para ajustar el modelo,
 * 15 coordenadas de textura generadas (modos 1 a 3 de TEXIMAGE_PARAM), 16 y 17 interpolación del color a lo largo
 * de la línea y de las aristas.
 */
#include "../gfx/gfx.h"

#define GXP(c) REG32(0x04000400u + 4u * (c))
#define R_DISP3DCNT 0x04000060u
#define R_CLEAR_COLOR 0x04000350u
#define R_CLEAR_DEPTH 0x04000354u
#define R_ALPHA_TEST 0x04000340u
#define R_EDGE_COLOR 0x04000330u
#define R_FOG_COLOR 0x04000358u
#define R_FOG_OFFSET 0x0400035Cu
#define R_FOG_TABLE 0x04000360u
#define R_TOON 0x04000380u

enum {
	MTX_MODE = 0x10, MTX_PUSH = 0x11, MTX_POP = 0x12, MTX_IDENTITY = 0x15, MTX_LOAD_4x4 = 0x16, MTX_TRANS = 0x1C,
	COLOR = 0x20, TEXCOORD = 0x22, VTX_16 = 0x23, POLY_ATTR = 0x29, TEXIMAGE = 0x2A, PLTT = 0x2B,
	BEGIN = 0x40, END = 0x41, SWAP = 0x50, VIEWPORT = 0x60,
};

#define ATTR(mode, alpha, id) (((uint32_t)(mode) << 4) | 0xC0u | ((uint32_t)(alpha) << 16) | ((uint32_t)(id) << 24))
#define A_TDEPTH (1u << 11)
#define A_FAR (1u << 12)
#define A_EQUAL (1u << 14)
#define A_FOG (1u << 15)

static uint32_t scene;

static void cmd(int c, uint32_t v)
{
	GXP(c) = v;
}

/* Pantalla → 1.3.12 en proyección ortográfica identidad (x exacta; y con la división truncada del viewport). */
static int32_t sxv(int x)
{
	return 32 * x - 4096;
}
static int32_t syv(int y)
{
	return 4096 - (y * 4096 + 95) / 96;
}

static void vtx(int32_t x, int32_t y, int32_t z)
{
	GXP(VTX_16) = ((uint32_t)x & 0xFFFFu) | ((uint32_t)y << 16);
	GXP(VTX_16) = (uint32_t)z & 0xFFFFu;
}
/* vértice en coordenadas de pantalla (ortográfica) */
static void sv(int x, int y, int32_t z)
{
	vtx(sxv(x), syv(y), z);
}
static void tc(int s, int t) /* texel */
{
	cmd(TEXCOORD, ((uint32_t)(s * 16) & 0xFFFFu) | ((uint32_t)(t * 16) << 16));
}

static void ident_all(void)
{
	cmd(MTX_MODE, 0);
	cmd(MTX_IDENTITY, 0);
	cmd(MTX_MODE, 3);
	cmd(MTX_IDENTITY, 0);
	cmd(MTX_MODE, 2);
	cmd(MTX_IDENTITY, 0);
}

/* Proyección en perspectiva sencilla: x' = x, y' = y·4/3 (pantalla 4:3), z' = z·a + b, w = -z */
static void persp(void)
{
	static const int32_t m[16] = { 0x1000, 0, 0, 0, 0, 0x1555, 0, 0, 0, 0, -0x1100, -0x1000, 0, 0, -0x0400, 0 };

	cmd(MTX_MODE, 0);
	for (int i = 0; i < 16; i++)
		GXP(MTX_LOAD_4x4) = (uint32_t)m[i];
	cmd(MTX_MODE, 2);
	cmd(MTX_IDENTITY, 0);
}

static void quad(int x0, int y0, int x1, int y1, int32_t z, uint16_t c)
{
	cmd(BEGIN, 1);
	cmd(COLOR, c);
	sv(x0, y0, z);
	sv(x0, y1, z);
	sv(x1, y1, z);
	sv(x1, y0, z);
	cmd(END, 0);
}

static void tri(int x0, int y0, int x1, int y1, int x2, int y2, int32_t z, uint16_t c)
{
	cmd(BEGIN, 0);
	cmd(COLOR, c);
	sv(x0, y0, z);
	sv(x1, y1, z);
	sv(x2, y2, z);
	cmd(END, 0);
}

/* cuadrilátero con textura: (s0,t0) arriba a la izquierda, (s1,t1) abajo a la derecha */
static void tquad(int x0, int y0, int x1, int y1, int32_t z, int s0, int t0, int s1, int t1)
{
	cmd(BEGIN, 1);
	cmd(COLOR, RGB5(31, 31, 31));
	tc(s0, t0);
	sv(x0, y0, z);
	tc(s0, t1);
	sv(x0, y1, z);
	tc(s1, t1);
	sv(x1, y1, z);
	tc(s1, t0);
	sv(x1, y0, z);
	cmd(END, 0);
}

/* ---- píxeles de prueba ----------------------------------------------------------------------------- */

#define NPROBE 64
static struct {
	uint16_t x, y, c;
} probe[NPROBE];
static int nprobe;

static void expect(int x, int y, uint16_t c)
{
	sds_expect_pixel(0, x, y, c);
	if (nprobe < NPROBE) {
		probe[nprobe].x = (uint16_t)x;
		probe[nprobe].y = (uint16_t)y;
		probe[nprobe].c = c;
		nprobe++;
	}
}

/* ---- texturas (bancos B y C en modo textura, E en modo paleta) ----------------------------------------------- */

#define TEXB 0x06820000u /* banco B por LCDC: ranura 0 */
#define TEXC 0x06840000u /* banco C por LCDC: ranura 1 (índices del 4x4) */
#define PALE 0x06880000u /* banco E por LCDC: paletas */

/* Desplazamientos en la ranura 0 */
#define T_DIRECT 0x0000u /* 32×32 directo, 2 KB */
#define T_256 0x0800u    /* 32×32 de 256 colores */
#define T_16 0x0C00u     /* 32×32 de 16 colores */
#define T_4 0x0E00u      /* 32×32 de 4 colores */
#define T_A3I5 0x1000u
#define T_A5I3 0x1400u
#define T_4X4 0x1800u    /* 32×32 comprimido: 64 bloques de 4 bytes; índices en 0x20000 + 0xC00 */
#define T_CHECK 0x2000u  /* 64×64 directo: tablero de ajedrez */
#define P_256 0x0000u
#define P_16 0x0200u
#define P_4 0x0240u
#define P_A3I5 0x0280u
#define P_A5I3 0x0300u
#define P_4X4 0x0400u

static uint32_t texparam(uint32_t off, int ss, int ts, int fmt, int c0t, int rep, int flip)
{
	return (off >> 3) | ((uint32_t)(rep & 1) << 16) | ((uint32_t)(rep >> 1) << 17) | ((uint32_t)(flip & 1) << 18) |
	       ((uint32_t)(flip >> 1) << 19) | ((uint32_t)ss << 20) | ((uint32_t)ts << 23) | ((uint32_t)fmt << 26) |
	       ((uint32_t)c0t << 29);
}

static uint16_t pcolor(int i) /* paleta de prueba: colores bien separados */
{
	return RGB5((i * 7) & 31, (i * 13 + 5) & 31, (i * 3 + 20) & 31);
}

static void make_textures(void)
{
	REG8(R_VRAMCNT(1)) = 0x80; /* B: LCDC */
	REG8(R_VRAMCNT(2)) = 0x80; /* C: LCDC */
	REG8(R_VRAMCNT(4)) = 0x80; /* E: LCDC */
	/* directo 32×32: cuadrantes de colores, borde transparente en la columna 31 */
	for (int t = 0; t < 32; t++)
		for (int s = 0; s < 32; s++) {
			uint16_t c = RGB5(s, t, (s ^ t) & 31) | 0x8000u;

			if (s == 31)
				c = 0;
			V16(TEXB + T_DIRECT)[t * 32 + s] = c;
		}
	/* 256 colores: índice = (s + 4t) & 255 */
	for (int i = 0; i < 32 * 32; i += 2)
		V16(TEXB + T_256)[i / 2] = (uint16_t)((((i % 32) + 4 * (i / 32)) & 255) | (((((i + 1) % 32) + 4 * (i / 32)) & 255) << 8));
	/* 16 colores: índice = (s / 2 + t / 4) & 15 */
	for (int i = 0; i < 32 * 32; i += 4) {
		uint16_t v = 0;

		for (int k = 0; k < 4; k++)
			v |= (uint16_t)(((((i + k) % 32) / 2 + (i / 32) / 4) & 15) << (4 * k));
		V16(TEXB + T_16)[i / 4] = v;
	}
	/* 4 colores: índice = (s / 8 + t / 8) & 3 */
	for (int i = 0; i < 32 * 32; i += 8) {
		uint16_t v = 0;

		for (int k = 0; k < 8; k++)
			v |= (uint16_t)(((((i + k) % 32) / 8 + (i / 32) / 8) & 3) << (2 * k));
		V16(TEXB + T_4)[i / 8] = v;
	}
	/* A3I5: índice = s, alfa = t / 4 */
	for (int i = 0; i < 32 * 32; i += 2) {
		const int s0 = i % 32, t = i / 32;

		V16(TEXB + T_A3I5)[i / 2] = (uint16_t)((s0 | ((t / 4) << 5)) | (((s0 + 1) | ((t / 4) << 5)) << 8));
	}
	/* A5I3: índice = s / 4, alfa = t */
	for (int i = 0; i < 32 * 32; i += 2) {
		const int s0 = i % 32, t = i / 32;

		V16(TEXB + T_A5I3)[i / 2] = (uint16_t)(((s0 / 4) | (t << 3)) | ((((s0 + 1) / 4) | (t << 3)) << 8));
	}
	/* 4x4: cada bloque con un patrón de índices y uno de los 4 modos; paleta del bloque b en P_4X4 + 8·(b & 7) */
	for (int b = 0; b < 64; b++) {
		const uint32_t data = 0xE4E4E4E4u ^ ((uint32_t)b * 0x01010101u & 0x55555555u);

		V32(TEXB + T_4X4)[b] = data;
		V16(TEXC + 0x0C00u)[b] = (uint16_t)(((uint32_t)(b & 7) * 2u) | ((uint32_t)(b & 3) << 14));
	}
	/* tablero 64×64 directo, casillas de 8 */
	for (int t = 0; t < 64; t++)
		for (int s = 0; s < 64; s++)
			V16(TEXB + T_CHECK)[t * 64 + s] = (((s >> 3) ^ (t >> 3)) & 1) ? (RGB5(31, 31, 0) | 0x8000u) : (RGB5(0, 0, 25) | 0x8000u);
	/* paletas */
	for (int i = 0; i < 256; i++)
		V16(PALE + P_256)[i] = pcolor(i);
	for (int i = 0; i < 16; i++)
		V16(PALE + P_16)[i] = pcolor(i * 17 + 3);
	for (int i = 0; i < 4; i++)
		V16(PALE + P_4)[i] = pcolor(i * 60 + 9);
	for (int i = 0; i < 32; i++)
		V16(PALE + P_A3I5)[i] = pcolor(i * 5 + 1);
	for (int i = 0; i < 8; i++)
		V16(PALE + P_A5I3)[i] = pcolor(i * 31 + 2);
	for (int i = 0; i < 32; i++)
		V16(PALE + P_4X4)[i] = pcolor(i * 9 + 4);
	REG8(R_VRAMCNT(1)) = 0x83; /* B: textura, ranura 0 */
	REG8(R_VRAMCNT(2)) = 0x8B; /* C: textura, ranura 1 */
	REG8(R_VRAMCNT(4)) = 0x83; /* E: paletas 0-3 */
}

static void use_tex(uint32_t tp, uint32_t pltt)
{
	cmd(TEXIMAGE, tp);
	cmd(PLTT, pltt);
}

/* ---- escenas --------------------------------------------------------------------------------------------- */

#define RED RGB5(31, 0, 0)
#define GREEN RGB5(0, 31, 0)
#define BLUE RGB5(0, 0, 31)
#define WHITE RGB5(31, 31, 31)
#define YELLOW RGB5(31, 31, 0)
#define BACK RGB5(4, 4, 8)

static void s_coverage(void)
{
	ident_all();
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	quad(10, 10, 40, 30, 0, RED);                  /* rectángulo: x 10..39, y 10..29 */
	expect(10, 10, RED);
	expect(39, 29, RED);
	expect(40, 20, BACK);
	expect(20, 30, BACK);
	tri(50, 10, 45, 60, 60, 60, 0, GREEN);         /* aristas y-mayores */
	expect(52, 40, GREEN);
	tri(70, 10, 70, 16, 140, 20, 0, BLUE);         /* x-mayor larga y delgada */
	expect(100, 15, BLUE);
	tri(150, 10, 200, 10, 175, 40, 0, YELLOW);     /* arriba plano */
	expect(175, 20, YELLOW);
	tri(210, 40, 230, 10, 250, 40, 0, WHITE);      /* abajo plano */
	expect(230, 30, WHITE);
	tri(20, 50, 23, 50, 21, 53, 0, RED);           /* diminuto */
	cmd(POLY_ATTR, ATTR(0, 0, 2));                 /* alambre */
	quad(60, 70, 120, 110, 0, GREEN);
	expect(60, 90, GREEN);
	expect(90, 90, BACK);
	cmd(POLY_ATTR, ATTR(0, 31, 3));
	tri(150, 80, 200, 80, 175, 80, 0, WHITE);      /* altura cero */
	/* que cruza el borde de la pantalla (recorte del motor de geometría) */
	cmd(BEGIN, 0);
	cmd(COLOR, BLUE);
	vtx(sxv(230), syv(150), 0);
	vtx(sxv(270) > 0x7FFF ? 0x7FFF : sxv(270), syv(170), 0);
	vtx(sxv(240), syv(200) < -0x8000 ? -0x8000 : syv(200), 0);
	cmd(END, 0);
	/* tira de triángulos y de cuadriláteros */
	cmd(BEGIN, 2);
	cmd(COLOR, RED);
	sv(10, 130, 0);
	cmd(COLOR, GREEN);
	sv(10, 170, 0);
	cmd(COLOR, BLUE);
	sv(40, 130, 0);
	cmd(COLOR, YELLOW);
	sv(50, 175, 0);
	cmd(COLOR, WHITE);
	sv(80, 125, 0);
	cmd(END, 0);
	cmd(BEGIN, 3);
	cmd(COLOR, GREEN);
	sv(100, 130, 0);
	sv(100, 180, 0);
	sv(130, 130, 0);
	sv(135, 185, 0);
	sv(170, 120, 0);
	sv(160, 180, 0);
	cmd(END, 0);
}

static void gtri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1, int x2, int y2, uint16_t c2)
{
	cmd(BEGIN, 0);
	cmd(COLOR, c0);
	sv(x0, y0, 0);
	cmd(COLOR, c1);
	sv(x1, y1, 0);
	cmd(COLOR, c2);
	sv(x2, y2, 0);
	cmd(END, 0);
}

static void s_gouraud(void)
{
	ident_all();
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	gtri(20, 10, RED, 10, 120, GREEN, 120, 100, BLUE);
	cmd(BEGIN, 1);
	cmd(COLOR, RGB5(0, 0, 0));
	sv(130, 20, 0);
	cmd(COLOR, RGB5(31, 0, 0));
	sv(130, 180, 0);
	cmd(COLOR, RGB5(31, 31, 31));
	sv(250, 180, 0);
	cmd(COLOR, RGB5(0, 31, 0));
	sv(250, 20, 0);
	cmd(END, 0);
	expect(130, 20, RGB5(0, 0, 0));
}

/* Un cuadrilátero inclinado en perspectiva con el tablero (w de 1 a ~4) */
static void s_persp(void)
{
	persp();
	use_tex(texparam(T_CHECK, 3, 3, 7, 0, 3, 0), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	cmd(BEGIN, 1);
	cmd(COLOR, WHITE);
	tc(0, 0);
	vtx(-0x1800, -0x0800, -0x1000);
	tc(0, 128);
	vtx(-0x1800, -0x0800, -0x5000);
	tc(128, 128);
	vtx(0x1800, -0x0800, -0x5000);
	tc(128, 0);
	vtx(0x1800, -0x0800, -0x1000);
	cmd(END, 0);
	/* y otro de pie, girado: Gouraud con perspectiva */
	cmd(BEGIN, 0);
	cmd(COLOR, RED);
	vtx(-0x0C00, 0x0200, -0x1400);
	cmd(COLOR, GREEN);
	vtx(-0x0400, 0x0C00, -0x3800);
	cmd(COLOR, BLUE);
	vtx(0x0800, 0x0000, -0x2000);
	cmd(END, 0);
}

static void s_formats(void)
{
	static const struct {
		uint32_t off, pal;
		int fmt, c0t;
	} f[7] = {
		{ T_A3I5, P_A3I5 >> 4, 1, 0 }, { T_4, P_4 >> 3, 2, 1 }, { T_16, P_16 >> 4, 3, 1 }, { T_256, P_256 >> 4, 4, 0 },
		{ T_4X4, P_4X4 >> 4, 5, 0 },   { T_A5I3, P_A5I3 >> 4, 6, 0 }, { T_DIRECT, 0, 7, 0 },
	};

	ident_all();
	for (int i = 0; i < 7; i++) {
		const int x = 4 + (i % 4) * 62, y = 4 + (i / 4) * 62;

		use_tex(texparam(f[i].off, 2, 2, f[i].fmt, f[i].c0t, 0, 0), f[i].pal);
		cmd(POLY_ATTR, ATTR(0, 31, i + 1));
		tquad(x, y, x + 48, y + 48, 0, 0, 0, 32, 32);
	}
	/* repetir, espejo y recorte: el directo con coordenadas de -32 a 64 */
	use_tex(texparam(T_DIRECT, 2, 2, 7, 0, 3, 0), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 10));
	tquad(4, 130, 60, 186, 0, -32, -32, 64, 64);
	use_tex(texparam(T_DIRECT, 2, 2, 7, 0, 3, 3), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 11));
	tquad(66, 130, 122, 186, 0, -32, -32, 64, 64);
	use_tex(texparam(T_DIRECT, 2, 2, 7, 0, 0, 0), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 12));
	tquad(128, 130, 184, 186, 0, -32, -32, 64, 64);
	use_tex(texparam(T_DIRECT, 2, 2, 7, 0, 1, 2), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 13));
	tquad(190, 130, 246, 186, 0, -32, -32, 64, 64);
}

/* Coordenadas de textura generadas (TEXIMAGE_PARAM bits 30-31): 1 = (s, t, 1, 1) por la matriz de textura, 2 = la normal
 * por la matriz más (s, t), 3 = el vértice por la matriz más (s, t). Tablero de celdas; cada una con su matriz. */
static void nrm(int x, int y, int z)
{
	cmd(0x21, ((uint32_t)x & 0x3FFu) | (((uint32_t)y & 0x3FFu) << 10) | (((uint32_t)z & 0x3FFu) << 20));
}

static void texgen_cell(int x, int y, uint32_t mode, int32_t dg, int32_t tx, int32_t ty, int32_t tz, int s0, int t0)
{
	static const int vn[4][3] = { { -400, -300, 200 }, { -300, 350, -200 }, { 400, 300, 100 }, { 300, -350, -100 } };
	static const int vp[4][2] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
	const int sz = 56;

	cmd(MTX_MODE, 3);
	cmd(MTX_IDENTITY, 0);
	for (int i = 0; i < 16; i++)
		GXP(MTX_LOAD_4x4) = (uint32_t)((i == 0 || i == 5 || i == 10) ? dg : i == 12 ? tx : i == 13 ? ty : i == 14 ? tz : i == 15 ? 0x1000 : i == 1 ? dg / 7 : i == 8 ? dg / 5 : i == 9 ? dg / 3 : 0);
	cmd(MTX_MODE, 2);
	use_tex(texparam(T_DIRECT, 2, 2, 7, 0, 3, 0) | (mode << 30), 0);
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	cmd(BEGIN, 1);
	for (int k = 0; k < 4; k++) {
		tc(s0, t0); /* el modo 2 calcula en NORMAL: un TEXCOORD después lo pisaría */
		nrm(vn[k][0], vn[k][1], vn[k][2]);
		cmd(COLOR, RGB5(31, 31, 31));
		sv(x + vp[k][0] * sz, y + vp[k][1] * sz, 0);
	}
	cmd(END, 0);
}

static void s_texgen(void)
{
	ident_all();
	/* modo 1 (s, t, 1, 1) x M; modo 2 normal x M; modo 3 vértice x M; sin y con traslación */
	texgen_cell(4, 4, 1, 0x1000, 0, 0, 0, 3, 5);
	texgen_cell(66, 4, 1, 0x2000, 0x3000, 0x5000, 0x1000, 3, 5);
	texgen_cell(128, 4, 2, 0x10000, 0, 0, 0, 0, 0);
	texgen_cell(190, 4, 2, 0x100000, 0, 0, 0, 0, 0);
	texgen_cell(4, 66, 2, 0x400000, 0, 0, 0, 0, 0);
	texgen_cell(66, 66, 2, 0x100000, 0x400000, 0x200000, 0x100000, 0, 0);
	texgen_cell(128, 66, 3, 0x100000, 0, 0, 0, 0, 0);
	texgen_cell(190, 66, 3, 0x1000000, 0, 0, 0, 0, 0);
	texgen_cell(4, 128, 3, 0x100000, 0x400000, 0x200000, 0x100000, 0, 0);
	texgen_cell(66, 128, 1, 0x10000, 0, 0, 0, 0, 0);
	texgen_cell(128, 128, 1, 0x10000, 0x40000, 0x80000, 0x100000, 0, 0);
	texgen_cell(190, 128, 1, 0x4000, 0x10000, 0x20000, 0, 7, 9);
}

static void s_translucent(int manual)
{
	ident_all();
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	quad(20, 20, 236, 172, 0x0800, RGB5(0, 10, 20));
	/* dos translúcidos con ids distintos que se pisan: los dos se mezclan */
	cmd(POLY_ATTR, ATTR(0, 16, 2));
	quad(30, 30, 110, 90, 0, RED);
	cmd(POLY_ATTR, ATTR(0, 16, 3));
	quad(70, 50, 150, 110, 0, GREEN);
	/* mismo id: el segundo no se mezcla donde ya está el primero */
	cmd(POLY_ATTR, ATTR(0, 12, 4));
	quad(160, 30, 220, 90, 0, BLUE);
	quad(190, 60, 230, 120, 0, YELLOW);
	/* orden: dos translúcidos, el de arriba enviado después (con orden automático se dibuja antes) */
	cmd(POLY_ATTR, ATTR(0, 20, 5));
	quad(40, 130, 120, 170, 0, RED);
	cmd(POLY_ATTR, ATTR(0, 8, 6));
	quad(60, 115, 140, 160, 0, WHITE);
	/* translúcido que escribe profundidad y un opaco detrás después */
	cmd(POLY_ATTR, ATTR(0, 20, 7) | A_TDEPTH);
	quad(160, 130, 220, 170, -0x0400, GREEN);
	cmd(POLY_ATTR, ATTR(0, 31, 8));
	quad(180, 140, 240, 180, 0x0400, RED);
	cmd(SWAP, manual ? 1u : 0u);
}

static void s_depth(int wbuf)
{
	persp();
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	cmd(BEGIN, 1);
	cmd(COLOR, RED);
	vtx(-0x1800, -0x1000, -0x1200);
	vtx(-0x1800, 0x1000, -0x1200);
	vtx(0x1800, 0x1000, -0x4800);
	vtx(0x1800, -0x1000, -0x4800);
	cmd(END, 0);
	cmd(POLY_ATTR, ATTR(0, 31, 2));
	cmd(BEGIN, 1);
	cmd(COLOR, GREEN);
	vtx(-0x1800, -0x0C00, -0x4800);
	vtx(-0x1800, 0x0C00, -0x4800);
	vtx(0x1800, 0x0C00, -0x1200);
	vtx(0x1800, -0x0C00, -0x1200);
	cmd(END, 0);
	cmd(POLY_ATTR, ATTR(0, 31, 3));
	cmd(BEGIN, 0);
	cmd(COLOR, BLUE);
	vtx(-0x0400, -0x1400, -0x2000);
	vtx(0x0000, 0x1400, -0x3800);
	vtx(0x0600, -0x1400, -0x1800);
	cmd(END, 0);
	cmd(SWAP, wbuf ? 2u : 0u);
}

static void s_modes(int highlight)
{
	ident_all();
	REG16(R_DISP3DCNT) = (uint16_t)(1u | (highlight ? 2u : 0u) | 8u);
	for (int i = 0; i < 32; i++)
		REG16(R_TOON + 2u * (uint32_t)i) = RGB5(i, 31 - i, (i * 3) & 31);
	/* decal con A5I3 (alfa por texel) sobre Gouraud */
	use_tex(texparam(T_A5I3, 2, 2, 6, 0, 0, 0), P_A5I3 >> 4);
	cmd(POLY_ATTR, ATTR(1, 31, 1));
	cmd(BEGIN, 1);
	cmd(COLOR, RED);
	tc(0, 0);
	sv(10, 10, 0);
	cmd(COLOR, GREEN);
	tc(0, 32);
	sv(10, 90, 0);
	cmd(COLOR, BLUE);
	tc(32, 32);
	sv(110, 90, 0);
	cmd(COLOR, WHITE);
	tc(32, 0);
	sv(110, 10, 0);
	cmd(END, 0);
	/* toon/highlight sin textura: rampa de rojo */
	use_tex(0, 0);
	cmd(POLY_ATTR, ATTR(2, 31, 2));
	cmd(BEGIN, 1);
	cmd(COLOR, RGB5(0, 10, 20));
	sv(130, 10, 0);
	cmd(COLOR, RGB5(0, 10, 20));
	sv(130, 90, 0);
	cmd(COLOR, RGB5(31, 10, 20));
	sv(250, 90, 0);
	cmd(COLOR, RGB5(31, 10, 20));
	sv(250, 10, 0);
	cmd(END, 0);
	/* toon/highlight con textura de 256 colores */
	use_tex(texparam(T_256, 2, 2, 4, 0, 0, 0), 0);
	cmd(POLY_ATTR, ATTR(2, 31, 3));
	cmd(BEGIN, 1);
	cmd(COLOR, RGB5(4, 4, 4));
	tc(0, 0);
	sv(10, 100, 0);
	tc(0, 32);
	sv(10, 180, 0);
	cmd(COLOR, RGB5(28, 4, 4));
	tc(32, 32);
	sv(120, 180, 0);
	tc(32, 0);
	sv(120, 100, 0);
	cmd(END, 0);
	/* modulate con A3I5 translúcido */
	use_tex(texparam(T_A3I5, 2, 2, 1, 0, 0, 0), P_A3I5 >> 4);
	cmd(POLY_ATTR, ATTR(0, 31, 4));
	tquad(130, 100, 250, 180, 0, 0, 0, 32, 32);
}

static void s_effects(void)
{
	ident_all();
	REG16(R_DISP3DCNT) = (uint16_t)(1u | 4u | 8u | 32u | 128u | (2u << 8));
	REG16(R_ALPHA_TEST) = 12;
	for (int i = 0; i < 8; i++)
		REG16(R_EDGE_COLOR + 2u * (uint32_t)i) = RGB5(31 - i * 4, i * 4, 31);
	REG32(R_FOG_COLOR) = RGB5(20, 20, 31) | (31u << 16);
	REG16(R_FOG_OFFSET) = 0x4000;
	for (int i = 0; i < 32; i++)
		REG8(R_FOG_TABLE + (uint32_t)i) = (uint8_t)(i * 4 > 127 ? 127 : i * 4);
	/* alpha test: A5I3 con alfa por fila */
	use_tex(texparam(T_A5I3, 2, 2, 6, 0, 0, 0), P_A5I3 >> 4);
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	tquad(10, 10, 74, 74, 0, 0, 0, 32, 32);
	use_tex(0, 0);
	/* edge marking: polígonos de ids distintos que se tocan y se tapan */
	cmd(POLY_ATTR, ATTR(0, 31, 8));
	quad(90, 10, 160, 70, 0x0000, RED);
	cmd(POLY_ATTR, ATTR(0, 31, 17));
	quad(130, 40, 200, 100, -0x0800, GREEN);
	cmd(POLY_ATTR, ATTR(0, 31, 17));
	quad(200, 40, 240, 100, -0x0800, GREEN);
	/* niebla: tiras a profundidades crecientes con y sin la bandera */
	for (int i = 0; i < 8; i++) {
		cmd(POLY_ATTR, ATTR(0, 31, 30 + i) | ((i & 1) ? 0u : A_FOG));
		quad(10 + i * 30, 120, 36 + i * 30, 180, -0x0F00 + i * 0x0400, YELLOW);
	}
}

static void s_shadow(void)
{
	ident_all();
	REG16(R_DISP3DCNT) = 8u;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	quad(20, 20, 236, 172, 0x0400, RGB5(20, 20, 20));
	cmd(POLY_ATTR, ATTR(0, 31, 2));
	quad(60, 60, 120, 120, -0x0400, BLUE);
	/* volumen de sombra: máscara (id 0) donde falla la profundidad y sombra (id 5) */
	cmd(POLY_ATTR, ATTR(3, 20, 0));
	quad(40, 40, 160, 140, 0x0600, 0);
	cmd(POLY_ATTR, ATTR(3, 20, 5));
	quad(40, 40, 160, 140, -0x0600, RGB5(0, 0, 0));
}

static void s_over2d(void)
{
	/* BG1 de texto detrás (franjas de colores) y el 3D translúcido encima: BLDCNT objetivo 2 = BG1 */
	REG8(R_VRAMCNT(3)) = 0x81; /* D: fondos del motor A (0x06000000) */
	for (int i = 0; i < 4; i++)
		PAL_BG(0)[i + 1] = (uint16_t)(i == 0 ? RED : i == 1 ? GREEN : i == 2 ? BLUE : YELLOW);
	for (int t = 0; t < 4; t++)
		for (int k = 0; k < 16; k++)
			V16(0x06000000u + 0x20u * (uint32_t)(t + 1))[k] = (uint16_t)(0x1111u * (uint32_t)(t + 1));
	for (int ty = 0; ty < 32; ty++)
		for (int tx = 0; tx < 32; tx++)
			V16(0x06008000u)[ty * 32 + tx] = (uint16_t)(1 + ((tx / 4) & 3));
	ER16(0, E_BGCNT(1)) = (uint16_t)(1u | (16u << 8)); /* prioridad 1, mapa en 0x8000, teselas en 0 */
	ER16(0, E_BGCNT(0)) = 0;                           /* 3D con prioridad 0 */
	ER32(0, E_DISPCNT) = DC_MODE(0) | DC_GRAPHICS | DC_BG(0) | DC_BG(1) | (1u << 3);
	ER16(0, E_BLDCNT) = (uint16_t)((1u << 9) | (1u << 0) | (1u << 6)); /* 1.º BG0, 2.º BG1, mezcla alfa */
	ER16(0, E_BLDALPHA) = (uint16_t)(8u | (8u << 8));
	ER16(0, E_BGHOFS(0)) = 8; /* el 3D se desplaza 8 píxeles */
	ident_all();
	REG16(R_DISP3DCNT) = 8u;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	quad(20, 20, 120, 90, 0, WHITE);
	cmd(POLY_ATTR, ATTR(0, 10, 2));
	quad(130, 20, 240, 90, 0, WHITE);
	cmd(POLY_ATTR, ATTR(0, 24, 3));
	quad(20, 110, 240, 170, 0, RGB5(31, 0, 31));
}

static void s_equal(void)
{
	ident_all();
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	quad(20, 20, 236, 172, 0x0200, BLUE);
	/* misma profundidad (y un poco distinta) con «igual» */
	cmd(POLY_ATTR, ATTR(0, 31, 2) | A_EQUAL);
	quad(40, 40, 120, 100, 0x0200, RED);
	cmd(POLY_ATTR, ATTR(0, 31, 3) | A_EQUAL);
	quad(130, 40, 210, 100, 0x0201, GREEN);
	cmd(POLY_ATTR, ATTR(0, 31, 4) | A_EQUAL);
	quad(40, 110, 120, 160, 0x0240, YELLOW);
	cmd(POLY_ATTR, ATTR(0, 31, 5) | A_EQUAL);
	quad(130, 110, 210, 160, 0x0300, WHITE);
}

/* 13/14: triángulos al azar, uno por celda de 32×32 (8×6 celdas), para ajustar las reglas de cobertura (13, color
 * plano) y de interpolación del color (14, Gouraud) contra melonDS. Generador: el mismo LCG que tools/r3d-fit.py. */
static uint32_t lcg_state;
static int rnd(int n)
{
	lcg_state = lcg_state * 1103515245u + 12345u;
	return (int)((lcg_state >> 16) % (uint32_t)n);
}

static void s_random(int gouraud)
{
	ident_all();
	lcg_state = 1234u + (uint32_t)gouraud;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int cy = 0; cy < 6; cy++)
		for (int cx = 0; cx < 8; cx++) {
			int x[3], y[3];

			for (int k = 0; k < 3; k++) {
				x[k] = cx * 32 + 1 + rnd(30);
				y[k] = cy * 32 + 1 + rnd(30);
			}
			cmd(BEGIN, 0);
			for (int k = 0; k < 3; k++) {
				cmd(COLOR, gouraud ? (uint16_t)rnd(0x8000) : (uint16_t)(RGB5(31, 31, 31) - (uint16_t)(cx + cy)));
				sv(x[k], y[k], 0);
			}
			cmd(END, 0);
		}
}

/* 16/17: interpolación del color de vértice aislada. 16: rectángulos cuyos lados verticales llevan un color cada uno
 * (la interpolación sólo corre a lo largo de la línea, de 2 a 240 píxeles); 17: rectángulos con un color arriba y otro
 * abajo (sólo corre por las aristas, de 2 a 180 líneas). Colores y largos con el LCG de rnd(); tools/r3d-fit.py los
 * repite para ajustar la regla. */
static const uint8_t span_len[19] = { 2, 3, 4, 5, 6, 7, 8, 10, 13, 16, 21, 31, 40, 57, 64, 100, 127, 200, 240 };

static void s_interp(int vertical)
{
	ident_all();
	lcg_state = 777u + (uint32_t)vertical;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int i = 0; i < 38; i++) {
		const int len = span_len[i % 19];
		uint16_t ca, cb;
		int x0, y0, x1, y1;

		ca = RGB5(rnd(32), rnd(32), rnd(32));
		cb = RGB5(rnd(32), rnd(32), rnd(32));
		if (vertical) {
			x0 = 2 + 6 * i;
			y0 = 2 + rnd(8);
			x1 = x0 + 4;
			y1 = y0 + (len > 180 ? 180 : len);
			cmd(BEGIN, 1);
			cmd(COLOR, ca);
			sv(x0, y0, 0);
			cmd(COLOR, cb);
			sv(x0, y1, 0);
			cmd(COLOR, cb);
			sv(x1, y1, 0);
			cmd(COLOR, ca);
			sv(x1, y0, 0);
		} else {
			x0 = 2 + rnd(12);
			y0 = 2 + 5 * i;
			x1 = x0 + len;
			y1 = y0 + 4;
			cmd(BEGIN, 1);
			cmd(COLOR, ca);
			sv(x0, y0, 0);
			cmd(COLOR, ca);
			sv(x0, y1, 0);
			cmd(COLOR, cb);
			sv(x1, y1, 0);
			cmd(COLOR, cb);
			sv(x1, y0, 0);
		}
		cmd(END, 0);
	}
}

/* 18: interpolación del color con w distinta en los lados (perspectiva): los lados verticales de cada rectángulo están a
 * w0 y w1 (de 1,0 a 7,0), cada uno con su color. w = 4096 + z con la proyección de abajo; x e y se eligen para que el
 * vértice caiga en el píxel pedido. */
static void pv(int px, int py, int32_t z)
{
	const int32_t w = 4096 + z;

	vtx((int32_t)(((int64_t)px * w + 127) / 128) - w, w - (int32_t)(((int64_t)py * w + 95) / 96), z);
}

static void s_interp_w(void)
{
	static const int32_t m[16] = { 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0x1000 };

	cmd(MTX_MODE, 0);
	for (int i = 0; i < 16; i++)
		GXP(MTX_LOAD_4x4) = (uint32_t)m[i];
	cmd(MTX_MODE, 2);
	cmd(MTX_IDENTITY, 0);
	lcg_state = 4242u;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int i = 0; i < 38; i++) {
		const int len = span_len[i % 19] < 8 ? 8 + span_len[i % 19] : span_len[i % 19];
		const int x0 = 2 + rnd(12), y0 = 2 + 5 * i, x1 = x0 + len, y1 = y0 + 4;
		const int32_t zl = rnd(2) ? rnd(24000) : rnd(2000), zr = rnd(2) ? rnd(24000) : rnd(2000);
		const uint16_t ca = RGB5(rnd(32), rnd(32), rnd(32)), cb = RGB5(rnd(32), rnd(32), rnd(32));

		cmd(BEGIN, 1);
		cmd(COLOR, ca);
		pv(x0, y0, zl);
		cmd(COLOR, ca);
		pv(x0, y1, zl);
		cmd(COLOR, cb);
		pv(x1, y1, zr);
		cmd(COLOR, cb);
		pv(x1, y0, zr);
		cmd(END, 0);
	}
}

/* 19: lo mismo, pero con la w distinta arriba y abajo (la interpolación por las aristas, en perspectiva). */
static void s_interp_wv(void)
{
	static const int32_t m[16] = { 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0x1000 };

	cmd(MTX_MODE, 0);
	for (int i = 0; i < 16; i++)
		GXP(MTX_LOAD_4x4) = (uint32_t)m[i];
	cmd(MTX_MODE, 2);
	cmd(MTX_IDENTITY, 0);
	lcg_state = 9191u;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int i = 0; i < 38; i++) {
		const int len = span_len[i % 19] < 8 ? 8 + span_len[i % 19] : (span_len[i % 19] > 180 ? 180 : span_len[i % 19]);
		const int x0 = 2 + 6 * i, y0 = 2 + rnd(8), x1 = x0 + 4, y1 = y0 + len;
		const int32_t zt = rnd(2) ? rnd(24000) : rnd(2000), zb = rnd(2) ? rnd(24000) : rnd(2000);
		const uint16_t ca = RGB5(rnd(32), rnd(32), rnd(32)), cb = RGB5(rnd(32), rnd(32), rnd(32));

		cmd(BEGIN, 1);
		cmd(COLOR, ca);
		pv(x0, y0, zt);
		cmd(COLOR, cb);
		pv(x0, y1, zb);
		cmd(COLOR, cb);
		pv(x1, y1, zb);
		cmd(COLOR, ca);
		pv(x1, y0, zt);
		cmd(END, 0);
	}
}

/* 20/21: color por una arista inclinada. Triángulos con la arista izquierda (de arriba a abajo) de un solo tramo: el
 * primer píxel de cada línea lleva el color de esa arista. 20: aristas casi verticales (y-mayores), 21: casi
 * horizontales (x-mayores, hacia la izquierda y hacia la derecha). */
static void s_slope(int shallow)
{
	ident_all();
	lcg_state = 555u + (uint32_t)shallow;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int i = 0; i < (shallow ? 18 : 18); i++) {
		int tx, ty, bx, by, rx, ry, c0, r0;
		uint16_t ct, cb, cr;

		if (!shallow) {
			c0 = (i % 6) * 42;
			r0 = (i / 6) * 62;
			tx = c0 + 2 + rnd(19);
			ty = r0 + 1 + rnd(6);
			bx = c0 + 1 + rnd(22);
			by = r0 + 54 + rnd(6);
			rx = c0 + 30 + rnd(10);
			ry = r0 + 20 + rnd(30);
		} else {
			const int right = i & 1;

			c0 = (i % 3) * 84;
			r0 = (i / 3) * 31;
			tx = c0 + (right ? 4 + rnd(10) : 30 + rnd(30));
			ty = r0 + 1 + rnd(3);
			bx = c0 + (right ? 40 + rnd(25) : 1 + rnd(20));
			by = r0 + 14 + rnd(10);
			rx = c0 + 75 + rnd(8);
			ry = r0 + 4 + rnd(5);
		}
		ct = RGB5(rnd(32), rnd(32), rnd(32));
		cb = RGB5(rnd(32), rnd(32), rnd(32));
		cr = RGB5(rnd(32), rnd(32), rnd(32));
		cmd(BEGIN, 0);
		cmd(COLOR, ct);
		sv(tx, ty, 0);
		cmd(COLOR, cb);
		sv(bx, by, 0);
		cmd(COLOR, cr);
		sv(rx, ry, 0);
		cmd(END, 0);
	}
}

/* 22: la interpolación a lo largo de la línea con los bordes inclinados: paralelogramos con los lados de un color cada
 * uno (sin interpolación por las aristas) y pendientes distintas. */
static void s_para(void)
{
	ident_all();
	lcg_state = 31337u;
	cmd(POLY_ATTR, ATTR(0, 31, 1));
	for (int i = 0; i < 36; i++) {
		const int c0 = (i % 4) * 64, r0 = (i / 4) * 21, H = 19, sl = rnd(2 * H - 3) - (H - 2), L = 6 + rnd(40);
		const int x0 = c0 + 1 + (sl < 0 ? -sl : 0), y0 = r0 + 1, y1 = y0 + H;
		const uint16_t ca = RGB5(rnd(32), rnd(32), rnd(32)), cb = RGB5(rnd(32), rnd(32), rnd(32));

		cmd(BEGIN, 1);
		cmd(COLOR, ca);
		sv(x0, y0, 0);
		cmd(COLOR, ca);
		sv(x0 + sl, y1, 0);
		cmd(COLOR, cb);
		sv(x0 + sl + L, y1, 0);
		cmd(COLOR, cb);
		sv(x0 + L, y0, 0);
		cmd(END, 0);
	}
}

int main(void)
{
	scene = *(volatile uint32_t *)0x027FFE0Cu & 0xFFFFu;
	if (scene > 22)
		scene = 0;
	sds_init_quiet();
	REG16(R_POWCNT1) = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 9) | (1 << 15);
	gfx_banks();
	gfx_clear_all();
	make_textures();
	PAL_BG(0)[0] = BACK;
	ER32(0, E_DISPCNT) = DC_MODE(0) | DC_GRAPHICS | DC_BG(0) | (1u << 3);
	REG16(R_DISP3DCNT) = 1u | 8u; /* texturas y mezcla */
	REG32(R_CLEAR_COLOR) = 0;     /* transparente: se ve el color de fondo del 2D */
	REG16(R_CLEAR_DEPTH) = 0x7FFF;
	GXP(VIEWPORT) = 0u | (0u << 8) | (255u << 16) | (191u << 24);
	switch (scene) {
	case 0: s_coverage(); break;
	case 1: s_gouraud(); break;
	case 2: s_persp(); break;
	case 3: s_formats(); break;
	case 4: s_translucent(0); break;
	case 5: s_depth(0); break;
	case 6: s_depth(1); break;
	case 7: s_modes(0); break;
	case 8: s_modes(1); break;
	case 9: s_effects(); break;
	case 10: s_shadow(); break;
	case 11: s_over2d(); break;
	case 12: s_equal(); break;
	case 13: s_random(0); break;
	case 14: s_random(1); break;
	case 15: s_texgen(); break;
	case 16: s_interp(0); break;
	case 17: s_interp(1); break;
	case 18: s_interp_w(); break;
	case 19: s_interp_wv(); break;
	case 20: s_slope(0); break;
	case 21: s_slope(1); break;
	case 22: s_para(); break;
	}
	if (scene != 4 && scene != 5 && scene != 6)
		cmd(SWAP, 0);
	gfx_frames(3);
	gfx_capture();
	gfx_frames(3);
	/* si hubo captura (bit 31 en 0), los píxeles de prueba contra lo capturado */
	if (!(ER32(0, E_DISPCAP) & 0x80000000u))
		for (int i = 0; i < nprobe; i++)
			SDS_DIVERGE_ID(V16(0x06800000u)[probe[i].y * 256 + probe[i].x] & 0x7FFFu, probe[i].c, 1000u + (uint32_t)i);
	sds_finish_quiet(1);
}
