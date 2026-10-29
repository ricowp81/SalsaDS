/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "audio/wavrec.h"
#include <math.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

void test_wavrec(void)
{
	uint8_t buf[WAVREC_HDR_FLOAT + 4 * 2 * 480];
	float sig[2 * 480], back;
	int16_t s16[8] = { 0, 1, -1, 32767, -32768, 0x1234, -2, 100 };
	size_t h, n;

	for (int i = 0; i < 480; i++) {
		sig[2 * i] = 0.5f * sinf(6.2831853f * 1000.0f * (float)i / 48000.0f);
		sig[2 * i + 1] = -sig[2 * i];
	}
	sig[0] = 1.25f;
	h = wavrec_header(buf, 1, 2, 48000, 480);
	CHECK_EQ(h, WAVREC_HDR_FLOAT);
	n = wavrec_encode_f32(buf + h, sig, 2 * 480);
	CHECK_EQ(n, 4 * 960);
	CHECK(memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVEfmt ", 8) == 0 && memcmp(buf + 38, "fact", 4) == 0 &&
	      memcmp(buf + 50, "data", 4) == 0);
	CHECK_EQ(rd32(buf + 4), h - 8 + n);
	CHECK_EQ(rd16(buf + 20), 3);
	CHECK_EQ(rd16(buf + 22), 2);
	CHECK_EQ(rd32(buf + 24), 48000);
	CHECK_EQ(rd32(buf + 28), 48000 * 8);
	CHECK_EQ(rd16(buf + 32), 8);
	CHECK_EQ(rd16(buf + 34), 32);
	CHECK_EQ(rd32(buf + 54), n);
	for (int i = 0; i < 960; i += 97) {
		uint32_t u = rd32(buf + h + 4 * i);

		memcpy(&back, &u, 4);
		CHECK(back == sig[i]);
	}
	CHECK_EQ(rd32(buf + h), 0x3FA00000u); /* 1,25 en IEEE, little-endian en el archivo */

	h = wavrec_header(buf, 0, 2, 32728, 4);
	CHECK_EQ(h, WAVREC_HDR_PCM);
	n = wavrec_encode_i16(buf + h, s16, 8);
	CHECK_EQ(n, 16);
	CHECK_EQ(rd16(buf + 20), 1);
	CHECK_EQ(rd32(buf + 24), 32728);
	CHECK_EQ(rd32(buf + 40), 16);
	CHECK_EQ(rd32(buf + 4), 36 + 16);
	for (int i = 0; i < 8; i++)
		CHECK_EQ((int16_t)rd16(buf + h + 2 * i), s16[i]);
}
