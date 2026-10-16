/* SalsaDS — pruebas de la distribución de las dos pantallas (Fase 0.6). */
#include <math.h>

#include "check.h"
#include "frontend/layout.h"

static int overlap(const struct sds_rect *a, const struct sds_rect *b)
{
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

void test_layout(void)
{
	static const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 720, 480 }, { 720, 576 }, { 1024, 768 } };
	for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		const int w = sizes[i][0], h = sizes[i][1];
		for (int l = 0; l < SDS_LAYOUT_COUNT; l++) {
			for (int sc = 0; sc < SDS_SCALE_COUNT; sc++) {
				struct sds_rect r[2];
				sds_layout_compute((enum sds_layout)l, (enum sds_scale)sc, w, h, r);
				for (int k = 0; k < 2; k++) {
					CHECK(r[k].x >= 0 && r[k].y >= 0);
					CHECK(r[k].x + r[k].w <= w);
					CHECK(r[k].y + r[k].h <= h);
					CHECK(r[k].w > 0 && r[k].h > 0);
					/* la proporción de la DS (4:3) se conserva */
					CHECK(fabsf((float)r[k].w / (float)r[k].h - 256.0f / 192.0f) < 0.02f);
					if (sc == SDS_SCALE_INTEGER) {
						CHECK(r[k].w == (int)r[k].scale * 256);
						CHECK(r[k].h == (int)r[k].scale * 192);
					}
				}
				CHECK(!overlap(&r[0], &r[1]));
			}
		}
	}
	struct sds_rect r[2];
	/* Valores conocidos a 720p y 1080p. */
	sds_layout_compute(SDS_LAYOUT_SIDE, SDS_SCALE_INTEGER, 1280, 720, r);
	CHECK_EQ(r[0].w, 512);
	CHECK_EQ(r[1].x, r[0].x + 512);
	sds_layout_compute(SDS_LAYOUT_SIDE, SDS_SCALE_FIT, 1280, 720, r);
	CHECK_EQ(r[0].w, 640);  /* 2,5× */
	CHECK_EQ(r[0].x, 0);
	CHECK_EQ(r[1].x, 640);
	sds_layout_compute(SDS_LAYOUT_STACKED, SDS_SCALE_FIT, 1280, 720, r);
	CHECK_EQ(r[0].h, 360);  /* 1,875× */
	CHECK_EQ(r[1].y, 360);
	sds_layout_compute(SDS_LAYOUT_STACKED, SDS_SCALE_INTEGER, 1920, 1080, r);
	CHECK_EQ(r[0].w, 512);  /* 2× */
	sds_layout_compute(SDS_LAYOUT_BIG_TOP, SDS_SCALE_INTEGER, 1280, 720, r);
	CHECK_EQ(r[0].w, 768);  /* 3× */
	CHECK_EQ(r[1].w, 512);  /* 2× */
	sds_layout_compute(SDS_LAYOUT_BIG_BOTTOM, SDS_SCALE_INTEGER, 1920, 1080, r);
	CHECK_EQ(r[1].w, 1280);  /* 5×, la táctil */
	CHECK_EQ(r[0].w, 512);
	sds_layout_compute(SDS_LAYOUT_BIG_TOP, SDS_SCALE_FIT, 1920, 1080, r);
	CHECK_EQ(r[0].w, 1280);
	CHECK_EQ(r[1].w, 640);
	CHECK_EQ(r[0].x + r[0].w + r[1].w, 1920);
}
