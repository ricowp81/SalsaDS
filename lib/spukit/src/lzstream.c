/* SPDX-License-Identifier: MIT */
/*
 * spukit — compresión en flujo (lado PPE). Ver spukit/lzstream.h.
 *
 * El anillo de salida son SPUKIT_LZS_SLOTS búferes de SPUKIT_LZ_BOUND(64 KB)
 * + 32 bytes: el PPE publica hasta ocho bloques por delante, el SPE escribe
 * cada resultado en su lugar y el PPE los va entregando en orden. Un bloque
 * que el SPE no logra achicar se entrega desde el búfer de origen, sin
 * copia.
 */
#include <malloc.h>
#include <string.h>
#include <sys/systime.h>

#include "spukit/lzjob.h"
#include "spukit/lzstream.h"
#include "spukit/spukit.h"

#define SLOT_BYTES ((SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK) + 32u + 127u) & ~127u)

struct spukit_lzs {
	spukit_worker *w;
	uint8_t *ring;  /* SPUKIT_LZS_SLOTS * SLOT_BYTES */
	uint16_t table[SPUKIT_LZ_TABLE_ENTRIES] __attribute__((aligned(16)));
};

spukit_lzs *spukit_lzs_open(const void *spu_elf)
{
	spukit_lzs *z = memalign(128, (sizeof *z + 127u) & ~127u);
	if (!z)
		return NULL;
	memset(z, 0, sizeof *z);
	z->ring = memalign(128, SPUKIT_LZS_SLOTS * SLOT_BYTES);
	if (!z->ring) {
		free(z);
		return NULL;
	}
	if (spu_elf)
		z->w = spukit_worker_start(spu_elf, "spukit_lz", 0);
	return z;
}

void spukit_lzs_close(spukit_lzs *z)
{
	if (!z)
		return;
	if (z->w)
		spukit_worker_stop(z->w);
	free(z->ring);
	free(z);
}

int spukit_lzs_has_spu(const spukit_lzs *z)
{
	return z && z->w;
}

uint32_t spukit_lzs_ppe_block(spukit_lzs *z, const void *src, uint32_t n, void *out)
{
	size_t c = spukit_lz_compress(src, n, out, SPUKIT_LZ_BOUND(n) + 16, z->table);
	return c && c < n ? (uint32_t)c : 0;
}

static uint8_t *slot(spukit_lzs *z, uint32_t b)
{
	return z->ring + (b % SPUKIT_LZS_SLOTS) * SLOT_BYTES;
}

static void post(spukit_lzs *z, const uint8_t *src, uint32_t len, uint32_t b, uint32_t *seqs)
{
	uint32_t off = b * SPUKIT_LZ_BLOCK;
	struct spukit_lz_job job = { (uint64_t)(uintptr_t)(src + off), (uint64_t)(uintptr_t)slot(z, b),
				     len - off < SPUKIT_LZ_BLOCK ? len - off : SPUKIT_LZ_BLOCK, SLOT_BYTES };
	seqs[b % SPUKIT_LZS_SLOTS] = spukit_post(z->w, SPUKIT_LZ_JOB_COMPRESS, &job, sizeof job);
}

int spukit_lzs_run(spukit_lzs *z, const void *src_, uint32_t len, spukit_lzs_sink sink, void *ctx,
		   spukit_lzs_stats *stats)
{
	const uint8_t *src = src_;
	spukit_lzs_stats st;
	memset(&st, 0, sizeof st);
	uint64_t t0 = sysGetSystemTime();
	uint32_t nblocks = (len + SPUKIT_LZ_BLOCK - 1) / SPUKIT_LZ_BLOCK;
	uint32_t seqs[SPUKIT_LZS_SLOTS] = { 0 };
	uint32_t posted = 0, done = 0;
	int rc = 0;

	while (done < nblocks) {
		while (z->w && posted < nblocks && posted - done < SPUKIT_LZS_SLOTS)
			post(z, src, len, posted++, seqs);

		uint32_t off = done * SPUKIT_LZ_BLOCK;
		uint32_t n = len - off < SPUKIT_LZ_BLOCK ? len - off : SPUKIT_LZ_BLOCK;
		const uint8_t *out = slot(z, done);
		uint32_t clen;
		if (z->w) {
			const spukit_result *r = spukit_wait(z->w, seqs[done % SPUKIT_LZS_SLOTS]);
			struct spukit_lz_result lr;
			memcpy(&lr, r->data, sizeof lr);
			st.spu_ticks += r->ticks;
			if (r->error) {
				clen = spukit_lzs_ppe_block(z, src + off, n, (void *)out);
				st.ppe_blocks++;
			} else {
				clen = lr.clen;
				st.spu_blocks++;
			}
		} else {
			clen = spukit_lzs_ppe_block(z, src + off, n, (void *)out);
			st.ppe_blocks++;
		}
		uint8_t hdr[4];
		const void *body = clen ? (const void *)out : (const void *)(src + off);
		uint32_t blen = clen ? clen : n;
		spukit_lz_frame(hdr, blen, clen == 0);
		done++;
		if (rc == 0) {
			uint64_t t1 = sysGetSystemTime();
			if (sink(ctx, hdr, 4) != 0 || sink(ctx, body, blen) != 0)
				rc = -1;
			st.sink_us += sysGetSystemTime() - t1;
			st.blocks++;
			st.stored += clen == 0;
			st.in_bytes += n;
			st.out_bytes += 4 + blen;
		}
		/* Con la salida caída se siguen consumiendo los trabajos ya
		 * publicados (ni se publican más: posted queda donde está si
		 * z->w, y sin SPE no hay nada en vuelo), así el anillo queda
		 * limpio para la próxima llamada. */
		if (rc != 0 && !z->w)
			break;
		if (rc != 0)
			nblocks = posted;
	}
	st.wall_us = sysGetSystemTime() - t0;
	if (stats)
		*stats = st;
	return rc;
}
