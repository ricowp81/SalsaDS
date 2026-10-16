/*
 * SalsaDS — Fase 0.5: composición 2D de las dos pantallas en un SPE.
 *
 * Para cada escena sintética (src/gpu2d/gpu2d_scene.c):
 *   1. el PPE dibuja la referencia (src/gpu2d/gpu2d_ref.c), píxel por píxel;
 *   2. un SPE compone las dos pantallas (4 fondos de texto + 128 OBJ cada
 *      una, VRAM por DMA, registro de cambios por línea) y el PPE compara las
 *      dos imágenes píxel por píxel;
 *   3. se repite el trabajo N veces y se reporta la duración medida por el
 *      propio SPE (decrementador, 79,8 MHz): mediana, mínimo y máximo, por
 *      pantalla y en total.
 * Después, escenas aleatorias (configuración, OAM y cambios al azar) solo
 * para verificar contra la referencia.
 *
 * Criterio de la Fase 0.5: < 4 ms por cuadro para las dos pantallas en un
 * SPE (en la PS3 real; RPCS3 solo verifica la imagen: sus tiempos no valen).
 * El RESULTADO final solo depende de que las imágenes coincidan.
 *
 * Con el archivo /dev_hdd0/salsads/gpu2d-dump, además escribe las imágenes
 * de cada escena (RGBA5551, 256×192, 2 bytes por píxel) para verlas.
 */
#include <malloc.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>

#include "gpu2d/gpu2d.h"
#include "job.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "spukit/spukit.h"
#include "spu_gpu2d_bin.h"

#ifndef GPU2D_VERSION
#define GPU2D_VERSION "desconocida"
#endif
#define GPU2D_TITLE "SalsaDS 2D"
#define GPU2D_APPID "SLDS00003"
#define GPU2D_DUMP_FLAG SALSADS_DATA_DIR "/gpu2d-dump"

SYS_PROCESS_PARAM(1001, 0x10000);

#define IMG_PIXELS (GPU2D_W * GPU2D_H)
#define MAX_FRAMES 256
#define CRITERION_US 4000

static int failures;

typedef struct screen {
	gpu2d_scene_mem mem;
	uint16_t *out;   /* lo escribe el SPE */
	uint16_t *ref;   /* lo escribe el PPE */
} screen;

static screen scr[2];
static gpu2d_frame *frame;

static void *amem(size_t n)
{
	void *p = memalign(128, (n + 127) & ~(size_t)127);
	if (p)
		memset(p, 0, n);
	return p;
}

static int alloc_screens(void)
{
	frame = amem(sizeof *frame);
	for (int e = 0; e < 2; e++) {
		screen *s = &scr[e];
		s->mem.bg_size = gpu2d_scene_bg_size(e);
		s->mem.obj_size = gpu2d_scene_obj_size(e);
		s->mem.chg_cap = GPU2D_SCENE_CHG_CAP;
		s->mem.bg_vram = amem(s->mem.bg_size);
		s->mem.obj_vram = amem(s->mem.obj_size);
		s->mem.pal = amem(1024);
		s->mem.oam = amem(1024);
		s->mem.chg = amem(sizeof(gpu2d_change) * s->mem.chg_cap);
		s->out = amem(IMG_PIXELS * 2);
		s->ref = amem(IMG_PIXELS * 2);
		if (!s->mem.bg_vram || !s->mem.obj_vram || !s->mem.pal || !s->mem.oam || !s->mem.chg || !s->out || !s->ref)
			return -1;
	}
	return frame ? 0 : -1;
}

static const char *const scene_name[GPU2D_SCENE_COUNT] = { "tipica", "pesada", "extrema", "aleatoria" };

static void build_scene(int scene, uint32_t seed)
{
	for (int e = 0; e < 2; e++) {
		gpu2d_scene_build(&frame->eng[e], &scr[e].mem, e, scene, seed);
		frame->eng[e].out_ea = (uint64_t)(uintptr_t)scr[e].out;
		memset(scr[e].out, 0xA5, IMG_PIXELS * 2);
	}
}

static uint32_t us_of(uint32_t ticks, uint64_t hz)
{
	return (uint32_t)((uint64_t)ticks * 1000000ull / hz);
}
/* Microsegundos como «a.bc» ms. */
#define MS(us) (unsigned)((us) / 1000), (unsigned)((us) % 1000 / 10)

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

/* Devuelve 1 si alguna de las dos pantallas difiere de la referencia. */
static int compare(const char *tag)
{
	int bad = 0;
	for (int e = 0; e < 2; e++) {
		int diffs = 0, first = -1;
		for (int i = 0; i < IMG_PIXELS; i++) {
			if (scr[e].out[i] != scr[e].ref[i]) {
				if (first < 0)
					first = i;
				diffs++;
			}
		}
		if (diffs) {
			salsads_log("FALLO %s pantalla %c: %d píxeles distintos; primero en (%d,%d): SPE 0x%04x, ref 0x%04x",
				    tag, 'A' + e, diffs, first % GPU2D_W, first / GPU2D_W, scr[e].out[first],
				    scr[e].ref[first]);
			bad = 1;
		}
	}
	if (bad)
		failures++;
	return bad;
}

static void dump_images(const char *name)
{
	struct stat st;
	if (stat(GPU2D_DUMP_FLAG, &st) != 0)
		return;
	for (int e = 0; e < 2; e++) {
		char path[96];
		snprintf(path, sizeof path, SALSADS_DATA_DIR "/gpu2d-%s-%c.rgba5551", name, 'A' + e);
		FILE *f = fopen(path, "wb");
		if (!f) {
			salsads_log("no pude escribir %s", path);
			continue;
		}
		fwrite(scr[e].out, 2, IMG_PIXELS, f);
		fclose(f);
	}
}

static int run_job(spukit_worker *w, job_result *res, uint32_t *wall_us)
{
	job_compose jc = { (uint64_t)(uintptr_t)frame, 3 };
	const uint64_t t0 = sysGetSystemTime();
	const uint32_t seq = spukit_post(w, JOB_COMPOSE, &jc, sizeof jc);
	const spukit_result *r = spukit_wait(w, seq);
	*wall_us = (uint32_t)(sysGetSystemTime() - t0);
	if (r->error != 0) {
		salsads_log("FALLO el SPE devolvió error %d", r->error);
		return -1;
	}
	memcpy(res, r->data, sizeof *res);
	for (int e = 0; e < 2; e++) {
		if (res->rc[e] != 0) {
			salsads_log("FALLO gpu2d_compose_engine(pantalla %c) = %d", 'A' + e, res->rc[e]);
			return -1;
		}
	}
	return 0;
}

static void perf_scene(spukit_worker *w, int scene, int frames)
{
	static uint32_t tk_all[MAX_FRAMES], tk_a[MAX_FRAMES], tk_b[MAX_FRAMES], wall[MAX_FRAMES], us[MAX_FRAMES];
	const uint64_t hz = spukit_tick_hz();
	job_result res;
	uint32_t wall_us;

	build_scene(scene, 1);
	const uint64_t t0 = sysGetSystemTime();
	for (int e = 0; e < 2; e++)
		gpu2d_ref_engine(&frame->eng[e], scr[e].ref);
	const uint32_t ref_us = (uint32_t)(sysGetSystemTime() - t0);

	if (run_job(w, &res, &wall_us) != 0) {
		failures++;
		return;
	}
	char tag[48];
	snprintf(tag, sizeof tag, "escena %s", scene_name[scene]);
	const int bad = compare(tag);
	dump_images(scene_name[scene]);
	if (bad)
		return;
	if (res.ctx_bytes)
		salsads_log("gpu2d_ctx del SPE: %u bytes", res.ctx_bytes);

	int n = 0;
	for (; n < frames; n++) {
		if (run_job(w, &res, &wall_us) != 0) {
			failures++;
			return;
		}
		tk_a[n] = res.ticks[0];
		tk_b[n] = res.ticks[1];
		tk_all[n] = res.ticks[0] + res.ticks[1];
		wall[n] = wall_us;
	}
	compare(tag);  /* después de N cuadros sigue igual */
	if (n == 0)
		return;

	for (int i = 0; i < n; i++)
		us[i] = us_of(tk_all[i], hz);
	sort_u32(us, n);
	const uint32_t med = us[n / 2], mn = us[0], mx = us[n - 1];
	for (int i = 0; i < n; i++)
		us[i] = us_of(tk_a[i], hz);
	sort_u32(us, n);
	const uint32_t med_a = us[n / 2];
	for (int i = 0; i < n; i++)
		us[i] = us_of(tk_b[i], hz);
	sort_u32(us, n);
	const uint32_t med_b = us[n / 2];
	sort_u32(wall, n);
	salsads_log("DATO gpu2d %-8s SPE por cuadro (A+B): mediana %u.%02u ms, mín %u.%02u, máx %u.%02u "
		    "[A %u.%02u, B %u.%02u]; PPE (post→wait) mediana %u.%02u ms; %d cuadros; criterio < 4 ms: %s",
		    scene_name[scene], MS(med), MS(mn), MS(mx), MS(med_a), MS(med_b), MS(wall[n / 2]), n,
		    med < CRITERION_US ? "SÍ" : "NO");
	salsads_log("DATO gpu2d %-8s contadores A: filas de teselas %u, esperas por el mapa %u, filas de OBJ %u, "
		    "OBJ-línea dibujados %u, cambios %u; B: %u, %u, %u, %u, %u", scene_name[scene],
		    res.stats[0].bg_fetches, res.stats[0].map_waits, res.stats[0].obj_fetches, res.stats[0].obj_drawn,
		    res.stats[0].chg_applied, res.stats[1].bg_fetches, res.stats[1].map_waits, res.stats[1].obj_fetches,
		    res.stats[1].obj_drawn, res.stats[1].chg_applied);
	salsads_log("DATO gpu2d %-8s referencia en el PPE (A+B, píxel por píxel): %u.%02u ms", scene_name[scene],
		    MS(ref_us));
}

static void verify_random(spukit_worker *w, int seeds)
{
	job_result res;
	uint32_t wall_us;
	int bad = 0;
	for (int seed = 1; seed <= seeds; seed++) {
		build_scene(GPU2D_SCENE_ALEATORIA, (uint32_t)seed);
		for (int e = 0; e < 2; e++)
			gpu2d_ref_engine(&frame->eng[e], scr[e].ref);
		char tag[48];
		snprintf(tag, sizeof tag, "aleatoria semilla %d", seed);
		if (run_job(w, &res, &wall_us) != 0) {
			failures++;
			return;
		}
		bad += compare(tag);
	}
	salsads_log("aleatorias: %d semillas, %d con diferencias", seeds, bad);
}

int main(void)
{
	char banner[160];
	snprintf(banner, sizeof banner, "spe-gpu2d %s (%s, %s)", GPU2D_VERSION, GPU2D_TITLE, GPU2D_APPID);
	salsads_log_init_banner("spe-gpu2d", banner);
	const int autotest = salsads_autotest();
	const int frames = autotest ? 3 : 120;
	const int seeds = autotest ? 6 : 24;

	if (alloc_screens() != 0) {
		salsads_log("RESULTADO spe-gpu2d: FALLO sin memoria");
		salsads_log_shutdown();
		return 1;
	}
	if (spukit_init(1) != 0) {
		salsads_log("RESULTADO spe-gpu2d: FALLO spukit_init");
		salsads_log_shutdown();
		return 1;
	}
	spukit_worker *w = spukit_worker_start(spu_gpu2d_bin, "gpu2d", 0);
	if (!w) {
		salsads_log("RESULTADO spe-gpu2d: FALLO spukit_worker_start (%d)", spukit_last_error());
		salsads_log_shutdown();
		return 1;
	}
	salsads_log("decrementador del SPE %u Hz; %d cuadros por escena%s", (unsigned)spukit_tick_hz(), frames,
		    autotest ? " (modo de prueba automática: los tiempos de RPCS3 no valen)" : "");

	for (int s = GPU2D_SCENE_TIPICA; s < GPU2D_SCENE_ALEATORIA; s++)
		perf_scene(w, s, frames);
	verify_random(w, seeds);

	const int rc = spukit_worker_stop(w);
	if (rc != 0)
		salsads_log("el SPE salió con %d", rc);
	salsads_log("RESULTADO spe-gpu2d: %s", failures ? "FALLO" : "OK");
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
