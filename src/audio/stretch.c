/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio/stretch.h"
#include <string.h>

float audio_soft_limit(float x)
{
	const float knee = 0.70710678f, ceiling = 0.98f;
	float a = x < 0 ? -x : x, d;
	if (a <= knee)
		return x;
	d = a - knee;
	a = d >= 2 * (ceiling - knee) ? ceiling : a - d * d / (4 * (ceiling - knee));
	return x < 0 ? -a : a;
}

static void remember(audio_stretch *s, const float *in, uint32_t n)
{
	uint32_t keep = s->history_n;
	if (keep + n > STRETCH_HISTORY)
		keep = STRETCH_HISTORY - n;
	memmove(s->history, s->history + 2 * (s->history_n - keep), keep * 2 * sizeof(float));
	memcpy(s->history + 2 * keep, in, n * 2 * sizeof(float));
	s->history_n = keep + n;
}

uint32_t audio_stretch_run(audio_stretch *s, const float *in, uint32_t n, float *out)
{
	const float *grain = in, *ref = s->blending ? s->overlap : in;
	uint32_t used = STRETCH_HOP, blend = s->blending;
	if (n < STRETCH_GRAIN && s->history_n >= STRETCH_GRAIN) {
		/* Error cuadrático estéreo: maximiza correlación y penaliza diferencias
		 * de energía. Paso grueso 4, refinamiento a una muestra; ambos canales
		 * comparten desplazamiento. No se interpola ni se altera la frecuencia. */
		float best = 1e30f;
		uint32_t pos = 0, max = s->history_n - STRETCH_GRAIN;
		uint32_t count = blend ? STRETCH_OVERLAP : (n < STRETCH_OVERLAP ? n : STRETCH_OVERLAP);
		for (uint32_t p = 0; p <= max; p += 4) {
			float e = 0;
			for (uint32_t j = 0; j < count; j += 4)
				for (uint32_t c = 0; c < 2; c++) {
					float d = ref[2*j+c] - s->history[2*(p+j)+c];
					e += d*d;
				}
			if (e < best) { best = e; pos = p; }
		}
		uint32_t lo = pos > 3 ? pos - 3 : 0, hi = pos + 3 < max ? pos + 3 : max;
		best = 1e30f;
		for (uint32_t p = lo; p <= hi; p++) {
			float e = 0;
			for (uint32_t j = 0; j < count; j++)
				for (uint32_t c = 0; c < 2; c++) {
					float d = ref[2*j+c] - s->history[2*(p+j)+c];
					e += d*d;
				}
			if (e < best) { best = e; pos = p; }
		}
		grain = s->history + 2 * pos;
		used = 0;
		blend = count == STRETCH_OVERLAP;
		s->inserted += STRETCH_HOP;
	}
	if (used && n < STRETCH_HOP) {
		/* Solo durante el arranque sin historia: termina con fundido. */
		memset(out, 0, 2 * STRETCH_HOP * sizeof(float));
		for (uint32_t i = 0; i < n; i++)
			for (uint32_t c = 0; c < 2; c++)
				out[2*i+c] = in[2*i+c] * (float)(n-i) / (float)n;
		s->missing += STRETCH_HOP - n;
		s->blending = 0;
		remember(s, in, n);
		return n;
	}
	memcpy(out, grain, 2 * STRETCH_HOP * sizeof(float));
	if (blend) {
		for (uint32_t i = 0; i < STRETCH_OVERLAP; i++) {
			/* Ventana cúbica complementaria: suma unidad y derivada nula
			 * en ambos extremos, evita escalones y modulación de ganancia. */
			float w = (float)i / (STRETCH_OVERLAP - 1);
			w = w*w*(3-2*w);
			for (uint32_t c = 0; c < 2; c++)
				out[2*i+c] = ref[2*i+c] + w*(grain[2*i+c] - ref[2*i+c]);
		}
	}
	if (!used || blend)
		s->active += STRETCH_HOP;
	if (!used)
		memcpy(s->overlap, grain + 2 * STRETCH_HOP, sizeof s->overlap);
	s->blending = !used;
	if (used)
		remember(s, in, used);
	return used;
}
