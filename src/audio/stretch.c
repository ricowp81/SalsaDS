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

float audio_rate_adjust(uint32_t level, uint32_t target)
{
	const float limit = 0.0025f; /* ±0,25 %: cubre 59,94 frente a 59,83 Hz sin cambiar el tono de forma audible */
	float adj = ((float)level - (float)target) * (limit / 512.0f);

	return adj > limit ? limit : adj < -limit ? -limit : adj;
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

/* Dos polos reales a ~750 Hz. Solo guía la búsqueda: el audio emitido
 * conserva todas sus muestras y su banda. Canales independientes. */
static void alignment_lowpass(const float *in, float *out, uint32_t n)
{
	for (uint32_t c = 0; c < 2; c++) {
		float a = in[c], b = a;
		for (uint32_t i = 0; i < n; i++) {
			a += 0.094f * (in[2*i+c] - a);
			b += 0.094f * (a - b);
			out[2*i+c] = b;
		}
	}
}

/* Un ataque de banda ancha se copia a velocidad local 1:1. Así la búsqueda
 * no vuelve a colocar el mismo golpe en el grano siguiente. La reserva
 * absorbe ese avance y el control recupera la extensión entre ataques. */
static int has_attack(const float *in, uint32_t n)
{
	float total = 0, peak = 0, signal = 0;
	for (uint32_t base = 0; base < n; base += 80) {
		float energy = 0;
		for (uint32_t j = base; j < base+80 && j < n; j++)
			for (uint32_t c = 0; c < 2; c++) {
				float x = in[2*j+c];
				signal += x*x;
				if (j >= 2) {
					float d = x - 2*in[2*(j-1)+c] + in[2*(j-2)+c];
					energy += d*d;
				}
			}
		total += energy;
		if (energy > peak) peak = energy;
	}
	return total > 0.002f*signal && peak > total * (480.0f/n);
}

static uint32_t stretch2_run(audio_stretch2 *s, const float *in, uint32_t n, float *out, int long_grain)
{
	const uint32_t hop = long_grain ? STRETCH2_LONG_HOP : STRETCH2_HOP;
	const uint32_t overlap = long_grain ? STRETCH2_LONG_OVERLAP : STRETCH2_OVERLAP;
	const uint32_t grain = hop + overlap;
	const uint32_t search = long_grain ? 1920u : 960u;
	uint32_t used = 0;
	uint32_t available = n;
	if (n > STRETCH2_INPUT) n = STRETCH2_INPUT;
	if (!s->initialized) { s->rate = 1; s->initialized = 1; }
	else {
		s->arrivals += (float)available - s->previous + s->consumed;
		/* Ventana de 50 ms y polo suave: las entregas por cuadro no
   * modulan directamente el salto. La reserva cubre cambios rápidos. */
		if (++s->ticks == 5) {
			s->rate += 0.4f * (s->arrivals / (5*STRETCH_HOP) - s->rate);
			if (s->rate < 0) s->rate = 0;
			if (s->rate > 1.05f) s->rate = 1.05f;
			s->arrivals = 0; s->ticks = 0;
		}
	}
	if (s->queued) {
		memcpy(out, s->output + 2*s->position, 2*STRETCH_HOP*sizeof(float));
		s->position += STRETCH_HOP;
		s->queued -= STRETCH_HOP;
		s->credit = s->grain_used*s->queued/hop;
		s->active += STRETCH_HOP;
	} else if (!s->valid && (n >= grain + hop || (s->rate >= 0.98f && n >= 2 * STRETCH_HOP))) {
		/* La entrega por cuadros hace oscilar la estimación de llegadas aunque
		 * sobren muestras. Con producción nominal y anillo sano la ruta es
		 * identidad aunque una entrega puntual llegue tarde. */
		memcpy(out, in, 2*STRETCH_HOP*sizeof(float));
		used = STRETCH_HOP;
	} else {
		uint32_t pos = 0;
		int leave = s->rate >= 0.98f && n >= grain + hop;
		int attack = n >= grain && has_attack(in, n < grain + search ? n : grain + search);
		const float *source = in;
		if (n < grain) {
			if (!s->valid) {
				uint32_t got = n < STRETCH_HOP ? n : STRETCH_HOP;
				memset(out, 0, 2*STRETCH_HOP*sizeof(float));
				for (uint32_t i = 0; i < got; i++)
					for (uint32_t c = 0; c < 2; c++)
						out[2*i+c] = in[2*i+c] * (float)(got-i)/(float)got;
				used = got; s->missing += STRETCH_HOP-got;
				goto finish;
			}
			source = s->grain;
			/* Sin nueva entrada: prolongación de la historia, nunca remuestreo. */
		} else {
			float advance = s->rate + ((float)n - 4000) / 4800;
			if (advance < 0) advance = 0;
			if (advance > 1) advance = 1;
			s->fraction += advance * hop;
			used = (uint32_t)s->fraction;
			s->fraction -= used;
			/* Reserva de un grano más 20 ms de búsqueda: también al arrancar
			 * con entregas espaciadas debe caber un período de los graves. */
			uint32_t room = n > grain + search ? n - grain - search : 0;
			if (used > room) used = room;
		}
		if (leave || attack) used = hop;
		if (s->valid && !leave && !attack) {
			uint32_t max = source == in ? n - grain : 0;
			if (max > search) max = search;
			alignment_lowpass(source, s->filtered, max + overlap);
			alignment_lowpass(s->grain + 2*hop, s->reference, overlap);
			float best = 1e30f;
			/* Decimación tras el pasabajos; refinamiento de posición a una muestra.
    * Se omiten 64 muestras de asentamiento de los polos. */
			for (uint32_t p = 0; p <= max; p += 4) {
				float error = 0;
				for (uint32_t j = 64; j < overlap; j += 8)
					for (uint32_t c = 0; c < 2; c++) {
						float d = s->reference[2*j+c] - s->filtered[2*(p+j)+c];
						float wide = s->grain[2*(hop+j)+c] - source[2*(p+j)+c];
						error += d*d + 0.08f*wide*wide;
					}
				if (error < best) { best = error; pos = p; }
			}
			uint32_t lo = pos > 3 ? pos-3 : 0, hi = pos+3 < max ? pos+3 : max;
			for (uint32_t p = lo; p <= hi; p++) {
				float error = 0;
				for (uint32_t j = 64; j < overlap; j += 8)
					for (uint32_t c = 0; c < 2; c++) {
						float d = s->reference[2*j+c] - s->filtered[2*(p+j)+c];
						float wide = s->grain[2*(hop+j)+c] - source[2*(p+j)+c];
						error += d*d + 0.08f*wide*wide;
					}
				if (error < best) { best = error; pos = p; }
			}
		}
		memcpy(s->output, source + 2*pos, 2*hop*sizeof(float));
		if (s->valid) {
			for (uint32_t i = 0; i < overlap; i++) {
				float w = (float)i / (float)(overlap-1);
				w = w*w*(3-2*w);
				for (uint32_t c = 0; c < 2; c++) {
					float old = s->grain[2*(hop+i)+c];
					s->output[2*i+c] = old + w*(s->output[2*i+c]-old);
				}
			}
			s->joins++;
		}
		memmove(s->grain, source + 2*pos, 2*grain*sizeof(float));
		s->valid = !leave;
		s->position = STRETCH_HOP; s->queued = hop-STRETCH_HOP;
		s->grain_used = used; s->credit = used*s->queued/hop;
		memcpy(out, s->output, 2*STRETCH_HOP*sizeof(float));
		s->inserted += hop-used;
		s->active += STRETCH_HOP;
	}
finish:
	s->previous = available; s->consumed = used;
	return used;
}

uint32_t audio_stretch2_run(audio_stretch2 *s, const float *in, uint32_t n, float *out)
{
	return stretch2_run(s, in, n, out, 0);
}

uint32_t audio_stretch2_run_long(audio_stretch2 *s, const float *in, uint32_t n, float *out)
{
	return stretch2_run(s, in, n, out, 1);
}
