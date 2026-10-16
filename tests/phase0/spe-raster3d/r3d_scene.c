/*
 * SalsaDS — Fase 0.4: escena sintética. Ver r3d_scene.h.
 */
#include "r3d_scene.h"

#include <string.h>

static uint32_t rng(uint32_t *s)
{
	uint32_t x = *s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static uint32_t urange(uint32_t *s, uint32_t lo, uint32_t hi) { return lo + rng(s) % (hi - lo + 1u); }
static int32_t srange(uint32_t *s, int32_t lo, int32_t hi) { return lo + (int32_t)(rng(s) % (uint32_t)(hi - lo + 1)); }

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0;
	while ((r + 1) * (r + 1) <= v)
		r++;
	return r;
}

static void wr16le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static uint32_t bgr555(uint32_t r, uint32_t g, uint32_t b) { return (r & 31u) | (g & 31u) << 5 | (b & 31u) << 10; }

uint32_t r3d_scene_textures(uint8_t *vram_tex, uint8_t *vram_pal, unsigned ntex, struct r3d_texdef *defs)
{
	uint32_t seed = 0x7E57u, off = 0, poff = 0;
	memset(vram_tex, 0, R3D_VRAM_TEX_SIZE);
	memset(vram_pal, 0, R3D_VRAM_PAL_SIZE);
	if (ntex > R3D_SCENE_MAX_TEX)
		ntex = R3D_SCENE_MAX_TEX;
	for (unsigned k = 0; k < ntex; k++) {
		const unsigned m = k % 12u;
		const uint32_t ss = m < 8 ? 2u : 3u, ts = m < 8 ? 2u : m < 11 ? 3u : 2u;  /* 8 << n */
		const uint32_t w = 8u << ss, h = 8u << ts;
		const uint32_t fmt = k % 3u == 0 ? R3D_FMT_DIRECT : k % 3u == 1 ? R3D_FMT_PAL256 : R3D_FMT_PAL16;
		uint32_t param = R3D_TEXPARAM(off / 8u, ss, ts, fmt);
		if (k % 4u != 3u)
			param |= R3D_TEX_REPEAT_S | (k % 8u == 5u ? 0u : R3D_TEX_REPEAT_T);
		if (fmt != R3D_FMT_DIRECT && k % 2u == 1u)
			param |= R3D_TEX_COLOR0;
		defs[k].param = param;
		defs[k].pltt = 0;

		const uint32_t c1 = bgr555(urange(&seed, 8, 31), urange(&seed, 8, 31), urange(&seed, 8, 31));
		const uint32_t c2 = bgr555(urange(&seed, 0, 20), urange(&seed, 0, 20), urange(&seed, 0, 20));
		uint8_t *t = vram_tex + off;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				const uint32_t i = y * w + x, chk = ((x >> 2) ^ (y >> 2)) & 1u;
				switch (fmt) {
				case R3D_FMT_DIRECT: {
					uint32_t c = chk ? c1 : c2;
					c ^= (x * 32u / w) & 7u;            /* degradado en el rojo */
					c ^= ((y * 32u / h) & 7u) << 10;    /* y en el azul */
					wr16le(t + 2 * i, c | ((x + y) % 13u == 0 ? 0u : 0x8000u));
					break;
				}
				case R3D_FMT_PAL256:
					t[i] = (uint8_t)((x + y) % 11u == 0 ? 0u
									    : 1u + ((((x >> 3) ^ (y >> 3)) * 29u + x / 2u + y) % 255u));
					break;
				default: {
					uint32_t idx = (x + y) % 9u == 0 ? 0u : 1u + ((((x >> 2) ^ (y >> 2)) + x / 8u) % 15u);
					t[i / 2] |= (uint8_t)(idx << ((i & 1u) * 4u));
					break;
				}
				}
			}
		const uint32_t bytes = fmt == R3D_FMT_DIRECT ? w * h * 2u : fmt == R3D_FMT_PAL256 ? w * h : w * h / 2u;
		off += bytes;
		if (k & 1u)
			off += 8u;  /* el siguiente queda alineado a 8 y no a 16 */
		off = (off + 7u) & ~7u;

		if (fmt != R3D_FMT_DIRECT) {
			const uint32_t entries = fmt == R3D_FMT_PAL256 ? 256u : 16u;
			defs[k].pltt = (uint16_t)(poff / 16u);
			for (uint32_t e = 0; e < entries; e++)
				wr16le(vram_pal + poff + 2 * e,
				       bgr555(urange(&seed, 4, 31), urange(&seed, 4, 31), urange(&seed, 4, 31)));
			poff += entries * 2u;
		}
	}
	return off;
}

uint64_t r3d_scene_tris(struct r3d_tri *out, unsigned n, uint32_t seed, const struct r3d_texdef *defs,
			unsigned ntex)
{
	static struct r3d_tri trans[R3D_MAX_TRIS];
	static uint16_t perm[R3D_MAX_TRIS];
	uint32_t s = seed ? seed : 1u;
	uint64_t total = 0;
	unsigned nop = 0, ntr = 0;
	if (n > R3D_MAX_TRIS)
		n = R3D_MAX_TRIS;
	if (!n)
		return 0;

	/* Capas de z: una permutación al azar. */
	for (unsigned i = 0; i < n; i++)
		perm[i] = (uint16_t)i;
	for (unsigned i = n - 1; i > 0; i--) {
		unsigned j = rng(&s) % (i + 1u);
		uint16_t t = perm[i];
		perm[i] = perm[j];
		perm[j] = t;
	}
	const uint32_t spacing = 15099494u / n;  /* 0,9 · 2^24 / n */
	const int32_t jit = (int32_t)(spacing / 4u < 1000u ? spacing / 4u : 1000u);

	/* Lado de la caja de cada triángulo: un triángulo al azar en un cuadrado
	 * de lado L tiene área media ~0,076·L². */
	const uint32_t area = 3u * R3D_W1 * R3D_H1 / n;
	const int32_t L16 = (int32_t)isqrt(13u * area) * 16;

	for (unsigned i = 0; i < n; i++) {
		struct r3d_tri t;
		int64_t a2 = 0;
		memset(&t, 0, sizeof t);
		int32_t cx = 0, cy = 0;
		for (int attempt = 0; attempt < 64; attempt++) {
			cx = srange(&s, -8 * 16, 264 * 16);
			cy = srange(&s, -8 * 16, 200 * 16);
			const int32_t Li = L16 * (int32_t)urange(&s, 60, 140) / 100;
			for (int k = 0; k < 3; k++) {
				int32_t x = cx + srange(&s, -Li / 2, Li / 2), y = cy + srange(&s, -Li / 2, Li / 2);
				x = x < -32 * 16 ? -32 * 16 : x > 288 * 16 - 1 ? 288 * 16 - 1 : x;
				y = y < -32 * 16 ? -32 * 16 : y > 224 * 16 - 1 ? 224 * 16 - 1 : y;
				t.x[k] = (int16_t)x;
				t.y[k] = (int16_t)y;
			}
			a2 = ((int64_t)t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - ((int64_t)t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
			if ((a2 < 0 ? -a2 : a2) * 25 >= (int64_t)Li * Li)  /* nada de astillas */
				break;
		}
		total += (uint64_t)(a2 < 0 ? -a2 : a2);

		const uint32_t zc = 838861u + perm[i] * spacing + spacing / 2u;
		const struct r3d_texdef *d = &defs[rng(&s) % ntex];
		const int32_t tw = (int32_t)R3D_TEX_SIZE_S(d->param), th = (int32_t)R3D_TEX_SIZE_T(d->param);
		const int32_t s0 = srange(&s, 0, tw * 16), t0 = srange(&s, 0, th * 16);
		const int32_t ks = srange(&s, 8, 32), kt = srange(&s, 8, 32), rot = srange(&s, -8, 8);
		for (int k = 0; k < 3; k++) {
			t.z[k] = (uint32_t)((int32_t)zc + srange(&s, -jit, jit));
			t.w[k] = urange(&s, 4096, 6 * 4096);
			const int32_t dx = t.x[k] - cx, dy = t.y[k] - cy;
			int32_t sv = s0 + (dx * ks + dy * rot) / 16, tv = t0 + (dy * kt - dx * rot) / 16;
			sv = sv < -16000 ? -16000 : sv > 16000 ? 16000 : sv;
			tv = tv < -16000 ? -16000 : tv > 16000 ? 16000 : tv;
			t.s[k] = (int16_t)sv;
			t.t[k] = (int16_t)tv;
			t.rgb[k] = (uint16_t)bgr555(urange(&s, 10, 31), urange(&s, 10, 31), urange(&s, 10, 31));
		}
		t.texparam = d->param;
		t.pltt = d->pltt;
		if (rng(&s) % 4u == 0) {
			t.attr = (uint16_t)(urange(&s, 6, 26) | R3D_ATTR_TRANS | (rng(&s) % 8u == 0 ? R3D_ATTR_DEPTHWRITE : 0u));
			trans[ntr++] = t;
		} else {
			t.attr = 31;
			out[nop++] = t;
		}
	}
	memcpy(out + nop, trans, ntr * sizeof *trans);
	return total / 512u;  /* 2·área en 1/256 de px² → px² */
}
