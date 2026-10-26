/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — cabecera y codificación de WAV (siempre little-endian, también en la PS3 big-endian). Sin E/S ni memoria
 * dinámica: lo usa la grabación digital de audio_out.c y se prueba en el host.
 */
#ifndef SALSADS_AUDIO_WAVREC_H
#define SALSADS_AUDIO_WAVREC_H

#include <stddef.h>
#include <stdint.h>

#define WAVREC_HDR_PCM 44u   /* PCM entero de 16 bits */
#define WAVREC_HDR_FLOAT 58u /* IEEE flotante de 32 bits (formato 3, con fact) */

/* Escribe la cabecera en `h` (al menos WAVREC_HDR_FLOAT bytes) y devuelve su tamaño. `frames` = pares/ranuras completas. */
size_t wavrec_header(uint8_t *h, int is_float, uint32_t channels, uint32_t rate, uint32_t frames);
/* Codifican `n` muestras en little-endian; devuelven los bytes escritos (2n o 4n). */
size_t wavrec_encode_i16(uint8_t *dst, const int16_t *src, size_t n);
size_t wavrec_encode_f32(uint8_t *dst, const float *src, size_t n);

#endif
