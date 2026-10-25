/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — corre una ROM de prueba (tests/roms) en la máquina de src/core y lee su veredicto del bloque de
 * resultados (tests/roms/src/common/sds.h), como hace tools/melonds-run.py con melonDS.
 *
 *   salsads-cpurun [--wav FILE.wav] [--snd-original] [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] [--keys HEX] [--touch X,Y]
 *                  [--screens FILE.ppm] [--ref] [--vs-ref] [--expect] [--same-ab] [--unmapped] [--peek ADDR]... [--trace N [--trace7]] ROM.nds
 *
 * Código de salida: 0 = OK, 1 = FALLO, 2 = sin veredicto, 3 = una CPU cayó en un vector de excepción de la BIOS
 * sin manejador. Con --frames corre ese número de cuadros y termina con 0 (para sacar imágenes).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "core/nds.h"
#include "gpu3d/gx_loop.h"
#include "host/jitrun.h"
#include "jit/arec.h"
#include "util/endian.h"

#define SDS_ADDR 0x02380000u
#define SDS_ARM7_ADDR 0x02390000u
#define SDS_MAGIC 0x52534453u

static struct nds M;
static struct nds R; /* --lockstep: la referencia con el intérprete */
static int lockstep, jit_mode; /* jit_mode: 0 intérprete, 1 rápido, 2 estricto */

static void run_frame(void)
{
	if (lockstep)
		hj_lockstep_frame();
	else
		nds_run_frame(&M);
}

static void run_cycles(uint64_t k)
{
	if (lockstep)
		hj_lockstep_cycles(k);
	else
		nds_run(&M, k);
}

static void set_input(uint32_t keys, unsigned x, unsigned y)
{
	nds_set_input(&M, keys, x, y);
	if (lockstep)
		nds_set_input(&R, keys, x, y);
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* ROM por fragmentos (--rom-paged): lectura del archivo bajo demanda. */
static int file_rd(void *user, uint64_t off, uint8_t *dst, size_t len)
{
	FILE *f = user;

	if (fseek(f, (long)off, SEEK_SET) != 0)
		return -1;
	return (int)fread(dst, 1, len, f);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t clock_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* --r3d-aplazar L: simula el aplazamiento de la espera del 3D (docs/fase3.md, 3.9) con un «renderizador» propio que no hace nada en
 * segundo plano: al enviar el trabajo (línea 0) lo rasteriza ya a un búfer de referencia, y lo vuelve a rasterizar de verdad al
 * esperarlo (línea L del cuadro siguiente); si la imagen difiere, el cuadro de geometría o la instantánea de la VRAM que lee el
 * trabajo cambiaron entre las dos líneas (el aplazamiento no es seguro). */
static struct {
	int set;
	r3d_job job;
} fk_q[4];
static unsigned fk_n;
static uint32_t fk_truth[2][R3D_W * R3D_H] __attribute__((aligned(128)));
static r3d_ctx fk_ctx;
static uint32_t fk_checked, fk_bad, fk_maxq;

static void fk_render(const r3d_job *j)
{
	for (uint32_t u = 0; u < j->nunits; u++) {
		r3d_job jj = *j;
		r3d_stats st;

		jj.unit = u;
		if (r3d_core_render(&fk_ctx, &jj, &st) != 0)
			fk_bad += 1000000u;
	}
}

static void fk_submit3d(void *ctx, int set, r3d_job *j)
{
	r3d_job t = *j;

	(void)ctx;
	t.out_ea = (uint64_t)(uintptr_t)fk_truth[set];
	fk_render(&t);
	if (fk_n == 4)
		return;
	fk_q[fk_n].set = set;
	fk_q[fk_n].job = *j;
	fk_n++;
	if (fk_n > fk_maxq)
		fk_maxq = fk_n;
}

static void fk_wait3d(void *ctx, int set)
{
	int found = set < 0;

	(void)ctx;
	for (unsigned i = 0; i < fk_n && !found; i++)
		found = fk_q[i].set == set;
	while (fk_n && found) {
		const int s = fk_q[0].set;

		fk_render(&fk_q[0].job);
		fk_checked++;
		if (memcmp((const void *)(uintptr_t)fk_q[0].job.out_ea, fk_truth[s], sizeof fk_truth[s]) != 0)
			fk_bad++;
		memmove(&fk_q[0], &fk_q[1], (fk_n - 1u) * sizeof fk_q[0]);
		fk_n--;
		if (s == set)
			break;
	}
}

static void fk_submit2d(void *ctx, int set, gpu2d_frame *f)
{
	(void)ctx;
	(void)set;
	(void)f;
}

/* Motor de geometría (Fase 3.1): resumen por cuadro y, con --gx-dump, el último cuadro entregado en texto. */
/* Render 3D (Fase 3.2): tiempo y contadores por cuadro rasterizado */
static int r3d_defer_sim;
static void print_r3d_stats(void)
{
	const struct nds_video *v = &M.vid;
	const r3d_stats *s = &v->r3d_sum;
	const double n = v->r3d_frames ? (double)v->r3d_frames : 1.0;

	printf("render 3D: %u cuadros rasterizados (%s, bandas de %u líneas, %u unidades); %.3f ms por cuadro en el host\n",
	       v->r3d_frames, v->use_ref3d ? "referencia" : "código del SPE", v->r3d_band, v->r3d_units,
	       v->clock ? (double)v->r3d_ticks / n / 1e6 : 0.0);
	if (!v->use_ref3d)
		printf("render 3D por cuadro: %.0f polígonos, %.0f banda·polígono, %.0f líneas, %.0f píxeles, %.0f escritos; texeles %.0f "
		       "(sin decodificar %.2f %%), %.1f bloques o celdas decodificados, %.1f desalojos, %.1f líneas de VRAM (de paletas %.0f); DMA %.0f B\n",
		       s->polys / n, s->band_polys / n, s->rows / n, s->pixels / n, s->written / n, s->tex_px / n,
		       s->tex_px ? 100.0 * (1.0 - (double)(s->tex_misses - s->xdr_hits) / (double)s->tex_px) : 100.0, (s->tex_misses - s->xdr_hits) / n,
		       s->tex_evict / n, s->line_miss / n, s->pal_miss / n, s->dma_bytes / n);
	printf("XDR por cuadro: aciertos %.1f, DMA %.1f, decodificadas %.1f\n", s->xdr_hits / n, s->xdr_dma / n, (s->tex_misses - s->xdr_hits) / n);
	printf("Local por cuadro: aciertos %.1f (%.2f %%), lecturas XDR %.1f\n",
	       (s->tex_px - s->tex_misses) / n, s->tex_px ? 100.0 * (1.0 - (double)s->tex_misses / s->tex_px) : 100.0,
	       s->xdr_dma ? s->tex_misses / n : 0.0);
	if (r3d_defer_sim)
		printf("APLAZAR: línea %u, %u trabajos verificados, %u distintos del cuadro de línea 0, máximo %u en vuelo: %s\n",
		       v->r3d_defer_line, fk_checked, fk_bad, fk_maxq, fk_bad ? "NO ES SEGURO" : "seguro");
	if (v->vs_ref3d)
		printf("REF3D: %s (%u de %u cuadros distintos)\n", v->r3d_diff_frames ? "DISTINTO" : "IGUAL", v->r3d_diff_frames,
		       v->r3d_frames);
	if (r3d_io_errors)
		printf("render 3D: %u transferencias que el MFC rechazaría\n", r3d_io_errors);
#ifdef R3D_VERIFY_DECODE
	{
		extern uint32_t r3d_decode_errors;

		printf("render 3D: bloques de texturas contra r3d_decode: %u diferencias\n", r3d_decode_errors);
	}
#endif
}

/* La capa 3D del último cuadro como PPM (lo transparente, magenta) */
static void save_img3d(const char *path)
{
	FILE *f = fopen(path, "wb");
	const int set = M.vid.last;

	if (!f || set < 0) {
		if (f)
			fclose(f);
		return;
	}
	fprintf(f, "P6\n256 192\n255\n");
	for (int i = 0; i < R3D_W * R3D_H; i++) {
		const uint32_t p = M.vid.set[set].img3d[i];
		uint8_t rgb[3] = { 255, 0, 255 };

		if (R3D_PX_A(p)) {
			rgb[0] = (uint8_t)(R3D_PX_R(p) * 255 / 63);
			rgb[1] = (uint8_t)(R3D_PX_G(p) * 255 / 63);
			rgb[2] = (uint8_t)(R3D_PX_B(p) * 255 / 63);
		}
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* Agujeros de la capa 3D: píxeles transparentes con vecinos opacos a los dos lados (izquierda y derecha, o arriba y abajo) */
static uint32_t holes_total, holes_frames, holes_worst, holes_worst_frame;

static uint32_t count_holes(void)
{
	const int set = M.vid.last;
	uint32_t n = 0;

	if (set < 0)
		return 0;
	for (int y = 1; y < R3D_H - 1; y++)
		for (int x = 1; x < R3D_W - 1; x++) {
			const uint32_t *p = &M.vid.set[set].img3d[y * R3D_W + x];

			if (R3D_PX_A(*p))
				continue;
			if ((R3D_PX_A(p[-1]) && R3D_PX_A(p[1])) || (R3D_PX_A(p[-R3D_W]) && R3D_PX_A(p[R3D_W])))
				n++;
		}
	return n;
}

static void print_gx_stats(uint64_t frames)
{
	const gx_stats *s = gx_stats_get(&M.gx);
	const double nf = frames ? (double)frames : 1.0;

	printf("geometría: %llu SWAP_BUFFERS en %llu cuadros; por cuadro entregado %.1f polígonos y %.1f vértices (máximos %u y %u); "
	       "%llu órdenes, %llu polígonos, %llu cara, %llu recortados, %llu fuera, %llu sin sitio (polígonos), %llu sin sitio (vértices), "
	       "%llu escrituras perdidas\n",
	       (unsigned long long)s->swaps, (unsigned long long)frames,
	       s->swaps ? (double)s->frame_polys / (double)s->swaps : 0.0, s->swaps ? (double)s->frame_verts / (double)s->swaps : 0.0,
	       s->max_polys, s->max_verts, (unsigned long long)s->cmds, (unsigned long long)s->polys, (unsigned long long)s->culled,
	       (unsigned long long)s->clipped, (unsigned long long)s->rejected, (unsigned long long)s->dropped_poly,
	       (unsigned long long)s->dropped_vert, (unsigned long long)s->fifo_overflow);
	printf("geometría: rechazados: %llu fuera de un plano, %llu cruzan el plano lejano, %llu vacíos tras recortar\n",
	       (unsigned long long)s->rej_outside, (unsigned long long)s->rej_far, (unsigned long long)s->rej_empty);
	printf("geometría: motor ocupado %.1f%% del tiempo (%.0f ciclos del motor por cuadro); en el host %.1f us por cuadro (%.1f us por cuadro entregado)\n",
	       100.0 * (double)s->cycles / ((double)NDS_FRAME_CYCLES * nf), (double)s->cycles / 2.0 / nf,
	       (double)s->host_ticks / 1000.0 / nf, s->swaps ? (double)s->host_ticks / 1000.0 / (double)s->swaps : 0.0);
}

/* --gx-hash: FNV-1a de cada cuadro de geometría entregado (vértices y polígonos usados, registros, parámetros), para comparar
 * dos ejecuciones (p. ej. modo directo y diferido) o dos versiones del motor. */
static uint64_t gxh_all, gxh_n, gxh_last;
static int gxh_on;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n--)
		h = (h ^ *b++) * 1099511628211ull;
	return h;
}

static void gx_hash_poll(void)
{
	const gx_frame *f;
	uint64_t h = 1469598103934665603ull;

	if (!gxh_on || M.gx_frames == gxh_last || !(f = nds_gx_front(&M)))
		return;
	gxh_last = M.gx_frames;
	h = fnv(h, f->vtx, (size_t)f->nverts * sizeof f->vtx[0]);
	h = fnv(h, f->poly, (size_t)f->npolys * sizeof f->poly[0]);
	h = fnv(h, &f->npolys, 4 * 5);
	h = fnv(h, &f->regs, sizeof f->regs);
	gxh_all = (gxh_all ^ h) * 1099511628211ull;
	gxh_n++;
}

static void gx_cmds(void)
{
	printf("geometría, órdenes:");
	for (unsigned c = 0; c < 0x80; c++)
		if (M.gx.st.by_cmd[c])
			printf(" %02x=%llu", c, (unsigned long long)M.gx.st.by_cmd[c]);
	printf("\n");
}

static void gx_dump(const char *path)
{
	const gx_frame *f = nds_gx_front(&M);
	FILE *o = fopen(path, "w");

	if (!o || !f) {
		fprintf(stderr, "--gx-dump: no hay cuadro de geometría entregado\n");
		if (o)
			fclose(o);
		return;
	}
	fprintf(o, "cuadro %u: %u polígonos, %u vértices, swap %u\n", f->seq, f->npolys, f->nverts, f->swap_params);
	for (uint32_t i = 0; i < f->npolys; i++) {
		const gx_poly *p = &f->poly[i];

		fprintf(o, "poly %u attr=%08x tex=%08x pltt=%04x n=%u flags=%02x wshift=%u y=%d..%d\n", i, p->attr, p->texparam, p->pltt,
			p->nverts, p->flags, p->wshift, p->ymin, p->ymax);
		for (unsigned k = 0; k < p->nverts; k++) {
			const gx_vertex *v = &f->vtx[p->vidx[k]];

			fprintf(o, "  v %5d %5d z=%06x w=%d st=%d,%d rgb=%u,%u,%u\n", v->sx, v->sy, v->z, v->w, v->s, v->t, v->r, v->g, v->b);
		}
	}
	fclose(o);
}

/* Salida de audio a 48 kHz (estéreo): se junta en memoria y se escribe como WAV de 16 bits al terminar. */
static float *wav_buf;
static size_t wav_n, wav_cap;
static float wav_peak;

static void wav_sink(void *ctx, const float *out, uint32_t n, const int16_t *nat, uint32_t nticks)
{
	(void)ctx;
	(void)nat;
	(void)nticks;
	if (wav_n + n > wav_cap) {
		const size_t cap = wav_cap ? wav_cap * 2 : (size_t)1 << 20;
		float *nb = realloc(wav_buf, cap * 2 * sizeof(float));

		if (!nb)
			return;
		wav_buf = nb;
		wav_cap = cap;
	}
	memcpy(wav_buf + 2 * wav_n, out, (size_t)n * 2 * sizeof(float));
	for (uint32_t i = 0; i < 2 * n; i++) {
		const float a = out[i] < 0 ? -out[i] : out[i];

		if (a > wav_peak)
			wav_peak = a;
	}
	wav_n += n;
}

static void wav_write(const char *path)
{
	FILE *f = fopen(path, "wb");
	uint8_t h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
			  0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a' };
	const uint32_t data = (uint32_t)(wav_n * 4), rate = 48000;

	if (!f)
		return;
	for (int i = 0; i < 4; i++) {
		h[4 + i] = (uint8_t)((36 + data) >> (8 * i));
		h[24 + i] = (uint8_t)(rate >> (8 * i));
		h[28 + i] = (uint8_t)((rate * 4) >> (8 * i));
		h[40 + i] = (uint8_t)(data >> (8 * i));
	}
	fwrite(h, 1, sizeof h, f);
	for (size_t i = 0; i < wav_n * 2; i++) {
		float v = wav_buf[i] * 32768.0f;
		int32_t q = (int32_t)(v < 0 ? v - 0.5f : v + 0.5f);
		uint8_t b[2];

		q = q > 32767 ? 32767 : q < -32768 ? -32768 : q;
		b[0] = (uint8_t)q;
		b[1] = (uint8_t)(q >> 8);
		fwrite(b, 1, 2, f);
	}
	fclose(f);
	fprintf(stderr, "audio: %zu muestras estéreo (%.2f s) en %s, pico %.1f dBFS\n", wav_n, (double)wav_n / 48000.0, path,
		wav_peak > 0 ? 20.0 * log10((double)wav_peak) : -99.0);
}

static void log_to_stderr(void *ctx, const char *msg)
{
	(void)ctx;
	fprintf(stderr, "núcleo: %s\n", msg);
}

/* Las dos pantallas, una sobre la otra, en un PPM de 256×384. */
static void save_ppm(const char *path)
{
	static uint16_t top[NDS_SCREEN_W * NDS_SCREEN_H], bot[NDS_SCREEN_W * NDS_SCREEN_H];
	FILE *f = fopen(path, "wb");

	if (!f)
		return;
	nds_screens(&M, top, bot);
	fprintf(f, "P6\n256 384\n255\n");
	for (int s = 0; s < 2; s++)
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++) {
			const uint32_t v = (s ? bot : top)[i];
			const uint8_t px[3] = { (uint8_t)((v & 31) * 255 / 31), (uint8_t)(((v >> 5) & 31) * 255 / 31),
						(uint8_t)(((v >> 10) & 31) * 255 / 31) };

			fwrite(px, 1, 3, f);
		}
	fclose(f);
}

/* Lista de píxeles esperados que dejan las ROMs de gráficos (tests/roms/src/common/sds.h: sds_expect_pixel). Se comprueban
 * contra la imagen de cada motor del último cuadro. Devuelve el número de fallos (y los imprime). */
static int check_expect(void)
{
	const uint32_t n = nds_peek32(&M, 0, 0x02380804u);
	int bad = 0;

	if (nds_peek32(&M, 0, 0x02380800u) != 0x45505847u) {
		printf("EXPECT: la ROM no dejó lista de píxeles esperados\n");
		return 1;
	}
	nds_video_render_host(&M);
	for (uint32_t i = 0; i < n && i < 1000; i++) {
		const uint32_t a = nds_peek32(&M, 0, 0x02380808u + 8 * i), b = nds_peek32(&M, 0, 0x0238080Cu + 8 * i);
		const int scr = (int)(a & 1), x = (int)(a >> 16), y = (int)(b & 0xFFFF);
		const uint16_t want = gpu2d_to_5551((uint16_t)(b >> 16)), got = M.vid.img[scr][y * NDS_SCREEN_W + x];

		if (got != want) {
			if (bad < 20)
				printf("EXPECT fallo: motor %c (%d,%d): obtuvo 0x%04x, esperaba 0x%04x\n", 'A' + scr, x, y, got, want);
			bad++;
		}
	}
	printf("EXPECT: %u comprobaciones, %d fallos\n", n, bad);
	return bad;
}

/* Las dos pantallas iguales (las ROMs de gráficos montan la misma escena en los dos motores) */
static int check_same_ab(void)
{
	int diffs = 0, first = -1;

	nds_video_render_host(&M);
	for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
		if (M.vid.img[0][i] != M.vid.img[1][i]) {
			if (first < 0)
				first = i;
			diffs++;
		}
	if (diffs)
		printf("A==B: FALLO, %d píxeles distintos (primero en %d,%d: A 0x%04x, B 0x%04x)\n", diffs, first % NDS_SCREEN_W,
		       first / NDS_SCREEN_W, M.vid.img[0][first], M.vid.img[1][first]);
	else
		printf("A==B: OK\n");
	return diffs != 0;
}

/* El compositor (el del SPE, aquí en escalar) contra el renderizador de referencia, píxel por píxel, en los dos motores.
 * Si los mapas de los fondos afines no cupieron en la arena (el compositor no los dibuja), la comparación no vale. */
static int check_vs_ref(void)
{
	static uint16_t refimg[NDS_SCREEN_W * NDS_SCREEN_H];
	int set, diffs = 0, first = -1, fe = 0;
	const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

	if (!fr || M.vid.use_ref) {
		printf("REF: nada que comparar\n");
		return 0;
	}
	nds_video_render_host(&M);
	for (int e = 0; e < 2; e++) {
		gpu2d_ref_engine(&fr->eng[e], refimg);
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
			if (refimg[i] != M.vid.img[e][i]) {
				if (first < 0) {
					first = i;
					fe = e;
				}
				diffs++;
			}
	}
	if (diffs)
		printf("REF: FALLO, %d píxeles distintos (primero en motor %c %d,%d)\n", diffs, 'A' + fe, first % NDS_SCREEN_W,
		       first / NDS_SCREEN_W);
	else
		printf("REF: IGUAL\n");
	return diffs != 0;
}

/* Si el juego escribió en su memoria de guardado y hay archivo, lo deja actualizado. */
static void save_out(const char *path)
{
	size_t len;
	const uint8_t *d;
	FILE *f;

	if (!path || !nds_save_take_dirty(&M))
		return;
	d = nds_save_data(&M, &len);
	f = fopen(path, "wb");
	if (f) {
		fwrite(d, 1, len, f);
		fclose(f);
		fprintf(stderr, "núcleo: guardado escrito en %s (%zu bytes)\n", path, len);
	}
}

/* Registros que deciden la imagen, en el mismo formato que `tools/melonds-run.py --regs` (para compararlos). */
static void print_regs(void)
{
	printf("regs DISPCNT_A=%08x DISPCNT_B=%08x BRIGHT_A=%04x BRIGHT_B=%04x POWCNT1=%04x\n", nds_peek32(&M, 0, 0x04000000),
	       nds_peek32(&M, 0, 0x04001000), nds_peek32(&M, 0, 0x0400006C) & 0xFFFF, nds_peek32(&M, 0, 0x0400106C) & 0xFFFF,
	       nds_peek32(&M, 0, 0x04000304) & 0xFFFF);
	printf("regs VRAMCNT=");
	for (uint32_t a = 0x04000240; a <= 0x04000249; a++)
		printf("%02x", nds_peek8(&M, 0, a));
	printf("\n");
}

/* ¿Una CPU quedó atrapada en un vector de excepción de la BIOS (sin manejador)? */
static int stuck_in_vector(const struct nds *n, const char **which, uint32_t *pc)
{
	const uint32_t p9 = acore_pc(&n->a9), p7 = acore_pc(&n->a7);

	if ((p9 & 0xFFFFFFE0u) == 0xFFFF0000u && (p9 & 0x1Fu) != 0x18) {
		*which = "ARM9";
		*pc = p9;
		return 1;
	}
	if (p7 < 0x20 && (p7 & 0x1Fu) != 0x18) {
		*which = "ARM7";
		*pc = p7;
		return 1;
	}
	return 0;
}

/* --state-hash: un resumen del estado de la máquina (registros y contadores de las dos CPUs, RAM, WRAM, VRAM, paleta, OAM y la
 * imagen de las dos pantallas): dos corridas con el mismo hash dejaron la misma máquina (intérprete contra recompilador). */
static void print_state_hash(struct nds *n)
{
	static uint16_t top[NDS_SCREEN_W * NDS_SCREEN_H], bot[NDS_SCREEN_W * NDS_SCREEN_H];
	uint64_t hc = 0xCBF29CE484222325ull, hm = hc, hs = hc;
	const struct acore *cs[2] = { &n->a9, &n->a7 };

	for (int k = 0; k < 2; k++) {
		hc = fnv(hc, cs[k]->r, sizeof cs[k]->r);
		hc = fnv(hc, &cs[k]->cpsr, sizeof cs[k]->cpsr);
		hc = fnv(hc, &cs[k]->cycles, sizeof cs[k]->cycles);
		hc = fnv(hc, &cs[k]->icount, sizeof cs[k]->icount);
	}
	hm = fnv(hm, n->ram, sizeof n->ram);
	hm = fnv(hm, n->swram, sizeof n->swram);
	hm = fnv(hm, n->wram7, sizeof n->wram7);
	hm = fnv(hm, n->vram, sizeof n->vram);
	hm = fnv(hm, n->pal, sizeof n->pal);
	hm = fnv(hm, n->oam, sizeof n->oam);
	nds_screens(n, top, bot);
	hs = fnv(hs, top, sizeof top);
	hs = fnv(hs, bot, sizeof bot);
	printf("state-hash: cpus %016llx memoria %016llx pantallas %016llx; cuadros %llu, ARM9 %llu instr., ARM7 %llu instr.\n",
	       (unsigned long long)hc, (unsigned long long)hm, (unsigned long long)hs, (unsigned long long)n->frames,
	       (unsigned long long)n->a9.icount, (unsigned long long)n->a7.icount);
}

int main(int argc, char **argv)
{
	uint64_t max_cycles = 4000000000ull;
	uint32_t keys = 0, tx = 0, ty = 0, quantum = 0;
	const char *rom_path = NULL, *screens_path = NULL;
	int stats = 0, show_arm7 = 0, trace_cpu = 0;
	uint64_t trace_n = 0, frames = 0, skip_frames = 0, hist_n = 0, ring_n = 0;
	uint32_t break_pc = 0, dbg = 0, watch_addr = 0, save_size = 0;
	const char *save_cfg = NULL;
	const char *save_path = NULL;
	struct { uint32_t addr, len; const char *file; } dumps[8];
	uint32_t ndumps = 0;
	int dump_cpu = 0;
	uint32_t trace_from = 0;
	const char *wav_path = NULL;
	int snd_original = 0, g2d_stats = 0;
	const char *gxdump_path = NULL;
	int gx_clock = 0, r3d_stats_on = 0, holes_on = 0, gx_defer = 0, gx_lento = 0, geom_plain = 0, geom_scalar = 0;
	const char *holes_dump = NULL, *fdump_prefix = NULL;
	uint64_t fdump_frame = 0;
	const char *img3d_path = NULL;
	struct { uint64_t frame; uint32_t keys, x, y; } press[512];
	struct { uint64_t frame; const char *file; } shot[32];
	uint32_t npress = 0, nshot = 0;
	int watch_cpu = 0;
	uint64_t watch_max = 0;
	int break_cpu = -1;
	uint32_t peek[8], npeek = 0;
	int show_regs = 0, use_ref = 0, vs_ref = 0, expect = 0, same_ab = 0, unmapped = 0;
	size_t len;
	uint8_t *rom;
	double t0, t1;
	int rc_gfx = 0;
	int jit_nolink = 0, jit_far = 0, jit_nojl = 0, jit_arm7 = 1, jit_arm9 = 1, state_hash = 0;
	const char *jit_prof = NULL; /* --jit-prof PREFIJO: traza de bloques para tools/jit-perfil.py */
	unsigned pg_shift = 18;           /* --rom-paged PAGE_KB,CACHE_KB: fuerza fragmentos (por omisión 256 KB y 32 MB) */
	size_t pg_cache = (size_t)32 << 20, whole_max = (size_t)64 << 20; /* --rom-whole-max MB */
	int force_paged = 0;
	FILE *romf = NULL;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--wav") && i + 1 < argc)
			wav_path = argv[++i];
		else if (!strcmp(argv[i], "--g2d-stats"))
			g2d_stats = 1;
		else if (!strcmp(argv[i], "--geometria-lectura-escalar"))
			geom_scalar = 1;
		else if (!strcmp(argv[i], "--geometria-sin-reuso"))
			geom_plain = 1;
		else if (!strcmp(argv[i], "--gx-lento"))
			gx_lento = 1; /* sin las rutas cortas de la FIFO de geometría (ráfagas de la DMA y escrituras de la CPU): la forma anterior */
		else if (!strcmp(argv[i], "--gx-defer"))
			gx_defer = 1; /* motor de geometría en modo diferido con el ejecutor de prueba (mismos bits que el directo) */
		else if (!strcmp(argv[i], "--gx-hash"))
			gxh_on = 1;
		else if (!strcmp(argv[i], "--gx-clock"))
			gx_clock = 1; /* mide el tiempo del motor de geometría con el reloj del host (el reloj mismo cuesta: sólo para repartir) */
		else if (!strcmp(argv[i], "--ref3d"))
			M.vid.use_ref3d = 1; /* render 3D con la referencia (si no, con el código del SPE) */
		else if (!strcmp(argv[i], "--vs-ref3d"))
			M.vid.vs_ref3d = 1;  /* comparar el código del SPE con la referencia en cada cuadro */
		else if (!strcmp(argv[i], "--r3d-band") && i + 1 < argc)
			M.vid.r3d_band = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--r3d-units") && i + 1 < argc)
			M.vid.r3d_units = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--r3d-bloques"))
			M.vid.r3d_texmode = 1; /* caché de texturas de la 3.2 a la 3.8 (bloques de 256 texeles) siempre */
		else if (!strcmp(argv[i], "--r3d-aplazar") && i + 1 < argc) {
			M.vid.r3d_defer_line = (uint32_t)strtoul(argv[++i], NULL, 0);
			M.vid.ops.submit3d = fk_submit3d;
			M.vid.ops.wait3d = fk_wait3d;
			M.vid.ops.submit = fk_submit2d;
			r3d_defer_sim = 1;
		} else if (!strcmp(argv[i], "--r3d-celdas"))
			M.vid.r3d_texmode = 2; /* celdas de 4×4 siempre (si no, automático: bloques hasta que se desbordan) */
		else if (!strcmp(argv[i], "--texturas-xdr"))
			M.vid.r3d_flags |= R3D_F_XDR;
		else if (!strcmp(argv[i], "--r3d-win") && i + 1 < argc)
			sscanf(argv[++i], "%u:%u", &M.vid.r3d_win0, &M.vid.r3d_win1); /* estadísticas solo de esos cuadros */
		else if (!strcmp(argv[i], "--r3d-stats"))
			r3d_stats_on = 1;
		else if (!strcmp(argv[i], "--holes"))
			holes_on = 1; /* cuenta los agujeros de la capa 3D en cada cuadro */
		else if (!strcmp(argv[i], "--holes-dump") && i + 1 < argc)
			holes_dump = argv[++i], holes_on = 1; /* deja la imagen 3D del cuadro con más agujeros */
		else if (!strcmp(argv[i], "--frame-dump") && i + 1 < argc) {
			/* CUADRO:PREFIJO — antes de correr ese cuadro, vuelca el cuadro de geometría entregado (PREFIJO.gx); después, su capa 3D (PREFIJO.ppm) */
			char *e;

			fdump_frame = strtoull(argv[++i], &e, 0);
			fdump_prefix = *e == ':' ? e + 1 : "cuadro";
		} else if (!strcmp(argv[i], "--img3d") && i + 1 < argc)
			img3d_path = argv[++i];
		else if (!strcmp(argv[i], "--gx-dump") && i + 1 < argc)
			gxdump_path = argv[++i];
		else if (!strcmp(argv[i], "--snd-original"))
			snd_original = 1;
		else if (!strcmp(argv[i], "--cycles") && i + 1 < argc)
			max_cycles = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--quantum") && i + 1 < argc)
			quantum = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
			keys = (uint32_t)strtoul(argv[++i], NULL, 16);
		else if (!strcmp(argv[i], "--touch") && i + 1 < argc) {
			sscanf(argv[++i], "%u,%u", &tx, &ty);
			keys |= NDS_KEY_PEN;
		} else if (!strcmp(argv[i], "--stats"))
			stats = 1;
		else if (!strcmp(argv[i], "--arm7"))
			show_arm7 = 1;
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			trace_n = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--press") && i + 1 < argc && npress < 512) {
			/* CUADRO:TECLAS_HEX[:X,Y] — desde ese cuadro se mantiene ese estado de botones (y del toque) */
			char *e;

			press[npress].frame = strtoull(argv[++i], &e, 0);
			press[npress].keys = *e == ':' ? (uint32_t)strtoul(e + 1, &e, 16) : 0;
			press[npress].x = press[npress].y = 0;
			if (*e == ':') {
				sscanf(e + 1, "%u,%u", &press[npress].x, &press[npress].y);
				press[npress].keys |= NDS_KEY_PEN;
			}
			npress++;
		} else if (!strcmp(argv[i], "--shot") && i + 1 < argc && nshot < 32) {
			/* CUADRO:ARCHIVO.ppm — vuelca las dos pantallas al llegar a ese cuadro */
			char *e;

			shot[nshot].frame = strtoull(argv[++i], &e, 0);
			shot[nshot].file = *e == ':' ? e + 1 : "shot.ppm";
			nshot++;
		} else if (!strcmp(argv[i], "--trace-from") && i + 1 < argc)
			trace_from = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--trace7"))
			trace_cpu = 1;
		else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
			skip_frames = strtoull(argv[++i], NULL, 0);
		else if ((!strcmp(argv[i], "--watch9") || !strcmp(argv[i], "--watch7")) && i + 1 < argc) {
			watch_cpu = argv[i][7] == '7';
			watch_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--watch-max") && i + 1 < argc)
			watch_max = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dump-cpu") && i + 1 < argc)
			dump_cpu = atoi(argv[++i]) == 7 ? 1 : 0;
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc && ndumps < 8) {
			/* DIR:LONG:ARCHIVO — al terminar, vuelca esa memoria tal como la ve el ARM9 */
			char *a = argv[++i], *b = strchr(a, ':'), *c = b ? strchr(b + 1, ':') : NULL;

			if (!c) {
				fprintf(stderr, "--dump DIR:LONG:ARCHIVO\n");
				return 2;
			}
			*b = *c = 0;
			dumps[ndumps].addr = (uint32_t)strtoul(a, NULL, 0);
			dumps[ndumps].len = (uint32_t)strtoul(b + 1, NULL, 0);
			dumps[ndumps++].file = c + 1;
		} else if (!strcmp(argv[i], "--save") && i + 1 < argc)
			save_path = argv[++i];
		else if (!strcmp(argv[i], "--save-cfg") && i + 1 < argc)
			save_cfg = argv[++i];
		else if (!strcmp(argv[i], "--save-size") && i + 1 < argc)
			save_size = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dbg") && i + 1 < argc)
			dbg = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--hist") && i + 1 < argc)
			hist_n = strtoull(argv[++i], NULL, 0);
		else if ((!strcmp(argv[i], "--break9") || !strcmp(argv[i], "--break7")) && i + 1 < argc) {
			break_cpu = argv[i][7] == '7';
			break_pc = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--ring") && i + 1 < argc)
			ring_n = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--screens") && i + 1 < argc)
			screens_path = argv[++i];
		else if (!strcmp(argv[i], "--regs"))
			show_regs = 1;
		else if (!strcmp(argv[i], "--ref"))
			use_ref = 1;
		else if (!strcmp(argv[i], "--vs-ref"))
			vs_ref = 1;
		else if (!strcmp(argv[i], "--expect"))
			expect = 1;
		else if (!strcmp(argv[i], "--same-ab"))
			same_ab = 1;
		else if (!strcmp(argv[i], "--unmapped"))
			unmapped = 1;
		else if (!strcmp(argv[i], "--jit"))
			jit_mode = 1;
		else if (!strcmp(argv[i], "--jit-strict"))
			jit_mode = 2;
		else if (!strcmp(argv[i], "--state-hash"))
			state_hash = 1;
		else if (!strcmp(argv[i], "--jit-no7"))
			jit_arm7 = 0;
		else if (!strcmp(argv[i], "--jit-no9"))
			jit_arm9 = 0;
		else if (!strcmp(argv[i], "--jit-nolink"))
			jit_nolink = 1;
		else if (!strcmp(argv[i], "--jit-far"))
			jit_far = 1;
		else if (!strcmp(argv[i], "--jit-nojl"))
			jit_nojl = 1;
		else if (!strcmp(argv[i], "--jit-opts") && i + 1 < argc)
			hj_opts = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--jit-prof") && i + 1 < argc)
			jit_prof = argv[++i];
		else if (!strcmp(argv[i], "--lockstep"))
			lockstep = 1;
		else if (!strcmp(argv[i], "--rom-paged") && i + 1 < argc) {
			unsigned pk = 256, ck = 32768;

			sscanf(argv[++i], "%u,%u", &pk, &ck);
			pg_shift = 0;
			while ((1u << pg_shift) < pk * 1024u)
				pg_shift++;
			pg_cache = (size_t)ck * 1024u;
			force_paged = 1;
		} else if (!strcmp(argv[i], "--rom-whole-max") && i + 1 < argc)
			whole_max = (size_t)strtoul(argv[++i], NULL, 0) << 20;
		else if (!strcmp(argv[i], "--peek") && i + 1 < argc && npeek < 8)
			peek[npeek++] = strtoul(argv[++i], NULL, 0);
		else
			rom_path = argv[i];
	}
	if (!rom_path) {
		fprintf(stderr, "uso: salsads-cpurun [--cycles N] [--frames N] [--quantum N] [--stats] [--arm7] "
				"[--keys HEX] [--touch X,Y] [--screens FILE.ppm] [--ref] [--vs-ref] [--expect] [--same-ab] [--unmapped] [--peek ADDR] [--save-cfg FILE] [--save FILE | --save-size N] ROM.nds\n");
		return 2;
	}
	romf = fopen(rom_path, "rb");
	if (romf) {
		fseek(romf, 0, SEEK_END);
		len = (size_t)ftell(romf);
		fseek(romf, 0, SEEK_SET);
	}
	if (romf && (force_paged || len > whole_max)) {
		rom = NULL;
	} else {
		if (romf)
			fclose(romf), romf = NULL;
		rom = slurp(rom_path, &len);
	}
	if (!rom && !romf) {
		fprintf(stderr, "no puedo leer %s\n", rom_path);
		return 2;
	}
	M.log = log_to_stderr;
	M.quantum = quantum;
	M.dbg = dbg;
	M.gx_lento = (uint8_t)gx_lento;
	M.vid.use_ref = use_ref;
	nds_init(&M);
	if (gx_defer) {
		static gx_loop gloop;

		gx_loop_attach(&M.gx, &gloop);
	}
	if (gx_clock) {
		M.gx.clock = clock_ns;
		M.prof_clock = clock_ns;
	}
	if (r3d_stats_on)
		M.vid.clock = clock_ns;
	if (wav_path)
		M.snd.ops.sink = wav_sink;
	if (snd_original)
		nds_snd_set_mode(&M, SND_MODE_ORIGINAL);
	if (lockstep) {
		R.quantum = quantum;
		R.vid.use_ref = use_ref;
		R.gx_lento = (uint8_t)gx_lento;
		nds_init(&R);
		if (snd_original)
			nds_snd_set_mode(&R, SND_MODE_ORIGINAL);
		if (hj_lockstep_attach(&M, &R, jit_arm7, jit_arm9))
			return 2;
	} else if (jit_mode && jit_prof && arec_prof_open(jit_prof)) {
		fprintf(stderr, "--jit-prof: no se pudo abrir %s.tr / %s.blk\n", jit_prof, jit_prof);
		return 1;
	} else if (jit_mode && hj_attach(&M, jit_mode == 2, jit_nolink, jit_far, jit_nojl, jit_arm7, jit_arm9, NULL, NULL)) {
		return 2;
	}
	if (romf)
		fprintf(stderr, "núcleo: ROM por fragmentos (%zu bytes, páginas de %u KB, caché %zu KB)\n", len, 1u << (pg_shift - 10),
			pg_cache >> 10);
	if ((romf ? nds_load_rom_paged(&M, file_rd, romf, len, pg_shift, pg_cache) : nds_load_rom(&M, rom, len)) ||
	    nds_boot_direct(&M)) {
		fprintf(stderr, "ROM inválida\n");
		return 2;
	}
	if (lockstep && ((romf ? nds_load_rom_paged(&R, file_rd, romf, len, pg_shift, pg_cache) : nds_load_rom(&R, rom, len)) ||
			 nds_boot_direct(&R)))
		return 2;
	if (save_cfg) {
		size_t cl;
		uint8_t *cd = slurp(save_cfg, &cl);

		if (cd) {
			nds_save_cfg(&M, (const char *)cd, cl);
			if (lockstep)
				nds_save_cfg(&R, (const char *)cd, cl);
		}
		free(cd);
	}
	if (save_path) {
		size_t sl;
		uint8_t *sd = slurp(save_path, &sl);

		if (sd && lockstep)
			nds_save_attach(&R, sd, sl);
		const int attached = sd ? nds_save_attach(&M, sd, sl) : 0;
		if (attached == NDS_SAVE_INCOMPATIBLE) {
			fprintf(stderr, "núcleo: guardado incompatible; conserva %s y elige un archivo nuevo con --save\n", save_path);
			free(sd);
			return 2; /* Nunca llegar a save_out con el archivo original incompatible. */
		}
		if (sd && attached == 0)
			fprintf(stderr, "núcleo: guardado de %s: %zu bytes\n", save_path, sl);
		else if (sd)
			fprintf(stderr, "núcleo: %s no tiene el tamaño de ningún chip (%zu bytes): se ignora\n", save_path, sl);
		free(sd);
	} else if (save_size) {
		uint8_t *blank = malloc(save_size);

		if (blank) {
			memset(blank, 0xFF, save_size);
			if (lockstep)
				nds_save_attach(&R, blank, save_size);
			if (nds_save_attach(&M, blank, save_size))
				fprintf(stderr, "--save-size %u no es el tamaño de ningún chip\n", save_size);
			free(blank);
		}
	}
	M.gx.ex.poly_plain = (uint32_t)geom_plain;
	M.gx.ex.read_scalar = (uint32_t)geom_scalar;
	if (keys)
		set_input(keys, tx, ty);
	for (uint64_t f = 0; f < skip_frames; f++) {
		for (uint32_t k = 0; k < npress; k++)
			if (press[k].frame == f)
				set_input(press[k].keys, press[k].x, press[k].y);
		for (uint32_t k = 0; k < nshot; k++)
			if (shot[k].frame == f)
				save_ppm(shot[k].file);
		run_frame();
	}
	if (watch_addr) {
		/* paso a paso (1 ciclo del ARM9): imprime cada cambio de la palabra en watch_addr, vista por watch_cpu */
		uint32_t old = nds_peek32(&M, watch_cpu, watch_addr);
		uint64_t steps = 0, changes = 0;

		printf("vigilo 0x%08x (ARM%d): valor inicial %08x\n", watch_addr, watch_cpu ? 7 : 9, old);
		while (steps < max_cycles && changes < (watch_max ? watch_max : 50)) {
			const uint32_t v = nds_peek32(&M, watch_cpu, watch_addr);

			if (v != old) {
				printf("t=%llu cuadro %llu: %08x -> %08x  (ARM9 pc %08x lr %08x, ARM7 pc %08x lr %08x)\n",
				       (unsigned long long)M.time, (unsigned long long)M.frames, old, v, acore_pc(&M.a9), M.a9.r[14],
				       acore_pc(&M.a7), M.a7.r[14]);
				old = v;
				changes++;
			}
			nds_run(&M, 1);
			steps++;
		}
		return 0;
	}
	if (break_cpu >= 0) {
		/* corre de a un ciclo hasta que la CPU llegue a break_pc; luego imprime las últimas ring_n instrucciones
		 * de esa CPU (para ver cómo llegó) */
		struct acore *c = break_cpu ? &M.a7 : &M.a9;
		struct ent { uint32_t pc, insn, cpsr, r0, r1, r2, lr; } *ring;
		const uint64_t rn = ring_n ? ring_n : 256;
		uint64_t pos = 0, last_ic = c->icount, steps = 0;

		ring = calloc(rn, sizeof *ring);
		while (steps < max_cycles) {
			const uint32_t pc = acore_pc(c);

			if (c->icount != last_ic) {
				last_ic = c->icount;
			}
			if (pc == break_pc && !c->halted)
				break;
			{
				struct ent *e = &ring[pos++ % rn];
				const uint32_t insn = nds_peek32(&M, break_cpu, pc & ~3u);

				e->pc = pc;
				e->insn = (c->cpsr & ACORE_T) ? (insn >> ((pc & 2) * 8)) & 0xFFFF : insn;
				e->cpsr = c->cpsr;
				e->r0 = c->r[0];
				e->r1 = c->r[1];
				e->r2 = c->r[2];
				e->lr = c->r[14];
			}
			{
				const uint64_t ic = c->icount;

				do {
					nds_run(&M, 1);
					steps++;
				} while (c->icount == ic && steps < max_cycles && !c->halted);
				if (c->halted)
					nds_run(&M, 2);
			}
		}
		printf("llegó a %08x: ARM9 pc %08x, ARM7 pc %08x, cuadros %llu, ciclos %llu\n", break_pc, acore_pc(&M.a9),
		       acore_pc(&M.a7), (unsigned long long)M.frames, (unsigned long long)M.time);
		printf("pila (sp=%08x):", c->r[13]);
		for (uint32_t k = 0; k < 64; k++) {
			if (k % 8 == 0)
				printf("\n  %08x:", c->r[13] + 4 * k);
			printf(" %08x", nds_peek32(&M, break_cpu, c->r[13] + 4 * k));
		}
		printf("\nregistros:");
		for (int k = 0; k < 16; k++)
			printf(" r%d=%08x", k, c->r[k]);
		printf("\n");
		for (uint64_t i = pos > rn ? pos - rn : 0; i < pos; i++) {
			const struct ent *e = &ring[i % rn];

			printf("%08x: %08x  cpsr=%08x r0=%08x r1=%08x r2=%08x lr=%08x\n", e->pc, e->insn, e->cpsr, e->r0, e->r1, e->r2, e->lr);
		}
		return 0;
	}
	if (hist_n) {
		/* histograma de PC de las dos CPUs durante hist_n pasos de 1 ciclo del ARM9 (después de --skip cuadros) */
		struct { uint32_t pc; uint32_t n; } h[2][64];
		uint32_t nh[2] = { 0, 0 };

		for (uint64_t i = 0; i < hist_n; i++) {
			for (int c = 0; c < 2; c++) {
				const uint32_t pc = acore_pc(c ? &M.a7 : &M.a9);
				uint32_t k;

				for (k = 0; k < nh[c] && h[c][k].pc != pc; k++)
					;
				if (k == nh[c] && nh[c] < 64)
					h[c][nh[c]++].pc = pc, h[c][k].n = 0;
				if (k < nh[c])
					h[c][k].n++;
			}
			nds_run(&M, 2);
		}
		for (int c = 0; c < 2; c++) {
			printf("%s: %u PC distintos (máx 64)\n", c ? "ARM7" : "ARM9", nh[c]);
			for (uint32_t k = 0; k < nh[c]; k++)
				if (h[c][k].n * 50 >= hist_n)
					printf("  pc %08x  %5.1f %%\n", h[c][k].pc, 100.0 * h[c][k].n / (double)hist_n);
		}
		return 0;
	}
	if (trace_n) {
		/* traza de instrucciones de una CPU (la otra avanza a su ritmo) */
		struct acore *c = trace_cpu ? &M.a7 : &M.a9;

		if (trace_from) /* corre (de a una instrucción) hasta llegar a esa dirección */
			for (uint64_t k = 0; k < (uint64_t)4000000000ull && acore_pc(c) != trace_from; k++)
				nds_run(&M, 1);
		for (uint64_t i = 0; i < trace_n; i++) {
			const uint32_t pc = acore_pc(c);
			const uint32_t insn = nds_peek32(&M, trace_cpu, pc & ~3u);

			printf("%08x: %08x  cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x sp=%08x lr=%08x\n", pc,
			       (c->cpsr & ACORE_T) ? (insn >> ((pc & 2) * 8)) & 0xFFFF : insn, c->cpsr, c->r[0], c->r[1],
			       c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[13], c->r[14]);
			nds_run(&M, 1);
		}
		return 0;
	}
	t0 = now();
	if (frames) {
		const char *who;
		uint32_t pc;

		for (uint64_t f = 0; f < frames; f++) {
			for (uint32_t k = 0; k < npress; k++)
				if (press[k].frame == f)
					set_input(press[k].keys, press[k].x, press[k].y);
			for (uint32_t k = 0; k < nshot; k++)
				if (shot[k].frame == f)
					save_ppm(shot[k].file);
			if (fdump_prefix && f == fdump_frame) {
				char path[512];

				snprintf(path, sizeof path, "%s.gx", fdump_prefix);
				gx_dump(path);
			}
			run_frame();
			arec_prof_frame((uint32_t)f, (uint32_t)M.a9.icount, (uint32_t)M.a7.icount);
			gx_hash_poll();
			if (lockstep && hj_fail())
				break;
			if (fdump_prefix && f == fdump_frame) {
				char path[512];

				snprintf(path, sizeof path, "%s.ppm", fdump_prefix);
				save_img3d(path);
			}
			if (holes_on) {
				const uint32_t h = count_holes();

				if (h) {
					holes_total += h;
					holes_frames++;
				}
				if (h > holes_worst) {
					holes_worst = h;
					holes_worst_frame = (uint32_t)f;
					if (holes_dump)
						save_img3d(holes_dump);
				}
			}
			if (g2d_stats)
				nds_video_render_host(&M);
			if (stuck_in_vector(&M, &who, &pc)) {
				printf("EXCEPCIÓN %s sin manejador en el cuadro %llu: pc = 0x%08x (ARM9 pc 0x%08x, ARM7 pc 0x%08x)\n", who,
				       (unsigned long long)f, pc, acore_pc(&M.a9), acore_pc(&M.a7));
				break;
			}
		}
		t1 = now();
		arec_prof_close();
		printf("fin: ARM9 pc 0x%08x cpsr 0x%08x%s, ARM7 pc 0x%08x cpsr 0x%08x%s; cuadros %llu\n", acore_pc(&M.a9), M.a9.cpsr,
		       M.a9.halted ? " (en HALT)" : "", acore_pc(&M.a7), M.a7.cpsr, M.a7.halted ? " (en HALT)" : "",
		       (unsigned long long)M.frames);
		if (stats)
			printf("estadísticas: %llu cuadros, ARM9 %llu instr., ARM7 %llu instr., %.3f s, %.1f MIPS\n",
			       (unsigned long long)frames, (unsigned long long)M.a9.icount, (unsigned long long)M.a7.icount,
			       t1 - t0, (double)(M.a9.icount + M.a7.icount) / (t1 - t0) / 1e6);
		if (stats)
			printf("planificador: %.1f tramos, %.1f saltos ociosos y %.1f eventos por cuadro; %.1f us por cuadro\n",
			       (double)M.st.slices / (double)frames, (double)M.st.jumps / (double)frames,
			       (double)M.st.events / (double)frames, (t1 - t0) * 1e6 / (double)frames);
		if (stats)
			printf("FIFO de geometría%s: por cuadro %.1f palabras de DMA en %.1f ráfagas y %.1f palabras de la CPU por las rutas cortas\n",
			       M.gx_lento ? " (gx-lento)" : "", (double)M.st.gx_dma_words / (double)frames,
			       (double)M.st.gx_dma_bursts / (double)frames, (double)M.st.gx_cpu_words / (double)frames);
		if (stats && M.prof_clock)
			printf("tiempo por cuadro (con --gx-clock; los relojes cuestan): ARM9 %.1f us, ARM7 %.1f us, eventos %.1f us; ARM7 %.0f instr. por cuadro\n",
			       (double)M.st.prof_a9 / 1e3 / (double)frames, (double)M.st.prof_a7 / 1e3 / (double)frames,
			       (double)M.st.prof_ev / 1e3 / (double)frames, (double)M.a7.icount / (double)frames);
		if (M.cart.paged) {
			uint64_t acc, miss, ra;

			nds_rom_stats(&M, &acc, &miss, &ra);
			printf("rom-paged: %llu ventanas, %llu fallos (%.3f por cuadro), %llu páginas por adelantado, %llu errores\n",
			       (unsigned long long)acc, (unsigned long long)miss, (double)miss / (double)(frames ? frames : 1),
			       (unsigned long long)ra, (unsigned long long)M.cart.pg.errs);
		}
		if (gxh_on)
			printf("gx-hash: %llu cuadros de geometría, %016llx\n", (unsigned long long)gxh_n, (unsigned long long)gxh_all);
		if (stats) {
			static const char *const evn[EV_COUNT] = { "línea", "hblank", "tm9.0", "tm9.1", "tm9.2", "tm9.3", "tm7.0", "tm7.1",
								   "tm7.2", "tm7.3", "cartucho", "spi", "gx" };
			printf("eventos por cuadro (cuántos y us por cuadro con --gx-clock):");
			for (int k = 0; k < EV_COUNT; k++)
				if (M.st.ev_n[k])
					printf(" %s=%.1f/%.1f", evn[k], (double)M.st.ev_n[k] / (double)frames,
					       (double)M.st.ev_t[k] / 1e3 / (double)frames);
			printf("\n");
		}
		if (stats)
			print_gx_stats(M.frames), gx_cmds();
		if (M.jit9 || M.jit7)
			hj_print_stats(&M);
		if (state_hash)
			print_state_hash(&M);
		if (lockstep && hj_fail())
			rc_gfx = 1;
		if (gxdump_path)
			gx_dump(gxdump_path);
		if (img3d_path)
			save_img3d(img3d_path);
		if (r3d_stats_on || M.vid.vs_ref3d)
			print_r3d_stats();
		if (holes_on)
			printf("agujeros 3D: %u píxeles en %u cuadros de %llu; el peor, %u en el cuadro %u\n", holes_total, holes_frames,
			       (unsigned long long)frames, holes_worst, holes_worst_frame);
		if (screens_path)
			save_ppm(screens_path);
		if (unmapped) {
			static uint16_t tmp[NDS_SCREEN_W * NDS_SCREEN_H];
			int set;
			const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

			gpu2d_ref_unmapped_reads = 0;
			if (fr)
				gpu2d_ref_engine(&fr->eng[0], tmp);
			printf("VRAM sin banco: %u lecturas del motor A\n", gpu2d_ref_unmapped_reads);
		}
		if (expect && check_expect())
			rc_gfx = 1;
		if (same_ab && check_same_ab())
			rc_gfx = 1;
		if (vs_ref && check_vs_ref())
			rc_gfx = 1;
		for (uint32_t k = 0; k < npeek; k++)
			printf("peek 0x%08x = 0x%08x\n", peek[k], nds_peek32(&M, 0, peek[k]));
		if (show_regs)
			print_regs();
		save_out(save_path);
		if (wav_path)
			wav_write(wav_path);
		if (g2d_stats) {
			static const char *const st[G2D_ST_COUNT] = { "init", "plan", "texto", "afin", "obj", "compone", "extpal" };
			uint64_t tot[G2D_C_COUNT] = { 0 };
			const struct nds_video *vv = &M.vid;

			printf("DMA del 2D por cuadro y por etapa (host: A y B juntos; %llu cuadros): órdenes get / bytes / listas (elementos) / put / "
			       "esperas\n", (unsigned long long)frames);
			for (int k = 0; k < G2D_ST_COUNT; k++) {
				for (int q = 0; q < G2D_C_COUNT; q++)
					tot[q] += g2d_dma_cnt[k][q];
				printf("  %-8s get %8.1f (%9.0f B)  listas %7.1f (%8.1f elem, %9.0f B)  put %6.1f  esperas %8.1f\n", st[k],
				       (double)g2d_dma_cnt[k][G2D_C_GET] / (double)frames, (double)g2d_dma_cnt[k][G2D_C_GET_BYTES] / (double)frames,
				       (double)g2d_dma_cnt[k][G2D_C_LIST] / (double)frames, (double)g2d_dma_cnt[k][G2D_C_LIST_EL] / (double)frames,
				       (double)g2d_dma_cnt[k][G2D_C_LIST_BYTES] / (double)frames, (double)g2d_dma_cnt[k][G2D_C_PUT] / (double)frames,
				       (double)g2d_dma_cnt[k][G2D_C_WAIT] / (double)frames);
			}
			for (int e = 0; e < 2; e++) {
				const gpu2d_stats *q = &vv->stat_sum[e];
				const double nf = vv->stat_frames ? (double)vv->stat_frames : 1.0;

				printf("  motor %c por cuadro: filas de teselas %.0f, esperas de mapa %.0f, filas de OBJ %.0f, OBJ-línea dibujados %.0f, "
				       "cambios %.0f, líneas afines %.0f, bloques afines %.0f, vaciados de OBJ %.0f, líneas con efectos %.0f, "
				       "mapas sin sitio %.0f\n", 'A' + e, q->bg_fetches / nf, q->map_waits / nf, q->obj_fetches / nf,
				       q->obj_drawn / nf, q->chg_applied / nf, q->aff_lines / nf, q->aff_quads / nf, q->obj_flushes / nf,
				       q->fx_lines / nf, q->map_dropped / nf);
			}
			printf("  total    get %8.1f (%9.0f B)  listas %7.1f (%8.1f elem)  put %6.1f  esperas %8.1f\n",
			       (double)tot[G2D_C_GET] / (double)frames, (double)tot[G2D_C_GET_BYTES] / (double)frames,
			       (double)tot[G2D_C_LIST] / (double)frames, (double)tot[G2D_C_LIST_EL] / (double)frames,
			       (double)tot[G2D_C_PUT] / (double)frames, (double)tot[G2D_C_WAIT] / (double)frames);
		}
		for (uint32_t k = 0; k < ndumps; k++) {
			FILE *f = fopen(dumps[k].file, "wb");

			if (!f)
				continue;
			for (uint32_t o = 0; o < dumps[k].len; o++)
				fputc((int)nds_peek8(&M, dump_cpu, dumps[k].addr + o), f);
			fclose(f);
		}
		return rc_gfx;
	}
	{
		uint64_t done = 0;
		const uint64_t step = 1000000;
		uint32_t magic = 0, state = 0;
		const char *who;
		uint32_t pc;

		while (done < max_cycles) {
			run_cycles(step);
			done += step;
			if (lockstep && hj_fail())
				break;
			magic = nds_peek32(&M, 0, SDS_ADDR);
			state = nds_peek32(&M, 0, SDS_ADDR + 8);
			if (magic == SDS_MAGIC && state == 1)
				break;
			if (stuck_in_vector(&M, &who, &pc)) {
				printf("EXCEPCIÓN %s sin manejador: pc = 0x%08x (ARM9 pc 0x%08x, ARM7 pc 0x%08x)\n", who, pc,
				       acore_pc(&M.a9), acore_pc(&M.a7));
				return 3;
			}
		}
	}
	t1 = now();

	{
		const char *name = strrchr(rom_path, '/');
		uint32_t w[12];
		int rc;

		name = name ? name + 1 : rom_path;
		save_out(save_path);
		for (int i = 0; i < 12; i++)
			w[i] = nds_peek32(&M, 0, SDS_ADDR + 4 * (uint32_t)i);
		if (stats) {
			const double secs = t1 - t0;
			const double mips = (double)(M.a9.icount + M.a7.icount) / secs / 1e6;

			printf("estadísticas: ARM9 %llu instr. (%llu ciclos), ARM7 %llu instr. (%llu ciclos), %.3f s, %.1f MIPS, "
			       "%llu cuadros\n",
			       (unsigned long long)M.a9.icount, (unsigned long long)M.a9.cycles,
			       (unsigned long long)M.a7.icount, (unsigned long long)M.a7.cycles, secs, mips,
			       (unsigned long long)M.frames);
		}
		if (stats) {
			static const char *const evn[EV_COUNT] = { "línea", "hblank", "tm9.0", "tm9.1", "tm9.2", "tm9.3", "tm7.0", "tm7.1",
								   "tm7.2", "tm7.3", "cartucho", "spi", "gx" };
			printf("eventos por cuadro (cuántos y us por cuadro con --gx-clock):");
			for (int k = 0; k < EV_COUNT; k++)
				if (M.st.ev_n[k])
					printf(" %s=%.1f/%.1f", evn[k], (double)M.st.ev_n[k] / (double)frames,
					       (double)M.st.ev_t[k] / 1e3 / (double)frames);
			printf("\n");
		}
		if (stats)
			print_gx_stats(M.frames), gx_cmds();
		if (M.jit9 || M.jit7)
			hj_print_stats(&M);
		if (state_hash)
			print_state_hash(&M);
		if (lockstep && hj_fail())
			rc_gfx = 1;
		if (gxdump_path)
			gx_dump(gxdump_path);
		if (img3d_path)
			save_img3d(img3d_path);
		if (r3d_stats_on || M.vid.vs_ref3d)
			print_r3d_stats();
		if (screens_path)
			save_ppm(screens_path);
		if (unmapped) {
			static uint16_t tmp[NDS_SCREEN_W * NDS_SCREEN_H];
			int set;
			const gpu2d_frame *fr = nds_video_last_frame(&M, &set);

			gpu2d_ref_unmapped_reads = 0;
			if (fr)
				gpu2d_ref_engine(&fr->eng[0], tmp);
			printf("VRAM sin banco: %u lecturas del motor A\n", gpu2d_ref_unmapped_reads);
		}
		if (expect && check_expect())
			rc_gfx = 1;
		if (same_ab && check_same_ab())
			rc_gfx = 1;
		if (vs_ref && check_vs_ref())
			rc_gfx = 1;
		for (uint32_t k = 0; k < npeek; k++)
			printf("peek 0x%08x = 0x%08x\n", peek[k], nds_peek32(&M, 0, peek[k]));
		if (show_regs)
			print_regs();
		if (show_arm7 && nds_peek32(&M, 0, SDS_ARM7_ADDR) == SDS_MAGIC) {
			uint32_t w7[12];

			for (int i = 0; i < 12; i++)
				w7[i] = nds_peek32(&M, 0, SDS_ARM7_ADDR + 4 * (uint32_t)i);
			fprintf(stderr, "  ARM7: estado %u, %u grupos, %u comprobaciones, %u fallos, %u divergencias\n", w7[2], w7[5],
				w7[3], w7[4], w7[11]);
			for (uint32_t i = 0; i < w7[4] && i < 16; i++) {
				const uint32_t a = SDS_ARM7_ADDR + 0x40 + 16 * i;

				fprintf(stderr, "  ARM7 fallo: grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n",
					nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
					nds_peek32(&M, 0, a + 12));
			}
			for (uint32_t i = 0; i < w7[11] && i < 32; i++) {
				const uint32_t a = SDS_ARM7_ADDR + 0x140 + 16 * i;

				fprintf(stderr, "  ARM7 divergencia: grupo %u, id %u, obtuvo 0x%08x, documentado 0x%08x\n",
					nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
					nds_peek32(&M, 0, a + 12));
			}
		}
		for (uint32_t i = 0; i < w[4] && i < 16; i++) {
			const uint32_t a = SDS_ADDR + 0x40 + 16 * i;

			fprintf(stderr, "  fallo: grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n", nds_peek32(&M, 0, a),
				nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8), nds_peek32(&M, 0, a + 12));
		}
		for (uint32_t i = 0; i < w[11] && i < 32; i++) {
			const uint32_t a = SDS_ADDR + 0x140 + 16 * i;

			fprintf(stderr, "  divergencia: grupo %u, id %u, obtuvo 0x%08x, documentado 0x%08x\n",
				nds_peek32(&M, 0, a), nds_peek32(&M, 0, a + 4), nds_peek32(&M, 0, a + 8),
				nds_peek32(&M, 0, a + 12));
		}
		if (lockstep && hj_fail()) {
			printf("FALLO %s: el recompilador difiere del intérprete (--lockstep)\n", name);
			return 1;
		}
		if (w[0] != SDS_MAGIC || w[2] != 1) {
			printf("SIN VEREDICTO %s (ARM9 pc 0x%08x, ARM7 pc 0x%08x, marca %u)\n", name, acore_pc(&M.a9),
			       acore_pc(&M.a7), nds_peek32(&M, 0, SDS_ADDR + 0x30));
			return 2;
		}
		if (w[10] == 1 && w[4] == 0) {
			printf("OK %s: %u comprobaciones en %u grupos\n", name, w[3], w[5]);
			rc = 0;
		} else {
			printf("FALLO %s: %u de %u; primero en el grupo %u, id %u, obtuvo 0x%08x, esperaba 0x%08x\n", name,
			       w[4], w[3], w[6], w[7], w[8], w[9]);
			rc = 1;
		}
		return rc;
	}
}
