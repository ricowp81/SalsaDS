/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio/wavrec.h"
#include <string.h>

static void le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void le16(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

size_t wavrec_header(uint8_t *h, int is_float, uint32_t channels, uint32_t rate, uint32_t frames)
{
	const uint32_t bytes = is_float ? 4u : 2u, data = frames * channels * bytes;
	size_t o = 0;

	memcpy(h, "RIFF", 4);
	le32(h + 4, (is_float ? WAVREC_HDR_FLOAT : WAVREC_HDR_PCM) - 8u + data);
	memcpy(h + 8, "WAVEfmt ", 8);
	o = 16;
	le32(h + o, is_float ? 18u : 16u);
	le16(h + o + 4, is_float ? 3u : 1u);
	le16(h + o + 6, channels);
	le32(h + o + 8, rate);
	le32(h + o + 12, rate * channels * bytes);
	le16(h + o + 16, channels * bytes);
	le16(h + o + 18, bytes * 8u);
	o += 20;
	if (is_float) {
		le16(h + o, 0); /* cbSize */
		o += 2;
		memcpy(h + o, "fact", 4);
		le32(h + o + 4, 4);
		le32(h + o + 8, frames);
		o += 12;
	}
	memcpy(h + o, "data", 4);
	le32(h + o + 4, data);
	return o + 8;
}

size_t wavrec_encode_i16(uint8_t *dst, const int16_t *src, size_t n)
{
	for (size_t i = 0; i < n; i++)
		le16(dst + 2 * i, (uint16_t)src[i]);
	return 2 * n;
}

size_t wavrec_encode_f32(uint8_t *dst, const float *src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint32_t u;

		memcpy(&u, &src[i], 4);
		le32(dst + 4 * i, u);
	}
	return 4 * n;
}
