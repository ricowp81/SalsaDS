/*
 * SalsaDS — Fase 0.6: salida de dos pantallas con rsxkit, y entrada.
 *
 * Dos texturas de 256×192 por cuadro (RGBA5551 en memoria principal mapeada
 * al RSX, dos juegos que se turnan con una barrera) dibujadas como quads, en
 * cuatro distribuciones (lado a lado, una sobre otra, grande arriba y grande
 * abajo), a 720p y 1080p, con escala entera (filtro puntual) y ajustada (no
 * entera, filtro lineal). El DS3 se mapea a los botones de la DS y, sobre la
 * pantalla táctil, un puntero que «toca» mientras se mantiene apretado el
 * botón de toque (src/frontend/dspad.h; /dev_hdd0/salsads/controles.cfg).
 *
 * Arranque: un barrido automático (resolución × distribución × escala) que
 * mide, por combinación, el intervalo entre cuadros, la espera del flip, el
 * trabajo del PPE y los vblanks saltados, y escribe una línea DATO por
 * combinación. En la PS3 real sigue un modo interactivo:
 *
 *   L2 (o el botón DISPOSICION)       cambia de distribución
 *   L1 + R1 y además:  ✕ modo del puntero · ○ escala · △ resolución ·
 *                      □ borra el dibujo · START sale
 *
 * El «dibujo» de la pantalla táctil es lo que un juego vería: cada toque
 * (x, y) que llegaría a la DS pinta un punto y los toques seguidos se unen
 * con una línea, para ver la precisión y el temblor del puntero.
 *
 * Con el archivo de prueba automática (RPCS3) el barrido es corto y además
 * se leen píxeles del búfer trasero para comprobar la distribución.
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

#include "frontend/dspad.h"
#include "frontend/layout.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "rsxkit/rsxkit.h"
#include "rsxkit/text.h"
#include "rsxkit/ui.h"

#ifndef SCREENS_VERSION
#define SCREENS_VERSION "desconocida"
#endif
#define SCREENS_TITLE "SalsaDS pantallas"
#define SCREENS_APPID "SLDS00004"
#define CFG_PATH SALSADS_DATA_DIR "/controles.cfg"

SYS_PROCESS_PARAM(1001, 0x100000);

#define TEX_W 256
#define TEX_H 192
#define TEX_PITCH 512
#define TEX_BYTES (TEX_PITCH * TEX_H)
#define UI_W 1280.0f
#define UI_H 720.0f
#define CLEAR_RGB 0x101010u
#define MAX_FRAMES 600
#define WARMUP 12

/* ---- Estado global ------------------------------------------------------- */

static volatile int quit;
static int autotest;
static int failures;

static struct sds_padcfg cfg;
static struct sds_input input;
static struct sds_keys keys;
static struct sds_ds3 ds3;
static int pad_present;

static uint8_t *tex_mem;            /* 1 MB alineado, mapeado al RSX */
static uint32_t tex_off;            /* offset RSX de tex_mem */
static uint32_t set_fence[2];       /* barrera del último cuadro que usó el juego de texturas */
static int set_used[2];
static uint32_t frame_no;

static uint8_t paint[TEX_W * TEX_H];  /* el «dibujo» de la pantalla táctil */
static int last_tx = -1, last_ty = -1;

static enum sds_layout layout = SDS_LAYOUT_SIDE;
static enum sds_scale scale_mode = SDS_SCALE_FIT;
static int res_1080;

/* ---- Utilidades ---------------------------------------------------------- */

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

/* Las esperas largas del RSX (el menú del sistema retiene los flips) siguen
 * atendiendo al sistema y permiten salir. */
static bool wait_hook(void *ctx, const rsxkit_wait_info *info)
{
	(void)ctx;
	(void)info;
	sysUtilCheckCallback();
	return !quit;
}

static uint16_t rgb5551(uint32_t rgb)
{
	return (uint16_t)((((rgb >> 19) & 31u) << 11) | (((rgb >> 11) & 31u) << 6) | (((rgb >> 3) & 31u) << 1) | 1u);
}

/* ---- Texturas de las dos pantallas ------------------------------------------ */

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

static void line_paint(int x0, int y0, int x1, int y1)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
	for (;;) {
		for (int oy = -1; oy <= 1; oy++)
			for (int ox = -1; ox <= 1; ox++) {
				const int px = x0 + ox, py = y0 + oy;
				if (px >= 0 && px < TEX_W && py >= 0 && py < TEX_H)
					paint[py * TEX_W + px] = 1;
			}
		if (x0 == x1 && y0 == y1)
			break;
		const int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static rsxkit_canvas canvas_of(uint16_t *px)
{
	rsxkit_canvas c = { px, TEX_W, TEX_H, TEX_PITCH, RSXKIT_RGBA5551 };
	return c;
}

static void key_label(const rsxkit_canvas *c, int x, int y, const char *txt, int down)
{
	if (down)
		rsxkit_fill(c, x - 1, y - 1, (int)strlen(txt) * 8 + 2, 15, 0xE0C000);
	rsxkit_text_draw(c, x, y, txt, 0, down ? 0x000000 : 0x8090A0, 1);
}

/* Pantalla de arriba: barras de color, un marcador que se mueve y los botones
 * de la DS que llegan. En la esquina inferior izquierda, un bloque rojo (para
 * comprobar la distribución). */
static void draw_top(uint16_t *px)
{
	for (int y = 0; y < TEX_H; y++) {
		uint16_t *row = px + y * (TEX_PITCH / 2);
		for (int x = 0; x < TEX_W; x++) {
			uint16_t v;
			if (y < 64) {
				const int bar = x >> 5;
				static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
								  0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
				v = rgb5551(bars[bar]);
			} else {
				const uint32_t g = (uint32_t)(x * 255 / (TEX_W - 1));
				v = rgb5551((g << 16) | ((255 - g) << 8) | (uint32_t)(40 + y / 2));
			}
			row[x] = v;
		}
	}
	const rsxkit_canvas c = canvas_of(px);
	const int mx = (int)((frame_no * 2u) & 255u);
	rsxkit_fill(&c, mx, 64, 2, TEX_H - 64, 0xFFFFFF);
	rsxkit_fill(&c, 0, 70, TEX_W, 62, 0x101820);
	rsxkit_text_draw(&c, 8, 72, "SALSADS  ARRIBA", 0, 0xFFFFFF, 1);
	char buf[40];
	snprintf(buf, sizeof buf, "cuadro %u", (unsigned)frame_no);
	rsxkit_text_draw(&c, 8, 86, buf, 0, 0x80FF80, 1);
	const uint16_t k = keys.keys;
	key_label(&c, 8, 104, "L", (k >> SDS_KEY_L) & 1);
	key_label(&c, 24, 104, "R", (k >> SDS_KEY_R) & 1);
	key_label(&c, 48, 104, "A", (k >> SDS_KEY_A) & 1);
	key_label(&c, 64, 104, "B", (k >> SDS_KEY_B) & 1);
	key_label(&c, 80, 104, "X", (k >> SDS_KEY_X) & 1);
	key_label(&c, 96, 104, "Y", (k >> SDS_KEY_Y) & 1);
	key_label(&c, 120, 104, "SEL", (k >> SDS_KEY_SELECT) & 1);
	key_label(&c, 152, 104, "STA", (k >> SDS_KEY_START) & 1);
	key_label(&c, 192, 104, "^", (k >> SDS_KEY_UP) & 1);
	key_label(&c, 204, 104, "v", (k >> SDS_KEY_DOWN) & 1);
	key_label(&c, 216, 104, "<", (k >> SDS_KEY_LEFT) & 1);
	key_label(&c, 228, 104, ">", (k >> SDS_KEY_RIGHT) & 1);
	rsxkit_fill(&c, 4, TEX_H - 12, 8, 8, 0xFF0000);  /* marca para comprobar la distribución */
}

/* Pantalla táctil: cuadrícula, el dibujo y las coordenadas del toque. */
static void draw_bottom(uint16_t *px)
{
	const uint16_t bg = rgb5551(0x101C30), grid = rgb5551(0x20304C), ink = rgb5551(0xFFFFFF);
	for (int y = 0; y < TEX_H; y++) {
		uint16_t *row = px + y * (TEX_PITCH / 2);
		for (int x = 0; x < TEX_W; x++)
			row[x] = paint[y * TEX_W + x] ? ink : ((x & 31) == 0 || (y & 31) == 0 ? grid : bg);
	}
	const rsxkit_canvas c = canvas_of(px);
	rsxkit_text_draw(&c, 8, 8, "PANTALLA TACTIL", 0, 0x80C0FF, 1);
	int tx, ty, down;
	sds_touch_state(&input, &tx, &ty, &down);
	char buf[48];
	snprintf(buf, sizeof buf, "toque %s  x=%3d y=%3d", down ? "SI" : "no", tx, ty);
	rsxkit_text_draw(&c, 8, 24, buf, 0, down ? 0xFFE000 : 0x7080A0, 1);
	rsxkit_fill(&c, 4, TEX_H - 12, 8, 8, 0x0000FF);  /* marca para comprobar la distribución */
}

/* Llena el juego de texturas `set` con el cuadro actual. */
static void fill_textures(int set)
{
	if (set_used[set])
		rsxkit_fence_wait(set_fence[set]);  /* el RSX terminó de leer el cuadro anterior de este juego */
	draw_top(tex_px(set, 0));
	draw_bottom(tex_px(set, 1));
}

/* ---- Dibujo -------------------------------------------------------------- */

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

static void draw_screens(int set, const struct sds_rect r[2], int out_w, int out_h)
{
	rsxkit_vertex *vb = vb_ptr(set);
	make_quad(vb, &r[0]);
	make_quad(vb + 6, &r[1]);
	rsxkit_bind_frame((float)out_w, (float)out_h);
	rsxkit_vertices(vb_offset(set), RSXKIT_MEM_MAIN);
	for (int s = 0; s < 2; s++) {
		rsxkit_draw d;
		memset(&d, 0, sizeof d);
		d.blend = RSXKIT_BLEND_OFF;
		d.cull = RSXKIT_CULL_NONE;
		d.ntex = 1;
		d.scissor[2] = (float)out_w;
		d.scissor[3] = (float)out_h;
		d.tex[0].offset = tex_offset(set, s);
		d.tex[0].pitch = TEX_PITCH;
		d.tex[0].width = TEX_W;
		d.tex[0].height = TEX_H;
		d.tex[0].location = RSXKIT_MEM_MAIN;
		d.tex[0].format = RSXKIT_RGBA5551;
		d.tex[0].wrap_s = d.tex[0].wrap_t = RSXKIT_WRAP_CLAMP;
		d.tex[0].filter = scale_mode == SDS_SCALE_INTEGER ? RSXKIT_FILTER_NEAREST : RSXKIT_FILTER_LINEAR;
		d.tex[0].xf[0] = d.tex[0].xf[1] = 1.0f;
		for (int c = 0; c < 4; c++)
			d.k[RSXKIT_K_T0][1][c] = 1.0f;  /* color = textura × color del vértice (blanco) */
		rsxkit_draw_triangles(&d, (uint32_t)s * 6, 6);
	}
}

typedef struct osd_info {
	uint32_t interval_us, wait_us, cpu_us;
	const char *phase;
} osd_info;

static void draw_pointer(const struct sds_rect *touch, float k)
{
	/* El puntero en la pantalla táctil, en coordenadas lógicas de la interfaz. */
	const float cx = ((float)touch->x + (input.x + 0.5f) * touch->scale) * k;
	const float cy = ((float)touch->y + (input.y + 0.5f) * touch->scale) * k;
	if (input.touching) {
		/* Tocando: cuadro lleno rojo con aro blanco. */
		rsxkit_ui_rect(cx - 9, cy - 9, 18, 18, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 7, cy - 7, 14, 14, 0xE02020FF);
	} else {
		/* Sin tocar: mira de líneas finas (blanca con borde oscuro), hueca. */
		rsxkit_ui_rect(cx - 14, cy - 2, 28, 4, 0x000000C0);
		rsxkit_ui_rect(cx - 2, cy - 14, 4, 28, 0x000000C0);
		rsxkit_ui_rect(cx - 13, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx + 2, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy - 13, 2, 11, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy + 2, 2, 11, 0xFFFFFFFF);
	}
}

static void draw_ui(const struct sds_rect r[2], int out_w, int out_h, const osd_info *o)
{
	const float k = UI_W / (float)out_w;
	rsxkit_ui_begin(UI_W, UI_H);
	draw_pointer(&r[1], k);
	char line[160];
	rsxkit_ui_rect(6, 6, 520, 96, 0x000000A0);
	snprintf(line, sizeof line, "%dp  %s  escala %s  %s", out_h, sds_layout_name(layout), sds_scale_name(scale_mode),
		 o->phase);
	rsxkit_ui_text(12, 10, line, 1.5f, 0xFFFFFFFF, 0);
	snprintf(line, sizeof line, "cuadro %u.%02u ms  espera %u.%02u  PPE %u.%02u", o->interval_us / 1000,
		 o->interval_us % 1000 / 10, o->wait_us / 1000, o->wait_us % 1000 / 10, o->cpu_us / 1000,
		 o->cpu_us % 1000 / 10);
	rsxkit_ui_text(12, 32, line, 1.5f, 0xC0FFC0FF, 0);
	sds_padcfg_summary(&cfg, line, sizeof line);
	rsxkit_ui_text(12, 54, line, 1.2f, 0xC0D0FFFF, 0);
	if (cfg.pointer == SDS_POINTER_GYRO)
		snprintf(line, sizeof line, "giro: %s  sesgo %d  (calibra %d/%d)", pad_present && ds3.sensors_ok ? "sensores" : "SIN SENSORES",
			 (int)input.gyro_bias, input.gyro_cal_n, SDS_GYRO_CAL_FRAMES);
	else if (cfg.pointer == SDS_POINTER_TILT)
		snprintf(line, sizeof line, "inclinacion: %s  roll %d pitch %d (grados, filtrado)",
			 pad_present && ds3.sensors_ok ? "sensores" : "SIN SENSORES", (int)(input.f_roll - input.c_roll),
			 (int)(input.f_pitch - input.c_pitch));
	else
		snprintf(line, sizeof line, "L1+R1: X modo puntero, O escala, TRI resolucion, CUA borra, START sale");
	rsxkit_ui_text(12, 74, line, 1.2f, 0xFFD0A0FF, 0);
	rsxkit_ui_end();
}

/* ---- Entrada --------------------------------------------------------------- */

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

/* Estadísticas del temblor y de los sensores, una ventana por segundo. */
static struct {
	uint64_t t0;
	uint32_t n;
	double sx, sy, sxx, syy;
	int still_ok;
	int amin[3], amax[3];
	int gmin, gmax;
	/* modo GIRO: ventana de 10 s */
	uint64_t t10;
	double g_sum, g_sum2;
	uint32_t g_n;
	int still10;
	int a10min[3], a10max[3];
	float drift0;
} jit;

static void jitter_reset(void)
{
	memset(&jit, 0, sizeof jit);
	jit.t0 = now_us();
	jit.still_ok = 1;
	for (int i = 0; i < 3; i++) {
		jit.amin[i] = 1024;
		jit.amax[i] = -1;
	}
	jit.gmin = 1024;
	jit.gmax = -1;
	jit.t10 = jit.t0;
	jit.still10 = 1;
	jit.drift0 = input.gyro_dx_net;
	for (int i = 0; i < 3; i++) {
		jit.a10min[i] = 1024;
		jit.a10max[i] = -1;
	}
}

static void jitter_sample(void)
{
	if (!pad_present)
		return;  /* los rangos de los sensores se registran en cualquier modo; el temblor solo vale en INCLINACION */
	/* Solo cuenta con el stick derecho quieto. */
	if (abs((int)ds3.rx - 128) > 20 || abs((int)ds3.ry - 128) > 20)
		jit.still_ok = 0;
	jit.n++;
	jit.sx += input.x;
	jit.sy += input.y;
	jit.sxx += (double)input.x * input.x;
	jit.syy += (double)input.y * input.y;
	for (int i = 0; i < 3; i++) {
		if ((int)ds3.accel[i] < jit.amin[i])
			jit.amin[i] = ds3.accel[i];
		if ((int)ds3.accel[i] > jit.amax[i])
			jit.amax[i] = ds3.accel[i];
	}
	if ((int)ds3.gyro < jit.gmin)
		jit.gmin = ds3.gyro;
	if ((int)ds3.gyro > jit.gmax)
		jit.gmax = ds3.gyro;
	if (cfg.pointer == SDS_POINTER_GYRO && pad_present && ds3.sensors_ok) {
		/* Ruido y deriva del giroscopio con el control quieto (10 s). */
		if (abs((int)ds3.rx - 128) > 20 || abs((int)ds3.ry - 128) > 20 || input.gyro_cal_n < SDS_GYRO_CAL_FRAMES)
			jit.still10 = 0;
		for (int i = 0; i < 3; i++) {
			if ((int)ds3.accel[i] < jit.a10min[i])
				jit.a10min[i] = ds3.accel[i];
			if ((int)ds3.accel[i] > jit.a10max[i])
				jit.a10max[i] = ds3.accel[i];
		}
		jit.g_n++;
		jit.g_sum += ds3.gyro;
		jit.g_sum2 += (double)ds3.gyro * ds3.gyro;
		if (now_us() - jit.t10 >= 10000000 && jit.g_n > 100) {
			const double m = jit.g_sum / jit.g_n, v = jit.g_sum2 / jit.g_n - m * m, sd = sqrt(v > 0 ? v : 0);
			int moved = 0;
			for (int i = 0; i < 3; i++)
				if (jit.a10max[i] - jit.a10min[i] > 8)
					moved = 1;
			const float drift = input.gyro_dx_net - jit.drift0;
			const int quiet = jit.still10 && !moved;
			salsads_log("giroscopio (10 s): sesgo calibrado %d.%02d, media %d.%02d, σ %u.%02u unidades, deriva horizontal %d px; "
				    "%s%s", (int)input.gyro_bias, (int)(fabsf(input.gyro_bias) * 100) % 100, (int)m,
				    (int)(fabs(m) * 100) % 100, (unsigned)sd, (unsigned)(sd * 100) % 100, (int)drift,
				    quiet ? "control quieto: " : "el control se movió, la deriva no vale: ",
				    quiet ? (fabsf(drift) <= 40.0f && sd <= 3.0 ? "giroscopio UTIL" : "giroscopio INSERVIBLE (ruido o deriva: quedarse con INCLINACION)") : "repetir quieto");
			jit.t10 = now_us();
			jit.g_n = 0;
			jit.g_sum = jit.g_sum2 = 0;
			jit.still10 = 1;
			jit.drift0 = input.gyro_dx_net;
			for (int i = 0; i < 3; i++) {
				jit.a10min[i] = 1024;
				jit.a10max[i] = -1;
			}
		}
	}
	if (now_us() - jit.t0 >= 1000000 && jit.n > 10) {
		const double mx = jit.sx / jit.n, my = jit.sy / jit.n;
		const double vx = jit.sxx / jit.n - mx * mx, vy = jit.syy / jit.n - my * my;
		const double sdx = sqrt(vx > 0 ? vx : 0), sdy = sqrt(vy > 0 ? vy : 0);
		salsads_log("sixaxis: acel X %d..%d Y %d..%d Z %d..%d giro %d..%d; ángulos crudos roll %d pitch %d, "
			    "filtrados roll %d pitch %d (grados); puntero (%d,%d); temblor σ = (%u.%02u, %u.%02u) px en %u "
			    "muestras%s",
			    jit.amin[0], jit.amax[0], jit.amin[1], jit.amax[1], jit.amin[2], jit.amax[2], jit.gmin, jit.gmax,
			    (int)input.raw_roll, (int)input.raw_pitch, (int)input.f_roll, (int)input.f_pitch, (int)input.x,
			    (int)input.y, (unsigned)sdx, (unsigned)(sdx * 100) % 100, (unsigned)sdy, (unsigned)(sdy * 100) % 100,
			    jit.n, cfg.pointer == SDS_POINTER_STICK ? " (modo stick: el temblor no aplica)" : jit.still_ok ? " (stick quieto: con el control sobre la mesa, mide el ruido)" : "");
		jitter_reset();
	}
}

static uint32_t combo_prev;

/* Lee el DS3, mapea y mueve el puntero. Devuelve las órdenes de la prueba
 * (L1 + R1 + botón): bit 0 modo del puntero, 1 escala, 2 resolución, 3 borrar,
 * 4 salir. */
static uint32_t poll_input(float dt)
{
	static struct sds_ds3 last;
	static int sensor_set;
	padInfo info;
	padData data;

	if (ioPadGetInfo(&info) == 0 && info.status[0]) {
		if (!pad_present) {
			salsads_log("mando 1 conectado");
			sensor_set = 0;
		}
		pad_present = 1;
		if (!sensor_set) {
			const s32 rc = ioPadSetSensorMode(0, PAD_SENSOR_MODE_ON);
			salsads_log("ioPadSetSensorMode(ON) = %d; modo actual %d", (int)rc, (int)ioPadInfoSensorMode(0));
			sensor_set = 1;
		}
		if (ioPadGetData(0, &data) == 0 && data.len > 0)
			convert_pad(&data, &last);
		ds3 = last;
	} else {
		pad_present = 0;
		ds3 = neutral_pad();
	}
	if (autotest) {
		/* Sin mando en RPCS3: el stick derecho da vueltas, R2 toca a ratos y
		 * L1, A y la cruceta cambian, para recorrer entrada → puntero → toque
		 * → dibujo → pantalla. */
		const float a = (float)frame_no * 0.12f;
		ds3.rx = (uint8_t)(128.0f + 110.0f * cosf(a));
		ds3.ry = (uint8_t)(128.0f + 110.0f * sinf(a));
		ds3.buttons = (((frame_no / 15u) & 1u) ? 0u : (1u << SDS_DS3_R2)) | (1u << SDS_DS3_CIRCLE) |
			      (((frame_no / 7u) & 1u) ? (1u << SDS_DS3_UP) : (1u << SDS_DS3_RIGHT));
		ds3.sensors_ok = 0;
	}
	sds_pad_map(&cfg, &input, &ds3, &keys);
	sds_pointer_update(&cfg, &input, &ds3, dt, &keys);
	if (keys.layout)
		layout = (enum sds_layout)((layout + 1) % SDS_LAYOUT_COUNT);

	/* Pintar con el toque. */
	int tx, ty, down;
	sds_touch_state(&input, &tx, &ty, &down);
	if (down) {
		line_paint(last_tx >= 0 ? last_tx : tx, last_ty >= 0 ? last_ty : ty, tx, ty);
		last_tx = tx;
		last_ty = ty;
	} else {
		last_tx = last_ty = -1;
	}
	jitter_sample();

	/* Órdenes de la prueba. */
	uint32_t cmd = 0;
	const uint32_t b = ds3.buttons;
	if ((b & (1u << SDS_DS3_L1)) && (b & (1u << SDS_DS3_R1))) {
		const uint32_t now = (((b >> SDS_DS3_CROSS) & 1u) << 0) | (((b >> SDS_DS3_CIRCLE) & 1u) << 1) |
				     (((b >> SDS_DS3_TRIANGLE) & 1u) << 2) | (((b >> SDS_DS3_SQUARE) & 1u) << 3) |
				     (((b >> SDS_DS3_START) & 1u) << 4);
		cmd = now & ~combo_prev;
		combo_prev = now;
	} else {
		combo_prev = 0;
	}
	return cmd;
}

/* ---- Un cuadro --------------------------------------------------------------- */

typedef struct frame_times {
	uint32_t interval_us, wait_us, cpu_us, vblanks;
} frame_times;

static uint64_t last_begin_us;
static uint64_t last_vblank;
static int cur_w, cur_h;

static int check_pixels(const struct sds_rect r[2]);
static void dump_back_buffer(void);

/* Dibuja un cuadro completo. `verify` lee píxeles del búfer trasero (solo en
 * la prueba automática: leerlo es lento). */
static uint32_t render_frame(const char *phase, frame_times *ft, int verify)
{
	sysUtilCheckCallback();  /* el menú del sistema (salir) se atiende en cada cuadro */
	const uint64_t t0 = now_us();
	rsxkit_frame_begin(CLEAR_RGB);
	const uint64_t t1 = now_us();
	ft->wait_us = (uint32_t)(t1 - t0);
	ft->interval_us = last_begin_us ? (uint32_t)(t1 - last_begin_us) : 0;
	last_begin_us = t1;
	const uint64_t vb = rsxkit_vblank_count();
	ft->vblanks = last_vblank ? (uint32_t)(vb - last_vblank) : 1;
	last_vblank = vb;

	float dt = ft->interval_us ? (float)ft->interval_us / 1e6f : 1.0f / 60.0f;
	if (dt < 1.0f / 120.0f)
		dt = 1.0f / 120.0f;
	if (dt > 1.0f / 20.0f)
		dt = 1.0f / 20.0f;
	const uint32_t cmd = poll_input(dt);

	const int set = (int)(frame_no & 1u);
	fill_textures(set);
	struct sds_rect r[2];
	sds_layout_compute(layout, scale_mode, cur_w, cur_h, r);
	draw_screens(set, r, cur_w, cur_h);
	osd_info o = { ft->interval_us, ft->wait_us, 0, phase };
	o.cpu_us = (uint32_t)(now_us() - t1);
	draw_ui(r, cur_w, cur_h, &o);
	if (verify && !check_pixels(r))
		failures++;
	if (verify)
		dump_back_buffer();
	set_fence[set] = rsxkit_fence();
	set_used[set] = 1;
	const uint64_t t2 = now_us();
	ft->cpu_us = (uint32_t)(t2 - t1);
	rsxkit_frame_end();
	frame_no++;
	return cmd;
}

/* Con el archivo dual-screen-dump, escribe el búfer trasero (X8R8G8B8, la
 * resolución actual) para verlo: tools/raw-png.py xrgb8888. */
static void dump_back_buffer(void)
{
	struct stat st;
	if (stat(SALSADS_DATA_DIR "/dual-screen-dump", &st) != 0)
		return;
	uint32_t pitch = 0;
	const uint32_t *bb = rsxkit_back_buffer(&pitch);
	if (!bb)
		return;
	char path[96];
	snprintf(path, sizeof path, SALSADS_DATA_DIR "/dual-screen-%dp-%d%c.xrgb8888", cur_h, (int)layout,
		 scale_mode == SDS_SCALE_INTEGER ? 'e' : 'f');
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	for (int y = 0; y < cur_h; y++)
		fwrite(bb + y * (pitch / 4), 4, (size_t)cur_w, f);
	fclose(f);
}

/* Lee el búfer trasero: la marca roja de la pantalla de arriba y la azul de la
 * táctil deben estar donde dice la distribución, y la esquina inferior
 * derecha (que ninguna cubre) debe ser el color de fondo. */
static int pixel_close(uint32_t got, uint32_t want)
{
	for (int s = 0; s < 24; s += 8) {
		const int a = (int)((got >> s) & 255), b = (int)((want >> s) & 255);
		if (abs(a - b) > 12)
			return 0;
	}
	return 1;
}

static int check_pixels(const struct sds_rect r[2])
{
	rsxkit_finish();
	uint32_t pitch = 0;
	const uint32_t *bb = rsxkit_back_buffer(&pitch);
	if (!bb) {
		salsads_log("FALLO no hay búfer trasero para comprobar píxeles");
		return 0;
	}
	int ok = 1;
	const uint32_t expect[2] = { 0xFF0000u, 0x0000FFu };
	for (int s = 0; s < 2; s++) {
		/* el centro del bloque de 8×8 de la esquina inferior izquierda de la textura */
		const int px = r[s].x + (int)(8.0f * r[s].scale), py = r[s].y + (int)((TEX_H - 8.0f) * r[s].scale);
		const uint32_t got = bb[py * (pitch / 4) + px] & 0xFFFFFFu;
		if (!pixel_close(got, expect[s])) {
			salsads_log("FALLO píxel de la pantalla %s en (%d,%d): 0x%06x, esperado 0x%06x (%s, %s)",
				    s ? "táctil" : "de arriba", px, py, got, expect[s], sds_layout_name(layout),
				    sds_scale_name(scale_mode));
			ok = 0;
		}
	}
	const int cx = cur_w - 3, cy = cur_h - 3;
	const uint32_t corner = bb[cy * (pitch / 4) + cx] & 0xFFFFFFu;
	if (!pixel_close(corner, CLEAR_RGB)) {
		salsads_log("FALLO la esquina libre (%d,%d) no es el fondo: 0x%06x (%s, %s)", cx, cy, corner,
			    sds_layout_name(layout), sds_scale_name(scale_mode));
		ok = 0;
	}
	return ok;
}

/* ---- Barrido ------------------------------------------------------------------ */

static void sort_u32(uint32_t *a, int n)
{
	for (int i = 1; i < n; i++) {
		uint32_t v = a[i];
		int j = i - 1;
		while (j >= 0 && a[j] > v) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = v;
	}
}

#define MS(us) (unsigned)((us) / 1000), (unsigned)((us) % 1000 / 10)

static uint32_t buf_iv[MAX_FRAMES], buf_wait[MAX_FRAMES], buf_cpu[MAX_FRAMES];

/* Un cuadro de la combinación actual; devuelve la orden de la prueba. */
static int sweep_phase(int frames, uint32_t *cmd_out)
{
	char phase[32];
	int n = 0, over20 = 0, skipped = 0;
	uint32_t worst = 0;
	snprintf(phase, sizeof phase, "barrido");
	*cmd_out = 0;
	for (int i = 0; i < frames + WARMUP && !quit; i++) {
		frame_times ft;
		const int verify = autotest && i == WARMUP - 2;  // dentro del calentamiento: leer el búfer trasero es lento
		const uint32_t cmd = render_frame(phase, &ft, verify);
		*cmd_out |= cmd;
		if (i < WARMUP)
			continue;
		buf_iv[n] = ft.interval_us;
		buf_wait[n] = ft.wait_us;
		buf_cpu[n] = ft.cpu_us;
		if (ft.interval_us > 20000)
			over20++;
		if (ft.interval_us > worst)
			worst = ft.interval_us;
		if (ft.vblanks > 1)
			skipped += (int)ft.vblanks - 1;
		n++;
	}
	if (!n)
		return -1;
	sort_u32(buf_iv, n);
	sort_u32(buf_wait, n);
	sort_u32(buf_cpu, n);
	const uint32_t med = buf_iv[n / 2], p95 = buf_iv[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1];
	/* 60 Hz estable: mediana a ±0,5 ms de 16,68 ms, ningún vblank saltado y ningún cuadro de más de 25 ms. */
	const int stable = med > 16180 && med < 17180 && skipped == 0 && worst < 25000;
	salsads_log("DATO pantallas %dp %-15s %-8s: intervalo mediana %u.%02u ms, p95 %u.%02u, máx %u.%02u; "
		    "> 20 ms: %d; vblanks saltados %d; espera del flip mediana %u.%02u ms; trabajo del PPE por cuadro "
		    "mediana %u.%02u ms, p95 %u.%02u; %d cuadros; 60 Hz estable: %s",
		    cur_h, sds_layout_name(layout), sds_scale_name(scale_mode), MS(med), MS(p95), MS(worst), over20, skipped,
		    MS(buf_wait[n / 2]), MS(buf_cpu[n / 2]), MS(buf_cpu[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1]), n,
		    stable ? "SÍ" : "NO");
	return stable;
}

static int set_resolution(int is_1080)
{
	const int32_t modes[1] = { is_1080 ? VIDEO_RESOLUTION_1080 : VIDEO_RESOLUTION_720 };
	rsxkit_config c;
	memset(&c, 0, sizeof c);
	c.modes = modes;
	c.num_modes = 1;
	const int rc = rsxkit_video_reconfigure(&c);
	if (rc != 0) {
		salsads_log("rsxkit_video_reconfigure(%s) = %d: esa resolución no está disponible", is_1080 ? "1080p" : "720p",
			    rc);
		return -1;
	}
	const rsxkit_screen *si = rsxkit_screen_info();
	cur_w = (int)si->width;
	cur_h = (int)si->height;
	last_begin_us = 0;
	last_vblank = 0;
	res_1080 = is_1080;
	salsads_log("salida %ux%u (aspecto %u/100)", (unsigned)si->width, (unsigned)si->height, (unsigned)(si->aspect * 100));
	return 0;
}

static void load_config(void)
{
	sds_padcfg_default(&cfg);
	FILE *f = fopen(CFG_PATH, "r");
	char text[4096];
	if (f) {
		const size_t n = fread(text, 1, sizeof text - 1, f);
		fclose(f);
		char err[96];
		const int bad = sds_padcfg_parse(&cfg, text, n, err, sizeof err);
		salsads_log("%s leído (%u bytes)%s%s", CFG_PATH, (unsigned)n, bad ? "; no entendí líneas: " : "", bad ? err : "");
	} else {
		/* Primera vez: se deja un archivo con los valores por omisión para
		 * editarlo (por FTP) y se comprueba que quedó escrito. */
		const int n = sds_padcfg_format(&cfg, text, sizeof text);
		f = n > 0 ? fopen(CFG_PATH, "w") : NULL;
		int ok = 0;
		if (f) {
			ok = fwrite(text, 1, (size_t)n, f) == (size_t)n;
			ok = fclose(f) == 0 && ok;
		}
		struct stat st;
		if (ok && stat(CFG_PATH, &st) == 0 && st.st_size == n)
			salsads_log("%s no existía: lo creé con los valores por omisión (%d bytes)", CFG_PATH, n);
		else
			salsads_log("no pude crear %s; sigo con los valores por omisión", CFG_PATH);
	}
	char sum[160];
	sds_padcfg_summary(&cfg, sum, sizeof sum);
	salsads_log("controles: %s", sum);
}

int main(void)
{
	char banner[160];
	snprintf(banner, sizeof banner, "dual-screen %s (%s, %s)", SCREENS_VERSION, SCREENS_TITLE, SCREENS_APPID);
	salsads_log_init_banner("dual-screen", banner);
	autotest = salsads_autotest();
	sysUtilRegisterCallback(0, sysutil_cb, NULL);

	load_config();
	sds_input_init(&input);
	memset(&keys, 0, sizeof keys);
	ds3 = neutral_pad();
	ioPadInit(7);

	if (rsxkit_init(NULL) != 0) {
		salsads_log("RESULTADO dual-screen: FALLO rsxkit_init");
		salsads_log_shutdown();
		return 1;
	}
	rsxkit_set_wait_hook(wait_hook, NULL, 100000, 100000);
	if (rsxkit_ui_init() != 0) {
		salsads_log("RESULTADO dual-screen: FALLO rsxkit_ui_init");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	tex_mem = memalign(1u << 20, 1u << 20);
	if (!tex_mem || rsxkit_map_main(tex_mem, 1u << 20, &tex_off) != 0) {
		salsads_log("RESULTADO dual-screen: FALLO memoria de texturas");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	memset(tex_mem, 0, 1u << 20);
	const rsxkit_screen *si = rsxkit_screen_info();
	cur_w = (int)si->width;
	cur_h = (int)si->height;
	res_1080 = cur_h >= 1000;
	salsads_log("salida inicial %dx%d; %d cuadros por combinación%s", cur_w, cur_h, autotest ? 12 : 180,
		    autotest ? " (modo de prueba automática: RPCS3 no mide)" : "");
	jitter_reset();

	/* Barrido: resolución × distribución × escala. */
	const int frames = autotest ? 12 : 180;
	int stable_n = 0, combos = 0;
	for (int rs = 0; rs < 2 && !quit; rs++) {
		const int want_1080 = rs == 1;
		if (res_1080 != want_1080 && set_resolution(want_1080) != 0)
			continue;
		for (int l = 0; l < SDS_LAYOUT_COUNT && !quit; l++) {
			for (int sc = 0; sc < SDS_SCALE_COUNT && !quit; sc++) {
				layout = (enum sds_layout)l;
				scale_mode = (enum sds_scale)sc;
				uint32_t cmd;
				const int st = sweep_phase(frames, &cmd);
				combos++;
				if (st > 0)
					stable_n++;
				if (cmd & 16u)
					quit = 1;
			}
		}
	}
	salsads_log("barrido: %d combinaciones, %d con 60 Hz estable", combos, stable_n);
	if (autotest) {
		/* La entrada sintética tocó y arrastró: el dibujo no puede estar vacío. */
		int painted = 0;
		for (int i = 0; i < TEX_W * TEX_H; i++)
			painted += paint[i];
		salsads_log("entrada sintética: %d píxeles dibujados con el toque", painted);
		if (painted < 50)
			fail("el toque sintético no dibujó nada");
	}
	if (!combos)
		fail("ninguna combinación se pudo medir");

	/* Interactivo (solo en la PS3 real). */
	if (!autotest && !quit) {
		layout = SDS_LAYOUT_SIDE;
		scale_mode = SDS_SCALE_FIT;
		last_begin_us = 0;
		salsads_log("modo interactivo: L2 cambia la distribución; L1+R1 con X/O/TRI/CUA/START: modo del puntero, "
			    "escala, resolución, borrar, salir");
		jitter_reset();
		while (!quit) {
			frame_times ft;
			const uint32_t cmd = render_frame("interactivo", &ft, 0);
			if (cmd & 1u) {
				cfg.pointer = (uint8_t)((cfg.pointer + 1) % 3);
				input.gyro_cal_n = 0;
				input.gyro_cal_sum = 0;
				input.tilt_init = 0;
				salsads_log("modo del puntero: %s", cfg.pointer == SDS_POINTER_GYRO ? "giro" : cfg.pointer == SDS_POINTER_TILT ? "inclinación" : "stick");
				jitter_reset();
			}
			if (cmd & 2u)
				scale_mode = scale_mode == SDS_SCALE_FIT ? SDS_SCALE_INTEGER : SDS_SCALE_FIT;
			if (cmd & 4u)
				set_resolution(!res_1080);
			if (cmd & 8u)
				memset(paint, 0, sizeof paint);
			if (cmd & 16u)
				quit = 1;
		}
	}

	salsads_log("RESULTADO dual-screen: %s", failures ? "FALLO" : "OK");
	if (!rsxkit_lost()) {
		rsxkit_finish();
		rsxkit_shutdown();
	}
	ioPadEnd();
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
