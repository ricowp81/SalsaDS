/*
 * SalsaDS — mezclador de sonido de la DS (ver snd.h). Código propio; el comportamiento sale de GBATEK y de lo medido
 * contra melonDS 1.1 con la ROM `snd` (tools/snd-compare.py). Sin asignaciones ni libc: compila en el SPE.
 *
 * Presupuesto del local store (SPE de audio): snd_state ≈ 3,4 KB; la entrada de escrituras (SND_LOG_MAX * 16 =
 * 32 KB), 2 * SND_TICKS_MAX muestras nativas (2,5 KB), la salida flotante (SND_OUT_MAX * 8 = 8 KB) y el escalón de la
 * captura (2 * 1 KB): menos de 60 KB en total.
 */
#include "audio/snd.h"
#include "audio/snd_fir.h"
#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

static const int16_t ima_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
	118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
	1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
	6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};
static const int8_t ima_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

void snd_reset(snd_state *st)
{
	uint8_t *p = (uint8_t *)st;

	for (uint32_t i = 0; i < sizeof *st; i++)
		p[i] = 0;
	for (int c = 0; c < SND_NCH; c++)
		st->ch[c].tag = 0xFFFFFFFFu;
	st->bias = 0x200;
}

/* ---- acceso a la memoria del ARM7 ------------------------------------------------------------------------- */

static inline uint32_t fetch_byte(const snd_bus *bus, snd_chan *c, uint32_t addr)
{
	const uint32_t line = addr & ~(uint32_t)(SND_LINE - 1);

	if (c->tag != line) {
		bus->fetch(bus->ctx, line, c->line);
		c->tag = line;
	}
	return c->line[addr & (SND_LINE - 1)];
}

/* ---- escrituras a registros -------------------------------------------------------------------------------- */

static void chan_start(snd_chan *c, int idx)
{
	(void)idx;
	c->on = 1;
	c->tcnt = c->tmr;
	c->pos = ((c->cnt >> 29) & 3u) == 3u ? 0 : -3;
	c->out = 0;
	c->hdr = 0;
	c->adv = c->adi = c->alv = c->ali = 0;
	c->lfsr = 0x7FFF;
	c->tag = 0xFFFFFFFFu;
}

static void cap_start(snd_cap *k)
{
	k->on = 1;
	k->pos = 0;
	k->base = k->dad & 0x07FFFFFCu;
	k->bytes = (k->len ? k->len : 1u) * 4u;
}

static void apply(snd_state *st, const snd_ent *e)
{
	const uint32_t off = e->off;
	const uint32_t v = e->val, m = e->mask;

	if (off < 0x100u) {
		snd_chan *c = &st->ch[off >> 4];

		switch (off & 15u) {
		case 0:
			c->cnt = (c->cnt & ~m) | (v & m);
			break;
		case 2: {
			const uint32_t hi = (c->cnt >> 16) & 0x7FFFu;
			const uint32_t nhi = (hi & ~(m & 0x7FFFu)) | (v & m & 0x7FFFu);

			c->cnt = (c->cnt & 0xFFFFu) | (nhi << 16);
			if (m & 0x8000u) {
				const int want = (v & 0x8000u) != 0;

				if (want && !c->on)
					chan_start(c, (int)(off >> 4));
				else if (!want && c->on) {
					c->on = 0;
					c->out = 0;
				}
			}
			break;
		}
		case 4:
			c->sad = ((c->sad & ~m) | (v & m)) & 0x07FFFFFCu;
			break;
		case 6:
			c->sad = ((c->sad & ~(m << 16)) | ((v & m) << 16)) & 0x07FFFFFCu;
			break;
		case 8:
			c->tmr = (uint16_t)((c->tmr & ~m) | (v & m));
			break;
		case 10:
			c->pnt = (uint16_t)((c->pnt & ~m) | (v & m));
			break;
		case 12:
			c->len = ((c->len & ~m) | (v & m)) & 0x3FFFFFu;
			break;
		case 14:
			c->len = ((c->len & ~(m << 16)) | ((v & m) << 16)) & 0x3FFFFFu;
			break;
		default:
			break;
		}
		return;
	}
	switch (off) {
	case 0x100:
		st->soundcnt = (uint16_t)((st->soundcnt & ~m) | (v & m)) & 0xBF7Fu;
		break;
	case 0x104:
		st->bias = (uint16_t)(((st->bias & ~m) | (v & m)) & 0x3FFu);
		break;
	case 0x108: /* SNDCAP0CNT (byte bajo) y SNDCAP1CNT (byte alto) */
		for (int i = 0; i < 2; i++) {
			snd_cap *k = &st->cap[i];
			const uint32_t bm = (m >> (8 * i)) & 0xFFu, bv = (v >> (8 * i)) & 0xFFu;

			if (!bm)
				continue;
			{
				const uint32_t old = k->cnt, nw = (old & ~bm) | (bv & bm);

				k->cnt = (uint8_t)(nw & 0x8F);
				if (!(old & 0x80u) && (nw & 0x80u))
					cap_start(k);
				else if ((old & 0x80u) && !(nw & 0x80u))
					k->on = 0;
			}
		}
		break;
	case 0x110:
		st->cap[0].dad = (st->cap[0].dad & ~m) | (v & m);
		break;
	case 0x112:
		st->cap[0].dad = (st->cap[0].dad & ~(m << 16)) | ((v & m) << 16);
		break;
	case 0x114:
		st->cap[0].len = (st->cap[0].len & ~m) | (v & m);
		break;
	case 0x118:
		st->cap[1].dad = (st->cap[1].dad & ~m) | (v & m);
		break;
	case 0x11A:
		st->cap[1].dad = (st->cap[1].dad & ~(m << 16)) | ((v & m) << 16);
		break;
	case 0x11C:
		st->cap[1].len = (st->cap[1].len & ~m) | (v & m);
		break;
	default:
		break;
	}
}

/* ---- un canal: un desbordamiento del temporizador -------------------------------------------------------- */

static void chan_step(snd_chan *c, int idx, const snd_bus *bus)
{
	const uint32_t fmt = (c->cnt >> 29) & 3u, mode = (c->cnt >> 27) & 3u;
	uint32_t total, loopstart;

	if (fmt == 3) {
		if (idx >= 14) { /* ruido: LFSR de 15 bits, un paso por desbordamiento */
			const uint32_t carry = c->lfsr & 1u;

			c->lfsr >>= 1;
			if (carry) {
				c->lfsr ^= 0x6000u;
				c->out = -0x7FFF;
			} else {
				c->out = 0x7FFF;
			}
		} else if (idx >= 8) { /* PSG: 8 pasos por ciclo; el nivel alto son los últimos duty + 1 (el 7 no sube nunca) */
			const uint32_t duty = (c->cnt >> 24) & 7u;
			const int hi = duty != 7u && (uint32_t)c->pos >= 7u - duty;

			c->out = hi ? 0x7FFF : -0x7FFF;
			c->pos = (c->pos + 1) & 7;
		} else {
			c->out = 0;
		}
		return;
	}
	if (c->pos < 0 && ++c->pos < 0) /* tres desbordamientos de arranque: el tercero ya lee la muestra 0 */
		return;
	total = (uint32_t)(c->pnt + c->len);
	loopstart = c->pnt;
	if (fmt == 0) {
		total *= 4u;
		loopstart *= 4u;
	} else if (fmt == 1) {
		total *= 2u;
		loopstart *= 2u;
	} else {
		total *= 8u;
		loopstart *= 8u;
	}
	if ((uint32_t)c->pos >= total) {
		if (mode == 2) { /* una vez: se apaga (y la salida queda en cero) */
			c->on = 0;
			c->out = 0;
			return;
		}
		c->pos = (int32_t)loopstart;
		if (fmt == 2) {
			c->adv = c->alv;
			c->adi = c->ali;
		}
	}
	if (fmt == 2 && (uint32_t)c->pos == loopstart) {
		c->alv = c->adv;
		c->ali = c->adi;
	}
	if (fmt == 0) {
		c->out = (int32_t)(int8_t)fetch_byte(bus, c, c->sad + (uint32_t)c->pos) * 256;
	} else if (fmt == 1) {
		const uint32_t a = c->sad + (uint32_t)c->pos * 2u;

		c->out = (int32_t)(int16_t)(uint16_t)(fetch_byte(bus, c, a) | fetch_byte(bus, c, a + 1u) << 8);
	} else {
		if (c->pos < 8) {
			/* la cabecera (4 bytes = 8 nibbles): la salida sigue siendo el valor de antes; al llegar al último nibble se
			 * cargan el valor y el índice iniciales (la primera vez; después de un bucle el estado viene del punto de bucle) */
			c->out = c->adv;
			if (c->pos == 7 && !c->hdr) {
				uint32_t h = 0;

				for (uint32_t i = 0; i < 4; i++)
					h |= fetch_byte(bus, c, c->sad + i) << (8 * i);
				c->adv = (int32_t)(int16_t)(uint16_t)h;
				c->adi = (int32_t)((h >> 16) & 0x7Fu);
				if (c->adi > 88)
					c->adi = 88;
				c->hdr = 1;
			}
			c->pos++;
			return;
		} else {
			const uint32_t b = fetch_byte(bus, c, c->sad + ((uint32_t)c->pos >> 1));
			const uint32_t nib = (c->pos & 1) ? b >> 4 : b & 15u;
			const int32_t step = ima_step[c->adi];
			int32_t diff = step >> 3;

			if (nib & 1u)
				diff += step >> 2;
			if (nib & 2u)
				diff += step >> 1;
			if (nib & 4u)
				diff += step;
			c->adv += (nib & 8u) ? -diff : diff;
			if (c->adv > 0x7FFF)
				c->adv = 0x7FFF;
			if (c->adv < -0x7FFF)
				c->adv = -0x7FFF;
			c->adi += ima_adj[nib & 7u];
			if (c->adi < 0)
				c->adi = 0;
			if (c->adi > 88)
				c->adi = 88;
			c->out = c->adv;
		}
	}
	c->pos++;
}

/* ---- captura ------------------------------------------------------------------------------------------------ */

#define CAP_STAGE 1024

typedef struct cap_run {
	uint8_t buf[CAP_STAGE] __attribute__((aligned(16)));
	uint32_t addr, n;
} cap_run;

static void cap_flush(const snd_bus *bus, cap_run *r)
{
	if (r->n) {
		bus->store(bus->ctx, r->addr, r->buf, r->n);
		r->n = 0;
	}
}

static void cap_put(const snd_bus *bus, snd_cap *k, cap_run *r, int32_t v)
{
	const uint32_t size = (k->cnt & 8u) ? 1u : 2u;
	const uint32_t at = k->base + k->pos;

	if (r->n && (r->addr + r->n != at || r->n + size > CAP_STAGE))
		cap_flush(bus, r);
	if (!r->n)
		r->addr = at;
	if (size == 1u) {
		r->buf[r->n++] = (uint8_t)(v >> 8);
	} else {
		r->buf[r->n++] = (uint8_t)v;
		r->buf[r->n++] = (uint8_t)(v >> 8);
	}
	k->pos += size;
	if (k->pos >= k->bytes) {
		if (k->cnt & 4u) {
			k->on = 0;
			k->cnt = (uint8_t)(k->cnt & ~0x80u);
		} else {
			k->pos = 0;
		}
	}
}

/* ---- remuestreo ------------------------------------------------------------------------------------------- */

/*
 * Remuestreo de 32,7 kHz a 48 kHz con un FIR polifásico (tabla de snd_fir.h, generada por tools/gen-snd-fir.py: 32
 * coeficientes, 64 fases con interpolación lineal entre filas, pasabajos de 15 kHz con ventana de Kaiser). Las muestras
 * van planas por canal en wl/wr: las primeras SND_HIST son las últimas del trabajo anterior (`st->hist`) y después las
 * `n` nuevas. Cada salida mira 15 muestras antes y 16 después de su posición, así que sale retrasada 16 muestras
 * nativas (~0,5 ms) y el trabajo acaba con la posición `n + 16` de la ventana, que en el siguiente trabajo vuelve a ser
 * la 16. `st->frac` es la fase dentro de la muestra. La suma usa 4 acumuladores (los carriles de un vector) y se junta
 * como (0+1)+(2+3): lo mismo en C y en el SPE (vectorizado: 4 coeficientes por vector, 8 madd por canal y salida).
 */
#define SND_AFTER_MAX 16 /* muestras mirando hacia delante */
#define FIR_K0 (SND_HIST - SND_AFTER_MAX) /* posición inicial en la ventana: 16 */

#ifdef __SPU__
typedef vec_float4 fir_vec;

static inline void fir_coefs(float *c, const float *h0, const float *h1, float a)
{
	const vec_float4 av = spu_splats(a);

	for (int v = 0; v < SND_FIR_TAPS / 4; v++) {
		const vec_float4 x0 = ((const vec_float4 *)h0)[v];

		((vec_float4 *)c)[v] = spu_madd(av, spu_sub(((const vec_float4 *)h1)[v], x0), x0);
	}
}

/* x puede estar en cualquier múltiplo de 4 bytes: 9 vectores alineados dan los 8 desplazados */
static inline float fir_dot(const float *c, const float *x)
{
	static const vec_uchar16 pats[4] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
		{ 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 },
		{ 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
		{ 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 }
	};
	const unsigned off = (unsigned)((uintptr_t)x & 15u);
	const vec_float4 *xv = (const vec_float4 *)((uintptr_t)x & ~(uintptr_t)15);
	const vec_uchar16 pat = pats[off >> 2];
	vec_float4 acc = spu_splats(0.0f), cur = xv[0];

	for (int v = 0; v < SND_FIR_TAPS / 4; v++) {
		const vec_float4 nxt = xv[v + 1];

		acc = spu_madd(((const vec_float4 *)c)[v], spu_shuffle(cur, nxt, pat), acc);
		cur = nxt;
	}
	return (spu_extract(acc, 0) + spu_extract(acc, 1)) + (spu_extract(acc, 2) + spu_extract(acc, 3));
}
#else
static inline void fir_coefs(float *c, const float *h0, const float *h1, float a)
{
	for (int j = 0; j < SND_FIR_TAPS; j++)
		c[j] = h0[j] + a * (h1[j] - h0[j]);
}

static inline float fir_dot(const float *c, const float *x)
{
	float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

	for (int j = 0; j < SND_FIR_TAPS; j += 4) {
		a0 += c[j] * x[j];
		a1 += c[j + 1] * x[j + 1];
		a2 += c[j + 2] * x[j + 2];
		a3 += c[j + 3] * x[j + 3];
	}
	return (a0 + a1) + (a2 + a3);
}
#endif

static uint32_t snd_resample(snd_state *st, const float *wl, const float *wr, uint32_t n, float *out, uint32_t out_max)
{
	const float ratio = (SND_RATE_NATIVE / SND_RATE_OUT) * (1.0f + st->ratio_adj);
	const uint32_t last = SND_HIST + n - SND_AFTER_MAX; /* se puede sacar mientras k < last (k + 16 < SND_HIST + n) */
	float f = st->frac;
	uint32_t k = FIR_K0, produced = 0;
	float c[SND_FIR_TAPS] __attribute__((aligned(16)));

	while (produced < out_max) {
		float pf, a;
		uint32_t ph;

		while (f >= 1.0f) {
			f -= 1.0f;
			k++;
		}
		if (k >= last)
			break;
		pf = f * (float)SND_FIR_PHASES;
		ph = (uint32_t)pf;
		a = pf - (float)ph;
		fir_coefs(c, snd_fir[ph], snd_fir[ph + 1], a);
		out[2 * produced] = fir_dot(c, wl + (k - SND_FIR_BEFORE));
		out[2 * produced + 1] = fir_dot(c, wr + (k - SND_FIR_BEFORE));
		produced++;
		f += ratio;
	}
	/* si faltó sitio en `out` lo pendiente se descarta: la ventana siguiente empieza en n + 16 */
	if (k < last)
		f = 0.0f;
	st->frac = f;
	return produced;
}

/* ---- el trabajo ---------------------------------------------------------------------------------------------- */

static inline int32_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

uint32_t snd_run(snd_state *st, const snd_ent *log, uint32_t nlog, uint32_t nticks, const snd_bus *bus, int16_t *nat,
		 float *out, uint32_t out_max, uint32_t *out_n)
{
	static const uint8_t divsh[4] = { 0, 1, 2, 4 };
	cap_run cr[2];
	float wl_[SND_HIST + SND_TICKS_MAX + 4] __attribute__((aligned(16)));
	float wr_[SND_HIST + SND_TICKS_MAX + 4] __attribute__((aligned(16)));
	float *const wl = wl_, *const wr = wr_;
	uint32_t ie = 0, peak = 0;

	if (nticks > SND_TICKS_MAX)
		nticks = SND_TICKS_MAX;
	cr[0].n = cr[1].n = 0;
	for (uint32_t i = 0; i < SND_HIST; i++) {
		wl[i] = st->hist[0][i];
		wr[i] = st->hist[1][i];
	}
	for (int c = 0; c < SND_NCH; c++)
		st->ch[c].tag = 0xFFFFFFFFu; /* lo que haya en RAM ahora (el juego pudo rellenar el búfer) */
	for (uint32_t t = 0; t < nticks; t++) {
		const uint64_t T = st->tick + t;
		int32_t mix[2] = { 0, 0 }, c1[3] = { 0, 0, 0 }, c3[3] = { 0, 0, 0 };
		int32_t L, R, capv[2];
		uint32_t sc;

		while (ie < nlog && log[ie].tick < T)
			apply(st, &log[ie++]);
		for (int ci = 0; ci < SND_NCH; ci++) {
			snd_chan *c = &st->ch[ci];
			int32_t x, l, r;
			uint32_t vol, pan;

			if (!c->on)
				continue;
			c->tcnt += 512u;
			while (c->tcnt >= 0x10000u && c->on) {
				c->tcnt = c->tcnt - 0x10000u + c->tmr;
				chan_step(c, ci, bus);
			}
			if (!c->on)
				continue;
			vol = c->cnt & 127u;
			if (vol == 127u)
				vol = 128u;
			pan = (c->cnt >> 16) & 127u;
			if (pan == 127u)
				pan = 128u;
			/* un solo desplazamiento al final (medido contra melonDS: con redondeos intermedios cambian muestras sueltas) */
			l = (c->out * (int32_t)(vol * (128u - pan))) >> (14 + divsh[(c->cnt >> 8) & 3u]);
			r = (c->out * (int32_t)(vol * pan)) >> (14 + divsh[(c->cnt >> 8) & 3u]);
			x = (c->out * (int32_t)vol) >> (7 + divsh[(c->cnt >> 8) & 3u]);
			if (ci == 1) {
				c1[0] = l;
				c1[1] = r;
				c1[2] = x;
				if (st->soundcnt & 0x1000u)
					continue;
			} else if (ci == 3) {
				c3[0] = l;
				c3[1] = r;
				c3[2] = x;
				if (st->soundcnt & 0x2000u)
					continue;
			}
			mix[0] += l;
			mix[1] += r;
		}
		/* salida izquierda y derecha: el mezclador o los canales 1 y 3 (SOUNDCNT bits 8-11) */
		sc = st->soundcnt;
		{
			const int32_t ml = clamp16(mix[0]), mr = clamp16(mix[1]);

			capv[0] = (st->cap[0].cnt & 2u) ? clamp16(c1[2]) : ml;
			capv[1] = (st->cap[1].cnt & 2u) ? clamp16(c3[2]) : mr;
			switch ((sc >> 8) & 3u) {
			case 0: L = ml; break;
			case 1: L = c1[0]; break;
			case 2: L = c3[0]; break;
			default: L = clamp16(c1[0] + c3[0]); break;
			}
			switch ((sc >> 10) & 3u) {
			case 0: R = mr; break;
			case 1: R = c1[1]; break;
			case 2: R = c3[1]; break;
			default: R = clamp16(c1[1] + c3[1]); break;
			}
		}
		for (int i = 0; i < 2; i++)
			if (st->cap[i].on)
				cap_put(bus, &st->cap[i], &cr[i], capv[i]);
		{
			int32_t mv = (int32_t)(sc & 127u);

			if (mv == 127)
				mv = 128;
			L = (L * mv) >> 7;
			R = (R * mv) >> 7;
			if (!(sc & 0x8000u))
				L = R = 0;
		}
		if (nat) {
			nat[2 * t] = (int16_t)L;
			nat[2 * t + 1] = (int16_t)R;
		}
		{
			const uint32_t al = (uint32_t)(L < 0 ? -L : L), ar = (uint32_t)(R < 0 ? -R : R);

			if (al > peak)
				peak = al;
			if (ar > peak)
				peak = ar;
		}
		if (st->mode & SND_MODE_ORIGINAL) { /* 10 bits con el sesgo: así la salida de la DS */
			int32_t ql = (L >> 6) + (int32_t)st->bias, qr = (R >> 6) + (int32_t)st->bias;

			ql = ql < 0 ? 0 : ql > 1023 ? 1023 : ql;
			qr = qr < 0 ? 0 : qr > 1023 ? 1023 : qr;
			wl[SND_HIST + t] = (float)(ql - 512) * (1.0f / 512.0f);
			wr[SND_HIST + t] = (float)(qr - 512) * (1.0f / 512.0f);
		} else {
			wl[SND_HIST + t] = (float)L * (1.0f / 32768.0f);
			wr[SND_HIST + t] = (float)R * (1.0f / 32768.0f);
		}
	}
	cap_flush(bus, &cr[0]);
	cap_flush(bus, &cr[1]);
	st->tick += nticks;
	st->busy = 0;
	for (int c = 0; c < SND_NCH; c++)
		if (st->ch[c].on)
			st->busy |= 1u << c;
	if (st->cap[0].on)
		st->busy |= 1u << 16;
	if (st->cap[1].on)
		st->busy |= 1u << 17;
	st->peak = peak;
	st->ticks_run = nticks;
	st->log_applied = ie;
	if (out && out_n) {
		*out_n = snd_resample(st, wl, wr, nticks, out, out_max);
		for (uint32_t i = 0; i < SND_HIST; i++) { /* historia del siguiente trabajo: las últimas SND_HIST de la ventana */
			st->hist[0][i] = wl[nticks + i];
			st->hist[1][i] = wr[nticks + i];
		}
		st->nout = *out_n;
	}
	return ie;
}
