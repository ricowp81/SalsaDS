/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Adaptación de desmume/src/SPU.cpp y SPU.h, commit
 * fc504ee0bc9ac3434cafcaae7b129c025499a609 (TASEmulators/desmume).
 * Copyright (C) 2006 yopyop
 * Copyright (C) 2006 Theo Berkau
 * Copyright (C) 2008-2025 DeSmuME team
 * Ideas borrowed from Stephane Dallongeville's SCSP core
 *
 * This file is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this software. If not, see <https://www.gnu.org/licenses/>.
 *
 */
/* Mezcla portable PPE/SPE: mismo reloj entero y misma captura.
 * Historia de origen y diferencias en docs/THIRD_PARTY.md y docs/fase2.md.
 * Núcleo anterior seleccionable con SND_MODE_CORE_OLD. */
#include "audio/snd.h"
#include <stddef.h>
#include "audio/snd_fir.h"
#ifdef SND_DIAGNOSTIC
extern void snd_trace_tick(const snd_state *, uint64_t, const int16_t *, const int16_t *);
#endif
#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

static const int16_t ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
	118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
	6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};
static const int8_t ima_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

#define CAP_STAGE 1024
typedef struct cap_run {
	uint8_t buf[CAP_STAGE] __attribute__((aligned(16)));
	uint32_t addr, n;
	snd_state *coherent; /* solo el núcleo nuevo: actualizar las líneas al vaciar */
} cap_run;

#include "audio/snd_voice.h"

void snd_reset(snd_state *st)
{
	uint8_t *p = (uint8_t *)st;

	for (uint32_t i = 0; i < sizeof *st; i++)
		p[i] = 0;
	for (int c = 0; c < SND_NCH; c++)
		st->ch[c].tag = 0xFFFFFFFFu;
	st->bias = 0x200;
}

/* ---- acceso a la memoria del ARM7 ------------------------------------------------------------------------- */

static inline uint32_t fetch_byte(const snd_bus *bus, snd_chan *c, uint32_t addr)
{
	const uint32_t line = addr & ~(uint32_t)(SND_LINE - 1);

	if (c->tag != line) {
		bus->fetch(bus->ctx, line, c->line);
		c->tag = line;
	}
	return c->line[addr & (SND_LINE - 1)];
}

/* ---- escrituras a registros -------------------------------------------------------------------------------- */

static void chan_start(snd_chan *c, int idx)
{
	(void)idx;
	c->on = 1;
	c->tcnt = c->tmr;
	c->pos = ((c->cnt >> 29) & 3u) == 3u ? 0 : -3;
	c->out = 0;
	c->hdr = 0;
	c->adv = c->adi = c->alv = c->ali = 0;
	c->lfsr = 0x7FFF;
	c->tag = 0xFFFFFFFFu;
}

static void cap_start(snd_cap *k)
{
	k->on = 1;
	k->pos = 0;
	k->base = k->dad & 0x07FFFFFCu;
	k->bytes = (k->len ? k->len : 1u) * 4u;
}

static void apply(snd_state *st, const snd_ent *e, const snd_bus *bus, cap_run *cr)
{
	const uint32_t off = e->off;
	const uint32_t v = e->val, m = e->mask;

	if (off < 0x100u) {
		snd_chan *c = &st->ch[off >> 4];

		switch (off & 15u) {
		case 0:
			c->cnt = (c->cnt & ~m) | (v & m);
			break;
		case 2: {
			const uint32_t hi = (c->cnt >> 16) & 0x7FFFu;
			const uint32_t nhi = (hi & ~(m & 0x7FFFu)) | (v & m & 0x7FFFu);

			c->cnt = (c->cnt & 0xFFFFu) | (nhi << 16);
			if (m & 0x8000u) {
				const int want = (v & 0x8000u) != 0;

				c->keyon = (uint8_t)want;
				if (want && !c->on && ((st->mode & SND_MODE_CORE_OLD) || (st->soundcnt & 0x8000u))) {
					chan_start(c, (int)(off >> 4));
					if (!(st->mode & SND_MODE_CORE_OLD)) voice_start(c, bus, cr);
				}
				else if (!want && c->on) {
					c->on = 0;
					c->out = 0;
				}
			}
			break;
		}
		case 4:
			{
				const uint32_t sad = ((c->sad & ~m) | (v & m)) & 0x07FFFFFCu;

				if (sad != c->sad)
					c->tag = 0xFFFFFFFFu;
				c->sad = sad;
			}
			break;
		case 6:
			{
				const uint32_t sad = ((c->sad & ~(m << 16)) | ((v & m) << 16)) & 0x07FFFFFCu;

				if (sad != c->sad)
					c->tag = 0xFFFFFFFFu;
				c->sad = sad;
			}
			break;
		case 8:
			c->tmr = (uint16_t)((c->tmr & ~m) | (v & m));
			break;
		case 10:
			c->pnt = (uint16_t)((c->pnt & ~m) | (v & m));
			break;
		case 12:
			c->len = ((c->len & ~m) | (v & m)) & 0x3FFFFFu;
			break;
		case 14:
			c->len = ((c->len & ~(m << 16)) | ((v & m) << 16)) & 0x3FFFFFu;
			break;
		default:
			break;
		}
		return;
	}
	switch (off) {
	case 0x100:
		st->soundcnt = (uint16_t)((st->soundcnt & ~m) | (v & m)) & 0xBF7Fu;
		if (!(st->mode & SND_MODE_CORE_OLD))
			for (int i = 0; i < SND_NCH; i++) {
				snd_chan *c = &st->ch[i];
				if (!(st->soundcnt & 0x8000u)) c->on = 0, c->out = 0;
				else if (c->keyon && !c->on) {
					chan_start(c, i); voice_start(c, bus, cr);
				}
			}
		break;
	case 0x104:
		st->bias = (uint16_t)(((st->bias & ~m) | (v & m)) & 0x3FFu);
		break;
	case 0x108: /* SNDCAP0CNT (byte bajo) y SNDCAP1CNT (byte alto) */
		for (int i = 0; i < 2; i++) {
			snd_cap *k = &st->cap[i];
			const uint32_t bm = (m >> (8 * i)) & 0xFFu, bv = (v >> (8 * i)) & 0xFFu;

			if (!bm)
				continue;
			{
				const uint32_t old = k->cnt, nw = (old & ~bm) | (bv & bm);

				k->cnt = (uint8_t)(nw & 0x8F);
				if ((nw & 0x80u) && (!(old & 0x80u) || !(st->mode & SND_MODE_CORE_OLD))) {
					cap_start(k);
					k->phase = st->mode & SND_MODE_CORE_OLD ? 0 : st->ch[1 + 2*i].tmr;
					k->fifo_head = k->fifo_count = 0;
					for (int j = 0; j < 16; j++) k->fifo[j] = 0, st->hdcap_fifo[i][j] = 0;
					st->hdcap_written[i] = 0;
				}
				else if ((old & 0x80u) && !(nw & 0x80u))
					k->on = 0;
			}
		}
		break;
	case 0x110:
		st->cap[0].dad = (st->cap[0].dad & ~m) | (v & m);
		break;
	case 0x112:
		st->cap[0].dad = (st->cap[0].dad & ~(m << 16)) | ((v & m) << 16);
		break;
	case 0x114:
		st->cap[0].len = (st->cap[0].len & ~m) | (v & m);
		break;
	case 0x118:
		st->cap[1].dad = (st->cap[1].dad & ~m) | (v & m);
		break;
	case 0x11A:
		st->cap[1].dad = (st->cap[1].dad & ~(m << 16)) | ((v & m) << 16);
		break;
	case 0x11C:
		st->cap[1].len = (st->cap[1].len & ~m) | (v & m);
		break;
	default:
		break;
	}
}

/* ---- un canal: un desbordamiento del temporizador -------------------------------------------------------- */

static void chan_step_old(snd_chan *c, int idx, const snd_bus *bus)
{
	const uint32_t fmt = (c->cnt >> 29) & 3u, mode = (c->cnt >> 27) & 3u;
	uint32_t total, loopstart;

	if (fmt == 3) {
		if (idx >= 14) { /* ruido: LFSR de 15 bits, un paso por desbordamiento */
			const uint32_t carry = c->lfsr & 1u;

			c->lfsr >>= 1;
			if (carry) {
				c->lfsr ^= 0x6000u;
				c->out = -0x7FFF;
			} else {
				c->out = 0x7FFF;
			}
		} else if (idx >= 8) { /* PSG: 8 pasos por ciclo; el nivel alto son los últimos duty + 1 (el 7 no sube nunca) */
			const uint32_t duty = (c->cnt >> 24) & 7u;
			const int hi = duty != 7u && (uint32_t)c->pos >= 7u - duty;

			c->out = hi ? 0x7FFF : -0x7FFF;
			c->pos = (c->pos + 1) & 7;
		} else {
			c->out = 0;
		}
		return;
	}
	if (c->pos < 0 && ++c->pos < 0) /* tres desbordamientos de arranque: el tercero ya lee la muestra 0 */
		return;
	total = (uint32_t)(c->pnt + c->len);
	loopstart = c->pnt;
	if (fmt == 0) {
		total *= 4u;
		loopstart *= 4u;
	} else if (fmt == 1) {
		total *= 2u;
		loopstart *= 2u;
	} else {
		total *= 8u;
		loopstart *= 8u;
	}
	if ((uint32_t)c->pos >= total) {
		if (mode == 2) { /* una vez: se apaga (y la salida queda en cero) */
			c->on = 0;
			c->out = 0;
			return;
		}
		c->pos = (int32_t)loopstart;
		c->tag = 0xFFFFFFFFu; /* el juego puede haber rellenado el búfer antes de esta vuelta */
		if (fmt == 2) {
			c->adv = c->alv;
			c->adi = c->ali;
		}
	}
	if (fmt == 2 && (uint32_t)c->pos == loopstart) {
		c->alv = c->adv;
		c->ali = c->adi;
	}
	if (fmt == 0) {
		c->out = (int32_t)(int8_t)fetch_byte(bus, c, c->sad + (uint32_t)c->pos) * 256;
	} else if (fmt == 1) {
		const uint32_t a = c->sad + (uint32_t)c->pos * 2u;

		c->out = (int32_t)(int16_t)(uint16_t)(fetch_byte(bus, c, a) | fetch_byte(bus, c, a + 1u) << 8);
	} else {
		if (c->pos < 8) {
			/* la cabecera (4 bytes = 8 nibbles): la salida sigue siendo el valor de antes; al llegar al último nibble se
			 * cargan el valor y el índice iniciales (la primera vez; después de un bucle el estado viene del punto de bucle) */
			c->out = c->adv;
			if (c->pos == 7 && !c->hdr) {
				uint32_t h = 0;

				for (uint32_t i = 0; i < 4; i++)
					h |= fetch_byte(bus, c, c->sad + i) << (8 * i);
				c->adv = (int32_t)(int16_t)(uint16_t)h;
				c->adi = (int32_t)((h >> 16) & 0x7Fu);
				if (c->adi > 88)
					c->adi = 88;
				c->hdr = 1;
			}
			c->pos++;
			return;
		} else {
			const uint32_t b = fetch_byte(bus, c, c->sad + ((uint32_t)c->pos >> 1));
			const uint32_t nib = (c->pos & 1) ? b >> 4 : b & 15u;
			const int32_t step = ima_step[c->adi];
			int32_t diff = step >> 3;

			if (nib & 1u)
				diff += step >> 2;
			if (nib & 2u)
				diff += step >> 1;
			if (nib & 4u)
				diff += step;
			c->adv += (nib & 8u) ? -diff : diff;
			if (c->adv > 0x7FFF)
				c->adv = 0x7FFF;
			if (c->adv < -0x7FFF)
				c->adv = -0x7FFF;
			c->adi += ima_adj[nib & 7u];
			if (c->adi < 0)
				c->adi = 0;
			if (c->adi > 88)
				c->adi = 88;
			c->out = c->adv;
		}
	}
	c->pos++;
}

/* ---- captura ------------------------------------------------------------------------------------------------ */

static void cap_flush(const snd_bus *bus, cap_run *r)
{
	if (r->n) {
		bus->store(bus->ctx, r->addr, r->buf, r->n);
		if (r->coherent)
			for (int i = 0; i < SND_NCH; i++) {
				snd_chan *c = &r->coherent->ch[i];
				/* La captura pendiente también se consulta antes de la caché.
				 * Al vaciarla, conservar esos mismos bytes en las líneas vivas. */
				if (c->tag != UINT32_MAX)
					for (uint32_t j = 0; j < SND_LINE; j++) {
						const uint32_t d = c->tag + j - r->addr;
						if (d < r->n) c->line[j] = r->buf[d];
					}
			}
		r->n = 0;
	}
}

static void cap_put(const snd_bus *bus, snd_cap *k, cap_run *r, int32_t v)
{
	const uint32_t size = (k->cnt & 8u) ? 1u : 2u;
	const uint32_t at = k->base + k->pos;

	if (r->n && (r->addr + r->n != at || r->n + size > CAP_STAGE))
		cap_flush(bus, r);
	if (!r->n)
		r->addr = at;
	if (size == 1u) {
		r->buf[r->n++] = (uint8_t)(v >> 8);
	} else {
		r->buf[r->n++] = (uint8_t)v;
		r->buf[r->n++] = (uint8_t)(v >> 8);
	}
	k->pos += size;
	if (k->pos >= k->bytes) {
		if (k->cnt & 4u) {
			k->on = 0;
			k->cnt = (uint8_t)(k->cnt & ~0x80u);
		} else {
			k->pos = 0;
		}
	}
}

/* ---- remuestreo ------------------------------------------------------------------------------------------- */

/*
 * Remuestreo de 32,7 kHz a 48 kHz con un FIR polifásico (tabla de snd_fir.h, generada por tools/gen-snd-fir.py: 32
 * coeficientes, 64 fases con interpolación lineal entre filas, pasabajos de 15 kHz con ventana de Kaiser). Las muestras
 * van planas por canal en wl/wr: las primeras SND_HIST son las últimas del trabajo anterior (`st->hist`) y después las
 * `n` nuevas. Cada salida mira 15 muestras antes y 16 después de su posición, así que sale retrasada 16 muestras
 * nativas (~0,5 ms) y el trabajo acaba con la posición `n + 16` de la ventana, que en el siguiente trabajo vuelve a ser
 * la 16. `st->frac` es la fase dentro de la muestra. La suma usa 4 acumuladores (los carriles de un vector) y se junta
 * como (0+1)+(2+3): lo mismo en C y en el SPE (vectorizado: 4 coeficientes por vector, 8 madd por canal y salida).
 */
#define SND_AFTER_MAX 16 /* muestras mirando hacia delante */
#define FIR_K0 (SND_HIST - SND_AFTER_MAX) /* posición inicial en la ventana: 16 */

#ifdef __SPU__
typedef vec_float4 fir_vec;

static inline void fir_coefs(float *c, const float *h0, const float *h1, float a)
{
	const vec_float4 av = spu_splats(a);

	for (int v = 0; v < SND_FIR_TAPS / 4; v++) {
		const vec_float4 x0 = ((const vec_float4 *)h0)[v];

		((vec_float4 *)c)[v] = spu_madd(av, spu_sub(((const vec_float4 *)h1)[v], x0), x0);
	}
}

/* x puede estar en cualquier múltiplo de 4 bytes: 9 vectores alineados dan los 8 desplazados */
static inline float fir_dot(const float *c, const float *x)
{
	static const vec_uchar16 pats[4] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
		{ 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
		{ 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
		{ 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 }
	};
	const unsigned off = (unsigned)((uintptr_t)x & 15u);
	const vec_float4 *xv = (const vec_float4 *)((uintptr_t)x & ~(uintptr_t)15);
	const vec_uchar16 pat = pats[off >> 2];
	vec_float4 acc = spu_splats(0.0f), cur = xv[0];

	for (int v = 0; v < SND_FIR_TAPS / 4; v++) {
		const vec_float4 nxt = xv[v + 1];

		acc = spu_madd(((const vec_float4 *)c)[v], spu_shuffle(cur, nxt, pat), acc);
		cur = nxt;
	}
	return (spu_extract(acc, 0) + spu_extract(acc, 1)) + (spu_extract(acc, 2) + spu_extract(acc, 3));
}
#else
static inline void fir_coefs(float *c, const float *h0, const float *h1, float a)
{
	for (int j = 0; j < SND_FIR_TAPS; j++)
		c[j] = h0[j] + a * (h1[j] - h0[j]);
}

static inline float fir_dot(const float *c, const float *x)
{
	float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

	for (int j = 0; j < SND_FIR_TAPS; j += 4) {
		a0 += c[j] * x[j];
		a1 += c[j + 1] * x[j + 1];
		a2 += c[j + 2] * x[j + 2];
		a3 += c[j + 3] * x[j + 3];
	}
	return (a0 + a1) + (a2 + a3);
}
#endif

static uint32_t snd_resample(snd_state *st, const float *wl, const float *wr, uint32_t n, float *out, uint32_t out_max)
{
	/* 33 513 982 / 1024 / 48 000 en Q0.32. La fase entera evita que el
	 * PPE y el SPU acumulen redondeos distintos entre trabajos. */
	const uint64_t base = 0xAE8D4F30u;
	const int32_t adj = (int32_t)(st->ratio_adj * 65536.0f);
	const uint32_t step = (uint32_t)(base + (int64_t)base * adj / 65536);
	const uint32_t last = SND_HIST + n - SND_AFTER_MAX; /* se puede sacar mientras k < last (k + 16 < SND_HIST + n) */
	uint32_t phase = st->resample_phase;
	uint32_t k = FIR_K0, produced = 0;
	float c[SND_FIR_TAPS] __attribute__((aligned(16)));

	while (produced < out_max) {
		float a;
		uint32_t ph;
		uint64_t next;

		if (k >= last)
			break;
		ph = phase >> 26;
		a = (float)(phase & 0x03FFFFFFu) * (1.0f / 67108864.0f);
		if (st->mode & SND_MODE_RESAMPLE_SIMPLE) {
			const float f = (float)phase * (1.0f / 4294967296.0f);

			out[2 * produced] = wl[k] + f * (wl[k + 1] - wl[k]);
			out[2 * produced + 1] = wr[k] + f * (wr[k + 1] - wr[k]);
		} else {
			fir_coefs(c, snd_fir[ph], snd_fir[ph + 1], a);
			out[2 * produced] = fir_dot(c, wl + (k - SND_FIR_BEFORE));
			out[2 * produced + 1] = fir_dot(c, wr + (k - SND_FIR_BEFORE));
		}
		produced++;
		next = (uint64_t)phase + step;
		phase = (uint32_t)next;
		k += (uint32_t)(next >> 32);
	}
	/* si faltó sitio en `out` lo pendiente se descarta: la ventana siguiente empieza en n + 16 */
	if (k < last)
		phase = 0;
	st->resample_phase = phase;
	return produced;
}

/* HD comparte el decodificador y sus lecturas: divide los mismos 512 ciclos por tick
 * en los instantes de salida. No anticipa muestras ni modifica la captura. Hermite
 * causal retrasa dos muestras de cada voz; PSG/ruido conservan sus escalones. */
static void chan_step(snd_chan *c, int ci, const snd_bus *bus, cap_run *cr, int old)
{
	if (old) chan_step_old(c, ci, bus);
	else voice_step(c, ci, bus, cr);
}

/* La muestra que la voz acaba de leer (posición `pos` antes del paso): si es el anillo de una captura y ya tiene
 * espejo interpolado, se oye el espejo; si no, el PCM fiel. Solo PCM8/PCM16 con el mismo tamaño que la captura. */
static float hd_voice_out(const snd_state *st, const snd_chan *c, int32_t pos)
{
	const uint32_t fmt = c->cnt >> 29 & 3u;

	if (pos >= 0 && fmt < 2u && !(st->mode & (SND_MODE_HD_CAP_OFF | SND_MODE_CORE_OLD)))
		for (int i = 0; i < 2; i++) {
			const snd_cap *k = &st->cap[i];
			const uint32_t size = (k->cnt & 8u) ? 1u : 2u;

			if (k->on && c->sad == k->base && (uint32_t)(c->pnt + c->len) * 4u == k->bytes &&
			    fmt == size - 1u && (uint32_t)pos < st->hdcap_written[i] && (uint32_t)pos < SND_HDCAP_MAX)
				return st->hdcap[i][pos];
		}
	return (float)c->out;
}

static void hd_advance(snd_state *st, snd_chan *c, int ci, const snd_bus *bus, uint32_t cycles, float *h, cap_run *cr,
		       int old)
{
	c->tcnt += cycles;
	while (c->tcnt >= 0x10000u && c->on) {
		const int32_t pos = c->pos;

		c->tcnt = c->tcnt - 0x10000u + c->tmr;
		chan_step(c, ci, bus, cr, old);
		h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = hd_voice_out(st, c, pos);
	}
}

static float hd_sample(snd_state *st, int ci, float subcycle)
{
	const snd_chan *c = &st->ch[ci];
	const float *h = st->hd_hist[ci];
	float x = 0;
	if (c->on) {
		if ((c->cnt >> 29 & 3u) == 3u) x = (float)c->out;
		else {
			float f = ((float)((int32_t)c->tcnt - (int32_t)c->tmr) + subcycle) * st->hd_inv[ci];
			/* Un cambio de TMR puede dejar el contador antes de la nueva recarga. */
			if (f < 0) f = 0;
			if (f > 1) f = 1;
			const float m0 = 0.5f * (h[2] - h[0]), m1 = 0.5f * (h[3] - h[1]);
			x = ((2*(h[1]-h[2])+m0+m1)*f + 3*(h[2]-h[1])-2*m0-m1)*f*f + m0*f + h[1];
		}
	}
	return x;
}

static float hd_state_sample(float x)
{
	/* La envolvente solo conserva el ancla de una rampa posterior. Una muestra
	 * entera evita que la contracción FMA distinta del PPE y el SPU cambie el
	 * estado persistente, sin cuantizar la salida actual. */
	return (float)(int32_t)(x + (x < 0 ? -0.5f : 0.5f));
}

/* Compensación posterior al volumen/balance: apagar escribiendo CNT=0 también
 * conserva la cola. Rampa finita de 32 pares (0,67 ms), sin continua residual. */
static void hd_declick(snd_state *st, int ci, float *l, float *r)
{
	float x[2] = {*l, *r};
	const uint32_t on = st->ch[ci].on;
	if (st->hd_on[ci] != on) {
		for (int side = 0; side < 2; side++) st->hd_anchor[ci][side] = st->hd_last[ci][side];
		st->hd_fade[ci] = 32;
		st->hd_on[ci] = on;
	}
	for (int side = 0; side < 2; side++) {
		if (st->hd_fade[ci]) {
			const float a = (float)st->hd_fade[ci] / 32.0f;
			x[side] = x[side] * (1.0f - a) + st->hd_anchor[ci][side] * a;
		}
		st->hd_last[ci][side] = hd_state_sample(x[side]);
	}
	if (st->hd_fade[ci]) st->hd_fade[ci]--;
	*l = x[0]; *r = x[1];
}

/* ---- el trabajo ---------------------------------------------------------------------------------------------- */

static inline int32_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

uint32_t snd_run(snd_state *st, const snd_ent *log, uint32_t nlog, uint32_t nticks, const snd_bus *bus, int16_t *nat,
		 float *out, uint32_t out_max, uint32_t *out_n)
{
	static const uint8_t divsh[4] = { 0, 1, 2, 4 };
	cap_run cr[2];
	float wl_[SND_HIST + SND_TICKS_MAX + 4] __attribute__((aligned(16)));
	float wr_[SND_HIST + SND_TICKS_MAX + 4] __attribute__((aligned(16)));
	float *const wl = wl_, *const wr = wr_;
	uint32_t ie = 0, peak = 0, hd_n = 0;
	const int hd = (st->mode & SND_MODE_HD) != 0;
	const int old = (st->mode & SND_MODE_CORE_OLD) != 0;
	const int hdcap = hd && !old && !(st->mode & SND_MODE_HD_CAP_OFF);
	/* Reloj racional idéntico en PPE/SPE: evita desplazar flancos PSG por el
	 * redondeo hacia cero del SPU. Deriva cuantizada a 1/65536 (15 ppm). */
	const int32_t adj = (int32_t)(st->ratio_adj * 65536.0f);
	const uint32_t hd_step = (uint32_t)(16756991 + (int64_t)16756991 * adj / 65536);
	if (hd && !st->hd_active) {
		st->hd_phase = 0;
		for (int c = 0; c < SND_NCH; c++) {
			for (int k = 0; k < 4; k++) st->hd_hist[c][k] = (float)st->ch[c].out;
			for (int side = 0; side < 2; side++) st->hd_last[c][side] = st->hd_anchor[c][side] = 0;
			st->hd_fade[c] = st->hd_on[c] = 0;
		}
		st->hdcap_written[0] = st->hdcap_written[1] = 0; /* el espejo se rellena desde la próxima vuelta */
	}
	st->hd_active = (uint32_t)hd;

	if (nticks > SND_TICKS_MAX)
		nticks = SND_TICKS_MAX;
	cr[0].n = cr[1].n = 0;
	cr[0].addr = cr[1].addr = 0;
	cr[0].coherent = cr[1].coherent = old ? NULL : st;
	for (uint32_t i = 0; i < SND_HIST; i++) {
		wl[i] = st->hist[0][i];
		wr[i] = st->hist[1][i];
	}
	/* Las voces 1 y 3 suelen reproducir lo que escriben SOUNDCAP0/1. La línea
	 * que ya estaban leyendo es parte de su estado: volver a traerla aquí
	 * cambia de golpe a la instantánea de RAM del cuadro siguiente. Se refresca
	 * al cambiar SAD, al arrancar y al volver al principio del bucle. */
	for (int c = 0; c < SND_NCH; c++)
		if ((st->mode & SND_MODE_FRAME_CACHE_OLD) || (c != 1 && c != 3))
			st->ch[c].tag = 0xFFFFFFFFu;
	for (uint32_t t = 0; t < nticks; t++) {
		const uint64_t T = st->tick + t;
#ifdef SND_DIAGNOSTIC
		int16_t trace_raw[16] = {0}, trace_mix[32] = {0};
#endif
		int32_t mix[2] = { 0, 0 }, c1[3] = { 0, 0, 0 }, c3[3] = { 0, 0, 0 };
		int32_t direct[4] = {0};
		int32_t L, R, capv[2];
		uint32_t sc;
		float hm[2][2] = {{0}}, h1[2][2] = {{0}}, h3[2][2] = {{0}}, subcycle[2];
		float hcm[2] = {0, 0}, hdir[4] = {0}; /* mezcla y voces directas HD en el instante del tick (espejo de captura) */
		uint32_t sample_at[2];
		uint32_t hn = 0;
		if (hd) {
			while (st->hd_phase < 24576000u && hn < 2) {
				sample_at[hn] = st->hd_phase / 48000u;
				subcycle[hn++] = (float)(st->hd_phase % 48000u) * (1.0f / 48000.0f);
				st->hd_phase += hd_step;
			}
			st->hd_phase -= 24576000u;
		}
		while (ie < nlog && log[ie].tick < T) {
			const snd_ent *e = &log[ie++];
			if (hd && e->off < 0x100 && (e->off & 15u) == 2 &&
			    (e->mask & e->val & 0x8000u) && !st->ch[e->off >> 4].on) {
				for (int k = 0; k < 4; k++) st->hd_hist[e->off >> 4][k] = 0;
				st->hd_on[e->off >> 4] = 0;
			}
			apply(st, e, bus, cr);
		}
		for (int ci = 0; ci < SND_NCH; ci++) {
			snd_chan *c = &st->ch[ci];
			int32_t x, l, r;
			uint32_t vol, pan;
			const int active = c->on;

			if (!c->on && (!hd || (!st->hd_on[ci] && !st->hd_fade[ci]))) {
				if (hd) st->hd_last[ci][0] = st->hd_last[ci][1] = 0;
				continue;
			}
			if (hd) {
				const int started = c->on;
				const uint32_t period = 0x10000u - c->tmr;
				if (st->hd_period[ci] != period) {
					st->hd_period[ci] = period;
					st->hd_inv[ci] = 1.0f / (float)period;
				}
				uint32_t elapsed = 0;
				const uint32_t v = (c->cnt & 127u) == 127u ? 128u : c->cnt & 127u;
				const uint32_t p = (c->cnt >> 16 & 127u) == 127u ? 128u : c->cnt >> 16 & 127u;
				const float gain = (float)v * (1.0f/262144.0f) * (float)(16u >> divsh[c->cnt >> 8 & 3u]);
				const uint32_t pl = old || p == 0 || p == 128 ? 128u - p : 127u - p;
				const int excluded = (ci == 1 && (st->soundcnt & 0x1000u)) || (ci == 3 && (st->soundcnt & 0x2000u));
				for (uint32_t j = 0; j < hn; j++) {
					const uint32_t at = sample_at[j];
					if (started) hd_advance(st, c, ci, bus, at - elapsed, st->hd_hist[ci], cr, old);
					elapsed = at;
					const float sample = hd_sample(st, ci, subcycle[j]) * gain;
					float l = sample * (float)pl, r = sample * (float)p;
					hd_declick(st, ci, &l, &r);
					if (ci == 1) { h1[j][0] = l; h1[j][1] = r; }
					if (ci == 3) { h3[j][0] = l; h3[j][1] = r; }
					if (excluded) continue;
					hm[j][0] += l; hm[j][1] += r;
				}
				if (started) hd_advance(st, c, ci, bus, 512u - elapsed, st->hd_hist[ci], cr, old);
				if (hdcap && started) {
					/* lo que la captura toma en este tick, con las voces interpoladas en el límite del tick */
					const float xs = hd_sample(st, ci, 0.0f);
					if (ci < 4) hdir[ci] = xs * (1.0f / (float)(1u << divsh[c->cnt >> 8 & 3u]));
					if (!excluded) { hcm[0] += xs * gain * (float)pl; hcm[1] += xs * gain * (float)p; }
				}
			}
			if (!active || (old && !c->on))
				continue;
			if (!hd) c->tcnt += 512u;
			while (c->tcnt >= 0x10000u && c->on) {
				c->tcnt = c->tcnt - 0x10000u + c->tmr;
				chan_step(c, ci, bus, cr, old);
			}
			if (old && !c->on)
				continue;
			vol = c->cnt & 127u;
			if (vol == 127u)
				vol = 128u;
			pan = (c->cnt >> 16) & 127u;
			if (pan == 127u)
				pan = 128u;
			/* un solo desplazamiento al final (medido contra melonDS: con redondeos intermedios cambian muestras sueltas) */
			l = (c->out * (int32_t)(vol * (128u - pan))) >> (14 + divsh[(c->cnt >> 8) & 3u]);
			r = (c->out * (int32_t)(vol * pan)) >> (14 + divsh[(c->cnt >> 8) & 3u]);
			x = (c->out * (int32_t)vol) >> (7 + divsh[(c->cnt >> 8) & 3u]);
			if (!old) {
				const uint32_t p = c->cnt >> 16 & 127u;
				x = voice_mul7(c->out, c->cnt & 127u) >> divsh[c->cnt >> 8 & 3u];
				l = voice_mul7(x, 127u - p);
				r = voice_mul7(x, p);
				if (ci < 4) direct[ci] = c->out >> divsh[c->cnt >> 8 & 3u];
			}
#ifdef SND_DIAGNOSTIC
			trace_raw[ci] = (int16_t)c->out;
			trace_mix[2 * ci] = (int16_t)l;
			trace_mix[2 * ci + 1] = (int16_t)r;
#endif
			if (ci == 1) {
				c1[0] = l;
				c1[1] = r;
				c1[2] = x;
				if (st->soundcnt & 0x1000u)
					continue;
			} else if (ci == 3) {
				c3[0] = l;
				c3[1] = r;
				c3[2] = x;
				if (st->soundcnt & 0x2000u)
					continue;
			}
			mix[0] += l;
			mix[1] += r;
		}
		/* salida izquierda y derecha: el mezclador o los canales 1 y 3 (SOUNDCNT bits 8-11) */
		sc = st->soundcnt;
		if (hd) {
			const uint32_t mv = (sc & 127u) == 127u ? 128u : sc & 127u;
			for (uint32_t j = 0; j < hn; j++) {
				for (int side = 0; side < 2; side++) {
					float x;
					switch (sc >> (8 + 2*side) & 3u) {
					case 0: x = hm[j][side]; break;
					case 1: x = h1[j][side]; break;
					case 2: x = h3[j][side]; break;
					default: x = h1[j][side] + h3[j][side]; break;
					}
					if (out && hd_n < out_max) out[2*hd_n + side] = (sc & 0x8000u) ? x * ((float)mv / (128.0f*32768.0f)) : 0;
				}
				if (hd_n < out_max) hd_n++;
			}
		}
		{
			const int32_t ml = old ? clamp16(mix[0]) : mix[0], mr = old ? clamp16(mix[1]) : mix[1];

			capv[0] = (st->cap[0].cnt & 2u) ? clamp16(c1[2]) : ml;
			capv[1] = (st->cap[1].cnt & 2u) ? clamp16(c3[2]) : mr;
			if (!old)
				for (int i = 0; i < 2; i++) {
					const uint32_t cc = st->cap[i].cnt;
					capv[i] = clamp16(cc & 2u ? direct[2*i] + (cc & 1u ? direct[2*i+1] : 0) : mix[i]);
				}
			switch ((sc >> 8) & 3u) {
			case 0: L = ml; break;
			case 1: L = c1[0]; break;
			case 2: L = c3[0]; break;
			default: L = old ? clamp16(c1[0] + c3[0]) : c1[0] + c3[0]; break;
			}
			switch ((sc >> 10) & 3u) {
			case 0: R = mr; break;
			case 1: R = c1[1]; break;
			case 2: R = c3[1]; break;
			default: R = old ? clamp16(c1[1] + c3[1]) : c1[1] + c3[1]; break;
			}
		}
		for (int i = 0; i < 2; i++) {
			snd_cap *k = &st->cap[i];
			if (!k->on) continue;
			if (old) cap_put(bus, k, &cr[i], capv[i]);
			else {
				const uint32_t cc = k->cnt;
				const float hv = !hdcap ? 0.0f : cc & 2u ? hdir[2*i] + (cc & 1u ? hdir[2*i+1] : 0.0f) : hcm[i];

				k->phase += 512;
				while (k->phase >= 0x10000u && k->on) {
					k->phase = k->phase - 0x10000u + st->ch[1 + 2*i].tmr;
					if (k->fifo_count < 16) {
						st->hdcap_fifo[i][k->fifo_count] = hv;
						k->fifo[k->fifo_count++] = (int16_t)capv[i];
					} else {
						const int32_t sample = k->fifo[k->fifo_head];
						const float hs = st->hdcap_fifo[i][k->fifo_head];
						const uint32_t idx = k->pos / ((cc & 8u) ? 1u : 2u);

						k->fifo[k->fifo_head] = (int16_t)capv[i];
						st->hdcap_fifo[i][k->fifo_head] = hv;
						k->fifo_head = (k->fifo_head + 1) & 15u;
						cap_put(bus, k, &cr[i], sample);
						if (hdcap && idx < SND_HDCAP_MAX) {
							st->hdcap[i][idx] = hd_state_sample(hs);
							if (idx >= st->hdcap_written[i]) st->hdcap_written[i] = idx + 1;
						}
					}
				}
			}
		}
#ifdef SND_DIAGNOSTIC
		snd_trace_tick(st, T, trace_raw, trace_mix);
#endif
		{
			int32_t mv = (int32_t)(sc & 127u);

			if (mv == 127)
				mv = 128;
			L = (L * mv) >> 7;
			R = (R * mv) >> 7;
			if (!old) {
				L = clamp16(L + ((int32_t)st->bias - 512) * 64);
				R = clamp16(R + ((int32_t)st->bias - 512) * 64);
			}
			if (!(sc & 0x8000u))
				L = R = 0;
		}
		if (nat) {
			nat[2 * t] = (int16_t)L;
			nat[2 * t + 1] = (int16_t)R;
		}
		{
			const uint32_t al = (uint32_t)(L < 0 ? -L : L), ar = (uint32_t)(R < 0 ? -R : R);

			if (al > peak)
				peak = al;
			if (ar > peak)
				peak = ar;
		}
		if (st->mode & SND_MODE_ORIGINAL) { /* 10 bits con el sesgo: así la salida de la DS */
			int32_t ql = (L >> 6) + (old ? (int32_t)st->bias : 512), qr = (R >> 6) + (old ? (int32_t)st->bias : 512);

			ql = ql < 0 ? 0 : ql > 1023 ? 1023 : ql;
			qr = qr < 0 ? 0 : qr > 1023 ? 1023 : qr;
			wl[SND_HIST + t] = (float)(ql - 512) * (1.0f / 512.0f);
			wr[SND_HIST + t] = (float)(qr - 512) * (1.0f / 512.0f);
		} else {
			wl[SND_HIST + t] = (float)L * (1.0f / 32768.0f);
			wr[SND_HIST + t] = (float)R * (1.0f / 32768.0f);
		}
	}
	cap_flush(bus, &cr[0]);
	cap_flush(bus, &cr[1]);
	st->tick += nticks;
	st->busy = 0;
	for (int c = 0; c < SND_NCH; c++)
		if (st->ch[c].on)
			st->busy |= 1u << c;
	if (st->cap[0].on)
		st->busy |= 1u << 16;
	if (st->cap[1].on)
		st->busy |= 1u << 17;
	st->peak = peak;
	st->ticks_run = nticks;
	st->log_applied = ie;
	if (out && out_n) {
		*out_n = hd ? hd_n : snd_resample(st, wl, wr, nticks, out, out_max);
		for (uint32_t i = 0; i < SND_HIST; i++) { /* historia del siguiente trabajo: las últimas SND_HIST de la ventana */
			st->hist[0][i] = wl[nticks + i];
			st->hist[1][i] = wr[nticks + i];
		}
		st->nout = *out_n;
	}
	return ie;
}
