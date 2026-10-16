/* SPDX-License-Identifier: MIT */
/*
 * spukit — lado PPE: trabajadores SPU persistentes y su cola de trabajos.
 *
 * Protocolo (ver spukit/queue.h y spukit/spu.h):
 *   PPE  escribe jobs[slot], lwsync, head.posted = seq, sync, timbre
 *        (sysSpuThreadWriteMb con seq).
 *   SPU  duerme en su mailbox; al despertar lee head por DMA y atiende todo
 *        lo publicado (releyendo head después de cada trabajo); por cada
 *        uno escribe results[slot] y después status.done = seq.
 * Cada publicación toca el timbre: un timbre de más solo despierta al SPU
 * para encontrar la cola vacía, y uno perdido (mailbox llena) no importa
 * porque los que esperan en la mailbox ya lo van a despertar.
 */
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/spu.h>
#include <sys/systime.h>

#include "spukit/spukit.h"

#define ptr2ea(x) ((uint64_t)(uintptr_t)(x))

struct spukit_worker {
	int used;
	sysSpuImage image;
	sys_spu_group_t group;
	sys_spu_thread_t thread;
	spukit_ctrl *ctrl;
	uint32_t seq;  /* último publicado */
	spukit_stats stats;
	char name[32];
};

static struct spukit_worker workers[SPUKIT_MAX_WORKERS];
static int last_error;
static int initialized;

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }
static inline void hwsync(void) { __asm__ volatile("sync" ::: "memory"); }
/* Prioridad del hilo de hardware del PPE mientras se espera (HMT). */
static inline void hmt_low(void) { __asm__ volatile("or 1,1,1"); }
static inline void hmt_medium(void) { __asm__ volatile("or 2,2,2"); }

int spukit_init(uint32_t spus)
{
	if (initialized)
		return SPUKIT_OK;
	/* Puede fallar si otro componente ya lo llamó: no es fatal. */
	sysSpuInitialize(spus, 0);
	initialized = 1;
	return SPUKIT_OK;
}

int spukit_last_error(void)
{
	return last_error;
}

uint64_t spukit_tick_hz(void)
{
	return sysGetTimebaseFrequency();
}

spukit_worker *spukit_worker_start(const void *elf, const char *name, uint64_t arg)
{
	struct spukit_worker *w = NULL;
	for (int i = 0; i < SPUKIT_MAX_WORKERS; i++)
		if (!workers[i].used) {
			w = &workers[i];
			break;
		}
	if (!w || !elf) {
		last_error = SPUKIT_EARG;
		return NULL;
	}
	memset(w, 0, sizeof *w);
	snprintf(w->name, sizeof w->name, "%s", name ? name : "spukit");

	w->ctrl = memalign(128, sizeof *w->ctrl);
	if (!w->ctrl) {
		last_error = SPUKIT_ENOMEM;
		return NULL;
	}
	memset(w->ctrl, 0, sizeof *w->ctrl);
	hwsync();

	if (sysSpuImageImport(&w->image, elf, 0) != 0) {
		free(w->ctrl);
		last_error = SPUKIT_EIMAGE;
		return NULL;
	}
	sysSpuThreadGroupAttribute gattr = { strlen(w->name) + 1, w->name, 0, { 0 } };
	sysSpuThreadAttribute tattr = { w->name, strlen(w->name) + 1, SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument targ = { ptr2ea(w->ctrl), arg, 0, 0 };
	if (sysSpuThreadGroupCreate(&w->group, 1, 100, &gattr) != 0) {
		sysSpuImageClose(&w->image);
		free(w->ctrl);
		last_error = SPUKIT_ETHREAD;
		return NULL;
	}
	if (sysSpuThreadInitialize(&w->thread, w->group, 0, &w->image, &tattr, &targ) != 0 ||
	    sysSpuThreadGroupStart(w->group) != 0) {
		sysSpuThreadGroupDestroy(w->group);
		sysSpuImageClose(&w->image);
		free(w->ctrl);
		last_error = SPUKIT_ETHREAD;
		return NULL;
	}
	w->used = 1;
	last_error = SPUKIT_OK;
	return w;
}

static void ring(struct spukit_worker *w, uint32_t value)
{
	hwsync();  /* head y el descriptor, visibles antes del timbre */
	sysSpuThreadWriteMb(w->thread, value);
	w->stats.doorbells++;
}

uint32_t spukit_post(spukit_worker *w, uint32_t kind, const void *data, uint32_t size)
{
	if (size > SPUKIT_DATA_SIZE)
		size = SPUKIT_DATA_SIZE;
	uint32_t seq = w->seq + 1;
	if (seq == 0)  /* 0 significa "ninguno" */
		seq = 1;

	/* Cola llena: el lugar de seq lo ocupa seq - LEN, que debe haber terminado. */
	if (seq > SPUKIT_QUEUE_LEN && !spukit_done(w, seq - SPUKIT_QUEUE_LEN)) {
		uint64_t t0 = sysGetSystemTime();
		spukit_wait(w, seq - SPUKIT_QUEUE_LEN);
		w->stats.full_us += sysGetSystemTime() - t0;
	}

	spukit_job *job = &w->ctrl->jobs[SPUKIT_SLOT(seq)];
	job->kind = kind;
	job->size = size;
	if (size)
		memcpy(job->data, data, size);
	job->seq = seq;
	lwsync();
	w->ctrl->head.posted = seq;
	w->seq = seq;
	w->stats.posted++;
	ring(w, seq);
	return seq;
}

int spukit_done(const spukit_worker *w, uint32_t seq)
{
	return (int32_t)(w->ctrl->status.done - seq) >= 0;
}

const spukit_result *spukit_wait(spukit_worker *w, uint32_t seq)
{
	if (!spukit_done(w, seq)) {
		uint64_t t0 = sysGetSystemTime(), t;
		w->stats.waits++;
		/* Unos 50 µs girando con prioridad baja (deja el núcleo al otro
		 * hilo del PPE); después, a dormir de a poco. */
		hmt_low();
		while (!spukit_done(w, seq) && (t = sysGetSystemTime()) - t0 < 50)
			;
		hmt_medium();
		while (!spukit_done(w, seq))
			sysUsleep(20);
		w->stats.wait_us += sysGetSystemTime() - t0;
	}
	lwsync();  /* el resultado, después de ver done */
	return &w->ctrl->results[SPUKIT_SLOT(seq)];
}

int spukit_worker_stop(spukit_worker *w)
{
	if (!w || !w->used)
		return SPUKIT_EARG;
	w->ctrl->head.quit = 1;
	ring(w, 0);
	u32 cause = 0, status = 0;
	s32 exit_status = -1;
	sysSpuThreadGroupJoin(w->group, &cause, &status);
	sysSpuThreadGetExitStatus(w->thread, &exit_status);
	sysSpuThreadGroupDestroy(w->group);
	sysSpuImageClose(&w->image);
	free(w->ctrl);
	w->used = 0;
	return exit_status;
}

void spukit_get_stats(const spukit_worker *w, spukit_stats *out)
{
	*out = w->stats;
	out->wakeups = w->ctrl->status.wakeups;
}
