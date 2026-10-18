/*
 * SalsaDS — marco de pruebas para las ROMs de la DS. Ver sds.h.
 *
 * Pantalla: 256×192 en el modo de visualización directa de VRAM (DISPCNT
 * bits 16–17 = 2, banco A mapeado como LCDC en 0x06800000), RGB555. Texto de
 * 8×12 píxeles por celda (32 columnas × 16 filas) con una fuente de 3×5 al
 * doble de tamaño, dibujada aquí (no se usa libnds ni ninguna fuente ajena).
 */
#include "sds.h"

#define REG_DISPCNT_A (*(volatile uint32_t *)0x04000000)
#define REG_DISPCNT_B (*(volatile uint32_t *)0x04001000)
#define REG_VRAMCNT_A (*(volatile uint8_t *)0x04000240)
#define REG_POWCNT1 (*(volatile uint16_t *)0x04000304)
#define FB ((volatile uint16_t *)0x06800000)

#define COLS 32
#define ROWS 16
#define ROW_FIRST_GROUP 2
#define ROW_LAST_GROUP 11
#define ROW_FAIL 12
#define ROW_BANNER 14

#define RGB(r, g, b) ((uint16_t)(0x8000 | ((b) << 10) | ((g) << 5) | (r)))
#define C_BG RGB(2, 2, 8)
#define C_FG RGB(31, 31, 31)
#define C_DIM RGB(18, 18, 24)
#define C_OK RGB(4, 26, 8)
#define C_BAD RGB(28, 4, 4)

/* Glifos de 3×5: cinco filas de tres bits (bit 2 = columna izquierda). */
static const uint8_t font_digits[10][5] = {
	{ 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
	{ 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};
static const uint8_t font_letters[26][5] = {
	{ 2, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 }, { 3, 4, 4, 4, 3 }, { 6, 5, 5, 5, 6 }, { 7, 4, 6, 4, 7 },
	{ 7, 4, 6, 4, 4 }, { 3, 4, 5, 5, 3 }, { 5, 5, 7, 5, 5 }, { 7, 2, 2, 2, 7 }, { 1, 1, 1, 5, 2 },
	{ 5, 5, 6, 5, 5 }, { 4, 4, 4, 4, 7 }, { 5, 7, 7, 5, 5 }, { 6, 5, 5, 5, 5 }, { 2, 5, 5, 5, 2 },
	{ 6, 5, 6, 4, 4 }, { 2, 5, 5, 6, 3 }, { 6, 5, 6, 5, 5 }, { 3, 4, 2, 1, 6 }, { 7, 2, 2, 2, 2 },
	{ 5, 5, 5, 5, 7 }, { 5, 5, 5, 5, 2 }, { 5, 5, 7, 7, 5 }, { 5, 5, 2, 5, 5 }, { 5, 5, 2, 2, 2 },
	{ 7, 1, 2, 4, 7 },
};
static const uint8_t font_blank[5] = { 0, 0, 0, 0, 0 };

static const uint8_t *glyph(char c)
{
	static const uint8_t dot[5] = { 0, 0, 0, 0, 2 }, colon[5] = { 0, 2, 0, 2, 0 };
	static const uint8_t slash[5] = { 1, 1, 2, 4, 4 }, dash[5] = { 0, 0, 7, 0, 0 };
	static const uint8_t plus[5] = { 0, 2, 7, 2, 0 }, equal[5] = { 0, 7, 0, 7, 0 };
	static const uint8_t bang[5] = { 2, 2, 2, 0, 2 }, quest[5] = { 6, 1, 2, 0, 2 };

	if (c >= '0' && c <= '9')
		return font_digits[c - '0'];
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	if (c >= 'A' && c <= 'Z')
		return font_letters[c - 'A'];
	switch (c) {
	case '.': return dot;
	case ':': return colon;
	case '/': return slash;
	case '-': return dash;
	case '+': return plus;
	case '=': return equal;
	case '!': return bang;
	case '?': return quest;
	default: return font_blank;
	}
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			FB[j * 256 + i] = color;
}

static void draw_char(int col, int row, char c, uint16_t fg, uint16_t bg)
{
	const uint8_t *g = glyph(c);
	int x0 = col * 8, y0 = row * 12;

	fill_rect(x0, y0, 8, 12, bg);
	for (int r = 0; r < 5; r++)
		for (int b = 0; b < 3; b++)
			if (g[r] & (4 >> b))
				fill_rect(x0 + 1 + b * 2, y0 + 1 + r * 2, 2, 2, fg);
}

static void draw_text(int row, int col, const char *s, uint16_t fg, uint16_t bg)
{
	for (; *s && col < COLS; s++, col++)
		draw_char(col, row, *s, fg, bg);
}

static void clear_row(int row, uint16_t bg)
{
	fill_rect(0, row * 12, 256, 12, bg);
}

static void hex32(char *out, uint32_t v)
{
	for (int i = 0; i < 8; i++)
		out[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
	out[8] = 0;
}

static void dec(char *out, uint32_t v)
{
	char tmp[11];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*out++ = tmp[--n];
	*out = 0;
}

static void append(char *dst, const char *src)
{
	while (*dst)
		dst++;
	while ((*dst++ = *src++))
		;
}

/* Los resultados viven en RAM principal con la caché de datos activada:
 * limpiar la línea por MVA y vaciar el búfer de escritura para que un
 * observador externo (emulador o placa) vea los valores. */
static void flush_range(const void *p, uint32_t len)
{
	uint32_t a = (uint32_t)p & ~31u, end = (uint32_t)p + len;

	for (; a < end; a += 32)
		__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");
	__asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

static struct sds_results *const R = (struct sds_results *)SDS_RESULTS_ADDR;
static const char *group_name;
static uint32_t group_checks, group_failed, group_row = ROW_FIRST_GROUP;

static struct sds_fail *const FAILLOG = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_FAILLOG_OFFSET);

static void publish(void)
{
	flush_range(R, SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail));
}

void sds_init(const char *title)
{
	volatile uint32_t *w = (volatile uint32_t *)R;

	for (unsigned i = 0; i < (SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail)) / 4; i++)
		w[i] = 0;
	R->magic = SDS_MAGIC;
	R->version = 1;
	publish();

	REG_POWCNT1 = (1 << 0) | (1 << 1) | (1 << 9) | (1 << 15); /* LCD, motor A, motor B, A arriba */
	REG_VRAMCNT_A = 0x80;                                     /* banco A: LCDC */
	REG_DISPCNT_B = 0;                                        /* la de abajo, apagada */
	REG_DISPCNT_A = 2u << 16;                                 /* visualización directa de VRAM, banco A */
	fill_rect(0, 0, 256, 192, C_BG);
	draw_text(0, 0, title, C_FG, C_BG);
	draw_text(1, 0, "SALSADS TEST", C_DIM, C_BG);
}

void sds_init_quiet(void)
{
	volatile uint32_t *w = (volatile uint32_t *)R;

	for (unsigned i = 0; i < (SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail)) / 4; i++)
		w[i] = 0;
	R->magic = SDS_MAGIC;
	R->version = 1;
	publish();
	{
		volatile uint32_t *x = (volatile uint32_t *)SDS_EXPECT_ADDR;

		for (unsigned i = 0; i < 2 + 2 * SDS_EXPECT_MAX; i++)
			x[i] = 0;
		x[0] = SDS_EXPECT_MAGIC;
		flush_range((const void *)SDS_EXPECT_ADDR, 8u + 8u * SDS_EXPECT_MAX);
	}
}

void sds_expect_pixel(int screen, int x, int y, uint16_t color)
{
	volatile uint32_t *x32 = (volatile uint32_t *)SDS_EXPECT_ADDR;
	const uint32_t n = x32[1];

	if (n >= SDS_EXPECT_MAX)
		return;
	x32[2 + 2 * n] = (uint32_t)(screen & 1) | ((uint32_t)(x & 0xFFFF) << 16);
	x32[3 + 2 * n] = (uint32_t)(y & 0xFFFF) | ((uint32_t)color << 16);
	x32[1] = n + 1;
	flush_range((const void *)SDS_EXPECT_ADDR, 8u + 8u * SDS_EXPECT_MAX);
}

void sds_finish_quiet(int ok)
{
	R->verdict = ok ? SDS_VERDICT_OK : SDS_VERDICT_FAIL;
	R->failed = ok ? 0 : 1;
	R->state = 1;
	publish();
	for (;;)
		;
}

static void close_group(void)
{
	char line[40], num[12], padded[14];
	int n = 0;

	if (!group_name)
		return;
	R->groups++;
	/* nombre en 12 columnas, luego estado y cuenta */
	for (const char *p = group_name; *p && n < 12; p++)
		padded[n++] = *p;
	while (n < 12)
		padded[n++] = ' ';
	padded[n] = 0;
	line[0] = 0;
	append(line, padded);
	append(line, group_failed ? "FALLO " : "OK    ");
	dec(num, group_failed ? group_failed : group_checks);
	append(line, num);
	if (group_failed) {
		append(line, "/");
		dec(num, group_checks);
		append(line, num);
	}
	if (group_row <= ROW_LAST_GROUP) {
		clear_row((int)group_row, C_BG);
		draw_text((int)group_row, 0, line, group_failed ? C_BAD : C_OK, C_BG);
		group_row++;
	}
	group_name = 0;
	publish();
}

void sds_group(const char *name)
{
	close_group();
	group_name = name;
	group_checks = group_failed = 0;
}

void sds_check(uint32_t got, uint32_t want, uint32_t id)
{
	R->total++;
	group_checks++;
	if (got != want) {
		if (R->failed < SDS_FAILLOG_MAX) {
			FAILLOG[R->failed].group = R->groups + 1;
			FAILLOG[R->failed].id = id;
			FAILLOG[R->failed].got = got;
			FAILLOG[R->failed].want = want;
		}
		R->failed++;
		group_failed++;
		if (!R->first_fail_group) {
			char line[40] = "L", num[12];

			R->first_fail_group = R->groups + 1;
			R->first_fail_id = id;
			R->first_fail_got = got;
			R->first_fail_want = want;
			dec(num, id);
			append(line, num);
			clear_row(ROW_FAIL, C_BG);
			draw_text(ROW_FAIL, 0, line, C_BAD, C_BG);
			hex32(num, got);
			draw_text(ROW_FAIL + 1, 0, "G", C_DIM, C_BG);
			draw_text(ROW_FAIL + 1, 1, num, C_BAD, C_BG);
			hex32(num, want);
			draw_text(ROW_FAIL + 1, 11, "W", C_DIM, C_BG);
			draw_text(ROW_FAIL + 1, 12, num, C_BAD, C_BG);
		}
	}
}

void sds_diverge(uint32_t got, uint32_t want, uint32_t id)
{
	struct sds_fail *log = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_DIVERGE_OFFSET);

	if (got == want)
		return;
	if (R->diverge < SDS_DIVERGE_MAX) {
		log[R->diverge].group = R->groups + 1;
		log[R->diverge].id = id;
		log[R->diverge].got = got;
		log[R->diverge].want = want;
	}
	R->diverge++;
}

void sds_mark(uint32_t n)
{
	R->mark = n;
}

void sds_print_row(int row, const char *text)
{
	clear_row(row, C_BG);
	draw_text(row, 0, text, C_FG, C_BG);
}

void sds_finish(void)
{
	char line[40] = "", num[12];

	close_group();
	R->verdict = R->failed ? SDS_VERDICT_FAIL : SDS_VERDICT_OK;
	R->state = 1;
	publish();

	fill_rect(0, ROW_BANNER * 12, 256, 24, R->failed ? C_BAD : C_OK);
	if (R->failed) {
		append(line, "FALLO ");
		dec(num, R->failed);
		append(line, num);
		append(line, "/");
		dec(num, R->total);
		append(line, num);
	} else {
		append(line, "TODO OK ");
		dec(num, R->total);
		append(line, num);
	}
	draw_text(ROW_BANNER, 1, line, C_FG, R->failed ? C_BAD : C_OK);
	for (;;)
		;
}
