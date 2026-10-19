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

static snd_ent log_buf[64];
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
	snd_reset(&st);
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
	snd_reset(&st);
	w16(0x100, 0x807F);
	chan(0, CNT(37, 1, 77, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 5], -944);
	CHECK_EQ(nat[2 * 5 + 1], -1425);

	/* volumen 0 y balance en los extremos */
	reset();
	for (uint32_t i = 0; i < 128; i++)
		wr16le(mem + 0x100 + 2 * i, 0x4000);
	snd_reset(&st);
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
	snd_reset(&st);
	w16(0x100, 0x807F);
	for (unsigned c = 0; c < 6; c++)
		chan(c, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 6], 32767);

	/* una vez: se apaga y la salida queda en cero; el estado ocupado baja */
	reset();
	for (uint32_t i = 0; i < 16; i++)
		wr16le(mem + 0x100 + 2 * i, 0x2000);
	snd_reset(&st);
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
	snd_reset(&st);
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
	snd_reset(&st);
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
	snd_reset(&st);
	w16(0x100, 0x8040);
	chan(0, CNT(127, 0, 0, 1, 1), 0x02000100u, P512, 0, 64);
	run(&st, 10, nat);
	CHECK_EQ(nat[2 * 5], 8192);  /* 0x4000 * 64 / 128 */
	w16(0x100, 0x0040);
	nlog = 0;
	w16(0x100, 0x0040);
	snd_reset(&st);
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
	snd_reset(&st);
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
	snd_reset(&st);
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

/* Partir el trabajo en trozos da lo mismo que correrlo entero (muestras, estado y salida a 48 kHz). */
static void test_split(void)
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
	w16(0x100, 0x805A);
	chan(0, CNT(90, 1, 20, 1, 1), 0x02000100u, 0x10000u - 700u, 0, 128);
	chan(1, CNT(120, 0, 100, 1, 0), 0x02000400u, P512, 4, 12);
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
	CHECK(na >= 732 && na <= 735);
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

void test_snd(void)
{
	test_model();
	test_capture();
	test_split();
	test_resample();
}
