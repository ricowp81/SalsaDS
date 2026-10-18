/*
 * SalsaDS — marco de pruebas para las ROMs de la DS. Ver sds.h.
 *
 * Pantalla: 256×192 en el modo de visualización directa de VRAM (DISPCNT
 * bits 16–17 = 2, banco A mapeado como LCDC en 0x06800000), RGB555. Texto de
 * 8×12 píxeles por celda (32 columnas × 16 filas) con una fuente de 3×5 al
 * doble de tamaño, dibujada aquí (no se usa libnds ni ninguna fuente ajena).
 */
#include "sds.h"
#include <stdio.h>
#include <string.h>
#include <fat.h>
#include <nds/exceptions.h>
#include <nds/arm9/dldi.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef SDS_ROM_NAME
#define SDS_ROM_NAME "rom"
#endif
#ifndef SDS_GITREV
#define SDS_GITREV "desconocida"
#endif

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
#define C_WARN RGB(28, 24, 2)

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

/* Registro ampliado (solo para el log y la pantalla de divergencias; no forma parte del bloque de resultados). */
#define MAXG 32
#define MAXS 128
static struct {
	const char *name;
	uint32_t checks, failed;
} glog[MAXG];
static uint32_t nglog;
static struct {
	uint32_t id, got, doc, melon, group;
	const char *file;
} slog[MAXS];
static uint32_t nslog, nslog_total;
static int sd_state; /* 0 = no se intentó, 1 = montada, -1 = sin SD / falló */
static char sd_path[64];
static uint32_t sd_run;

static struct sds_fail *const FAILLOG = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_FAILLOG_OFFSET);

static void publish(void)
{
	flush_range(R, SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail));
}

/* Durante las pruebas NO se toca la SD: el log y el progreso se acumulan en este búfer de RAM y se escriben al final,
 * con el estado de libnds restaurado (restore_libnds). En hardware, escribir a la SD con el PU, las cachés y los
 * registros movidos por las pruebas hizo caer la ROM `bios` en la pantalla de excepción de libnds. */
#define LOGBUF_SIZE 32768
static char logbuf[LOGBUF_SIZE];
static uint32_t loglen;

static void lprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lprintf(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (loglen >= LOGBUF_SIZE - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(logbuf + loglen, LOGBUF_SIZE - loglen, fmt, ap);
	va_end(ap);
	if (n > 0)
		loglen += (uint32_t)n < LOGBUF_SIZE - loglen ? (uint32_t)n : LOGBUF_SIZE - loglen - 1;
}

void sds_notef(const char *fmt, ...)
{
	va_list ap;
	char tmp[96];

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	lprintf("[dato] %s\n", tmp);
}

/* Progreso: una línea en el búfer del log y en la fila 14 de la pantalla (lo único que queda si la ROM se cuelga). */
void sds_progress(const char *text)
{
	char line[40] = "> ";

	lprintf("[progreso] %s\n", text);
	append(line, text);
	clear_row(14, C_BG);
	draw_text(14, 0, line, C_WARN, C_BG);
}

/* Estado de libnds al arrancar la ROM (antes de que las pruebas apaguen el PU o muevan registros), para restaurarlo al
 * final y poder usar la SD (DLDI) como libnds espera. */
#define MRC(crn, crm, op2) ({ uint32_t v_; __asm__ volatile("mrc p15, 0, %0, " #crn ", " #crm ", " #op2 : "=r"(v_)); v_; })
#define MCRW(crn, crm, op2, val) __asm__ volatile("mcr p15, 0, %0, " #crn ", " #crm ", " #op2 : : "r"(val) : "memory")
static struct {
	uint32_t ctrl, dtcm, itcm, exmem, auxspi, ime, ie;
	uint8_t wramcnt;
} snap;

static void __attribute__((constructor, used)) snapshot_libnds(void)
{
	snap.ctrl = MRC(c1, c0, 0);
	snap.dtcm = MRC(c9, c1, 0);
	snap.itcm = MRC(c9, c1, 1);
	snap.exmem = *(volatile uint16_t *)0x04000204;
	snap.auxspi = *(volatile uint16_t *)0x040001A0;
	snap.wramcnt = *(volatile uint8_t *)0x04000247;
	snap.ime = *(volatile uint32_t *)0x04000208;
	snap.ie = *(volatile uint32_t *)0x04000210;
}

/* Devuelve la ARM9 al estado de libnds: DMA y temporizadores parados, IRQ enmascaradas (el DLDI sondea; así ninguna IRQ
 * cae en un manejador que las pruebas dejaron), ranura de cartucho y WRAM como estaban, TCM, PU y cachés como estaban. */
static void restore_libnds(void)
{
	uint32_t now;

	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	*(volatile uint32_t *)0x04000208 = 0;          /* IME */
	*(volatile uint32_t *)0x04000210 = snap.ie;    /* IE como lo dejó libnds */
	*(volatile uint32_t *)0x04000214 = 0xFFFFFFFFu; /* IF */
	for (int i = 0; i < 4; i++) {
		*(volatile uint16_t *)(0x040000BA + 12 * i) = 0; /* DMAxCNT_H */
		*(volatile uint16_t *)(0x04000102 + 4 * i) = 0;  /* TMxCNT_H */
	}
	*(volatile uint16_t *)0x04000204 = (uint16_t)snap.exmem;
	*(volatile uint8_t *)0x04000247 = snap.wramcnt;
	*(volatile uint16_t *)0x040001A0 = (uint16_t)(snap.auxspi & 0xFF);
	now = MRC(c1, c0, 0);
	if (MRC(c9, c1, 0) != snap.dtcm)
		MCRW(c9, c1, 0, snap.dtcm);
	if (MRC(c9, c1, 1) != snap.itcm)
		MCRW(c9, c1, 1, snap.itcm);
	if (now != snap.ctrl) {
		/* con la caché apagada no hay datos sucios: se invalida la de datos y la de instrucciones antes de reactivarlas */
		if (!(now & 4u)) {
			MCRW(c7, c6, 0, 0);
		}
		if (!(now & 0x1000u))
			MCRW(c7, c5, 0, 0);
		MCRW(c1, c0, 0, snap.ctrl);
	}
	/* IRQ como las tenía libnds (IME e IE, bit I del CPSR limpio): tras exit(0) la escritura pendiente se completaba con las
	 * IRQ habilitadas y sin ellas el log no llegaba a la SD en el R4 (hipótesis; ver docs/ds-real.md) */
	*(volatile uint32_t *)0x04000208 = snap.ime;
	__asm__ volatile("msr cpsr_c, #0x1F" : : : "cc");
}

static int sd_mounted, boot_state; /* boot_state: 1 = el archivo testigo del arranque se escribió, -1 = falló, 0 = no se intentó */
static uint32_t sd_tam;
#define SD_DIR "/SalsaDS-pruebas/resultados"

/* Monta la SD una sola vez: caché de páginas mínima (fatInit(1)) y, si no, la de por defecto. */
static int sd_mount(void)
{
	if (sd_mounted)
		return 1;
	if (!fatInit(1, true) && !fatInitDefault())
		return 0;
	mkdir("/SalsaDS-pruebas", 0777);
	mkdir(SD_DIR, 0777);
	sd_mounted = 1;
	return 1;
}

/* Primer nombre libre `<dir>/<base>-NNN.txt` (NNN desde 001): en el R4 crear archivos funciona y agregar a uno existente no, así que
 * cada corrida es un archivo nuevo. Devuelve el número elegido (0 si no hay libre). */
static uint32_t sd_free_name(char *out, size_t n, const char *base)
{
	for (uint32_t i = 1; i < 1000; i++) {
		FILE *t;

		snprintf(out, n, SD_DIR "/%s-%03lu.txt", base, (unsigned long)i);
		t = fopen(out, "r");
		if (!t)
			return i;
		fclose(t);
	}
	return 0;
}

/* Escribe `text` al final de `path`, lo vuelca (fflush + fsync) y lo cierra; devuelve el tamaño del archivo al cerrar o -1. */
static long sd_append(const char *path, const char *text, size_t len, const char *head)
{
	FILE *f = fopen(path, "a");
	long pos;

	if (!f)
		return -1;
	if (head && fputs(head, f) < 0)
		goto bad;
	if (len && fwrite(text, 1, len, f) != len)
		goto bad;
	if (fflush(f) != 0 || fsync(fileno(f)) != 0)
		goto bad;
	pos = ftell(f);
	if (fclose(f) != 0)
		return -1;
	return pos;
bad:
	fclose(f);
	return -1;
}

/* Archivo testigo al ARRANCAR (antes de las pruebas, con el estado de libnds intacto): dice si la escritura funciona entonces. */
static void sd_boot_witness(void)
{
	char head[96], path[64], base[40];

	if (boot_state)
		return;
	boot_state = -1;
	if (!sd_mount())
		return;
	snprintf(base, sizeof base, "_arranque-%s", SDS_ROM_NAME);
	if (!sd_free_name(path, sizeof path, base))
		return;
	snprintf(head, sizeof head, "arranque %s %s\n", SDS_ROM_NAME, SDS_GITREV);
	if (sd_append(path, "", 0, head) > 0)
		boot_state = 1;
}

/* Escribe el búfer en la SD y VERIFICA: reabre el archivo, mide su tamaño y busca la marca final de esta corrida. Sin SD o si
 * falla, sd_state = -1; si el tamaño no creció o falta la marca, sd_state = -2. */
static void sd_flush(void)
{
	char line[96], tail[64], mark[48];
	FILE *f;
	long pos, size;

	sd_state = -1;
	restore_libnds();
	if (!sd_mount())
		return;
	sd_run = sd_free_name(sd_path, sizeof sd_path, SDS_ROM_NAME);
	if (!sd_run)
		return;
	snprintf(line, sizeof line, "=== corrida %lu ===\nROM: %s  version (commit): %s\n", (unsigned long)sd_run, SDS_ROM_NAME,
		 SDS_GITREV);
	snprintf(mark, sizeof mark, "=== fin corrida %lu ===\n", (unsigned long)sd_run);
	f = fopen(sd_path, "w");
	if (!f)
		return;
	if (fputs(line, f) < 0 || fwrite(logbuf, 1, loglen, f) != loglen || fputs("\n", f) < 0 || fputs(mark, f) < 0 ||
	    fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		return;
	}
	pos = ftell(f);
	if (fclose(f) != 0)
		return;
	/* verificar: tamaño y última línea */
	f = fopen(sd_path, "r");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	sd_tam = (uint32_t)size;
	sd_state = -2;
	if (size >= pos && size >= (long)strlen(mark) && fseek(f, size - (long)strlen(mark), SEEK_SET) == 0) {
		size_t n = fread(tail, 1, strlen(mark), f);

		tail[n] = 0;
		if (n == strlen(mark) && !strcmp(tail, mark))
			sd_state = 1;
	}
	fclose(f);
	/* cerrar el dispositivo del DLDI: algunos controladores solo confirman las escrituras al apagarlo */
	{
		const DISC_INTERFACE *d = dldiGetInternal();

		if (d && d->shutdown)
			d->shutdown();
	}
}

/* Manejador de excepciones propio (ARM9): en vez de la pantalla de libnds en la de abajo, pinta en la de arriba el grupo, el PC
 * y los registros. NO toca la SD (con la pila o el estado dañados se colgaba) y no desreferencia nada que no esté en memoria
 * principal. Si una excepción ocurre dentro del propio manejador, la pantalla muestra primero la ORIGINAL (copia de los
 * registros al entrar) y luego la segunda. */
static int32_t exc_first[16];
static uint32_t exc_depth;

static int exc_readable(uint32_t a)
{
	return a >= 0x02000000u && a < 0x023FFFFCu;
}

static void exc_dump(int row0, const char *tag, const int32_t *reg, uint16_t bg)
{
	char line[40], num[12];
	const uint32_t pc = (uint32_t)reg[15];

	line[0] = 0;
	append(line, tag);
	append(line, " PC ");
	hex32(num, pc);
	append(line, num);
	draw_text(row0, 0, line, C_FG, bg);
	if (exc_readable(pc - 8) && exc_readable(pc)) {
		line[0] = 0;
		append(line, "I-4 ");
		hex32(num, *(volatile uint32_t *)(pc - 4));
		append(line, num);
		append(line, " I-8 ");
		hex32(num, *(volatile uint32_t *)(pc - 8));
		append(line, num);
	} else {
		line[0] = 0;
		append(line, "PC FUERA DE LA RAM");
	}
	draw_text(row0 + 1, 0, line, C_FG, bg);
	line[0] = 0;
	append(line, "SP ");
	hex32(num, (uint32_t)reg[13]);
	append(line, num);
	append(line, " LR ");
	hex32(num, (uint32_t)reg[14]);
	append(line, num);
	draw_text(row0 + 2, 0, line, C_FG, bg);
	for (int r = 0; r < 3; r++) {
		line[0] = 0;
		append(line, "R");
		dec(num, (uint32_t)(r * 4));
		append(line, num);
		append(line, "=");
		hex32(num, (uint32_t)reg[r * 4]);
		append(line, num);
		append(line, " ");
		dec(num, (uint32_t)(r * 4 + 1));
		append(line, "R");
		append(line, num);
		append(line, "=");
		hex32(num, (uint32_t)reg[r * 4 + 1]);
		append(line, num);
		draw_text(row0 + 3 + r, 0, line, C_FG, bg);
	}
}

static void exc_handler(void)
{
	const uint16_t bg = RGB(20, 2, 2);
	char line[40];

	if (exc_depth++ == 0)
		for (int r = 0; r < 16; r++)
			exc_first[r] = exceptionRegisters[r];
	fill_rect(0, 0, 256, 192, bg);
	line[0] = 0;
	append(line, "EXCEPCION EN ");
	append(line, group_name ? group_name : "-");
	draw_text(0, 0, line, C_FG, bg);
	exc_dump(1, exc_depth > 1 ? "1A" : "", exc_first, bg);
	if (exc_depth > 1) {
		draw_text(8, 0, "SEGUNDA EXCEPCION (EN EL MANEJADOR)", C_FG, bg);
		exc_dump(9, "2A", (const int32_t *)exceptionRegisters, bg);
	}
	draw_text(15, 0, "ANOTA ESTA PANTALLA (FOTO)", C_FG, bg);
	for (;;)
		;
}

void sds_init(const char *title)
{
	volatile uint32_t *w = (volatile uint32_t *)R;

	for (unsigned i = 0; i < (SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail)) / 4; i++)
		w[i] = 0;
	R->magic = SDS_MAGIC;
	R->version = 1;
	publish();

	setExceptionHandler(exc_handler);
	sd_boot_witness();
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
	if (nglog < MAXG) {
		glog[nglog].name = group_name;
		glog[nglog].checks = group_checks;
		glog[nglog].failed = group_failed;
	}
	nglog++;
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
	if (group_row > ROW_LAST_GROUP) { /* sin filas: subir la lista una fila (se ven los últimos grupos) */
		for (int j = (ROW_FIRST_GROUP + 1) * 12; j < (ROW_LAST_GROUP + 1) * 12; j++)
			for (int i = 0; i < 256; i++)
				FB[(j - 12) * 256 + i] = FB[j * 256 + i];
		group_row = ROW_LAST_GROUP;
	}
	clear_row((int)group_row, C_BG);
	draw_text((int)group_row, 0, line, group_failed ? C_BAD : C_OK, C_BG);
	group_row++;
	group_name = 0;
	publish();
}

void sds_group(const char *name)
{
	close_group();
	group_name = name;
	group_checks = group_failed = 0;
	{
		char t[40] = "GRUPO ";

		append(t, name);
		sds_progress(t);
	}
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

void sds_diverge_at(uint32_t got, uint32_t doc, uint32_t melon, uint32_t id, const char *file)
{
	const char *b = file;

	for (const char *p = file; *p; p++)
		if (*p == '/')
			b = p + 1;
	if (nslog < MAXS) {
		slog[nslog].id = id;
		slog[nslog].got = got;
		slog[nslog].doc = doc;
		slog[nslog].melon = melon;
		slog[nslog].group = R->groups + 1;
		slog[nslog].file = b;
		nslog++;
	}
	nslog_total++;
	sds_diverge(got, doc, id);
}

static const char *notes[4];
static uint32_t nnotes;

void sds_note(const char *text)
{
	if (nnotes < 4)
		notes[nnotes++] = text;
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

static const char *grp_name(uint32_t g)
{
	return g >= 1 && g <= nglog && g <= MAXG ? glog[g - 1].name : "?";
}

static void fmt_val(char *out, uint32_t v)
{
	if (v == SDS_MELON_NA)
		strcpy(out, "?");
	else
		snprintf(out, 12, "%08lX", (unsigned long)v);
}

static void write_log(void)
{
	const struct sds_x7 *x = (const struct sds_x7 *)SDS_X7_ADDR;
	int have7;
	char a[12], b[12], c[12];

	/* el ARM7 escribió aparte: invalidar la caché de datos de esas líneas antes de leer */
	for (uint32_t a2 = SDS_X7_ADDR; a2 < SDS_X7_ADDR + sizeof(struct sds_x7); a2 += 32)
		__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a2) : "memory");
	have7 = x->magic == SDS_X7_MAGIC;

	lprintf("--- fin de la ROM ---\n");
	lprintf("Veredicto: %s  comprobaciones: %lu  fallos: %lu  divergencias respecto de la doc: %lu\n",
		R->failed ? "FALLO" : "OK", (unsigned long)R->total, (unsigned long)R->failed, (unsigned long)R->diverge);
	for (uint32_t i = 0; i < nnotes; i++)
		lprintf("NOTA: %s\n", notes[i]);
	lprintf("Grupos (ARM9):\n");
	for (uint32_t i = 0; i < nglog && i < MAXG; i++)
		lprintf("  ARM9 %-12s total %lu fallos %lu\n", glog[i].name, (unsigned long)glog[i].checks,
			(unsigned long)glog[i].failed);
	if (have7) {
		lprintf("Grupos (ARM7):\n");
		for (uint32_t i = 0; i < x->ngroups && i < SDS_X7_GROUPS; i++)
			lprintf("  ARM7 %-12s total %lu fallos %lu\n", x->g[i].name, (unsigned long)x->g[i].checks,
				(unsigned long)x->g[i].failed);
	}
	if (R->failed) {
		lprintf("Primer fallo (ARM9): grupo %s id L%lu obtenido %08lX esperado %08lX\n", grp_name(R->first_fail_group),
			(unsigned long)R->first_fail_id, (unsigned long)R->first_fail_got, (unsigned long)R->first_fail_want);
		for (uint32_t i = 0; i < R->failed && i < SDS_FAILLOG_MAX; i++)
			lprintf("  FALLO ARM9 grupo %s id L%lu obtenido %08lX esperado %08lX\n", grp_name(FAILLOG[i].group),
				(unsigned long)FAILLOG[i].id, (unsigned long)FAILLOG[i].got, (unsigned long)FAILLOG[i].want);
	}
	{
		const volatile struct sds_results *R7 = (const volatile struct sds_results *)SDS_ARM7_RESULTS_ADDR;
		const volatile struct sds_fail *F7 = (const volatile struct sds_fail *)(SDS_ARM7_RESULTS_ADDR + SDS_FAILLOG_OFFSET);

		for (uint32_t a2 = SDS_ARM7_RESULTS_ADDR; a2 < SDS_ARM7_RESULTS_ADDR + 0x340; a2 += 32)
			__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a2) : "memory");
		if (have7 && R7->magic == SDS_MAGIC) {
			lprintf("ARM7: total %lu fallos %lu\n", (unsigned long)R7->total, (unsigned long)R7->failed);
			for (uint32_t i = 0; i < R7->failed && i < SDS_FAILLOG_MAX; i++)
				lprintf("  FALLO ARM7 grupo %lu id L%lu obtenido %08lX esperado %08lX\n", (unsigned long)F7[i].group,
					(unsigned long)F7[i].id, (unsigned long)F7[i].got, (unsigned long)F7[i].want);
		}
	}
	lprintf("Divergencias (hw = este hardware, doc = GBATEK/manual, melon = melonDS 1.1; ? = sin dato):\n");
	for (uint32_t i = 0; i < nslog && i < MAXS; i++) {
		fmt_val(a, slog[i].got);
		fmt_val(b, slog[i].doc);
		fmt_val(c, slog[i].melon);
		lprintf("  DIV ARM9 %s:%lu grupo %s hw=%s doc=%s melon=%s -> %s\n", slog[i].file, (unsigned long)slog[i].id,
			grp_name(slog[i].group), a, b, c,
			slog[i].got == slog[i].doc ? "coincide con la doc" : slog[i].got == slog[i].melon ? "coincide con melonDS" : "ninguno");
	}
	if (nslog_total > MAXS)
		lprintf("  (ARM9: %lu llamadas, solo las primeras %d)\n", (unsigned long)nslog_total, MAXS);
	if (have7) {
		for (uint32_t i = 0; i < x->nsites && i < SDS_X7_SITES; i++) {
			fmt_val(a, x->s[i].got);
			fmt_val(b, x->s[i].doc);
			fmt_val(c, x->s[i].melon);
			lprintf("  DIV ARM7 %.20s:%lu grupo %.12s hw=%s doc=%s melon=%s -> %s\n", x->s[i].file,
				(unsigned long)x->s[i].id,
				x->s[i].group >= 1 && x->s[i].group <= x->ngroups && x->s[i].group <= SDS_X7_GROUPS ? x->g[x->s[i].group - 1].name : "?",
				a, b, c,
				x->s[i].got == x->s[i].doc ? "coincide con la doc"
				: x->s[i].got == x->s[i].melon ? "coincide con melonDS" : "ninguno");
		}
		if (x->nsites_total > SDS_X7_SITES)
			lprintf("  (ARM7: %lu llamadas, solo las primeras %lu)\n", (unsigned long)x->nsites_total,
				(unsigned long)SDS_X7_SITES);
	}

}

/* Pantalla de divergencias (solo las que difieren de la doc en ESTE hardware): A/derecha siguiente, B/izquierda anterior. */
#define PER_PAGE 6
static uint32_t ndiv_screen(const struct sds_x7 *x, int have7)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < nslog && i < MAXS; i++)
		n += slog[i].got != slog[i].doc;
	if (have7)
		for (uint32_t i = 0; i < x->nsites && i < SDS_X7_SITES; i++)
			n += x->s[i].got != x->s[i].doc;
	return n;
}

static void div_page(const struct sds_x7 *x, int have7, uint32_t page, uint32_t total)
{
	uint32_t k = 0, row = 1;
	char line[40], num[12], t[12];

	fill_rect(0, 0, 256, 192, C_BG);
	line[0] = 0;
	append(line, "DIV ");
	dec(num, page + 1);
	append(line, num);
	append(line, "/");
	dec(num, (total + PER_PAGE - 1) / PER_PAGE);
	append(line, num);
	append(line, " A=SIG B=ANT");
	draw_text(0, 0, line, C_FG, C_BG);
	for (int cpu = 0; cpu < 2; cpu++) {
		uint32_t n = cpu ? (have7 ? x->nsites : 0) : nslog;

		for (uint32_t i = 0; i < n; i++) {
			uint32_t id = cpu ? x->s[i].id : slog[i].id, got = cpu ? x->s[i].got : slog[i].got,
				 doc = cpu ? x->s[i].doc : slog[i].doc, mel = cpu ? x->s[i].melon : slog[i].melon;
			uint16_t col;

			if (got == doc)
				continue;
			if (k / PER_PAGE == page) {
				const char *g = cpu ? x->g[0].name : grp_name(slog[i].group);
				uint32_t gi = cpu ? x->s[i].group : 0;

				if (cpu)
					g = gi >= 1 && gi <= SDS_X7_GROUPS ? x->g[gi - 1].name : "?";
				line[0] = 0;
				append(line, "DIV ");
				append(line, g);
				append(line, " L");
				dec(num, id);
				append(line, num);
				append(line, cpu ? " ARM7" : " ARM9");
				draw_text((int)row, 0, line, C_FG, C_BG);
				col = got == mel ? C_WARN : C_BAD;
				line[0] = 0;
				append(line, "H");
				hex32(t, got);
				append(line, t);
				append(line, " D");
				hex32(t, doc);
				append(line, t);
				append(line, " M");
				if (mel == SDS_MELON_NA)
					append(line, "?");
				else {
					hex32(t, mel);
					append(line, t);
				}
				draw_text((int)row + 1, 0, line, col, C_BG);
				row += 2;
			}
			k++;
		}
	}
	draw_text(14, 0, "H=HW D=DOC M=MELONDS", C_DIM, C_BG);
	draw_text(15, 0, "AMARILLO=COMO MELON", C_DIM, C_BG);
}

void sds_finish(void)
{
	char line[40] = "", num[12];
	const struct sds_x7 *x = (const struct sds_x7 *)SDS_X7_ADDR;
	int have7;
	uint32_t total, page = 0, pages, prev = 0x3FF;

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

	write_log();
	draw_text(1, 13, "SD:...", C_DIM, C_BG);
	sd_flush();
	fill_rect(13 * 8, 12, 19 * 8, 12, C_BG);
	{
		char st[28], num[12];

		st[0] = 0;
		append(st, sd_state == 1 ? "SD OK" : sd_state == -2 ? "SD ERR" : "SD NO");
		if (sd_state == 1 || sd_state == -2) {
			append(st, " T=");
			dec(num, sd_tam);
			append(st, num);
		}
		append(st, boot_state == 1 ? " A=+" : " A=-");
		draw_text(1, 13, st, sd_state == 1 ? C_OK : C_WARN, C_BG);
	}

	for (uint32_t a2 = SDS_X7_ADDR; a2 < SDS_X7_ADDR + sizeof(struct sds_x7); a2 += 32)
		__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(a2) : "memory");
	have7 = x->magic == SDS_X7_MAGIC;
	total = ndiv_screen(x, have7);
	pages = (total + PER_PAGE - 1) / PER_PAGE;
	if (total && !R->failed) {
		line[0] = 0;
		append(line, "A: VER DIVERGENCIAS ");
		dec(num, total);
		append(line, num);
		draw_text(ROW_FAIL, 0, line, C_WARN, C_BG);
	}
	/* sin teclas (emuladores: KEYINPUT = 0x3FF) no pasa nada; en la DS A/B/izquierda/derecha cambian de página;
	 * la página 0 es el resumen */
	for (;;) {
		uint32_t keys = *(volatile uint16_t *)0x04000130 & 0x3FF, pressed = prev & ~keys;

		prev = keys;
		if (pressed & (1u << 3)) { /* START: volver al menú del cargador (bootstub); si no hay, avisar */
			fill_rect(0, 0, 256, 192, C_BG);
			draw_text(6, 0, "APAGA LA CONSOLA PARA", C_FG, C_BG);
			draw_text(7, 0, "VOLVER AL MENU", C_FG, C_BG);
			/* sin exit(0): en el R4 reiniciaba la ROM una y otra vez (40 corridas en el log) */
			for (;;)
				;
		}
		if (!pages)
			continue;
		if (pressed & ((1u << 0) | (1u << 4))) { /* A, derecha */
			page = page >= pages ? 1 : page + 1;
			div_page(x, have7, page - 1, total);
		} else if (pressed & ((1u << 1) | (1u << 5))) { /* B, izquierda */
			page = page <= 1 ? pages : page - 1;
			div_page(x, have7, page - 1, total);
		}
	}
}
