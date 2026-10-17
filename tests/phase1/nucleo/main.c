/*
 * SalsaDS — «SalsaDS núcleo» (Fase 1.2): el núcleo de la DS en la PS3, con las dos pantallas.
 *
 * Corre la máquina de src/core (ARM9 y ARM7 con intérpretes, mapa de memoria, IRQ, temporizadores, DMA, IPC,
 * cartucho, SPI, BIOS en HLE) sobre una ROM y muestra las dos pantallas con la salida de la Fase 0.6 (rsxkit).
 * Por ahora solo se ve lo que no necesita los motores 2D (Fase 2): el modo de visualización directa de VRAM
 * del motor A y el color de fondo del motor B.
 *
 * ROM: /dev_hdd0/salsads/rom.nds si existe (un respaldo del autor, ya desencriptado, o una ROM propia); si no,
 * la ROM de demostración propia (tests/roms/src/demo), embebida en el ejecutable.
 *
 * Entrada (src/frontend/dspad.h, /dev_hdd0/salsads/controles.cfg): el DS3 a los botones de la DS; el puntero del
 * táctil con el stick derecho (R2 toca); L2 cambia la distribución de las pantallas; L3 + R3 muestra u oculta la
 * línea de estado. Se emula un cuadro de la DS por cada cuadro del RSX (60 Hz).
 *
 * Log: /dev_hdd0/salsads/nucleo.log, con la versión (commit) y el APPID en la primera línea. Cada ~2 s escribe
 * una línea DATO con el tiempo de emulación por cuadro y las instrucciones por cuadro de cada CPU.
 *
 * Con el archivo de prueba automática (RPCS3) corre 150 cuadros con entrada sintética, comprueba que los dos
 * procesadores corrieron y que la imagen de la demostración es la esperada, y sale.
 */
#include <io/pad.h>
#include <malloc.h>
#include <math.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include "core/nds.h"
#include "frontend/dspad.h"
#include "frontend/layout.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "rsxkit/rsxkit.h"
#include "rsxkit/text.h"
#include "rsxkit/ui.h"

#include "demo_bin.h"
#include "../../roms/src/demo/shared.h"
#include "util/endian.h"

#ifndef NUCLEO_VERSION
#define NUCLEO_VERSION "desconocida"
#endif
#define NUCLEO_TITLE "SalsaDS núcleo"
#define NUCLEO_APPID "SLDS00008"
#define ROM_PATH SALSADS_DATA_DIR "/rom.nds"
#define CFG_PATH SALSADS_DATA_DIR "/controles.cfg"

SYS_PROCESS_PARAM(1001, 0x100000);

#define TEX_W 256
#define TEX_H 192
#define TEX_PITCH 512
#define TEX_BYTES (TEX_PITCH * TEX_H)
#define UI_W 1280.0f
#define UI_H 720.0f
#define CLEAR_RGB 0x101010u
#define AUTOTEST_FRAMES 150
#define STATS_FRAMES 120

static volatile int quit;
static int autotest, failures;

static struct nds *nds;
static uint8_t *rom_buf;
static size_t rom_len;
static const char *rom_name;

static struct sds_padcfg cfg;
static struct sds_input input;
static struct sds_keys keys;
static struct sds_ds3 ds3;
static int pad_present;

static uint8_t *tex_mem;
static uint32_t tex_off;
static uint32_t set_fence[2];
static int set_used[2];
static uint32_t frame_no;

static uint16_t top555[TEX_W * TEX_H], bot555[TEX_W * TEX_H];
static enum sds_layout layout = SDS_LAYOUT_SIDE;
static int show_osd = 1;
static int cur_w, cur_h;

static inline uint64_t now_us(void)
{
	return sysGetSystemTime();
}

static void fail(const char *what)
{
	failures++;
	salsads_log("FALLO %s", what);
}

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
	(void)param;
	(void)usrdata;
	if (status == SYSUTIL_EXIT_GAME)
		quit = 1;
}

static bool wait_hook(void *ctx, const rsxkit_wait_info *info)
{
	(void)ctx;
	(void)info;
	sysUtilCheckCallback();
	return !quit;
}

static void core_log(void *ctx, const char *msg)
{
	(void)ctx;
	salsads_log("núcleo: %s", msg);
}

/* ---- ROM ------------------------------------------------------------------------------------------- */

static int load_rom(void)
{
	FILE *f = fopen(ROM_PATH, "rb");

	if (f) {
		long n;

		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (n >= 0x200 && n <= 0x10000000L) {
			rom_buf = memalign(128, (size_t)n);
			if (rom_buf && fread(rom_buf, 1, (size_t)n, f) == (size_t)n) {
				rom_len = (size_t)n;
				rom_name = ROM_PATH;
				fclose(f);
				salsads_log("ROM de %s: %u bytes", ROM_PATH, (unsigned)rom_len);
				return 0;
			}
			free(rom_buf);
			rom_buf = NULL;
			salsads_log("no pude leer %s (%ld bytes): uso la ROM de demostración", ROM_PATH, n);
		} else {
			salsads_log("%s no parece una ROM de la DS (%ld bytes): uso la demostración", ROM_PATH, n);
		}
		fclose(f);
	}
	rom_buf = (uint8_t *)demo_bin;
	rom_len = demo_bin_size;
	rom_name = "demostración embebida";
	salsads_log("ROM de demostración propia embebida: %u bytes", (unsigned)rom_len);
	return 0;
}

/* ---- pantallas ----------------------------------------------------------------------------------------- */

static uint16_t *tex_px(int set, int screen)
{
	return (uint16_t *)(void *)(tex_mem + (size_t)(set * 2 + screen) * TEX_BYTES);
}

static uint32_t tex_offset(int set, int screen)
{
	return tex_off + (uint32_t)((set * 2 + screen) * TEX_BYTES);
}

static uint32_t vb_offset(int set)
{
	return tex_off + 0x80000u + (uint32_t)set * 0x1000u;
}

static rsxkit_vertex *vb_ptr(int set)
{
	return (rsxkit_vertex *)(void *)(tex_mem + 0x80000u + (size_t)set * 0x1000u);
}

/* RGB555 de la DS (bits 0-4 rojo, 5-9 verde, 10-14 azul) a RGBA5551 del RSX. */
static void convert(uint16_t *dst, const uint16_t *src)
{
	for (int y = 0; y < TEX_H; y++) {
		uint16_t *row = dst + y * (TEX_PITCH / 2);

		for (int x = 0; x < TEX_W; x++) {
			const uint32_t v = src[y * TEX_W + x];

			row[x] = (uint16_t)(((v & 31u) << 11) | (((v >> 5) & 31u) << 6) | (((v >> 10) & 31u) << 1) | 1u);
		}
	}
}

static void make_quad(rsxkit_vertex *v, const struct sds_rect *r)
{
	const float x0 = (float)r->x, y0 = (float)r->y, x1 = (float)(r->x + r->w), y1 = (float)(r->y + r->h);
	static const float uv[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

	for (int i = 0; i < 6; i++) {
		v[i].pos[0] = uv[i][0] ? x1 : x0;
		v[i].pos[1] = uv[i][1] ? y1 : y0;
		v[i].pos[2] = 0.0f;
		v[i].pos[3] = 1.0f;
		v[i].st[0] = uv[i][0];
		v[i].st[1] = uv[i][1];
		v[i].rgba[0] = v[i].rgba[1] = v[i].rgba[2] = v[i].rgba[3] = 255;
		v[i].pad = 0;
	}
}

static void draw_screens(int set, const struct sds_rect r[2])
{
	rsxkit_vertex *vb = vb_ptr(set);

	make_quad(vb, &r[0]);
	make_quad(vb + 6, &r[1]);
	rsxkit_bind_frame((float)cur_w, (float)cur_h);
	rsxkit_vertices(vb_offset(set), RSXKIT_MEM_MAIN);
	for (int s = 0; s < 2; s++) {
		rsxkit_draw d;

		memset(&d, 0, sizeof d);
		d.blend = RSXKIT_BLEND_OFF;
		d.cull = RSXKIT_CULL_NONE;
		d.ntex = 1;
		d.scissor[2] = (float)cur_w;
		d.scissor[3] = (float)cur_h;
		d.tex[0].offset = tex_offset(set, s);
		d.tex[0].pitch = TEX_PITCH;
		d.tex[0].width = TEX_W;
		d.tex[0].height = TEX_H;
		d.tex[0].location = RSXKIT_MEM_MAIN;
		d.tex[0].format = RSXKIT_RGBA5551;
		d.tex[0].wrap_s = d.tex[0].wrap_t = RSXKIT_WRAP_CLAMP;
		d.tex[0].filter = RSXKIT_FILTER_LINEAR;
		d.tex[0].xf[0] = d.tex[0].xf[1] = 1.0f;
		for (int c = 0; c < 4; c++)
			d.k[RSXKIT_K_T0][1][c] = 1.0f;
		rsxkit_draw_triangles(&d, (uint32_t)s * 6, 6);
	}
}

static void draw_pointer(const struct sds_rect *touch, float k)
{
	const float cx = ((float)touch->x + (input.x + 0.5f) * touch->scale) * k;
	const float cy = ((float)touch->y + (input.y + 0.5f) * touch->scale) * k;

	if (input.touching) {
		rsxkit_ui_rect(cx - 9, cy - 9, 18, 18, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 7, cy - 7, 14, 14, 0xE02020FF);
	} else {
		rsxkit_ui_rect(cx - 14, cy - 2, 28, 4, 0x000000C0);
		rsxkit_ui_rect(cx - 2, cy - 14, 4, 28, 0x000000C0);
		rsxkit_ui_rect(cx - 13, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx + 2, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy - 13, 2, 11, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy + 2, 2, 11, 0xFFFFFFFF);
	}
}

struct stats {
	uint32_t emu_us, frame_us;
	double mips9, mips7;
};

static void draw_ui(const struct sds_rect r[2], const struct stats *st)
{
	const float k = UI_W / (float)cur_w;
	char line[160];

	rsxkit_ui_begin(UI_W, UI_H);
	draw_pointer(&r[1], k);
	if (show_osd) {
		rsxkit_ui_rect(6, 6, 600, 52, 0x000000A0);
		snprintf(line, sizeof line, "%s %s  %s", NUCLEO_TITLE, NUCLEO_VERSION, sds_layout_name(layout));
		rsxkit_ui_text(12, 10, line, 1.4f, 0xFFFFFFFF, 0);
		snprintf(line, sizeof line, "emulacion %u.%02u ms  cuadro %u.%02u ms  ARM9 %u.%u MIPS  ARM7 %u.%u MIPS",
			 st->emu_us / 1000, st->emu_us % 1000 / 10, st->frame_us / 1000, st->frame_us % 1000 / 10,
			 (unsigned)st->mips9, (unsigned)(st->mips9 * 10) % 10, (unsigned)st->mips7, (unsigned)(st->mips7 * 10) % 10);
		rsxkit_ui_text(12, 32, line, 1.2f, 0xC0FFC0FF, 0);
	}
	rsxkit_ui_end();
}

/* ---- entrada ---------------------------------------------------------------------------------------------- */

static struct sds_ds3 neutral_pad(void)
{
	struct sds_ds3 p;

	memset(&p, 0, sizeof p);
	p.lx = p.ly = p.rx = p.ry = 128;
	p.accel[0] = p.accel[1] = p.accel[2] = 512;
	p.gyro = 512;
	return p;
}

static void convert_pad(const padData *d, struct sds_ds3 *s)
{
	s->buttons = (uint32_t)d->BTN_CROSS << SDS_DS3_CROSS | (uint32_t)d->BTN_CIRCLE << SDS_DS3_CIRCLE |
		     (uint32_t)d->BTN_SQUARE << SDS_DS3_SQUARE | (uint32_t)d->BTN_TRIANGLE << SDS_DS3_TRIANGLE |
		     (uint32_t)d->BTN_L1 << SDS_DS3_L1 | (uint32_t)d->BTN_R1 << SDS_DS3_R1 |
		     (uint32_t)d->BTN_L2 << SDS_DS3_L2 | (uint32_t)d->BTN_R2 << SDS_DS3_R2 |
		     (uint32_t)d->BTN_L3 << SDS_DS3_L3 | (uint32_t)d->BTN_R3 << SDS_DS3_R3 |
		     (uint32_t)d->BTN_START << SDS_DS3_START | (uint32_t)d->BTN_SELECT << SDS_DS3_SELECT |
		     (uint32_t)d->BTN_UP << SDS_DS3_UP | (uint32_t)d->BTN_DOWN << SDS_DS3_DOWN |
		     (uint32_t)d->BTN_LEFT << SDS_DS3_LEFT | (uint32_t)d->BTN_RIGHT << SDS_DS3_RIGHT;
	s->lx = (uint8_t)d->ANA_L_H;
	s->ly = (uint8_t)d->ANA_L_V;
	s->rx = (uint8_t)d->ANA_R_H;
	s->ry = (uint8_t)d->ANA_R_V;
	s->accel[0] = (uint16_t)d->SENSOR_X;
	s->accel[1] = (uint16_t)d->SENSOR_Y;
	s->accel[2] = (uint16_t)d->SENSOR_Z;
	s->gyro = (uint16_t)d->SENSOR_G;
	s->sensors_ok = (d->SENSOR_X | d->SENSOR_Y | d->SENSOR_Z) != 0;
}

static void load_config(void)
{
	sds_padcfg_default(&cfg);
	FILE *f = fopen(CFG_PATH, "r");

	if (f) {
		char text[4096], err[96];
		const size_t n = fread(text, 1, sizeof text - 1, f);

		fclose(f);
		const int bad = sds_padcfg_parse(&cfg, text, n, err, sizeof err);

		salsads_log("%s leído (%u bytes)%s%s", CFG_PATH, (unsigned)n, bad ? "; no entendí líneas: " : "", bad ? err : "");
	} else {
		salsads_log("sin %s: controles por omisión (el programa de pantallas de la Fase 0 lo crea)", CFG_PATH);
	}
}

static uint32_t combo_prev;

/* Lee el DS3 y devuelve los botones de la DS y el toque para el núcleo. */
static uint32_t poll_input(float dt, unsigned *tx, unsigned *ty)
{
	static struct sds_ds3 last;
	padInfo info;
	padData data;
	int x, y, down;
	uint32_t k;

	if (ioPadGetInfo(&info) == 0 && info.status[0]) {
		if (!pad_present)
			salsads_log("mando 1 conectado");
		pad_present = 1;
		if (ioPadGetData(0, &data) == 0 && data.len > 0)
			convert_pad(&data, &last);
		ds3 = last;
	} else {
		pad_present = 0;
		ds3 = neutral_pad();
	}
	if (autotest) {
		/* RPCS3 sin mando: cruceta y A a ratos, y toque en el centro de la pantalla táctil durante los cuadros 40-110 */
		const uint32_t f = frame_no;

		ds3.buttons = ((f / 20u) & 1u ? (1u << SDS_DS3_RIGHT) : (1u << SDS_DS3_DOWN)) | ((f / 13u) & 1u ? (1u << SDS_DS3_CIRCLE) : 0u) |
			      (f >= 40 && f < 110 ? (1u << SDS_DS3_R2) : 0u);
		ds3.rx = ds3.ry = 128;
	}
	sds_pad_map(&cfg, &input, &ds3, &keys);
	sds_pointer_update(&cfg, &input, &ds3, dt, &keys);
	if (keys.layout)
		layout = (enum sds_layout)((layout + 1) % SDS_LAYOUT_COUNT);
	{
		const uint32_t b = ds3.buttons, both = (1u << SDS_DS3_L3) | (1u << SDS_DS3_R3);
		const uint32_t now = (b & both) == both;

		if (now && !combo_prev)
			show_osd = !show_osd;
		combo_prev = now;
	}
	sds_touch_state(&input, &x, &y, &down);
	k = keys.keys & 0x0FFFu; /* mismo orden de bits que NDS_KEY_* */
	if (down) {
		k |= NDS_KEY_PEN;
		*tx = (unsigned)x;
		*ty = (unsigned)y;
	} else {
		*tx = *ty = 0;
	}
	return k;
}

/* ---- un cuadro ------------------------------------------------------------------------------------------------ */

static uint32_t buf_emu[STATS_FRAMES], buf_iv[STATS_FRAMES];
static uint32_t stat_n;
static uint64_t last_begin_us;
static struct stats cur_stats;

static void sort_u32(uint32_t *a, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		const uint32_t v = a[i];
		int j = (int)i - 1;

		while (j >= 0 && a[j] > v) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = v;
	}
}

static void log_stats(void)
{
	uint32_t over20 = 0;
	static uint64_t prev_i9, prev_i7;
	const uint64_t i9 = nds->a9.icount, i7 = nds->a7.icount;

	for (uint32_t i = 0; i < stat_n; i++)
		over20 += buf_iv[i] > 20000;
	sort_u32(buf_emu, stat_n);
	sort_u32(buf_iv, stat_n);
	salsads_log("DATO nucleo: %u cuadros; emulación de un cuadro: mediana %u.%02u ms, p95 %u.%02u, máx %u.%02u; "
		    "intervalo del RSX: mediana %u.%02u ms, p95 %u.%02u; cuadros de más de 20 ms: %u; instrucciones por cuadro: "
		    "ARM9 %u, ARM7 %u; cuadros de la DS %u",
		    stat_n, buf_emu[stat_n / 2] / 1000, buf_emu[stat_n / 2] % 1000 / 10, buf_emu[stat_n * 95 / 100] / 1000,
		    buf_emu[stat_n * 95 / 100] % 1000 / 10, buf_emu[stat_n - 1] / 1000, buf_emu[stat_n - 1] % 1000 / 10,
		    buf_iv[stat_n / 2] / 1000, buf_iv[stat_n / 2] % 1000 / 10, buf_iv[stat_n * 95 / 100] / 1000,
		    buf_iv[stat_n * 95 / 100] % 1000 / 10, over20, (unsigned)((i9 - prev_i9) / stat_n),
		    (unsigned)((i7 - prev_i7) / stat_n), (unsigned)nds->frames);
	prev_i9 = i9;
	prev_i7 = i7;
	stat_n = 0;
}

static void render_frame(void)
{
	const uint64_t t0 = now_us();
	unsigned tx, ty;
	uint32_t k;
	const int set = (int)(frame_no & 1u);
	struct sds_rect r[2];
	uint64_t i9a, i7a;
	uint64_t t1, t2;

	sysUtilCheckCallback();
	rsxkit_frame_begin(CLEAR_RGB);
	{
		const uint64_t t = now_us();
		const uint32_t iv = last_begin_us ? (uint32_t)(t - last_begin_us) : 16667;

		last_begin_us = t;
		cur_stats.frame_us = iv;
		if (stat_n < STATS_FRAMES)
			buf_iv[stat_n] = iv;
	}
	{
		float dt = cur_stats.frame_us ? (float)cur_stats.frame_us / 1e6f : 1.0f / 60.0f;

		if (dt < 1.0f / 120.0f)
			dt = 1.0f / 120.0f;
		if (dt > 1.0f / 20.0f)
			dt = 1.0f / 20.0f;
		k = poll_input(dt, &tx, &ty);
	}
	nds_set_input(nds, k, tx, ty);
	i9a = nds->a9.icount;
	i7a = nds->a7.icount;
	t1 = now_us();
	nds_run_frame(nds);
	t2 = now_us();
	cur_stats.emu_us = (uint32_t)(t2 - t1);
	cur_stats.mips9 = (double)(nds->a9.icount - i9a) / (double)(cur_stats.emu_us ? cur_stats.emu_us : 1);
	cur_stats.mips7 = (double)(nds->a7.icount - i7a) / (double)(cur_stats.emu_us ? cur_stats.emu_us : 1);
	if (stat_n < STATS_FRAMES)
		buf_emu[stat_n] = cur_stats.emu_us;
	nds_screens(nds, top555, bot555);
	if (set_used[set])
		rsxkit_fence_wait(set_fence[set]);
	convert(tex_px(set, 0), top555);
	convert(tex_px(set, 1), bot555);
	sds_layout_compute(layout, SDS_SCALE_FIT, cur_w, cur_h, r);
	draw_screens(set, r);
	draw_ui(r, &cur_stats);
	set_fence[set] = rsxkit_fence();
	set_used[set] = 1;
	rsxkit_frame_end();
	frame_no++;
	if (++stat_n >= STATS_FRAMES)
		log_stats();
	(void)t0;
}

/* Prueba automática: la demostración tiene que estar viva. */
static void autotest_check(void)
{
	uint32_t magic;

	magic = nds_peek32(nds, NDS_CPU9, 0x023A0000u);
	if (magic != DEMO_MAGIC)
		fail("el ARM7 no dejó su marca en la RAM compartida");
	if (nds_peek32(nds, NDS_CPU9, 0x023A0004u) < 100)
		fail("el ARM7 no contó cuadros");
	/* la raya blanca de la demostración (y = 132) y el fondo de la pantalla de arriba */
	if (top555[132 * TEX_W + 10] != 0xFFFF)
		fail("la raya blanca de la demostración no está en la pantalla de arriba");
	if (top555[190 * TEX_W + 3] != (uint16_t)(0x8000 | (8 << 10) | (2 << 5) | 2) && top555[190 * TEX_W + 3] != 0xFFFF)
		fail("el fondo de la demostración no es el esperado");
	if (nds->frames < AUTOTEST_FRAMES - 2)
		fail("la DS no corrió todos los cuadros");
	/* el toque sintético tiñó la pantalla de abajo (motor B): su fondo cambia con la posición */
	salsads_log("autoprueba: cuadros de la DS %u, fondo abajo 0x%04x, marca ARM7 0x%08x", (unsigned)nds->frames, bot555[0],
		    magic);
}

int main(void)
{
	char banner[160];
	const rsxkit_screen *si;

	snprintf(banner, sizeof banner, "nucleo %s (%s, %s)", NUCLEO_VERSION, NUCLEO_TITLE, NUCLEO_APPID);
	salsads_log_init_banner("nucleo", banner);
	autotest = salsads_autotest();
	sysUtilRegisterCallback(0, sysutil_cb, NULL);
	load_config();
	sds_input_init(&input);
	memset(&keys, 0, sizeof keys);
	ds3 = neutral_pad();
	ioPadInit(7);

	nds = memalign(128, sizeof *nds);
	if (!nds || load_rom() != 0) {
		salsads_log("RESULTADO nucleo: FALLO sin memoria para el núcleo o sin ROM");
		salsads_log_shutdown();
		return 1;
	}
	memset(nds, 0, sizeof *nds);
	nds->log = core_log;
	nds_init(nds);
	if (nds_load_rom(nds, rom_buf, rom_len) != 0 || nds_boot_direct(nds) != 0) {
		salsads_log("RESULTADO nucleo: FALLO la ROM no es válida (cabecera o binarios)");
		salsads_log_shutdown();
		return 1;
	}
	salsads_log("ROM: %s; entrada ARM9 0x%08x, ARM7 0x%08x; cuántos bytes ocupa el núcleo: %u", rom_name,
		    (unsigned)rd32le(rom_buf + 0x24), (unsigned)rd32le(rom_buf + 0x34), (unsigned)sizeof *nds);

	if (rsxkit_init(NULL) != 0) {
		salsads_log("RESULTADO nucleo: FALLO rsxkit_init");
		salsads_log_shutdown();
		return 1;
	}
	rsxkit_set_wait_hook(wait_hook, NULL, 100000, 100000);
	if (rsxkit_ui_init() != 0) {
		salsads_log("RESULTADO nucleo: FALLO rsxkit_ui_init");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	tex_mem = memalign(1u << 20, 1u << 20);
	if (!tex_mem || rsxkit_map_main(tex_mem, 1u << 20, &tex_off) != 0) {
		salsads_log("RESULTADO nucleo: FALLO memoria de texturas");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	memset(tex_mem, 0, 1u << 20);
	si = rsxkit_screen_info();
	cur_w = (int)si->width;
	cur_h = (int)si->height;
	salsads_log("salida %dx%d%s", cur_w, cur_h, autotest ? " (prueba automática: RPCS3 no mide)" : "");

	while (!quit) {
		render_frame();
		if (autotest && frame_no >= AUTOTEST_FRAMES) {
			autotest_check();
			break;
		}
	}
	salsads_log("RESULTADO nucleo: %s", failures ? "FALLO" : "OK");
	if (!rsxkit_lost()) {
		rsxkit_finish();
		rsxkit_shutdown();
	}
	ioPadEnd();
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
