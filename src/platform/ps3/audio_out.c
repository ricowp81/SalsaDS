/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — salida de audio de la PS3. Ver audio_out.h. Código propio; el orden de apertura y cierre del puerto es el de
 * PSL1GHT (audio/audio.h) y lo que Salsa64 midió en la consola: un recurso de audio que quede enganchado al proceso puede
 * colgar la consola al salir, así que cada paso del cierre va con su resultado a una variable que el programa principal
 * puede llevar al log.
 */
#include "platform/ps3/audio_out.h"

#include <audio/audio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/wavrec.h"
#include "platform/ps3/log.h"
#include <sys/stat.h>
#include "audio/stretch.h"
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>

#define RING_FRAMES 16384u /* potencia de 2: 340 ms */
#define LEAD 4u            /* bloques llenos por delante del que suena */
#define PREBUF 2048u       /* muestras que se juntan antes de empezar a sonar */
#define TARGET 3072u       /* nivel que busca el control de deriva (64 ms) */
#define THREAD_PRIO 100

static audio_stretch stretch;
static audio_stretch2 stretch2;
static float pending[2 * STRETCH_HOP], input[2 * STRETCH2_INPUT], last_sample[2];
static uint32_t pending_pos = STRETCH_HOP, pending_used, pending_active;
static uint32_t control_tail;
static int no_stretch, no_limiter, stretch_v1;
static int active, paused = 1;
static volatile uint64_t c_inserted, c_active, c_stretch_us, c_played;
static volatile uint32_t c_stretch_max, c_joins, c_rate;

#define LIM_KNEE 0.70710678f /* igual que audio_soft_limit() */
static float out_gain = 1.0f, gain_db;
static volatile uint64_t c_lim_total, c_lim_n;
static volatile double c_lim_sum_db;
static volatile float c_lim_max_db;

/* Grabación digital (interruptor /dev_hdd0/salsads/audio-grabar): dos puntos de la cadena, 20 s a partir de los 5 s de
 * juego. `mix` = la mezcla nativa de la DS que devuelve el SPE (entero de 16 bits, 32 728 Hz, antes del FIR, el
 * estiramiento, el remuestreo, la ganancia y el limitador); `out` = lo último que se escribe en los bloques del puerto
 * (flotante de 32 bits, 2 canales, 48 kHz, después de todo). Todo se reserva al abrir el juego. Cada búfer lo escribe un
 * solo hilo (mix: emulación; out: audio) y la escritura a disco la hace el hilo de emulación, a trozos. */
#define REC_SKIP_S 5u
#define REC_LEN_S 20u
#define REC_MIX_RATE 32728u
#define REC_OUT_RATE 48000u
#define REC_PATH_MIX "/dev_hdd0/salsads/grab-mezcla.wav"
#define REC_PATH_OUT "/dev_hdd0/salsads/grab-salida.wav"
#define REC_SWITCH "/dev_hdd0/salsads/audio-grabar"
#define REC_CHUNK 32768u
static struct {
	int armed, finished;
	int16_t *mix;
	float *out;
	uint8_t *enc;
	uint32_t mix_cap, mix_seen, mix_n;
	uint32_t out_cap, out_seen, out_n;
	volatile int out_done, mix_done;
	uint64_t silent0, dropped0, late0;
	uint32_t under0;
	uint64_t silent, dropped, late;
	uint32_t under;
	int stage;      /* 0 espera, 1 mezcla, 2 salida, 3 listo */
	size_t off;     /* muestras ya escritas de la etapa actual */
	FILE *f;
} rec;

static float ring[2 * RING_FRAMES] __attribute__((aligned(128)));
static volatile uint32_t head, tail; /* en muestras (pares); head solo lo escribe el productor, tail el consumidor */

static struct {
	int open;
	u32 port;
	audioPortConfig config;
	sys_event_queue_t queue;
	sys_ipc_key_t key;
	sys_ppu_thread_t thread;
	volatile int quit;
	volatile int started;
} port;

static volatile uint64_t c_pushed, c_dropped, c_blocks, c_silent, c_busy;
static volatile uint32_t c_underruns, c_late, c_peak;

static float *block_ptr(uint32_t b)
{
	return (float *)(uintptr_t)port.config.audioDataStart + (size_t)b * 2 * AUDIO_BLOCK_SAMPLES;
}

static uint32_t level(void)
{
	return __atomic_load_n(&head, __ATOMIC_ACQUIRE) - __atomic_load_n(&control_tail, __ATOMIC_ACQUIRE);
}

void audio_out_push(const float *stereo, uint32_t frames)
{
	uint32_t h, t, free_n, n1;

	if (!port.open || !frames)
		return;
	h = head;
	t = __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
	free_n = RING_FRAMES - (h - t);
	c_pushed += frames;
	if (frames > free_n) {
		c_dropped += frames - free_n;
		frames = free_n;
	}
	n1 = RING_FRAMES - (h & (RING_FRAMES - 1));
	if (n1 > frames)
		n1 = frames;
	memcpy(&ring[2 * (h & (RING_FRAMES - 1))], stereo, (size_t)n1 * 8);
	if (frames > n1)
		memcpy(ring, stereo + 2 * n1, (size_t)(frames - n1) * 8);
	__atomic_store_n(&head, h + frames, __ATOMIC_RELEASE);
}

static void fill_legacy(float *dst)
{
	uint32_t t = tail, avail = __atomic_load_n(&head, __ATOMIC_ACQUIRE) - t, n = AUDIO_BLOCK_SAMPLES, got;
	uint32_t peak = c_peak;

	if (!port.started) {
		if (avail >= PREBUF)
			port.started = 1;
		else
			avail = 0;
	}
	got = port.started ? (avail < n ? avail : n) : 0;
	for (uint32_t i = 0; i < got; i++) {
		const float l = ring[2 * ((t + i) & (RING_FRAMES - 1))], r = ring[2 * ((t + i) & (RING_FRAMES - 1)) + 1];
		const float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
		uint32_t p = (uint32_t)((al > ar ? al : ar) * 32768.0f);

		dst[2 * i] = l;
		dst[2 * i + 1] = r;
		if (p > peak)
			peak = p;
	}
	if (got < n) {
		memset(dst + 2 * got, 0, (size_t)(n - got) * 8);
		if (port.started) {
			c_silent += n - got;
			c_underruns++;
		}
	}
	if (no_limiter) c_peak = peak;
	__atomic_store_n(&tail, t + got, __ATOMIC_RELEASE);
	__atomic_store_n(&control_tail, t + got, __ATOMIC_RELEASE);
}

static void fill_block(float *dst)
{
	if (!__atomic_load_n(&active, __ATOMIC_ACQUIRE)) {
		for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
			for (uint32_t c = 0; c < 2; c++)
				dst[2*i+c] = last_sample[c] * (1.0f - (float)i / (AUDIO_BLOCK_SAMPLES - 1));
		last_sample[0] = last_sample[1] = 0;
		pending_pos = STRETCH_HOP;
		memset(&stretch, 0, sizeof stretch);
		memset(&stretch2, 0, sizeof stretch2);
		__atomic_store_n(&tail, __atomic_load_n(&head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
		__atomic_store_n(&control_tail, tail, __ATOMIC_RELEASE);
		port.started = 0;
		__atomic_store_n(&paused, 1, __ATOMIC_RELEASE);
		return;
	}
	__atomic_store_n(&paused, 0, __ATOMIC_RELEASE);
	if (no_stretch) {
		fill_legacy(dst);
	} else {
		uint32_t done = 0;
		if (!port.started) {
			if (level() < (stretch_v1 ? PREBUF : 4096u)) {
				memset(dst, 0, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
				return;
			}
			port.started = 1;
		}
		while (done < AUDIO_BLOCK_SAMPLES) {
			if (pending_pos == STRETCH_HOP) {
				uint32_t t = tail, avail = __atomic_load_n(&head, __ATOMIC_ACQUIRE) - t;
				uint32_t limit = stretch_v1 ? STRETCH_GRAIN : STRETCH2_INPUT;
				uint32_t n = avail < limit ? avail : limit;
				uint32_t credit = stretch2.credit;
				uint64_t joins = stretch2.joins;
				uint32_t blending = stretch.blending;
				uint64_t t0, ins = stretch_v1 ? stretch.inserted : stretch2.inserted;
				uint64_t act = stretch_v1 ? stretch.active : stretch2.active;
				uint64_t miss = stretch_v1 ? stretch.missing : stretch2.missing;
				for (uint32_t i = 0; i < n; i++) {
					input[2*i] = ring[2*((t+i) & (RING_FRAMES-1))];
					input[2*i+1] = ring[2*((t+i) & (RING_FRAMES-1))+1];
				}
				t0 = sysGetSystemTime();
				uint32_t used = stretch_v1 ? audio_stretch_run(&stretch, input, n, pending) :
					audio_stretch2_run(&stretch2, input, avail, pending);
				uint32_t elapsed = (uint32_t)(sysGetSystemTime() - t0);
				c_stretch_us += elapsed;
				c_joins += stretch_v1 ? (!used || blending) : (uint32_t)(stretch2.joins - joins);
				c_rate = stretch_v1 ? 0 : (uint32_t)(stretch2.rate * 1000);
				if (elapsed > c_stretch_max) c_stretch_max = elapsed;
				pending_used = stretch_v1 ? used : used + credit - stretch2.credit;
				c_inserted += (stretch_v1 ? stretch.inserted : stretch2.inserted) - ins;
				pending_active = (stretch_v1 ? stretch.active : stretch2.active) != act;
				c_silent += (stretch_v1 ? stretch.missing : stretch2.missing) - miss;
				if ((stretch_v1 ? stretch.missing : stretch2.missing) != miss) c_underruns++;
				__atomic_store_n(&tail, t + used, __ATOMIC_RELEASE);
				pending_pos = 0;
			}
			uint32_t take = STRETCH_HOP - pending_pos;
			if (take > AUDIO_BLOCK_SAMPLES - done) take = AUDIO_BLOCK_SAMPLES - done;
			memcpy(dst + 2*done, pending + 2*pending_pos, 2*take*sizeof(float));
			/* El control de ±2 % ve las muestras aún pendientes, igual que
			 * la ruta de 256 pares; adelantar tail por un grano no cambia la razón. */
			uint32_t before = stretch_v1 ? (pending_pos < pending_used ? pending_pos : pending_used) :
				pending_pos * pending_used / STRETCH_HOP;
			uint32_t after = stretch_v1 ? (pending_pos + take < pending_used ? pending_pos + take : pending_used) :
				(pending_pos + take) * pending_used / STRETCH_HOP;
			__atomic_store_n(&control_tail, control_tail + after - before, __ATOMIC_RELEASE);
			if (pending_active) c_active += take;
			pending_pos += take;
			done += take;
		}
	}
	{
		uint32_t acting = 0;
		double sum_db = 0;
		float max_db = 0;

		for (uint32_t i = 0; i < 2 * AUDIO_BLOCK_SAMPLES; i++) {
			const float in = dst[i] * out_gain, a = in < 0 ? -in : in;
			float x = no_limiter ? in : audio_soft_limit(in);
			uint32_t p;

			if (!no_limiter && a > LIM_KNEE) {
				const float ax = x < 0 ? -x : x, db = 8.6858896f * logf(a / ax);

				acting++;
				sum_db += db;
				if (db > max_db) max_db = db;
			}
			p = (uint32_t)((x < 0 ? -x : x) * 32768.0f);
			dst[i] = x;
			if (p > c_peak) c_peak = p;
		}
		c_lim_total += 2 * AUDIO_BLOCK_SAMPLES;
		c_lim_n += acting;
		c_lim_sum_db += sum_db;
		if (max_db > c_lim_max_db) c_lim_max_db = max_db;
	}
	if (rec.armed && port.started && !rec.out_done) {
		if (rec.out_seen < REC_SKIP_S * REC_OUT_RATE) {
			rec.out_seen += AUDIO_BLOCK_SAMPLES;
		} else {
			uint32_t n = AUDIO_BLOCK_SAMPLES;

			if (rec.out_n == 0) {
				rec.silent0 = c_silent; rec.dropped0 = c_dropped; rec.under0 = c_underruns; rec.late0 = c_late;
			}
			if (n > rec.out_cap - rec.out_n) n = rec.out_cap - rec.out_n;
			memcpy(rec.out + 2 * (size_t)rec.out_n, dst, (size_t)n * 2 * sizeof(float));
			rec.out_n += n;
			if (rec.out_n == rec.out_cap) {
				rec.silent = c_silent - rec.silent0; rec.dropped = c_dropped - rec.dropped0;
				rec.under = c_underruns - rec.under0; rec.late = c_late - rec.late0;
				__atomic_store_n(&rec.out_done, 1, __ATOMIC_RELEASE);
			}
		}
	}
	if (port.started) c_played += AUDIO_BLOCK_SAMPLES;
	last_sample[0] = dst[2*AUDIO_BLOCK_SAMPLES-2];
	last_sample[1] = dst[2*AUDIO_BLOCK_SAMPLES-1];
}

/* ---- grabación digital ---------------------------------------------------------------------------------------- */

static void rec_free(void)
{
	free(rec.mix); free(rec.out); free(rec.enc);
	rec.mix = NULL; rec.out = NULL; rec.enc = NULL;
	rec.armed = 0;
}

static void rec_arm(void)
{
	struct stat fs;

	rec_free();
	memset(&rec, 0, sizeof rec);
	if (stat(REC_SWITCH, &fs) != 0)
		return;
	rec.mix_cap = REC_LEN_S * REC_MIX_RATE;
	rec.out_cap = REC_LEN_S * REC_OUT_RATE;
	rec.mix = malloc((size_t)rec.mix_cap * 2 * sizeof(int16_t));
	rec.out = malloc((size_t)rec.out_cap * 2 * sizeof(float));
	rec.enc = malloc(REC_CHUNK);
	if (!rec.mix || !rec.out || !rec.enc) {
		salsads_log("audio-grabar: sin memoria para la grabación (%u + %u KB)",
			    (unsigned)((size_t)rec.mix_cap * 4 >> 10), (unsigned)((size_t)rec.out_cap * 8 >> 10));
		rec_free();
		return;
	}
	rec.armed = 1;
	salsads_log("audio-grabar: armado; %u s a partir de los %u s; memoria %u KB (mezcla) + %u KB (salida)", REC_LEN_S, REC_SKIP_S,
		    (unsigned)((size_t)rec.mix_cap * 4 >> 10), (unsigned)((size_t)rec.out_cap * 8 >> 10));
}

/* Llamado desde snd_sink (hilo de emulación) con la mezcla nativa que devolvió el SPE. */
void audio_out_record_mix(const int16_t *nat, uint32_t nticks)
{
	uint32_t n = nticks, skip;

	if (!rec.armed || rec.mix_done || !nat)
		return;
	skip = REC_SKIP_S * REC_MIX_RATE;
	if (rec.mix_seen < skip) {
		const uint32_t drop = skip - rec.mix_seen < n ? skip - rec.mix_seen : n;

		rec.mix_seen += drop;
		nat += 2 * drop;
		n -= drop;
	}
	if (rec.mix_seen < skip || !n)
		return;
	if (n > rec.mix_cap - rec.mix_n)
		n = rec.mix_cap - rec.mix_n;
	memcpy(rec.mix + 2 * (size_t)rec.mix_n, nat, (size_t)n * 4);
	rec.mix_n += n;
	if (rec.mix_n == rec.mix_cap)
		rec.mix_done = 1;
}

static int rec_write_chunk(int is_float, const void *data, size_t total)
{
	const size_t sz = is_float ? 4u : 2u, per = REC_CHUNK / sz;
	size_t n = total - rec.off < per ? total - rec.off : per;
	size_t bytes = is_float ? wavrec_encode_f32(rec.enc, (const float *)data + rec.off, n) :
				  wavrec_encode_i16(rec.enc, (const int16_t *)data + rec.off, n);

	if (fwrite(rec.enc, 1, bytes, rec.f) != bytes)
		return -1;
	rec.off += n;
	return 0;
}

/* Avanza la escritura de los archivos (un trozo de 32 KB por llamada; con `all`, hasta el final). Hilo de emulación. */
static void rec_step(int all)
{
	uint8_t h[WAVREC_HDR_FLOAT];

	while (rec.armed && !rec.finished) {
		if (rec.stage == 0) {
			if (!all && !(rec.mix_done && __atomic_load_n(&rec.out_done, __ATOMIC_ACQUIRE)))
				return;
			rec.stage = 1;
		}
		if (rec.stage == 1 || rec.stage == 2) {
			const int is_out = rec.stage == 2;
			const uint32_t frames = is_out ? rec.out_n : rec.mix_n;

			if (!rec.f) {
				const size_t hl = wavrec_header(h, is_out, 2, is_out ? REC_OUT_RATE : REC_MIX_RATE, frames);

				rec.f = fopen(is_out ? REC_PATH_OUT : REC_PATH_MIX, "wb");
				rec.off = 0;
				if (!rec.f || fwrite(h, 1, hl, rec.f) != hl) {
					salsads_log("audio-grabar: no se pudo abrir %s", is_out ? REC_PATH_OUT : REC_PATH_MIX);
					if (rec.f) fclose(rec.f);
					rec.f = NULL;
					rec.finished = 1;
					return;
				}
			}
			if (rec_write_chunk(is_out, is_out ? (const void *)rec.out : (const void *)rec.mix, (size_t)frames * 2) != 0) {
				salsads_log("audio-grabar: error de escritura");
				fclose(rec.f); rec.f = NULL;
				rec.finished = 1;
				return;
			}
			if (rec.off >= (size_t)frames * 2) {
				fclose(rec.f);
				rec.f = NULL;
				rec.stage++;
			}
			if (!all)
				return;
			continue;
		}
		/* stage 3: informe */
		salsads_log("audio-grabar: %s (%u.%02u s, %u Hz, 16 bits) y %s (%u.%02u s, 48000 Hz, float32, 2 canales); "
			    "muestras perdidas por el anillo %llu, de silencio %llu, faltas %u, bloques tarde %u; %s",
			    REC_PATH_MIX, rec.mix_n / REC_MIX_RATE, rec.mix_n % REC_MIX_RATE * 100 / REC_MIX_RATE, REC_MIX_RATE,
			    REC_PATH_OUT, rec.out_n / REC_OUT_RATE, rec.out_n % REC_OUT_RATE * 100 / REC_OUT_RATE,
			    (unsigned long long)rec.dropped, (unsigned long long)rec.silent, (unsigned)rec.under, (unsigned)rec.late,
			    rec.mix_done && rec.out_done ? "completa" : "parcial (se cerró el juego antes de los 25 s)");
		rec.finished = 1;
		rec_free();
	}
}

void audio_out_record_service(void)
{
	if (rec.armed && !rec.finished)
		rec_step(0);
}

static void rec_close(void)
{
	if (!rec.armed)
		return;
	if (!rec.out_done) { /* parcial: el hilo de audio ya está parado */
		rec.silent = c_silent - rec.silent0; rec.dropped = c_dropped - rec.dropped0;
		rec.under = c_underruns - rec.under0; rec.late = c_late - rec.late0;
	}
	if (rec.mix_n || rec.out_n)
		rec_step(1);
	if (rec.armed)
		rec_free();
}

void audio_out_set_active(int enabled)
{
	if (!port.open) return;
	if (enabled) {
		rec_arm();
		__atomic_store_n(&active, 1, __ATOMIC_RELEASE);
		while (__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) sysUsleep(1000);
	} else {
		__atomic_store_n(&active, 0, __ATOMIC_RELEASE);
		/* Solo entre juegos, después de esperar al productor. El consumidor
		 * confirma que descartó la historia antes de permitir otra apertura. */
		while (!__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) sysUsleep(1000);
		rec_close();
	}
}

static void audio_thread(void *arg)
{
	const uint32_t n = (uint32_t)port.config.numBlocks;
	uint64_t rabs = 0, next = 1;
	uint32_t last = 0;

	(void)arg;
	while (!port.quit) {
		sys_event_t ev;
		uint32_t rmod;
		uint64_t t0;

		sysEventQueueReceive(port.queue, &ev, 100000);
		t0 = sysGetSystemTime();
		rmod = (uint32_t)(*(volatile uint64_t *)(uintptr_t)port.config.readIndex % n);
		rabs += (rmod + n - last) % n;
		last = rmod;
		if (next <= rabs) {
			c_late++;
			next = rabs + 1;
		}
		while (next <= rabs + LEAD) {
			fill_block(block_ptr((uint32_t)(next % n)));
			next++;
			c_blocks++;
		}
		c_busy += sysGetSystemTime() - t0;
	}
	sysThreadExit(0);
}

int audio_out_init(void)
{
	audioPortParam params = { AUDIO_PORT_2CH, AUDIO_BLOCK_16, AUDIO_PORT_INITLEVEL, 1.0f };
	int rc;
	struct stat fs;
	stretch_v1 = stat("/dev_hdd0/salsads/audio-estirar-v1", &fs) == 0;
	no_stretch = stat("/dev_hdd0/salsads/audio-sin-estirar", &fs) == 0;
	no_limiter = stat("/dev_hdd0/salsads/audio-sin-limitador", &fs) == 0;
	active = 0; paused = 1;
	out_gain = 1.0f; gain_db = 0;
	{
		FILE *g = fopen("/dev_hdd0/salsads/audio-ganancia", "r");
		float db;

		if (g) {
			if (fscanf(g, "%f", &db) == 1 && db > -60.0f && db < 20.0f) {
				gain_db = db;
				out_gain = powf(10.0f, db / 20.0f);
			}
			fclose(g);
		}
	}
	c_lim_total = c_lim_n = 0; c_lim_sum_db = 0; c_lim_max_db = 0;
	pending_pos = STRETCH_HOP;
	memset(&stretch, 0, sizeof stretch);
	memset(&stretch2, 0, sizeof stretch2);
	c_inserted = c_active = c_stretch_us = c_played = 0;
	c_stretch_max = c_joins = 0; c_rate = 1000;
	control_tail = 0;
	last_sample[0] = last_sample[1] = 0;

	memset(&port, 0, sizeof port);
	head = tail = 0;
	c_pushed = c_dropped = c_blocks = c_silent = c_busy = 0;
	c_underruns = c_late = c_peak = 0;
	if ((rc = audioInit()) != 0)
		return rc;
	if ((rc = audioPortOpen(&params, &port.port)) != 0 || (rc = audioGetPortConfig(port.port, &port.config)) != 0) {
		audioQuit();
		return rc ? rc : -1;
	}
	if ((rc = audioCreateNotifyEventQueue(&port.queue, &port.key)) != 0 || (rc = audioSetNotifyEventQueue(port.key)) != 0) {
		audioPortClose(port.port);
		audioQuit();
		return rc;
	}
	sysEventQueueDrain(port.queue);
	memset(block_ptr(0), 0, (size_t)port.config.numBlocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
	if ((rc = audioPortStart(port.port)) != 0 ||
	    (rc = sysThreadCreate(&port.thread, audio_thread, NULL, THREAD_PRIO, 0x10000, THREAD_JOINABLE, "salsads_audio")) != 0) {
		audioPortStop(port.port);
		audioRemoveNotifyEventQueue(port.key);
		audioPortClose(port.port);
		sysEventQueueDestroy(port.queue, 0);
		audioQuit();
		return rc;
	}
	port.open = 1;
	return 0;
}

int audio_out_is_open(void)
{
	return port.open;
}

float audio_out_ratio_adj(void)
{
	if (!port.open || !port.started)
		return 0.0f;
	return audio_rate_adjust(level(), TARGET);
}

void audio_out_stats(struct audio_out_stats *s, int reset_peak)
{
	s->joins = c_joins;
	s->rate_permil = c_rate;
	s->grain_ms = stretch_v1 ? 15 : STRETCH2_GRAIN / 48;
	s->played = c_played;
	s->stretch_max_us = c_stretch_max;
	s->inserted = c_inserted;
	s->active = c_active;
	s->stretch_us = c_stretch_us;
	s->stretch_enabled = no_stretch ? 0 : stretch_v1 ? 1 : 2;
	s->limiter_enabled = !no_limiter;
	s->pushed = c_pushed;
	s->dropped = c_dropped;
	s->blocks = c_blocks;
	s->silent = c_silent;
	s->underruns = c_underruns;
	s->late = c_late;
	s->level = level();
	s->peak = c_peak;
	s->busy_us = c_busy;
	s->lim_total = c_lim_total;
	s->lim_n = c_lim_n;
	s->lim_sum_db = c_lim_sum_db;
	s->lim_max_db = c_lim_max_db;
	s->gain_db = gain_db;
	if (reset_peak) {
		c_peak = 0;
		c_lim_total = c_lim_n = 0;
		c_lim_sum_db = 0;
		c_lim_max_db = 0;
	}
}

void audio_out_shutdown(void)
{
	u64 rv = 0;

	if (!port.open)
		return;
	port.quit = 1;
	sysThreadJoin(port.thread, &rv);
	audioPortStop(port.port);
	audioRemoveNotifyEventQueue(port.key);
	audioPortClose(port.port);
	if (sysEventQueueDestroy(port.queue, 0))
		sysEventQueueDestroy(port.queue, 1);
	audioQuit();
	port.open = 0;
}
