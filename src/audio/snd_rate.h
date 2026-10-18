/*
 * SalsaDS — control de la razón de remuestreo según el nivel del anillo de salida (portable: lo usa la PS3 y la prueba del host).
 *
 * Una llamada por cuadro con el nivel del anillo (muestras a 48 kHz). Proporcional (nivel contra target) más un término
 * integral que aprende el desfase sostenido: con la emulación más lenta que el tiempo real el audio se estira (razón
 * hasta SND_ADJ_MIN = -40 %) en vez de rellenar con silencio; con la emulación algo más rápida, hasta +3 %. La corrección
 * cambia como mucho SND_RATE_SLEW (1 %, ≈ 17 cents de tono) por cuadro, así que no hay saltos de tono bruscos.
 */
#ifndef SALSADS_AUDIO_SND_RATE_H
#define SALSADS_AUDIO_SND_RATE_H

#include "audio/snd.h"

#define SND_RATE_KP 0.12f
#define SND_RATE_KI 0.012f
#define SND_RATE_SLEW 0.01f

typedef struct snd_rate {
	float integ, out;
} snd_rate;

static inline float snd_rate_clamp(float v)
{
	return v > SND_ADJ_MAX ? SND_ADJ_MAX : v < SND_ADJ_MIN ? SND_ADJ_MIN : v;
}

static inline float snd_rate_update(snd_rate *r, uint32_t level, uint32_t target)
{
	float e = ((float)level - (float)target) * (1.0f / (float)target), t;

	e = e > 1.0f ? 1.0f : e < -1.0f ? -1.0f : e;
	r->integ = snd_rate_clamp(r->integ + SND_RATE_KI * e);
	t = snd_rate_clamp(SND_RATE_KP * e + r->integ);
	if (t > r->out + SND_RATE_SLEW)
		t = r->out + SND_RATE_SLEW;
	else if (t < r->out - SND_RATE_SLEW)
		t = r->out - SND_RATE_SLEW;
	r->out = t;
	return t;
}

#endif
