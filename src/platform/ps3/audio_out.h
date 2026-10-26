/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — salida de audio de la PS3 (PSL1GHT, puerto de audio de 2 canales a 48 kHz, bloques de 256 muestras).
 *
 *   mezclador (SPE) → audio_out_push (anillo) → hilo de audio: llena los bloques del puerto cuando el sistema los consume
 *
 * El hilo de la emulación empuja las muestras que entrega el mezclador (estéreo flotante a 48 kHz, intercaladas). El hilo de
 * audio conserva el tono con solape y búsqueda de correlación cuando faltan muestras. La mezcla a velocidad completa
 * pasa intacta por el estirador; la protección suave tras el FIR tiene su propio interruptor. El control de deriva
 * `audio_out_ratio_adj()` corrige la deriva de los relojes dentro de ±0,25 %.
 *
 * Nada de esto toca el log desde el hilo de audio (el logger no es seguro entre hilos): deja contadores.
 */
#ifndef SALSADS_PLATFORM_PS3_AUDIO_OUT_H
#define SALSADS_PLATFORM_PS3_AUDIO_OUT_H

#include <stdint.h>

struct audio_out_stats {
	uint64_t played; /* pares entregados durante reproducción, sin contar el selector */
	uint32_t stretch_max_us, joins, rate_permil, grain_ms;
	uint64_t inserted, active, stretch_us; /* pares añadidos, pares procesados con solape, costo acumulado */
	int stretch_enabled, limiter_enabled;
	uint64_t pushed;     /* muestras (pares) recibidas */
	uint64_t dropped;    /* muestras descartadas porque el anillo estaba lleno */
	uint64_t blocks;     /* bloques entregados al puerto */
	uint64_t silent;     /* muestras de silencio puestas por falta de datos */
	uint32_t underruns;  /* bloques con faltante */
	uint32_t late;       /* veces que el puerto alcanzó al hilo */
	uint32_t level;      /* muestras en el anillo ahora */
	uint32_t peak;       /* pico absoluto (0-32768) desde la última lectura */
	uint64_t busy_us;    /* tiempo del hilo de audio trabajando */
	uint64_t lim_total;  /* muestras (por canal) que pasaron por el limitador desde la última lectura con reinicio */
	uint64_t lim_n;      /* de ellas, las que superaron la rodilla (el limitador actuó) */
	double lim_sum_db;   /* suma de la reducción en dB en esas muestras */
	float lim_max_db;    /* mayor reducción en dB */
	float gain_db;       /* ganancia de salida aplicada antes del limitador (audio-ganancia) */
};

/* 0 = abierto; otro valor = sin audio (el resto de las funciones se pueden llamar igual y no hacen nada). */
int audio_out_init(void);
void audio_out_push(const float *stereo, uint32_t frames);
/* Corrección de la razón de remuestreo para mantener ~60 ms en el anillo (positiva: sobra audio, producir menos). */
float audio_out_ratio_adj(void);
void audio_out_stats(struct audio_out_stats *s, int reset_peak);
int audio_out_is_open(void);
void audio_out_shutdown(void);
/* Llamar al cerrar tras esperar al productor; confirma el vaciado antes del próximo juego. */
void audio_out_set_active(int enabled);

/* Grabación digital (archivo vacío audio-grabar): `nat` es la mezcla nativa de la DS (16 bits, 32 728 Hz, estéreo) que devuelve
 * el SPE; llamar con cada trabajo recogido. audio_out_record_service() escribe los archivos a trozos desde el hilo de
 * emulación (una vez por cuadro); el cierre del juego termina lo pendiente. */
void audio_out_record_mix(const int16_t *nat, uint32_t nticks);
void audio_out_record_service(void);

#endif
