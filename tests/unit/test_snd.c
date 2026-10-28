/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — pruebas del mezclador de sonido (src/audio, Fase 2.2): el modelo (volumen, balance, bucles, ADPCM, PSG, ruido),
 * que partir un trabajo en trozos da lo mismo que correrlo entero (el estado se arrastra), la captura y el remuestreo.
 * Lo que sale del mezclador se compara muestra a muestra con melonDS con la ROM `snd` (tools/snd-compare.py); aquí
 * se prueba lo que se puede decir sin él y que da igual en little y big endian. */
#include <math.h>
#include <string.h>

#include "audio/snd.h"
#include "check.h"
#include "util/endian.h"

#define MEM_SIZE 0x10000u
static uint8_t mem[MEM_SIZE] __attribute__((aligned(128)));

static void bus_fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	(void)ctx;
	if ((addr >> 23) == 4)
		memcpy(dst, mem + (addr & (MEM_SIZE - 1)), SND_LINE);
	else
		memset(dst, 0, SND_LINE);
}

static void bus_store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	(void)ctx;
	if ((addr >> 23) == 4)
		memcpy(mem + (addr & (MEM_SIZE - 1)), src, n);
}

static const snd_bus bus = { NULL, bus_fetch, bus_store };

static snd_ent log_buf[128];
static uint32_t nlog;

static void w16(uint32_t off, uint32_t v)
{
	log_buf[nlog++] = (snd_ent){ .tick = 0, .off = (uint16_t)off, .val = (uint16_t)v, .mask = 0xFFFF };
}

static void w32(uint32_t off, uint32_t v)
{
	w16(off, v & 0xFFFFu);
	w16(off + 2, v >> 16);
}

static void chan(unsigned c, uint32_t cnt, uint32_t sad, uint32_t tmr, uint32_t pnt, uint32_t len)
{
	w32(16 * c + 4, sad);
	w16(16 * c + 8, tmr);
	w16(16 * c + 10, pnt);
	w32(16 * c + 12, len);
	w32(16 * c, cnt | 0x80000000u);
}

#define CNT(vol, div, pan, mode, fmt) ((uint32_t)(vol) | (uint32_t)(div) << 8 | (uint32_t)(pan) << 16 | (uint32_t)(mode) << 27 | (uint32_t)(fmt) << 29)
#define P512 0xFE00u

static void reset(void)
{
	nlog = 0;
	memset(mem, 0, sizeof mem);
}

static void run(snd_state *st, uint32_t nticks, int16_t *nat)
{
	snd_run(st, log_buf, nlog, nticks, &bus, nat, NULL, 0, NULL);
}

static void test_model(void)
{
	static snd_state st;
	static int16_t nat[2 * SND_TICKS_MAX];

	/* PCM16 constante 0x4000, volumen 127 (cuenta como 128), balance al centro: la mitad en cada lado (8192) */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 64, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 20, nat);
	CHECK_EQ(nat[2 * 0], 0);   /* la muestra 0 llega al tercer desbordamiento del arranque */
	CHECK_EQ(nat[2 * 1], 0);
	CHECK_EQ(nat[2 * 2], 0);
	CHECK_EQ(nat[2 * 3], 8192);
	CHECK_EQ(nat[2 * 3 + 1], 8192);
	CHECK_EQ(nat[2 * 19], 8192);

	/* el divisor va después del balance (así redondea melonDS): vol 37, div 1, pan 77 sobre -16384 da -944 a la izquierda */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, (uint16_t)(int16_t)-16384);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(37, 1, 77, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 5], -944);
	CHECK_EQ(nat[2 * 5 + 1], -1425);

	/* volumen 0 y balance en los extremos */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	chan(1, CNT(127, 0, 127, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 5], 16384);      /* canal 0 todo a la izquierda */
	CHECK_EQ(nat[2 * 5 + 1], 16384);  /* canal 1 todo a la derecha */

	/* varias voces se suman y se recortan a 16 bits */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x7FFF);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	for (unsigned c = 0; c < 6; c++)
		chan(c, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 6], 32767);

	/* una vez: se apaga y la salida queda en cero; el estado ocupado baja */
	reset();
	for (uint32_t i = 0; i < 16; i++)
		wr16le(mem + 0x100 + 2 * i, 0x2000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 2, 1), 0x02000100u, P512, 0, 8);
	run(&st, 8, nat);
	CHECK_EQ(st.busy & 1u, 1u);
	run(&st, 30, nat);
	CHECK_EQ(st.busy & 1u, 0u);
	CHECK_EQ(nat[2 * 29], 0);

	/* bucle con PNT: vuelve a la muestra PNT * 2 */
	reset();
	for (uint32_t i = 0; i < 16; i++)
		wr16le(mem + 0x100 + 2 * i, (uint16_t)(i * 100));
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 2, 4); /* 6 palabras = 12 muestras; el bucle empieza en la 4 */
	run(&st, 40, nat);
	{
		/* muestras 0..11 (ticks 3..14) y después 4..11 una y otra vez */
		CHECK_EQ(nat[2 * 3], 0);
		CHECK_EQ(nat[2 * 14], 1100);
		CHECK_EQ(nat[2 * 15], 400);
		CHECK_EQ(nat[2 * 22], 1100);
		CHECK_EQ(nat[2 * 23], 400);
	}

	/* ruido: la secuencia del LFSR de 15 bits (primeros pasos con todo en uno) y PSG: ciclo de 8 pasos */
	reset();
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(14, CNT(127, 0, 0, 0, 3), 0, P512, 0, 0);
	chan(8, CNT(127, 0, 127, 0, 3) | (3u << 24), 0, P512, 0, 0);
	run(&st, 20, nat);
	CHECK_EQ(nat[2 * 1], -32767);  /* tick 1: primer desplazamiento del ruido, bit en uno: bajo */
	{
		/* PSG con duty 3: bajo los pasos 0-3, alto los 4-7; ticks 1..: pasos 0,1,2... */
		int16_t r[8];

		for (int i = 0; i < 8; i++)
			r[i] = nat[2 * (1 + i) + 1];
		CHECK_EQ(r[0], -32767);
		CHECK_EQ(r[3], -32767);
		CHECK_EQ(r[4], 32767);
		CHECK_EQ(r[7], 32767);
	}

	/* volumen general: SOUNDCNT bajo y apagado */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x8040);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 5], 8192);  /* 0x4000 * 64 / 128 */
	w16(0x100, 0x0040);
	nlog = 0;
	w16(0x100, 0x0040);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	run(&st, 4, nat);
	CHECK_EQ(nat[0], 0);
}

static void test_capture(void)
{
	static snd_state st;
	static int16_t nat[2 * SND_TICKS_MAX];

	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 64, 1, 1), 0x02000100u, P512, 0, 64);
	w32(0x110, 0x02001000u);
	w16(0x114, 4);   /* 4 palabras = 8 muestras PCM16 */
	w16(0x108, 0x84); /* una vez, PCM16, mezclador izquierdo */
	run(&st, 40, nat);
	/* la captura arranca en el tick siguiente al de la escritura y copia 8 muestras del mezclador: 0,0,0,8192,... */
	CHECK_EQ(rd16le(mem + 0x1000 + 2 * 0), 0);
	CHECK_EQ(rd16le(mem + 0x1000 + 2 * 3), 8192);
	CHECK_EQ(rd16le(mem + 0x1000 + 2 * 7), 8192);
	CHECK_EQ(rd16le(mem + 0x1000 + 2 * 8), 0);  /* no pasa de lo pedido */
	CHECK_EQ(st.busy >> 16 & 1u, 0u);

	/* PCM8: el byte alto de cada muestra */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st); st.mode = SND_MODE_CORE_OLD;
	nlog = 0;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 64, 1, 1), 0x02000100u, P512, 0, 64);
	w32(0x110, 0x02001000u);
	w16(0x114, 2);
	w16(0x108, 0x8C);
	run(&st, 20, nat);
	CHECK_EQ(mem[0x1000 + 4], 0x20);
	CHECK_EQ(mem[0x1000 + 7], 0x20);
	CHECK_EQ(mem[0x1000 + 8], 0);
}

static void test_capture_playback_boundary(void)
{
	static snd_state keep, old;
	static int16_t first[100], a[8], b[8];

	reset();
	for (uint32_t i = 0; i < 64; i++)
		wr16le(mem + 0x100 + 2 * i, 0x2000);
	snd_reset(&keep); keep.mode = SND_MODE_CORE_OLD;
	w16(0x100, 0x807F);
	chan(1, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 32);
	run(&keep, 20, first);
	old = keep;
	old.mode = SND_MODE_FRAME_CACHE_OLD | SND_MODE_CORE_OLD;
	nlog = 0;
	/* SOUNDCAP rellena esta línea mientras la voz 1 sigue leyéndola. La
	 * instantánea nueva se toma al volver al bucle, no en la frontera del
	 * trabajo. El modo de diagnóstico reproduce el salto anterior. */
	for (uint32_t i = 0; i < 64; i++)
		wr16le(mem + 0x100 + 2 * i, 0x6000);
	run(&keep, 4, a);
	run(&old, 4, b);
	CHECK_EQ(a[0], 0x2000);
	CHECK_EQ(b[0], 0x6000);
	run(&keep, 50, first);
	CHECK_EQ(first[2 * 49], 0x6000); /* el bucle ya refrescó la línea */
}

/* Partir el trabajo en trozos da lo mismo que correrlo entero (muestras, estado y salida a 48 kHz). */
static void test_split(uint32_t mode, float adj)
{
	static snd_state a, b;
	static int16_t nat_a[2 * SND_TICKS_MAX], nat_b[2 * SND_TICKS_MAX];
	static float out_a[2 * SND_OUT_MAX], out_b[2 * SND_OUT_MAX];
	uint32_t na = 0, nb = 0, consumed = 0, bad = 0, badf = 0;

	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, (uint16_t)(int16_t)((i * 997) & 0x7FFF) - 9000);
	for (uint32_t i = 0; i < 64; i++)
		mem[0x400 + i] = (uint8_t)(i * 37);
	snd_reset(&a);
	snd_reset(&b);
	a.mode = b.mode = mode;
	a.ratio_adj = b.ratio_adj = adj;
	w16(0x100, 0x805A);
	chan(0, CNT(90, 1, 20, 1, 1), 0x02000100u, 0x10000u - 700u, 0, 128);
	chan(1, CNT(120, 0, 100, 1, 0), 0x02000400u, P512, 4, 12);
	chan(2, CNT(77, 0, 65, 1, 2), 0x02000400u, 0xFC00u, 1, 3);
	chan(9, CNT(127, 0, 64, 0, 3) | (5u << 24), 0, 0xFF00u, 0, 0);
	chan(15, CNT(80, 0, 90, 0, 3), 0, 0xFFC0u, 0, 0);
	(void)snd_run(&a, log_buf, nlog, 500, &bus, nat_a, out_a, SND_OUT_MAX, &na);
	for (uint32_t done = 0; done < 500;) {
		const uint32_t k = done + 111 < 500 ? 111 : 500 - done;
		uint32_t n = 0;

		consumed += snd_run(&b, log_buf + consumed, nlog - consumed, k, &bus, nat_b + 2 * done, out_b + 2 * nb, SND_OUT_MAX, &n);
		nb += n;
		done += k;
	}
	CHECK_EQ(nb, na);
	for (uint32_t i = 0; i < 1000; i++)
		bad += nat_a[i] != nat_b[i];
	CHECK_EQ(bad, 0);
	for (uint32_t i = 0; i < 2 * na; i++)
		badf += out_a[i] != out_b[i];
	CHECK_EQ(badf, 0);
	CHECK_EQ(a.tick, b.tick);
	CHECK_EQ(a.busy, b.busy);
	/* 500 muestras de 32,7 kHz son 733,5 de 48 kHz */
	CHECK(fabsf((float)na - 500.0f*SND_RATE_OUT/(SND_RATE_NATIVE*(1.0f+adj))) < 2.0f);
}

static void test_resample(void)
{
	static snd_state st;
	static int16_t nat[2 * SND_TICKS_MAX];
	static float out[2 * SND_OUT_MAX];
	uint32_t n = 0, bad = 0;

	/* una continua da una continua a 48 kHz (las filas del FIR suman 1) y el modo «como el original» cuantiza a 10 bits */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x2000);
	snd_reset(&st);
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	snd_run(&st, log_buf, nlog, 300, &bus, nat, out, SND_OUT_MAX, &n);
	for (uint32_t i = 70; i < n; i++) /* el FIR tarda ~48 muestras de salida en llenar su ventana (32 nativas) */
		bad += fabsf(out[2 * i] - 0.25f) > 1e-6f;
	CHECK_EQ(bad, 0);
	CHECK(n > 430 && n < 445);
	/* ajuste de la razón: +2 % da menos muestras */
	snd_reset(&st);
	nlog = 0;
	st.ratio_adj = 0.02f;
	{
		uint32_t n2 = 0;

		snd_run(&st, log_buf, 0, 300, &bus, nat, out, SND_OUT_MAX, &n2);
		CHECK(n2 < n);
	}
	snd_reset(&st);
	st.mode = SND_MODE_ORIGINAL;
	nlog = 0;
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	{
		uint32_t n3 = 0;

		snd_run(&st, log_buf, nlog, 100, &bus, nat, out, SND_OUT_MAX, &n3);
		/* 0x2000 >> 6 = 128; con el sesgo 0x200: (128 + 512 - 512) / 512 = 0,25 */
		CHECK(fabsf(out[2 * (n3 - 5)] - 0.25f) < 1e-6f);
	}
}

/* HD debe conservar decodificación, temporizadores y capturas incluso con paros,
 * reinicios, recargas, rutas 1/3, saturación y RAM de captura como fuente. */
static void test_hd_native(uint32_t mode)
{
	static snd_state a, b;
	static uint8_t saved[MEM_SIZE], reference[MEM_SIZE];
	static int16_t na[2*SND_TICKS_MAX], nb[2*SND_TICKS_MAX];
	static float out[2*SND_OUT_MAX];
	uint32_t seed = 4917, produced;
	reset(); snd_reset(&a); snd_reset(&b); a.mode = mode; b.mode = mode | SND_MODE_HD;
	for (unsigned i = 0; i < MEM_SIZE; i++) mem[i] = (uint8_t)(i*37);
	for (unsigned frame = 0; frame < 96; frame++) {
		nlog = 0;
		w16(0x100, 0x8000u | ((frame*997u) & 0x3F7Fu));
		unsigned ci = frame % 16, fmt = frame / 16 % 4;
		seed = seed*1664525u + 1013904223u;
		chan(ci, CNT(90, frame%4, frame%128, frame%3, fmt), 0x02000100,
		     65536u - (1u + seed%3000u), frame%3, 8);
		w32(0x110, 0x02000100); w16(0x114, 32); w16(0x108, 0x80 | (frame%16));
		/* Parar otra voz a mitad del trabajo. */
		w16(((ci+3)%16)*16+2, 0); log_buf[nlog-1].tick = (uint32_t)a.tick+25;
		for (unsigned i=0; i+1<nlog; i++) log_buf[i].tick = (uint32_t)a.tick;
		memcpy(saved, mem, MEM_SIZE);
		snd_run(&a, log_buf, nlog, 120, &bus, na, out, SND_OUT_MAX, &produced);
		memcpy(reference, mem, MEM_SIZE); memcpy(mem, saved, MEM_SIZE);
		snd_run(&b, log_buf, nlog, 120, &bus, nb, out, SND_OUT_MAX, &produced);
		CHECK_EQ(memcmp(na, nb, 240*sizeof *na), 0);
		CHECK_EQ(memcmp(reference, mem, MEM_SIZE), 0);
		CHECK_EQ(memcmp(a.ch, b.ch, sizeof a.ch), 0);
		CHECK_EQ(memcmp(a.cap, b.cap, sizeof a.cap), 0);
		CHECK_EQ(a.busy, b.busy); CHECK_EQ(a.peak, b.peak);
		for (unsigned i=0; i<2*produced; i++) CHECK(isfinite(out[i]));
	}
}

static void test_hd_edges(void)
{
	static snd_state st;
	float out[2*SND_OUT_MAX];
	uint32_t n;
	reset(); snd_reset(&st); st.mode = SND_MODE_HD;
	for (unsigned i=0; i<128; i++) wr16le(mem+0x100+2*i, 0x4000);
	w16(0x100, 0x807F);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100, P512, 0, 64);
	snd_run(&st, log_buf, nlog, 120, &bus, NULL, out, SND_OUT_MAX, &n);
	CHECK(fabsf(out[2*(n-1)]-0.5f)<1e-6f);
	/* El apagado conserva el último valor y cae a cero en 32 pares. */
	nlog=0; w32(0, 0); log_buf[0].tick=log_buf[1].tick=(uint32_t)st.tick-1;
	snd_run(&st, log_buf, nlog, 120, &bus, NULL, out, SND_OUT_MAX, &n);
	CHECK(fabsf(out[0]-0.5f)<1e-6f);
	for (unsigned i=1; i<n; i++) CHECK(fabsf(out[2*i]-out[2*(i-1)]) <= 0.5f/32+1e-6f);
	CHECK_EQ(out[64], 0); CHECK_EQ(st.busy, 0);
	/* La mezcla HD mantiene margen flotante y no recorta antes del limitador. */
	nlog=0; snd_reset(&st); st.mode=SND_MODE_HD;
	w16(0x100, 0x807F);
	for (unsigned c=0; c<4; c++) chan(c, CNT(127,0,0,1,1),0x02000100,P512,0,64);
	snd_run(&st, log_buf, nlog, 120, &bus, NULL, out, SND_OUT_MAX, &n);
	CHECK(fabsf(out[2*(n-1)]-2.0f)<1e-6f);
}

void test_snd_core(void);

void test_snd(void)
{
	test_hd_edges();
	test_snd_core();
	test_hd_native(0);
	test_hd_native(SND_MODE_CORE_OLD);
	test_split(SND_MODE_CORE_OLD, 0);
	test_split(SND_MODE_CORE_OLD | SND_MODE_HD, 0);
	test_model();
	test_capture();
	test_capture_playback_boundary();
	test_split(0, 0);
	test_split(SND_MODE_HD, 0);
	test_split(SND_MODE_HD, -0.02f);
	test_split(SND_MODE_HD, 0.02f);
	test_resample();
}
