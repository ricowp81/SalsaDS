/*
 * SalsaDS — ROM demo: una demostración propia para ver el núcleo funcionando (la corre «SalsaDS núcleo»).
 *
 *  - Pantalla de arriba (motor A en visualización directa de VRAM): fondo del marco de pruebas, un cuadrado que
 *    rebota (la cruceta lo mueve; A lo para y lo suelta; L y R cambian su color), un rastro donde toca la
 *    pantalla táctil y dos líneas de texto con las teclas y el toque.
 *  - Pantalla de abajo (motor B, solo el color de fondo de la paleta): X e Y lo cambian; el toque lo tiñe según
 *    su posición.
 *  - Select intercambia las dos pantallas (POWCNT1.15). Start borra el rastro.
 *
 * Usa las interrupciones de libnds (VBlank), el ARM7 (teclas X/Y, lápiz y SPI del táctil) y la RAM compartida.
 * Es de SalsaDS: sin material de terceros.
 */
#include <nds.h>

#include "../common/hw.h"
#include "shared.h"

#define FB ((volatile uint16_t *)0x06800000)
#define C_BG ((uint16_t)(0x8000 | (8 << 10) | (2 << 5) | 2))
#define RGB(r, g, b) ((uint16_t)(0x8000 | ((b) << 10) | ((g) << 5) | (r)))

static volatile uint32_t n_vblank;
static void h_vblank(void) { n_vblank++; }

static void fill(int x, int y, int w, int h, uint16_t c)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			FB[j * 256 + i] = c;
}

static uint16_t square_color(uint32_t k)
{
	static const uint16_t pal[6] = { RGB(31, 6, 6), RGB(31, 24, 4), RGB(8, 28, 8), RGB(6, 24, 31), RGB(20, 8, 31), RGB(31, 31, 31) };

	return pal[k % 6];
}

static void hex2(char *o, uint32_t v)
{
	o[0] = "0123456789ABCDEF"[(v >> 4) & 15];
	o[1] = "0123456789ABCDEF"[v & 15];
}

static void dec3(char *o, uint32_t v)
{
	o[0] = (char)('0' + v / 100 % 10);
	o[1] = (char)('0' + v / 10 % 10);
	o[2] = (char)('0' + v % 10);
}

int main(void)
{
	struct demo_shared *sh = DEMO_SHARED;
	int sx = 40, sy = 150, vx = 2, vy = 1, auto_move = 1;
	uint32_t color = 0, prev_keys = 0xFFFFFFFFu, prev_touch = 0xFFFFFFFFu, bcolor = 0, frame = 0, swap = 1;
	char line[33];

	sds_nocache();
	MEM32(R_IME) = 0;
	sds_init("SALSADS DEMO");
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	MEM32(R_IME) = 1;
	REG16(R_DISPCNT_B + 0) = 0;
	REG32(R_DISPCNT_B) = 1u << 16; /* motor B: gráficos, sin capas: el fondo de la paleta */
	sds_print_row(2, "TECLAS:");
	sds_print_row(4, "TOQUE:");
	sds_print_row(6, "CRUZ MUEVE  A PARA  L R COLOR");
	sds_print_row(7, "X Y FONDO  SEL INTERCAMBIA");
	sds_print_row(8, "START BORRA EL RASTRO");
	fill(0, 132, 256, 2, RGB(31, 31, 31));
	for (;;) {
		uint32_t keys, ext, touch;
		int old_sx = sx, old_sy = sy;

		swiWaitForVBlank();
		frame++;
		keys = ~MEM16(R_KEYINPUT) & 0x3FF;
		ext = ~sh->ext & 0x43; /* X, Y y lápiz, apretado = 1 */
		touch = sh->pen ? (uint32_t)(sh->px | (sh->py << 8) | 0x10000u) : 0;
		if (keys & (1u << 4))
			sx += 3, auto_move = 0;
		if (keys & (1u << 5))
			sx -= 3, auto_move = 0;
		if (keys & (1u << 6))
			sy -= 3, auto_move = 0;
		if (keys & (1u << 7))
			sy += 3, auto_move = 0;
		if ((keys & ~prev_keys) & 1u) /* A: para y suelta */
			auto_move = !auto_move;
		if ((keys & ~prev_keys) & (1u << 8))
			color++;
		if ((keys & ~prev_keys) & (1u << 9))
			color += 5;
		if ((keys & ~prev_keys) & (1u << 2)) { /* Select */
			swap = !swap;
			REG16(R_POWCNT1) = (uint16_t)((REG16(R_POWCNT1) & 0x7FFF) | (swap << 15));
		}
		if ((keys & ~prev_keys) & (1u << 3)) /* Start: borra el rastro */
			fill(0, 136, 256, 56, C_BG);
		if (auto_move) {
			sx += vx;
			sy += vy;
		}
		if (sx < 0 || sx > 240) {
			vx = -vx;
			sx = sx < 0 ? 0 : 240;
		}
		if (sy < 136 || sy > 174) {
			vy = -vy;
			sy = sy < 136 ? 136 : 174;
		}
		fill(old_sx, old_sy, 16, 16, C_BG);
		fill(sx, sy, 16, 16, square_color(color));
		/* rastro del lápiz */
		if (sh->pen)
			fill(sh->px - 1 < 0 ? 0 : sh->px - 1, 136 + (sh->py * 54 / 192), 3, 3, RGB(31, 31, 4));
		/* texto */
		if (keys != prev_keys || ext != (prev_keys >> 16)) {
			static const char names[12] = { 'A', 'B', 'S', 'T', 'R', 'L', 'U', 'D', 'r', 'l', 'X', 'Y' };

			for (int i = 0; i < 10; i++)
				line[i] = (keys >> i) & 1 ? names[i] : '.';
			line[10] = (ext & 1) ? 'X' : '.';
			line[11] = (ext & 2) ? 'Y' : '.';
			line[12] = 0;
			sds_print_row(3, line);
		}
		if (touch != prev_touch) {
			if (touch) {
				line[0] = 'X'; line[1] = '='; dec3(line + 2, sh->px);
				line[5] = ' '; line[6] = 'Y'; line[7] = '='; dec3(line + 8, sh->py);
				line[11] = ' '; line[12] = 'A'; hex2(line + 13, sh->adc_x >> 4); hex2(line + 15, sh->adc_y >> 4);
				line[17] = 0;
			} else {
				line[0] = 'N'; line[1] = 'O'; line[2] = 0;
			}
			sds_print_row(5, line);
		}
		prev_keys = keys;
		prev_touch = touch;
		/* la pantalla de abajo: fondo por X/Y y por el toque */
		if (ext & 1)
			bcolor += 1;
		if (ext & 2)
			bcolor += 3;
		if (sh->pen)
			MEM16(0x05000400) = RGB((sh->px >> 3) & 31, (sh->py >> 3) & 31, 16);
		else
			MEM16(0x05000400) = RGB(2 + (bcolor & 15), 2, 12 + ((bcolor >> 2) & 15));
		/* contador: cada 8 cuadros */
		if (!(frame & 7)) {
			line[0] = 'C'; line[1] = 'U'; line[2] = 'A'; line[3] = 'D'; line[4] = 'R'; line[5] = 'O'; line[6] = ' ';
			hex2(line + 7, frame >> 8);
			hex2(line + 9, frame);
			line[11] = 0;
			sds_print_row(10, line);
		}
	}
}
