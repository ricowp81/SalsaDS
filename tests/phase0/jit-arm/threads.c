/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.7 (c): el ARM7 en el hilo 1 del PPE. Ver threads.h.
 */
#include "threads.h"

#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/sem.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "fixed.h"
#include "platform/ps3/log.h"

enum wait_kind { W_NONE, W_SPIN, W_SPIN_LOW, W_USLEEP, W_SEM, W_EVQ };
static const char *const wait_name[] = {
	"sin hilo del ARM7", "gira", "gira con prioridad baja de SMT", "duerme con sysUsleep(50)", "semáforo de lv2",
	"cola de eventos de lv2",
};

#define MAX_LAT 40000

static struct {
	volatile int stop;
	volatile uint64_t seq;        /* publicaciones del ARM9 */
	volatile uint64_t post_tb;    /* timebase de la última */
	enum wait_kind kind;
	sys_sem_t sem;
	sys_event_queue_t queue;
	sys_event_port_t port;
	uint64_t work_ticks;          /* trabajo del ARM7 en cada despertar */
	/* medidas del hilo del ARM7 */
	uint32_t lat[MAX_LAT];
	volatile uint32_t nlat;
} sh;

static inline uint64_t tb_now(void)
{
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static void arm7_work(void)
{
	const uint64_t t = tb_now() + sh.work_ticks;
	while (tb_now() < t)
		;
}

static void arm7_note(uint64_t seen_seq, uint64_t post_tb)
{
	(void)seen_seq;
	const uint64_t lat = tb_now() - post_tb;
	if (sh.nlat < MAX_LAT)
		sh.lat[sh.nlat++] = (uint32_t)lat;
}

static void arm7_thread(void *arg)
{
	(void)arg;
	uint64_t last = 0;
	switch (sh.kind) {
	case W_SPIN:
		while (!sh.stop) {
			const uint64_t s = sh.seq;
			if (s != last) {
				last = s;
				arm7_note(s, sh.post_tb);
				arm7_work();
			}
		}
		break;
	case W_SPIN_LOW:
		while (!sh.stop) {
			__asm__ volatile("or 1,1,1");  /* prioridad baja del hilo de hardware */
			const uint64_t s = sh.seq;
			if (s != last) {
				__asm__ volatile("or 2,2,2");  /* media */
				last = s;
				arm7_note(s, sh.post_tb);
				arm7_work();
			}
		}
		__asm__ volatile("or 2,2,2");
		break;
	case W_USLEEP:
		while (!sh.stop) {
			sysUsleep(50);
			const uint64_t s = sh.seq;
			if (s != last) {
				last = s;
				arm7_note(s, sh.post_tb);
				arm7_work();
			}
		}
		break;
	case W_SEM:
		while (!sh.stop) {
			if (sysSemWait(sh.sem, 0) != 0)
				break;
			if (sh.stop)
				break;
			arm7_note(sh.seq, sh.post_tb);
			arm7_work();
		}
		break;
	case W_EVQ:
		while (!sh.stop) {
			sys_event_t ev;
			if (sysEventQueueReceive(sh.queue, &ev, 0) != 0)
				break;
			if (sh.stop)
				break;
			arm7_note(sh.seq, sh.post_tb);
			arm7_work();
		}
		break;
	default:
		break;
	}
	sysThreadExit(0);
}

static int cmp_u32(const void *a, const void *b)
{
	const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

struct scen_result {
	double mips;
	double post_us_avg, post_us_max;
	uint32_t n, lat_med_us10, lat_p99_us10, lat_max_us10;  /* décimas de µs */
};

/* Una ventana: el ARM9 corre trozos y publica un evento cada `period_us`. */
static void window(arm9_chunk_fn chunk, void *ctx, double tb_hz, double secs, double period_us, struct scen_result *r)
{
	const uint64_t period = (uint64_t)(period_us * tb_hz / 1e6);
	const uint64_t span = (uint64_t)(secs * tb_hz);
	uint64_t instr = 0, post_sum = 0, post_max = 0, posts = 0;
	sh.nlat = 0;
	const uint64_t t0 = tb_now();
	uint64_t next = t0 + period;
	for (;;) {
		instr += chunk(ctx);
		const uint64_t now = tb_now();
		if (now - t0 >= span)
			break;
		if (sh.kind != W_NONE && now >= next) {
			sh.post_tb = tb_now();
			sh.seq++;
			const uint64_t a = tb_now();
			if (sh.kind == W_SEM)
				sysSemPost(sh.sem, 1);
			else if (sh.kind == W_EVQ)
				sysEventPortSend(sh.port, sh.seq, 0, 0);
			const uint64_t d = tb_now() - a;
			post_sum += d;
			if (d > post_max)
				post_max = d;
			posts++;
			next += period;
			if (next < now)
				next = now + period;
		}
	}
	const double el = (double)(tb_now() - t0) / tb_hz;
	r->mips = (double)instr / el / 1e6;
	r->post_us_avg = posts ? (double)post_sum / (double)posts * 1e6 / tb_hz : 0;
	r->post_us_max = (double)post_max * 1e6 / tb_hz;
	r->n = sh.nlat;
	if (sh.nlat) {
		uint32_t tmp[MAX_LAT];
		memcpy(tmp, (const void *)sh.lat, sh.nlat * sizeof tmp[0]);
		qsort(tmp, sh.nlat, sizeof tmp[0], cmp_u32);
		const double k = 1e7 / tb_hz;  /* décimas de µs por tick */
		r->lat_med_us10 = (uint32_t)(tmp[sh.nlat / 2] * k);
		r->lat_p99_us10 = (uint32_t)(tmp[(sh.nlat * 99) / 100 < sh.nlat ? (sh.nlat * 99) / 100 : sh.nlat - 1] * k);
		r->lat_max_us10 = (uint32_t)(tmp[sh.nlat - 1] * k);
	} else {
		r->lat_med_us10 = r->lat_p99_us10 = r->lat_max_us10 = 0;
	}
}

static int cmp_dbl(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static int run_scenario(enum wait_kind kind, double period_us, arm9_chunk_fn chunk, void *ctx, double tb_hz,
			double secs, int reps, double base_mips, struct scen_result *out)
{
	memset((void *)&sh.stop, 0, sizeof sh.stop);
	sh.stop = 0;
	sh.seq = 0;
	sh.kind = kind;
	sh.work_ticks = (uint64_t)(2.0 * tb_hz / 1e6);  /* 2 µs de ARM7 por despertar */
	sys_ppu_thread_t tid = 0;
	int have_thread = 0;

	if (kind == W_SEM) {
		sys_sem_attr_t a;
		memset(&a, 0, sizeof a);
		a.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
		a.attr_pshared = SYS_SEM_ATTR_PSHARED;
		memcpy(a.name, "arm7sem", 8);
		if (sysSemCreate(&sh.sem, &a, 0, 1 << 20) != 0)
			return -1;
	} else if (kind == W_EVQ) {
		sys_event_queue_attr_t qa;
		memset(&qa, 0, sizeof qa);
		qa.attr_protocol = SYS_EVENT_QUEUE_PRIO;
		qa.type = SYS_EVENT_QUEUE_PPU;
		memcpy(qa.name, "arm7evq", 8);
		int rc = sysEventQueueCreate(&sh.queue, &qa, SYS_EVENT_QUEUE_KEY_LOCAL, 127);
		if (rc != 0) {
			salsads_log("hilos: sysEventQueueCreate = 0x%08x", (unsigned)rc);
			return -1;
		}
		rc = sysEventPortCreate(&sh.port, SYS_EVENT_PORT_LOCAL, SYS_EVENT_PORT_NO_NAME);
		if (rc == 0)
			rc = sysEventPortConnectLocal(sh.port, sh.queue);
		if (rc != 0) {
			salsads_log("hilos: sysEventPort* = 0x%08x", (unsigned)rc);
			sysEventQueueDestroy(sh.queue, SYS_EVENT_QUEUE_FORCE_DESTROY);
			return -1;
		}
	}
	if (kind != W_NONE) {
		if (sysThreadCreate(&tid, arm7_thread, NULL, 1001, 64 * 1024, THREAD_JOINABLE, "arm7") != 0)
			return -1;
		have_thread = 1;
		sysUsleep(20000);  /* que arranque */
	}
	double mips[8], pavg = 0, pmax = 0;
	struct scen_result r, rl = { 0 };
	if (reps > 8)
		reps = 8;
	for (int i = 0; i < reps; i++) {
		window(chunk, ctx, tb_hz, secs, period_us, &r);
		mips[i] = r.mips;
		pavg += r.post_us_avg / reps;
		if (r.post_us_max > pmax)
			pmax = r.post_us_max;
		if (i == reps - 1)
			rl = r;
	}
	qsort(mips, (size_t)reps, sizeof mips[0], cmp_dbl);
	rl.mips = mips[reps / 2];
	rl.post_us_avg = pavg;
	rl.post_us_max = pmax;
	*out = rl;

	if (have_thread) {
		sh.stop = 1;
		if (kind == W_SEM)
			sysSemPost(sh.sem, 1);
		else if (kind == W_EVQ)
			sysEventPortSend(sh.port, 0, 0, 0);
		u64 rv;
		sysThreadJoin(tid, &rv);
	}
	if (kind == W_SEM)
		sysSemDestroy(sh.sem);
	if (kind == W_EVQ) {
		sysEventPortDestroy(sh.port);
		sysEventQueueDestroy(sh.queue, SYS_EVENT_QUEUE_FORCE_DESTROY);
	}
	(void)base_mips;
	return 0;
}

int threads_bench(arm9_chunk_fn chunk, void *ctx, double tb_hz, int quick)
{
	const double secs = quick ? 0.05 : 0.4;
	const int reps = quick ? 1 : 5;
	struct scen_result base;
	static const struct {
		enum wait_kind k;
		double period_us;
	} sc[] = {
		{ W_SPIN, 100 }, { W_SPIN_LOW, 100 }, { W_USLEEP, 100 }, { W_SEM, 1000 }, { W_SEM, 100 },
		{ W_SEM, 33 }, { W_EVQ, 1000 }, { W_EVQ, 100 },
	};
	int bad = 0;

	if (run_scenario(W_NONE, 100, chunk, ctx, tb_hz, secs, reps, 0, &base) != 0)
		return -1;
	salsads_log("DATO hilos %-34s ARM9 %u.%02u MIPS (referencia)", wait_name[W_NONE], F2(base.mips));
	for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
		struct scen_result r;
		if (run_scenario(sc[i].k, sc[i].period_us, chunk, ctx, tb_hz, secs, reps, base.mips, &r) != 0) {
			salsads_log("hilos: no pude armar «%s»", wait_name[sc[i].k]);
			bad = -1;
			continue;
		}
		salsads_log("DATO hilos %-34s despertar cada %4u µs: ARM9 %u.%02u MIPS (%c%u.%02u %% contra sin hilo del ARM7); "
			    "costo de avisar al ARM7 en el ARM9: %u.%02u µs de media, %u.%02u máx; latencia de despertar (%u): "
			    "mediana %u.%u µs, p99 %u.%u, máx %u.%u",
			    wait_name[sc[i].k], (unsigned)sc[i].period_us, F2(r.mips), SGN(r.mips / base.mips - 1.0),
			    F2((r.mips / base.mips - 1.0) * 100.0), F2(r.post_us_avg), F2(r.post_us_max), r.n, r.lat_med_us10 / 10, r.lat_med_us10 % 10, r.lat_p99_us10 / 10,
			    r.lat_p99_us10 % 10, r.lat_max_us10 / 10, r.lat_max_us10 % 10);
	}
	return bad;
}
