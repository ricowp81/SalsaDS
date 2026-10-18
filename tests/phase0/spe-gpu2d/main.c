/*
 * SalsaDS — «SalsaDS 2D» (Fase 0.5, completado en la 2.1): composición 2D de las dos pantallas, una por SPE.
 *
 * Para cada escena sintética (src/gpu2d/gpu2d_scene.c):
 *   1. el PPE dibuja la referencia (src/gpu2d/gpu2d_ref.c), píxel por píxel;
 *   2. dos SPE componen las dos pantallas (A en uno, B en otro; fondos de texto, afines y extendidos, OBJ de todas las
 *      clases, ventanas, mezclas, mosaico, paletas extendidas, VRAM por DMA, registro de cambios por línea) y el PPE
 *      compara las dos imágenes píxel por píxel;
 *   3. se repite el trabajo N veces y se reporta la duración medida por el propio SPE (decrementador, 79,8 MHz):
 *      mediana, mínimo y máximo, por pantalla; y el tiempo de pared de las dos juntas.
 * Después, escenas aleatorias (configuración, OAM, paginas de VRAM y cambios al azar) solo para verificar contra la
 * referencia.
 *
 * El RESULTADO final solo depende de que las imágenes coincidan (los tiempos de RPCS3 no valen).
 *
 * Con el archivo /dev_hdd0/salsads/gpu2d-dump, además escribe las imágenes de cada escena (RGBA5551, 256×192, 2 bytes
 * por píxel) para verlas.
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
#include "gpu2d/gpu2d_scene.h"
#include "spu/gpu2d/job.h"
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

static int failures;
static gpu2d_scene_mem mem;
static gpu2d_frame *frame;
static uint16_t *out[2], *ref[2];
static spukit_worker *wk[2];

static void *amem(size_t n)
{
	void *p = memalign(128, (n + 255) & ~(size_t)127);

	if (p)
		memset(p, 0, n);
	return p;
}

static int alloc_all(void)
{
	frame = amem(sizeof *frame);
	mem.vram = amem(G2D_VRAM_SIZE + G2D_VRAM_ZERO);
	mem.mmem = amem(GPU2D_H * 512);
	for (int e = 0; e < 2; e++) {
		mem.pal[e] = amem(1024);
		mem.oam[e] = amem(1024);
		mem.chg[e] = amem(sizeof(gpu2d_change) * (GPU2D_SCENE_CHG_CAP + 2));
		out[e] = amem(IMG_PIXELS * 2);
		ref[e] = amem(IMG_PIXELS * 2);
		if (!mem.pal[e] || !mem.oam[e] || !mem.chg[e] || !out[e] || !ref[e])
			return -1;
	}
	return frame && mem.vram && mem.mmem ? 0 : -1;
}

static const char *const scene_name[GPU2D_SCENE_COUNT] = { "tipica", "pesada", "extrema", "afin", "efectos", "aleatoria" };

static void build_scene(int scene, uint32_t seed)
{
	gpu2d_scene_build(frame, &mem, scene, seed);
	for (int e = 0; e < 2; e++) {
		frame->eng[e].out_ea = (uint64_t)(uintptr_t)out[e];
		memset(out[e], 0xA5, IMG_PIXELS * 2);
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
			if (out[e][i] != ref[e][i]) {
				if (first < 0)
					first = i;
				diffs++;
			}
		}
		if (diffs) {
			salsads_log("FALLO %s pantalla %c: %d píxeles distintos; primero en (%d,%d): SPE 0x%04x, ref 0x%04x", tag,
				    'A' + e, diffs, first % GPU2D_W, first / GPU2D_W, out[e][first], ref[e][first]);
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
		FILE *f;

		snprintf(path, sizeof path, SALSADS_DATA_DIR "/gpu2d-%s-%c.rgba5551", name, 'A' + e);
		f = fopen(path, "wb");
		if (!f) {
			salsads_log("no pude escribir %s", path);
			continue;
		}
		fwrite(out[e], 2, IMG_PIXELS, f);
		fclose(f);
	}
}

/* Publica la pantalla A en un SPE y la B en el otro, y espera a las dos. */
static int run_job(job_result res[2], uint32_t *wall_us)
{
	uint32_t seq[2];
	const uint64_t t0 = sysGetSystemTime();

	for (int e = 0; e < 2; e++) {
		job_compose jc = { (uint64_t)(uintptr_t)frame, (uint32_t)e };

		seq[e] = spukit_post(wk[e], JOB_COMPOSE, &jc, sizeof jc);
	}
	for (int e = 0; e < 2; e++) {
		const spukit_result *r = spukit_wait(wk[e], seq[e]);

		if (r->error != 0) {
			salsads_log("FALLO el SPE %d devolvió error %d", e, r->error);
			return -1;
		}
		memcpy(&res[e], r->data, sizeof res[e]);
		if (res[e].rc != 0) {
			salsads_log("FALLO gpu2d_compose_engine(pantalla %c) = %d", 'A' + e, res[e].rc);
			return -1;
		}
	}
	*wall_us = (uint32_t)(sysGetSystemTime() - t0);
	return 0;
}

static void perf_scene(int scene, int frames)
{
	static uint32_t tk[2][MAX_FRAMES], wall[MAX_FRAMES], us[MAX_FRAMES];
	const uint64_t hz = spukit_tick_hz();
	job_result res[2];
	uint32_t wall_us;
	uint64_t t0;
	uint32_t ref_us;
	char tag[48];
	int n = 0, bad;

	build_scene(scene, 1);
	t0 = sysGetSystemTime();
	for (int e = 0; e < 2; e++)
		gpu2d_ref_engine(&frame->eng[e], ref[e]);
	ref_us = (uint32_t)(sysGetSystemTime() - t0);

	if (run_job(res, &wall_us) != 0) {
		failures++;
		return;
	}
	snprintf(tag, sizeof tag, "escena %s", scene_name[scene]);
	bad = compare(tag);
	dump_images(scene_name[scene]);
	if (bad)
		return;
	if (res[0].ctx_bytes)
		salsads_log("gpu2d_ctx del SPE: %u bytes", res[0].ctx_bytes);

	for (; n < frames; n++) {
		if (run_job(res, &wall_us) != 0) {
			failures++;
			return;
		}
		tk[0][n] = res[0].ticks;
		tk[1][n] = res[1].ticks;
		wall[n] = wall_us;
	}
	compare(tag); /* después de N cuadros sigue igual */
	if (n == 0)
		return;
	{
		uint32_t med[2], mn[2], mx[2];

		for (int e = 0; e < 2; e++) {
			for (int i = 0; i < n; i++)
				us[i] = us_of(tk[e][i], hz);
			sort_u32(us, n);
			med[e] = us[n / 2];
			mn[e] = us[0];
			mx[e] = us[n - 1];
		}
		sort_u32(wall, n);
		salsads_log("DATO gpu2d %-8s SPE por pantalla: A mediana %u.%02u ms (mín %u.%02u, máx %u.%02u), B mediana %u.%02u ms "
			    "(mín %u.%02u, máx %u.%02u); las dos en paralelo, de pared: mediana %u.%02u ms; %d cuadros",
			    scene_name[scene], MS(med[0]), MS(mn[0]), MS(mx[0]), MS(med[1]), MS(mn[1]), MS(mx[1]), MS(wall[n / 2]), n);
	}
	salsads_log("DATO gpu2d %-8s contadores A: filas de teselas %u, esperas por el mapa %u, filas de OBJ %u, OBJ-línea %u, "
		    "cambios %u, líneas afines %u, bloques afines %u, vaciados de OBJ %u, líneas con efectos %u; B: %u, %u, %u, %u, %u, %u, %u, %u, %u",
		    scene_name[scene], res[0].stats.bg_fetches, res[0].stats.map_waits, res[0].stats.obj_fetches,
		    res[0].stats.obj_drawn, res[0].stats.chg_applied, res[0].stats.aff_lines, res[0].stats.aff_quads,
		    res[0].stats.obj_flushes, res[0].stats.fx_lines, res[1].stats.bg_fetches, res[1].stats.map_waits,
		    res[1].stats.obj_fetches, res[1].stats.obj_drawn, res[1].stats.chg_applied, res[1].stats.aff_lines,
		    res[1].stats.aff_quads, res[1].stats.obj_flushes, res[1].stats.fx_lines);
	salsads_log("DATO gpu2d %-8s referencia en el PPE (A+B, píxel por píxel): %u.%02u ms", scene_name[scene], MS(ref_us));
}

static void verify_random(int seeds)
{
	job_result res[2];
	uint32_t wall_us;
	int bad = 0, skipped = 0;

	for (int seed = 1; seed <= seeds; seed++) {
		char tag[48];

		build_scene(GPU2D_SCENE_ALEATORIA, (uint32_t)seed);
		for (int e = 0; e < 2; e++)
			gpu2d_ref_engine(&frame->eng[e], ref[e]);
		snprintf(tag, sizeof tag, "aleatoria semilla %d", seed);
		if (run_job(res, &wall_us) != 0) {
			failures++;
			return;
		}
		if (res[0].stats.map_dropped || res[1].stats.map_dropped) { /* mapas que no caben juntos: límite conocido */
			skipped++;
			continue;
		}
		bad += compare(tag);
	}
	salsads_log("aleatorias: %d semillas, %d con diferencias, %d omitidas por mapas que no caben juntos", seeds, bad, skipped);
}

int main(void)
{
	char banner[160];
	int autotest, frames, seeds;

	snprintf(banner, sizeof banner, "spe-gpu2d %s (%s, %s)", GPU2D_VERSION, GPU2D_TITLE, GPU2D_APPID);
	salsads_log_init_banner("spe-gpu2d", banner);
	autotest = salsads_autotest();
	frames = autotest ? 3 : 120;
	seeds = autotest ? 6 : 24;

	if (alloc_all() != 0) {
		salsads_log("RESULTADO spe-gpu2d: FALLO sin memoria");
		salsads_log_shutdown();
		return 1;
	}
	if (spukit_init(2) != 0) {
		salsads_log("RESULTADO spe-gpu2d: FALLO spukit_init");
		salsads_log_shutdown();
		return 1;
	}
	for (int e = 0; e < 2; e++) {
		wk[e] = spukit_worker_start(spu_gpu2d_bin, e ? "gpu2d-b" : "gpu2d-a", 0);
		if (!wk[e]) {
			salsads_log("RESULTADO spe-gpu2d: FALLO spukit_worker_start (%d)", spukit_last_error());
			salsads_log_shutdown();
			return 1;
		}
	}
	salsads_log("decrementador del SPE %u Hz; %d cuadros por escena%s", (unsigned)spukit_tick_hz(), frames,
		    autotest ? " (modo de prueba automática: los tiempos de RPCS3 no valen)" : "");

	for (int s = 0; s < GPU2D_SCENE_FIJAS; s++)
		perf_scene(s, frames);
	verify_random(seeds);

	for (int e = 0; e < 2; e++) {
		const int rc = spukit_worker_stop(wk[e]);

		if (rc != 0)
			salsads_log("el SPE %d salió con %d", e, rc);
	}
	salsads_log("RESULTADO spe-gpu2d: %s", failures ? "FALLO" : "OK");
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
