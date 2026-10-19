/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — salida de audio de la PS3. Ver audio_out.h. Código propio; el orden de apertura y cierre del puerto es el de
 * PSL1GHT (audio/audio.h) y lo que Salsa64 midió en la consola: un recurso de audio que quede enganchado al proceso puede
 * colgar la consola al salir, así que cada paso del cierre va con su resultado a una variable que el programa principal
 * puede llevar al log.
 */
#include "platform/ps3/audio_out.h"

#include <audio/audio.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>

#define RING_FRAMES 16384u /* potencia de 2: 340 ms */
#define LEAD 4u            /* bloques llenos por delante del que suena */
#define PREBUF 2048u       /* muestras que se juntan antes de empezar a sonar */
#define TARGET 3072u       /* nivel que busca el control de deriva (64 ms) */
#define THREAD_PRIO 100

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
	return __atomic_load_n(&head, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
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

static void fill_block(float *dst)
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
	c_peak = peak;
	__atomic_store_n(&tail, t + got, __ATOMIC_RELEASE);
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
	float adj;

	if (!port.open || !port.started)
		return 0.0f;
	adj = ((float)level() - (float)TARGET) * (0.02f / (float)TARGET);
	return adj > 0.02f ? 0.02f : adj < -0.02f ? -0.02f : adj;
}

void audio_out_stats(struct audio_out_stats *s, int reset_peak)
{
	s->pushed = c_pushed;
	s->dropped = c_dropped;
	s->blocks = c_blocks;
	s->silent = c_silent;
	s->underruns = c_underruns;
	s->late = c_late;
	s->level = level();
	s->peak = c_peak;
	s->busy_us = c_busy;
	if (reset_peak)
		c_peak = 0;
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
