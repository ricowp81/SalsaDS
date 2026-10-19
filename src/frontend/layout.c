/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — distribución de las dos pantallas. Ver layout.h.
 */
#include "frontend/layout.h"

#include <math.h>

static float minf(float a, float b)
{
	return a < b ? a : b;
}

static int imin(int a, int b)
{
	return a < b ? a : b;
}

static int ifloor_scale(float s)
{
	const int i = (int)floorf(s + 1e-4f);
	return i < 1 ? 1 : i;
}

static struct sds_rect make(int x, int y, float scale)
{
	struct sds_rect r;
	r.scale = scale;
	r.w = (int)floorf(SDS_SCREEN_W * scale + 0.5f);
	r.h = (int)floorf(SDS_SCREEN_H * scale + 0.5f);
	r.x = x;
	r.y = y;
	return r;
}

void sds_layout_compute(enum sds_layout layout, enum sds_scale scale, int out_w, int out_h, struct sds_rect out[2])
{
	const int integer = scale == SDS_SCALE_INTEGER;
	switch (layout) {
	case SDS_LAYOUT_SIDE: {
		float s = minf((float)out_w / (2.0f * SDS_SCREEN_W), (float)out_h / SDS_SCREEN_H);
		if (integer)
			s = (float)ifloor_scale(s);
		out[0] = make(0, 0, s);
		out[1] = make(0, 0, s);
		const int x0 = (out_w - 2 * out[0].w) / 2, y0 = (out_h - out[0].h) / 2;
		out[0].x = x0;
		out[0].y = y0;
		out[1].x = x0 + out[0].w;
		out[1].y = y0;
		break;
	}
	case SDS_LAYOUT_STACKED: {
		float s = minf((float)out_w / SDS_SCREEN_W, (float)out_h / (2.0f * SDS_SCREEN_H));
		if (integer)
			s = (float)ifloor_scale(s);
		out[0] = make(0, 0, s);
		out[1] = make(0, 0, s);
		const int x0 = (out_w - out[0].w) / 2, y0 = (out_h - 2 * out[0].h) / 2;
		out[0].x = out[1].x = x0;
		out[0].y = y0;
		out[1].y = y0 + out[0].h;
		break;
	}
	default: {  /* grande + chica */
		float big, small;
		if (integer) {
			int b = ifloor_scale(minf((float)out_w / (SDS_SCREEN_W * 1.5f), (float)out_h / SDS_SCREEN_H));
			/* La mayor entera que deja sitio, al menos 1×, para otra de 1×. */
			while (b > 1 && (b * SDS_SCREEN_W + SDS_SCREEN_W > out_w || b * SDS_SCREEN_H > out_h))
				b--;
			int s = imin(imin((out_w - b * SDS_SCREEN_W) / SDS_SCREEN_W, out_h / SDS_SCREEN_H), b);
			if (s < 1)
				s = 1;
			big = (float)b;
			small = (float)s;
		} else {
			big = minf((float)out_w / (SDS_SCREEN_W * 1.5f), (float)out_h / SDS_SCREEN_H);
			small = big / 2.0f;
		}
		struct sds_rect rb = make(0, 0, big), rs = make(0, 0, small);
		const int x0 = (out_w - (rb.w + rs.w)) / 2, y0 = (out_h - rb.h) / 2;
		rb.x = x0;
		rb.y = y0;
		rs.x = x0 + rb.w;
		rs.y = y0 + rb.h - rs.h;  /* abajo, a la altura del borde inferior de la grande */
		if (layout == SDS_LAYOUT_BIG_TOP) {
			out[0] = rb;
			out[1] = rs;
		} else {
			out[0] = rs;
			out[1] = rb;
		}
		break;
	}
	}
}

const char *sds_layout_name(enum sds_layout layout)
{
	static const char *const n[SDS_LAYOUT_COUNT] = { "lado a lado", "una sobre otra", "grande arriba", "grande abajo" };
	return (unsigned)layout < SDS_LAYOUT_COUNT ? n[layout] : "?";
}

const char *sds_scale_name(enum sds_scale scale)
{
	return scale == SDS_SCALE_INTEGER ? "entera" : "ajustada";
}
