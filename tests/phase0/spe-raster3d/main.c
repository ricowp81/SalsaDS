/*
 * SalsaDS — Fase 0.4: rasterizador 3D en SPEs (PS3 real).
 *
 * Escena sintética (r3d_scene.c) de N = 500, 1000 y 2048 triángulos con
 * textura y color de vértice, z-buffer de 24 bits y la VRAM de texturas en
 * memoria principal, dibujada por 1 o 2 SPEs (r3d_core.c, en
 * spu_main.c) a 1×, 2× y 4× de la resolución de la DS, por bandas de líneas
 * o por tiles de 256 de ancho. Mide ms por cuadro (desde que el PPE publica
 * hasta que el último SPE termina, con el timebase) y, por SPE, el reparto
 * del tiempo (setup, listas, DMA, texturas, dibujo, salida).
 *
 * Cada configuración se verifica antes de medir contra la referencia en C
 * (r3d_ref.c) que corre aquí en el PPE: en modo id (qué triángulo queda en
 * cada píxel) tiene que ser idéntica; en color, casi (el SPU redondea la
 * simple precisión hacia cero); y todas las configuraciones de una misma
 * escena tienen que dar los mismos bits. Si algo no coincide, FALLO.
 *
 * Las líneas «DATO …» son las que se copian a docs/fase0.md y perf-log.md.
 * RPCS3 solo sirve para ver que arranca y que la imagen sale bien: sus
 * tiempos no valen.
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

#include <spukit/spukit.h>

#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "r3d.h"
#include "r3d_core.h"
#include "r3d_ref.h"
#include "r3d_scene.h"
#include "spu_raster_bin.h"

#ifndef R3D_VERSION
#define R3D_VERSION "desconocida"
#endif
#define R3D_TITLE "SalsaDS raster"
#define R3D_APPID "SLDS00002"
#define PROGRAM   "spe-raster3d"

SYS_PROCESS_PARAM(1001, 0x10000);

#define CLEAR_COLOR r3d_rgba(4, 8, 16, 0)
#define CLEAR_DEPTH 0xFFFFFFu
#define NPIX_MAX ((size_t)R3D_W1 * R3D_MAX_SCALE * R3D_H1 * R3D_MAX_SCALE)
/* Tolerancia del color contra la referencia: a lo más 0,5 % de píxeles
 * «lejos» y al menos 95 % idénticos (en el host: 0,04 % y 99,9 %). */
#define FAR_MAX_PERMIL 5u
#define EXACT_MIN_PCT  95u

extern unsigned r3d_io_mem_errors;

static double tb_hz;
static int failures;
static int reps = 9, warmup = 2;

static uint8_t *vram_tex, *vram_pal;
static struct r3d_tri *tris;
static uint32_t *frame, *ref_color, *ref_id, *ref_depth;
static uint8_t *scratch[2];
static struct r3d_texdef defs[R3D_SCENE_MAX_TEX];
static spukit_worker *workers[2];

static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static void fail(const char *what)
{
	failures++;
	salsads_log("FALLO %s", what);
}

/* Centésimas como «a.bc» sin printf de flotantes. */
static unsigned cent(double v)
{
	if (v < 0)
		v = 0;
	return (unsigned)(v * 100.0 + 0.5);
}
#define F2(v) cent(v) / 100, cent(v) % 100

static double ms(double ticks) { return ticks * 1000.0 / tb_hz; }

static void *alloc128(size_t n)
{
	void *p = memalign(128, n);
	if (!p) {
		salsads_log("FALLO sin memoria (%u bytes)", (unsigned)n);
		salsads_log_shutdown();
		exit(1);
	}
	memset(p, 0, n);
	return p;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Gira hasta que el SPE termine el trabajo `seq`, con tope: si en 3 s no
 * terminó, lo deja dicho y sale (sin esperar al SPE, que no responde). */
static void wait_done(uint32_t i, uint32_t seq)
{
	const uint64_t t0 = tb_now(), limit = (uint64_t)(3.0 * tb_hz);
	while (!spukit_done(workers[i], seq))
		if (tb_now() - t0 > limit) {
			salsads_log("FALLO el SPE%u no terminó el trabajo %u en 3 s", (unsigned)i, (unsigned)seq);
			salsads_log("RESULTADO " PROGRAM ": FALLO");
			salsads_log_shutdown();
			exit(1);
		}
}

/* ---- Configuraciones ---------------------------------------------------------- */

struct cfg {
	uint32_t spes, split, tile;
	const char *name;
};

static const struct cfg cfgs[] = {
	{ 1, R3D_SPLIT_INTERLEAVE, 0, "1spe banda" },
	{ 2, R3D_SPLIT_INTERLEAVE, 0, "2spe banda alternas" },
	{ 2, R3D_SPLIT_HALVES, 0, "2spe banda mitades" },
	{ 1, R3D_SPLIT_INTERLEAVE, 1, "1spe tile" },
	{ 2, R3D_SPLIT_INTERLEAVE, 1, "2spe tile alternas" },
	{ 2, R3D_SPLIT_HALVES, 1, "2spe tile mitades" },
};
#define NCFG (sizeof cfgs / sizeof cfgs[0])

static struct r3d_job make_job(uint32_t n, uint32_t scale, const struct cfg *c, uint32_t i, uint32_t flags)
{
	return (struct r3d_job){
		.tris_ea = (uintptr_t)tris, .vram_tex_ea = (uintptr_t)vram_tex, .vram_pal_ea = (uintptr_t)vram_pal,
		.frame_ea = (uintptr_t)frame, .scratch_ea = (uintptr_t)scratch[i],
		.vram_tex_size = R3D_VRAM_TEX_SIZE, .vram_pal_size = R3D_VRAM_PAL_SIZE, .ntris = n, .scale = scale,
		.unit_w = c->tile ? 256u : r3d_frame_w(scale), .spe_index = i, .spe_count = c->spes, .split = c->split,
		.flags = flags, .clear_color = CLEAR_COLOR, .clear_depth = CLEAR_DEPTH,
	};
}

/* Un cuadro: publica un trabajo por SPE, gira hasta que terminan todos y
 * devuelve los ticks de pared; deja las estadísticas de cada SPE en st. */
static uint64_t run_frame(uint32_t n, uint32_t scale, const struct cfg *c, uint32_t flags, struct r3d_stats st[2],
			  uint32_t spe_ticks[2], int *err)
{
	struct r3d_job j[2];
	uint32_t seq[2];
	for (uint32_t i = 0; i < c->spes; i++)
		j[i] = make_job(n, scale, c, i, flags);
	const uint64_t t0 = tb_now();
	for (uint32_t i = 0; i < c->spes; i++)
		seq[i] = spukit_post(workers[i], R3D_KIND_FRAME, &j[i], sizeof j[i]);
	for (uint32_t i = 0; i < c->spes; i++)
		wait_done(i, seq[i]);
	const uint64_t t1 = tb_now();
	*err = 0;
	for (uint32_t i = 0; i < c->spes; i++) {
		const spukit_result *r = spukit_wait(workers[i], seq[i]);
		memcpy(&st[i], r->data, sizeof st[i]);
		spe_ticks[i] = r->ticks;
		if (r->error)
			*err = r->error;
	}
	return t1 - t0;
}

/* ---- Verificación ---------------------------------------------------------------- */

/* Las funciones vectoriales con intrínsecos del SPU contra las genéricas del
 * PPE: lo entero igual; el recíproco (frest + fi + Newton), con su error. */
static void selftest(void)
{
	static struct r3d_selftest want __attribute__((aligned(16)));
	r3d_selftest(&want);
	for (uint32_t i = 0; i < 2; i++) {
		uint32_t seq = spukit_post(workers[i], R3D_KIND_SELFTEST, NULL, 0);
		wait_done(i, seq);
		const spukit_result *r = spukit_wait(workers[i], seq);
		struct r3d_selftest got;
		memcpy(&got, r->data, sizeof got);
		double worst = 0;
		for (int k = 0; k < 4; k++) {
			double e = (double)got.recip[k] / (double)want.recip[k] - 1.0;
			e = e < 0 ? -e : e;
			worst = e > worst ? e : worst;
		}
		const int ints_ok = !memcmp(got.mul16, want.mul16, sizeof got.mul16) &&
				    !memcmp(got.mul32x16, want.mul32x16, sizeof got.mul32x16) &&
				    !memcmp(got.bytes, want.bytes, sizeof got.bytes) &&
				    !memcmp(got.cmp, want.cmp, sizeof got.cmp) && !memcmp(got.cvt, want.cvt, sizeof got.cvt) &&
				    got.bits == want.bits;
		salsads_log("DATO autoprueba SPE%u: enteros %s; recíproco error relativo máx %u.%02u e-7 (frest + fi + "
			    "Newton)", i, ints_ok ? "iguales al PPE" : "DISTINTOS", F2(worst * 1e7));
		if (r->error || !ints_ok || worst > 1e-6)
			fail("autoprueba de r3d_vec.h en el SPE");
	}
}

static int verify_id(uint32_t n, uint32_t scale, const struct cfg *c)
{
	const uint32_t W = r3d_frame_w(scale), npix = W * r3d_frame_h(scale);
	struct r3d_stats st[2];
	uint32_t tk[2];
	int err;
	memset(frame, 0xA5, (size_t)npix * 4);
	run_frame(n, scale, c, R3D_FLAG_ID, st, tk, &err);
	uint32_t bad = 0, first = npix;
	for (uint32_t i = 0; i < npix; i++)
		if (frame[i] != ref_id[i] && !bad++)
			first = i;
	if (err || bad) {
		char msg[200];
		snprintf(msg, sizeof msg, "%ux N=%u %s modo id: error %d, %u píxeles distintos de la referencia "
			 "(el primero en %u,%u: %u en vez de %u)", scale, n, c->name, err, bad, first % W, first / W,
			 first < npix ? frame[first] : 0, first < npix ? ref_id[first] : 0);
		fail(msg);
		return 0;
	}
	return 1;
}

static int verify_color(uint32_t n, uint32_t scale, const struct cfg *c, uint64_t *hash0, struct r3d_cmp *cmp)
{
	const uint32_t npix = r3d_frame_w(scale) * r3d_frame_h(scale);
	r3d_compare(frame, ref_color, npix, cmp);
	const uint64_t h = r3d_hash(frame, npix);
	int ok = cmp->far * 1000u <= npix * FAR_MAX_PERMIL && cmp->exact * 100u >= npix * EXACT_MIN_PCT;
	char msg[200];
	if (!ok) {
		snprintf(msg, sizeof msg, "%ux N=%u %s color: %u idénticos, %u cerca, %u lejos de %u (máx %u, el primero "
			 "lejos en %u)", scale, n, c->name, cmp->exact, cmp->close, cmp->far, npix, cmp->maxdiff,
			 cmp->first_far);
		fail(msg);
	}
	if (!*hash0) {
		*hash0 = h;
	} else if (h != *hash0) {
		snprintf(msg, sizeof msg, "%ux N=%u %s: hash %08x%08x distinto del de la primera configuración", scale, n,
			 c->name, (unsigned)(h >> 32), (unsigned)h);
		fail(msg);
		ok = 0;
	}
	return ok;
}

/* ---- Medición --------------------------------------------------------------------- */

static void log_spe(const char *tag, uint32_t i, const struct r3d_stats *s, uint32_t ticks)
{
	salsads_log("DATO %s SPE%u: total %u.%02u ms = setup %u.%02u + listas %u.%02u + dma %u.%02u + texturas %u.%02u "
		    "+ dibujo %u.%02u + salida %u.%02u | tri %u, unidades %u, unidad·tri %u, líneas %u, grupos %u, "
		    "frags %u, fallos tex %u (%u KB)",
		    tag, i, F2(ms(ticks)), F2(ms(s->t_setup)), F2(ms(s->t_bin)), F2(ms(s->t_gather)), F2(ms(s->t_tex)),
		    F2(ms(s->t_raster)), F2(ms(s->t_out)), s->tris_mine, s->units, s->unit_tris, s->rows, s->quads,
		    s->frags, s->tex_miss, s->tex_bytes / 1024u);
}

static void measure(uint32_t n, uint32_t scale, const struct cfg *c, unsigned ntex, uint64_t *hash0)
{
	char tag[96];
	snprintf(tag, sizeof tag, "r3d %ux N=%u %utex %s", scale, n, ntex, c->name);
	if (!verify_id(n, scale, c))
		return;

	struct r3d_stats st[2], last[2];
	uint32_t tk[2], last_tk[2] = { 0, 0 };
	uint64_t wall[16];
	int err = 0;
	for (int r = 0; r < warmup; r++)
		run_frame(n, scale, c, 0, st, tk, &err);
	for (int r = 0; r < reps; r++) {
		memset(frame, 0xA5, (size_t)r3d_frame_w(scale) * r3d_frame_h(scale) * 4);
		wall[r] = run_frame(n, scale, c, 0, st, tk, &err);
		if (err)
			break;
		/* las estadísticas del cuadro mediano no se conocen hasta ordenar: se
		 * guardan las del último (varían < 1 % entre cuadros) */
		memcpy(last, st, sizeof last);
		memcpy(last_tk, tk, sizeof last_tk);
	}
	if (err) {
		char msg[200];
		snprintf(msg, sizeof msg, "%s: el SPE devolvió el error %d", tag, err);
		fail(msg);
		return;
	}
	struct r3d_cmp cmp;
	verify_color(n, scale, c, hash0, &cmp);
	qsort(wall, (size_t)reps, sizeof wall[0], cmp_u64);
	const uint32_t npix = r3d_frame_w(scale) * r3d_frame_h(scale);
	salsads_log("DATO %s: %u.%02u ms por cuadro (mín %u.%02u, máx %u.%02u; %d cuadros) | color: lejos %u.%02u %%, "
		    "idénticos %u.%02u %%",
		    tag, F2(ms((double)wall[reps / 2])), F2(ms((double)wall[0])), F2(ms((double)wall[reps - 1])), reps,
		    F2(100.0 * cmp.far / npix), F2(100.0 * cmp.exact / npix));
	for (uint32_t i = 0; i < c->spes; i++)
		log_spe(tag, i, &last[i], last_tk[i]);
}

/* El mismo núcleo en el PPE (vectores genéricos de GCC, sin AltiVec): cuánto
 * costaría sin SPEs. */
static void measure_ppe(uint32_t n, uint32_t scale)
{
	struct r3d_job j = make_job(n, scale, &cfgs[0], 0, 0);
	struct r3d_stats st;
	uint64_t t[3];
	int e = 0;
	for (int r = 0; r < 3; r++) {
		uint64_t t0 = tb_now();
		e |= r3d_frame(&j, &st);
		t[r] = tb_now() - t0;
	}
	qsort(t, 3, sizeof t[0], cmp_u64);
	struct r3d_cmp cmp;
	const uint32_t npix = r3d_frame_w(scale) * r3d_frame_h(scale);
	r3d_compare(frame, ref_color, npix, &cmp);
	if (e || r3d_io_mem_errors || cmp.far * 1000u > npix * FAR_MAX_PERMIL)
		fail("núcleo en el PPE: error, DMA mal alineado o imagen distinta de la referencia");
	salsads_log("DATO r3d %ux N=%u PPE (el mismo núcleo, escalar): %u.%02u ms por cuadro (mediana de 3) | setup "
		    "%u.%02u, dibujo %u.%02u, texturas %u.%02u ms | lejos %u.%02u %%",
		    scale, n, F2(ms((double)t[1])), F2(ms(st.t_setup)), F2(ms(st.t_raster)), F2(ms(st.t_tex)),
		    F2(100.0 * cmp.far / npix));
}

static void save_ppm(const char *name, const uint32_t *img, uint32_t w, uint32_t h)
{
	char path[96];
	snprintf(path, sizeof path, SALSADS_DATA_DIR "/%s", name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		salsads_log("no se pudo escribir %s", path);
		return;
	}
	static uint8_t line[R3D_W1 * R3D_MAX_SCALE * 3];
	fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint32_t p = img[y * w + x];
			line[3 * x] = (uint8_t)((p & 63u) << 2);
			line[3 * x + 1] = (uint8_t)(((p >> 8) & 63u) << 2);
			line[3 * x + 2] = (uint8_t)(((p >> 16) & 63u) << 2);
		}
		fwrite(line, 1, w * 3, f);
	}
	fclose(f);
	salsads_log("imagen %s (%ux%u)", path, (unsigned)w, (unsigned)h);
}

/* Referencia (color e id) de la escena que está en `tris`. */
static void reference(uint32_t n, uint32_t scale)
{
	struct r3d_ref_in in = { tris, n, vram_tex, R3D_VRAM_TEX_SIZE, vram_pal, R3D_VRAM_PAL_SIZE, scale,
				 CLEAR_COLOR, CLEAR_DEPTH, 1 };
	uint64_t t0 = tb_now();
	r3d_ref_render(&in, ref_id, ref_depth);
	in.idmode = 0;
	r3d_ref_render(&in, ref_color, ref_depth);
	salsads_log("referencia %ux N=%u en el PPE: %u.%02u ms (color e id)", scale, n, F2(ms((double)(tb_now() - t0))));
}

static void scene(uint32_t n, unsigned ntex)
{
	uint32_t used = r3d_scene_textures(vram_tex, vram_pal, ntex, defs);
	uint64_t area = r3d_scene_tris(tris, n, 0x5A1D5000u + n, defs, ntex);
	uint32_t ntr = 0;
	for (uint32_t i = 0; i < n; i++)
		ntr += (tris[i].attr & R3D_ATTR_TRANS) != 0;
	salsads_log("DATO escena N=%u: %u translúcidos, área total %u px² a 1× (cubre %u.%02u veces la pantalla), "
		    "%u texturas (%u KB de VRAM)",
		    n, ntr, (unsigned)area, F2((double)area / (R3D_W1 * R3D_H1)), ntex, used / 1024u);
}

static void check_data_dir(int existed)
{
	struct stat st;
	char path[96];
	if (stat(SALSADS_DATA_DIR, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fail("no existe la carpeta " SALSADS_DATA_DIR " (no se pudo crear)");
		return;
	}
	snprintf(path, sizeof path, SALSADS_DATA_DIR "/%s.log", PROGRAM);
	int log_ok = stat(path, &st) == 0;
	salsads_log("carpeta %s: %s; log %s: %s", SALSADS_DATA_DIR, existed ? "ya existía" : "creada ahora", path,
		    log_ok ? "escribiéndose" : "NO EXISTE");
	if (!log_ok)
		fail("no se creó el archivo de log");
}

int main(void)
{
	struct stat st0;
	const int existed = stat(SALSADS_DATA_DIR, &st0) == 0 && S_ISDIR(st0.st_mode);
	salsads_log_init_banner(PROGRAM, PROGRAM " " R3D_VERSION " (" R3D_TITLE ", " R3D_APPID ")");
	check_data_dir(existed);
	tb_hz = (double)sysGetTimebaseFrequency();
	if (salsads_autotest()) {
		reps = 1;
		warmup = 0;
	}
	salsads_log("timebase %u Hz; %d cuadros por medición tras %d de calentamiento; cifras: mediana, mínimo y "
		    "máximo; un SPE = un trabajo de spukit por cuadro", (unsigned)tb_hz, reps, warmup);

	vram_tex = alloc128(R3D_VRAM_TEX_SIZE + 64);
	vram_pal = alloc128(R3D_VRAM_PAL_SIZE + 64);
	tris = alloc128(R3D_MAX_TRIS * sizeof *tris);
	frame = alloc128(NPIX_MAX * 4);
	ref_color = alloc128(NPIX_MAX * 4);
	ref_id = alloc128(NPIX_MAX * 4);
	ref_depth = alloc128(NPIX_MAX * 4);
	scratch[0] = alloc128(R3D_MAX_TRIS * R3D_REC_SIZE);
	scratch[1] = alloc128(R3D_MAX_TRIS * R3D_REC_SIZE);

	int e = spukit_init(2);
	workers[0] = e ? NULL : spukit_worker_start(spu_raster_bin, "raster0", 0);
	workers[1] = workers[0] ? spukit_worker_start(spu_raster_bin, "raster1", 0) : NULL;
	if (!workers[0] || !workers[1]) {
		salsads_log("FALLO spukit: init %d, trabajador %d", e, spukit_last_error());
		salsads_log("RESULTADO " PROGRAM ": FALLO");
		salsads_log_shutdown();
		return 1;
	}

	selftest();

	static const uint32_t ns[] = { 500, 1000, 2048 };
	for (uint32_t scale = 1; scale <= 4; scale *= 2) {
		salsads_log("== %ux: %ux%u, unidades de %u píxeles (banda %ux%u, tile 256x%u)", scale, r3d_frame_w(scale),
			    r3d_frame_h(scale), R3D_UNIT_PIXELS, r3d_frame_w(scale), R3D_UNIT_PIXELS / r3d_frame_w(scale),
			    R3D_UNIT_PIXELS / 256u);
		for (size_t k = 0; k < sizeof ns / sizeof ns[0]; k++) {
			const uint32_t n = ns[k];
			uint64_t hash0 = 0;
			scene(n, 12);
			reference(n, scale);
			for (size_t c = 0; c < NCFG; c++) {
				if (cfgs[c].tile && scale == 1)
					continue;  /* a 1× la banda ya es un tile de 256 */
				measure(n, scale, &cfgs[c], 12, &hash0);
			}
			if (scale == 1 && n == 2048) {
				measure_ppe(n, scale);
				/* la imagen del SPE para mirarla */
				struct r3d_stats st[2];
				uint32_t tk[2];
				int err;
				run_frame(n, scale, &cfgs[1], 0, st, tk, &err);
				save_ppm(PROGRAM "-1x-2048.ppm", frame, R3D_W1, R3D_H1);
			}
		}
	}

	/* Más texturas de las que caben en la caché: cuánto cuestan los desalojos. */
	salsads_log("== caché de texturas desbordada: 24 texturas (176 KB decodificadas) en 112 KB");
	for (uint32_t scale = 1; scale <= 2; scale++) {
		uint64_t hash0 = 0;
		scene(2048, 24);
		reference(2048, scale);
		measure(2048, scale, &cfgs[0], 24, &hash0);
		measure(2048, scale, &cfgs[1], 24, &hash0);
	}

	spukit_stats ss;
	spukit_get_stats(workers[0], &ss);
	salsads_log("spukit SPE0: %u trabajos, %u timbres, %u despertares", ss.posted, ss.doorbells, ss.wakeups);
	int s0 = spukit_worker_stop(workers[0]), s1 = spukit_worker_stop(workers[1]);
	if (s0 || s1)
		fail("los trabajadores SPU no terminaron bien");

	salsads_log("RESULTADO " PROGRAM ": %s", failures ? "FALLO" : "OK");
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
