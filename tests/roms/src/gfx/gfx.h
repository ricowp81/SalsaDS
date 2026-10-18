/*
 * SalsaDS — ayudas para las ROMs de gráficos 2D (Fase 2.1). Solo registros de hardware (GBATEK), sin libnds.
 *
 * Las ROMs de gráficos montan una escena en el motor A (y, la mayoría, la misma en el B), esperan unos cuadros y
 * dejan en RAM la lista de píxeles que deben tener ciertos valores (sds_expect_pixel). Al final disparan una captura
 * del motor A al banco A de VRAM (DISPCAPCNT) para que melonDS, que sí la implementa, entregue su imagen por el
 * stub de GDB (tools/melonds-run.py --vram) y se compare con la de SalsaDS (tools/compare-image.py). SalsaDS no
 * implementa la captura: la ROM no espera a que termine, solo deja pasar unos cuadros.
 */
#ifndef GFX_H
#define GFX_H

#include "../common/hw.h"

#define RGB5(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

/* Registros de un motor: e = 0 (A) o 1 (B) */
#define EB(e) ((e) ? 0x04001000u : 0x04000000u)
#define ER16(e, off) REG16(EB(e) + (off))
#define ER32(e, off) REG32(EB(e) + (off))
#define E_DISPCNT 0x00
#define E_BGCNT(b) (0x08 + 2 * (b))
#define E_BGHOFS(b) (0x10 + 4 * (b))
#define E_BGVOFS(b) (0x12 + 4 * (b))
#define E_BG2PA 0x20
#define E_BG2PB 0x22
#define E_BG2PC 0x24
#define E_BG2PD 0x26
#define E_BG2X 0x28
#define E_BG2Y 0x2C
#define E_BG3PA 0x30
#define E_BG3PB 0x32
#define E_BG3PC 0x34
#define E_BG3PD 0x36
#define E_BG3X 0x38
#define E_BG3Y 0x3C
#define E_WIN0H 0x40
#define E_WIN1H 0x42
#define E_WIN0V 0x44
#define E_WIN1V 0x46
#define E_WININ 0x48
#define E_WINOUT 0x4A
#define E_MOSAIC 0x4C
#define E_BLDCNT 0x50
#define E_BLDALPHA 0x52
#define E_BLDY 0x54
#define E_DISPCAP 0x64
#define E_MASTER_BRIGHT 0x6C

/* Memoria de un motor (con los bancos de gfx_banks) */
#define PAL_BG(e) ((volatile uint16_t *)((e) ? 0x05000400u : 0x05000000u))
#define PAL_OBJ(e) ((volatile uint16_t *)((e) ? 0x05000600u : 0x05000200u))
#define OAM(e) ((volatile uint16_t *)((e) ? 0x07000400u : 0x07000000u))
#define BGV(e) ((e) ? 0x06200000u : 0x06000000u)
#define OBJV(e) ((e) ? 0x06600000u : 0x06400000u)
#define V8(a) ((volatile uint8_t *)(uintptr_t)(a))
#define V16(a) ((volatile uint16_t *)(uintptr_t)(a))
#define V32(a) ((volatile uint32_t *)(uintptr_t)(a))

#define DC_MODE(m) ((uint32_t)(m))
#define DC_GRAPHICS (1u << 16)
#define DC_BG(b) (1u << (8 + (b)))
#define DC_OBJ (1u << 12)
#define DC_WIN0 (1u << 13)
#define DC_WIN1 (1u << 14)
#define DC_OBJWIN (1u << 15)
#define DC_OBJ1D (1u << 4)
#define DC_BGEXTPAL (1u << 30)
#define DC_OBJEXTPAL (1u << 31)

/* Espera a que empiece el VBlank (línea 192). */
static inline void gfx_vsync(void)
{
	while (REG16(R_VCOUNT) == 192)
		;
	while (REG16(R_VCOUNT) != 192)
		;
}

static inline void gfx_frames(int n)
{
	while (n-- > 0)
		gfx_vsync();
}

/* Motores y bancos: A queda como LCDC (para la captura); B: fondos del motor A (128 KB en 0x06000000); C: fondos del
 * motor B (0x06200000); D: OBJ del motor B (0x06600000); E: OBJ del motor A (64 KB en 0x06400000). Con `ext`:
 * F/G, H e I para las paletas extendidas en lugar de E. */
static inline void gfx_power(void)
{
	REG16(R_POWCNT1) = (1 << 0) | (1 << 1) | (1 << 9) | (1 << 15); /* LCD, motores A y B, A arriba */
}

static inline void gfx_banks(void)
{
	REG8(R_VRAMCNT(0)) = 0x80; /* A: LCDC */
	REG8(R_VRAMCNT(1)) = 0x81; /* B: fondos de A */
	REG8(R_VRAMCNT(2)) = 0x84; /* C: fondos de B */
	REG8(R_VRAMCNT(3)) = 0x84; /* D: OBJ de B */
	REG8(R_VRAMCNT(4)) = 0x82; /* E: OBJ de A */
}

static inline void gfx_clear32(uint32_t addr, uint32_t bytes)
{
	volatile uint32_t *p = V32(addr);

	for (uint32_t i = 0; i < bytes / 4; i++)
		p[i] = 0;
}

/* Limpia la memoria que usan las escenas (los bancos arrancan con basura en melonDS, en cero aquí). */
static inline void gfx_clear_all(void)
{
	gfx_clear32(0x06000000u, 0x20000u);
	gfx_clear32(0x06200000u, 0x20000u);
	gfx_clear32(0x06400000u, 0x10000u);
	gfx_clear32(0x06600000u, 0x20000u);
	for (int e = 0; e < 2; e++) {
		for (int i = 0; i < 256; i++) {
			PAL_BG(e)[i] = 0;
			PAL_OBJ(e)[i] = 0;
		}
		for (int i = 0; i < 128; i++) {
			OAM(e)[i * 4] = 0x200; /* desactivado */
			OAM(e)[i * 4 + 1] = 0;
			OAM(e)[i * 4 + 2] = 0;
			OAM(e)[i * 4 + 3] = 0;
		}
	}
}

/* La VRAM del ARM9 no admite escrituras de 8 bits (se ignoran): los datos se arman en RAM y se copian de 16 en 16. */
static inline void gfx_copy16(uint32_t dst, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 2)
		V16(dst)[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
}

/* Tesela de 4 bits sólida (color c) en la dirección a; de 8 bits con gfx_tile8 */
static inline void gfx_tile4(uint32_t a, int c)
{
	const uint32_t w = (uint32_t)c * 0x11111111u;

	for (int i = 0; i < 8; i++)
		V32(a)[i] = w;
}

static inline void gfx_tile8(uint32_t a, int c)
{
	const uint32_t w = (uint32_t)c * 0x01010101u;

	for (int i = 0; i < 16; i++)
		V32(a)[i] = w;
}

/* Tesela de 4 bits con una marca: la mitad de arriba y de la izquierda en color c1, el resto c2; el píxel (7, 0) en c3 */
static inline void gfx_tile4_mark(uint32_t a, int c1, int c2, int c3)
{
	uint8_t t[32];

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x += 2) {
			int lo = (x < 4 && y < 4) ? c1 : c2;
			int hi = ((x + 1) < 4 && y < 4) ? c1 : c2;

			if (y == 0 && x + 1 == 7)
				hi = c3;
			t[y * 4 + x / 2] = (uint8_t)(lo | (hi << 4));
		}
	gfx_copy16(a, t, 32);
}

static inline void gfx_tile8_mark(uint32_t a, int c1, int c2, int c3)
{
	uint8_t t[64];

	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			int c = (x < 4 && y < 4) ? c1 : c2;

			if (y == 0 && x == 7)
				c = c3;
			t[y * 8 + x] = (uint8_t)c;
		}
	gfx_copy16(a, t, 64);
}

/* Captura de la imagen del motor A al banco A (LCDC, 0x06800000), 256×192, de la pantalla gráfica. */
static inline void gfx_capture(void)
{
	ER32(0, E_DISPCAP) = 0x80000000u | (3u << 20);
}

/* Deja pasar unos cuadros, captura, deja pasar otros y termina con OK. */
static inline void gfx_finish(void) __attribute__((noreturn));
static inline void gfx_finish(void)
{
	gfx_frames(2);
	gfx_capture();
	gfx_frames(4);
	sds_finish_quiet(1);
}

#endif
