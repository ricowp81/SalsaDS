/*
 * SalsaDS — ROM snd, lado ARM7: ejercita el sonido (los 16 canales, SOUNDCNT/SOUNDBIAS y la captura) con casos de una
 * sola señal y deja lo que sale del mezclador, capturado por SNDCAP0/1 (izquierdo y derecho, PCM16, una vez), en RAM
 * principal: el caso k en SND_OUT + k * 0x400 (izquierdo en +0x000, derecho en +0x200; 128 muestras cada uno).
 * Sirve para comparar muestra a muestra el mezclador de SalsaDS con el de melonDS (tools/snd-compare.py): las muestras
 * del mezclador son lo único que el programa puede leer de vuelta del hardware de sonido.
 *
 * Los datos de origen los genera el propio programa en RAM (SND_DATA); el ADPCM sale de tools/gen-snd-data.py.
 */
#include "../common/hw.h"
#include "adpcm.h"
#include "snd.h"

#define CH(c) (0x04000400u + 16u * (c))
#define R_SOUNDCNT 0x04000500u
#define R_SOUNDBIAS 0x04000504u
#define R_SNDCAP0CNT 0x04000508u
#define R_SNDCAP1CNT 0x04000509u
#define R_SNDCAP0DAD 0x04000510u
#define R_SNDCAP0LEN 0x04000514u
#define R_SNDCAP1DAD 0x04000518u
#define R_SNDCAP1LEN 0x0400051Cu

/* SOUNDxCNT */
#define VOL(v) ((uint32_t)(v))
#define DIV(d) ((uint32_t)(d) << 8)
#define HOLD (1u << 15)
#define PAN(p) ((uint32_t)(p) << 16)
#define DUTY(d) ((uint32_t)(d) << 24)
#define MODE_LOOP (1u << 27)
#define MODE_ONCE (2u << 27)
#define FMT_PCM8 (0u << 29)
#define FMT_PCM16 (1u << 29)
#define FMT_ADPCM (2u << 29)
#define FMT_PSG (3u << 29)
#define START (1u << 31)

struct chcfg {
	uint8_t ch;
	uint32_t cnt, sad;
	uint16_t tmr, pnt;
	uint32_t len;
};

struct tcase {
	uint16_t master;
	uint8_t nch;
	struct chcfg c[6];
};

#define P512 0xFE00u  /* un paso del temporizador cada 512 ciclos de 16,757 MHz = una muestra por tick del mezclador */
#define BASE16 (VOL(127) | PAN(64) | MODE_LOOP | FMT_PCM16 | START)

#define PCM16(ch, cnt, sad, tmr) { ch, cnt, sad, tmr, 0, 128 }

static const struct tcase cases[] = {
	/*  - */ { 127, 1, { PCM16(0, BASE16, SND_D16C, P512) } }, /* calentamiento: la fase del primer caso respecto de los ticks es arbitraria */
	/*  0 */ { 127, 1, { PCM16(0, BASE16, SND_D16C, P512) } },
	/*  1 */ { 127, 1, { PCM16(0, (BASE16 & ~127u) | VOL(64), SND_D16C, P512) } },
	/*  2 */ { 127, 1, { PCM16(0, (BASE16 & ~127u) | VOL(1), SND_D16C, P512) } },
	/*  3 */ { 127, 1, { PCM16(0, (BASE16 & ~127u) | VOL(0), SND_D16C, P512) } },
	/*  4 */ { 127, 1, { PCM16(0, BASE16 | DIV(1), SND_D16C, P512) } },
	/*  5 */ { 127, 1, { PCM16(0, BASE16 | DIV(2), SND_D16C, P512) } },
	/*  6 */ { 127, 1, { PCM16(0, BASE16 | DIV(3), SND_D16C, P512) } },
	/*  7 */ { 127, 1, { PCM16(0, (BASE16 & ~PAN(127)) | PAN(0), SND_D16C, P512) } },
	/*  8 */ { 127, 1, { PCM16(0, (BASE16 & ~PAN(127)) | PAN(127), SND_D16C, P512) } },
	/*  9 */ { 127, 1, { PCM16(0, (BASE16 & ~PAN(127)) | PAN(32), SND_D16C, P512) } },
	/* 10 */ { 127, 1, { PCM16(0, (BASE16 & ~PAN(127)) | PAN(1), SND_D16C, P512) } },
	/* 11 */ { 127, 1, { PCM16(0, (BASE16 & ~PAN(127)) | PAN(126), SND_D16C, P512) } },
	/* 12 */ { 64, 1, { PCM16(0, BASE16, SND_D16C, P512) } },
	/* 13 */ { 1, 1, { PCM16(0, BASE16, SND_D16C, P512) } },
	/* 14 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_LOOP | FMT_PCM8 | START, SND_D8C, P512, 0, 64 } } },
	/* 15 */ { 127, 2, { PCM16(0, BASE16, SND_D16C, P512), PCM16(1, BASE16, SND_D16R, P512) } },
	/* 16 */ { 127, 4, { PCM16(0, BASE16, SND_D16M, P512), PCM16(1, BASE16, SND_D16M, P512), PCM16(2, BASE16, SND_D16M, P512),
			     PCM16(3, BASE16, SND_D16M, P512) } },
	/* 17 */ { 127, 1, { PCM16(0, BASE16, SND_D16R, P512) } },
	/* 18 */ { 127, 1, { PCM16(0, BASE16, SND_D16R, 0xFC00u) } },
	/* 19 */ { 127, 1, { PCM16(0, BASE16, SND_D16R, 0x10000u - 341u) } },
	/* 20 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_LOOP | FMT_PCM8 | START, SND_D8R, P512, 0, 64 } } },
	/* 21 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_ONCE | FMT_PCM16 | START, SND_D16R, P512, 0, 16 } } },
	/* 22 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_ONCE | FMT_PCM16 | HOLD | START, SND_D16R, P512, 0, 16 } } },
	/* 23 */ { 127, 1, { { 0, BASE16, SND_D16R, P512, 4, 8 } } },
	/* 24 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(0) | FMT_PSG | START, 0, 0xFE00u, 0, 0 } } },
	/* 25 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(3) | FMT_PSG | START, 0, 0xFE00u, 0, 0 } } },
	/* 26 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(6) | FMT_PSG | START, 0, 0xFE00u, 0, 0 } } },
	/* 27 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(7) | FMT_PSG | START, 0, 0xFE00u, 0, 0 } } },
	/* 28 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(1) | FMT_PSG | START, 0, (uint16_t)(0x10000u - 300u), 0, 0 } } },
	/* 29 */ { 127, 1, { { 8, VOL(127) | PAN(64) | DUTY(4) | FMT_PSG | START, 0, 0xFC00u, 0, 0 } } },
	/* 30 */ { 127, 1, { { 14, VOL(127) | PAN(64) | FMT_PSG | START, 0, 0xFFC0u, 0, 0 } } },
	/* 31 */ { 127, 1, { { 14, VOL(127) | PAN(64) | FMT_PSG | START, 0, 0xFFF0u, 0, 0 } } },
	/* 32 */ { 127, 1, { { 15, VOL(127) | PAN(64) | FMT_PSG | START, 0, 0xFE00u, 0, 0 } } },
	/* 33 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_LOOP | FMT_ADPCM | START, SND_ADPCM, P512, 0, 4 } } },
	/* 34 */ { 127, 1, { { 0, VOL(120) | PAN(30) | DIV(1) | MODE_LOOP | FMT_ADPCM | START, SND_ADPCM2, 0x10000u - 400u, 1, 3 } } },
	/* 35 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_LOOP | FMT_ADPCM | START, SND_ADPCM, P512, 2, 2 } } },
	/* 36 */ { 127, 1, { PCM16(0, BASE16, SND_D16S, P512) } },
	/* 37 */ { 127, 1, { PCM16(0, BASE16, SND_D16N, P512) } },
	/* 38 */ { 127, 1, { PCM16(0, (BASE16 & ~127u) | VOL(37), SND_D16S, P512) } },
	/* 39 */ { 100, 1, { PCM16(0, (BASE16 & ~127u) | VOL(37) | DIV(1) | PAN(77), SND_D16R, P512) } },
	/* 40 */ { 127, 1, { PCM16(0, (BASE16 & ~127u) | VOL(100) | DIV(2) | PAN(10), SND_D16R, 0x10000u - 700u) } },
	/* 41-52: sondas de tiempo: rampa de índice (L = 8 * índice de muestra: vol 127 y pan 0 dejan pasar la muestra) con distintos periodos */
#define PROBE(tmr) { 127, 1, { { 0, VOL(127) | PAN(0) | MODE_LOOP | FMT_PCM16 | START, SND_D16I, (uint16_t)(tmr), 0, 128 } } }
	/* 41 */ PROBE(0xFE00u), /* 42 */ PROBE(0xFF00u), /* 43 */ PROBE(0x10000u - 100u), /* 44 */ PROBE(0x10000u - 1000u),
	/* 45 */ PROBE(0x10000u - 341u), /* 46 */ PROBE(0x10000u - 2000u), /* 47 */ PROBE(0x10000u - 5000u),
	/* 48 */ PROBE(0x10000u - 20u), /* 49 */ PROBE(0x10000u - 4u), /* 50 */ PROBE(0x10000u - 513u),
	/* 51 */ PROBE(0x10000u - 511u), /* 52 */ PROBE(0x10000u - 700u),
	/* 53 */ { 127, 1, { { 14, VOL(127) | PAN(64) | FMT_PSG | START, 0, (uint16_t)(0x10000u - 1000u), 0, 0 } } },
	/* 54 */ { 127, 1, { { 0, VOL(127) | PAN(64) | MODE_LOOP | FMT_ADPCM | START, SND_ADPCM, P512, 1, 3 } } },
	/* 55: captura en PCM8 (los dos lados) */ { 127, 2, { PCM16(0, BASE16, SND_D16R, P512), PCM16(2, BASE16 | PAN(100), SND_D16S, P512) } },
	/* 56: captura de la salida de los canales 1 y 3 en lugar del mezclador */ { 127, 3, { PCM16(0, BASE16, SND_D16R, P512), PCM16(1, (BASE16 & ~(127u | PAN(127))) | VOL(100) | PAN(10), SND_D16S, P512), PCM16(3, (BASE16 & ~(127u | PAN(127))) | VOL(77) | PAN(99) | DIV(1), SND_D16C, P512) } },
};
#define NCASES (sizeof cases / sizeof cases[0])

/* SNDCAPxCNT al arrancar, por caso (0 = 0x84: una vez, PCM16, mezclador) */
static const uint8_t capcfg[64][2] = {
	[56] = { 0x8C, 0x8C }, /* captura en PCM8 (los dos lados) */
	[57] = { 0x86, 0x86 }, /* captura de la salida de los canales 1 y 3 en lugar del mezclador */
};

static void fill16(uint32_t addr, uint32_t n, int (*f)(uint32_t))
{
	for (uint32_t i = 0; i < n; i++)
		MEM16(addr + 2 * i) = (uint16_t)f(i);
}

static int f_c16(uint32_t i) { (void)i; return 0x4000; }
static int f_m16(uint32_t i) { (void)i; return 0x7FFF; }
static int f_n16(uint32_t i) { (void)i; return -0x4000; }
static int f_s16(uint32_t i) { (void)i; return 0x1235; }
static int f_i16(uint32_t i) { return (int)((i & 255) * 8); }
static int f_r16(uint32_t i) { return ((int)(i & 255) - 128) * 128; }

static void delay(uint32_t n)
{
	for (volatile uint32_t i = 0; i < n; i++)
		;
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	MEM32(R_IME) = 0;
	fill16(SND_D16C, 256, f_c16);
	for (uint32_t i = 0; i < 256; i++)
		MEM8(SND_D8C + i) = 0x40;
	fill16(SND_D16R, 256, f_r16);
	for (uint32_t i = 0; i < 256; i++)
		MEM8(SND_D8R + i) = (uint8_t)((int)i - 128);
	fill16(SND_D16M, 256, f_m16);
	fill16(SND_D16N, 256, f_n16);
	fill16(SND_D16S, 256, f_s16);
	fill16(SND_D16I, 256, f_i16);
	for (uint32_t i = 0; i < 128; i++)
		MEM32(SND_ZERO + 4 * i) = 0;
	for (uint32_t i = 0; i < sizeof adpcm_data; i++)
		MEM8(SND_ADPCM + i) = adpcm_data[i];
	for (uint32_t i = 0; i < sizeof adpcm_data2; i++)
		MEM8(SND_ADPCM2 + i) = adpcm_data2[i];
	for (uint32_t i = 0; i < NCASES * 0x400u / 4u; i++)
		MEM32(SND_OUT + 4 * i) = 0xDEADBEEFu;

	/* dos pasadas: la primera calienta (en melonDS los primeros casos arrancan en otra fase respecto de los ticks) y solo
	 * la segunda deja resultados */
	for (uint32_t pass = 0; pass < 2 * NCASES; pass++) {
		const struct tcase *t = &cases[pass % NCASES];
		const uint32_t k = pass % NCASES, wr = pass >= NCASES;

		REG16(R_SOUNDCNT) = 0;
		REG16(R_SOUNDBIAS) = 0x200;
		for (uint32_t c = 0; c < 16; c++)
			REG32(CH(c)) = 0;
		REG8(R_SNDCAP0CNT) = 0;
		REG8(R_SNDCAP1CNT) = 0;
		delay(2000);
		/* melonDS conserva la última muestra de un canal parado: se deja en cero tocando silencio antes del caso */
		for (uint32_t i = 0; i < t->nch; i++) {
			const uint32_t ch = t->c[i].ch;

			REG32(CH(ch) + 4) = SND_ZERO;
			REG16(CH(ch) + 8) = P512;
			REG16(CH(ch) + 10) = 0;
			REG32(CH(ch) + 12) = 64;
			REG32(CH(ch)) = VOL(0) | PAN(64) | MODE_LOOP | FMT_PCM16 | START;
		}
		delay(3000);
		for (uint32_t i = 0; i < t->nch; i++)
			REG32(CH(t->c[i].ch)) = 0;
		delay(500);
		REG16(R_SOUNDCNT) = (uint16_t)(0x8000u | t->master);
		for (uint32_t i = 0; i < t->nch; i++) {
			const struct chcfg *c = &t->c[i];

			REG32(CH(c->ch) + 4) = c->sad;
			REG16(CH(c->ch) + 8) = c->tmr;
			REG16(CH(c->ch) + 10) = c->pnt;
			REG32(CH(c->ch) + 12) = c->len;
		}
		REG32(R_SNDCAP0DAD) = wr ? SND_OUT + k * 0x400u : SND_SCRATCH;
		REG16(R_SNDCAP0LEN) = 64;
		REG32(R_SNDCAP1DAD) = wr ? SND_OUT + k * 0x400u + 0x200u : SND_SCRATCH + 0x200u;
		REG16(R_SNDCAP1LEN) = 64;
		for (uint32_t i = 0; i < t->nch; i++)
			REG32(CH(t->c[i].ch)) = t->c[i].cnt;
		{
			const uint32_t c0 = capcfg[k][0] ? capcfg[k][0] : 0x84u, c1 = capcfg[k][1] ? capcfg[k][1] : 0x84u;

			REG16(R_SNDCAP0CNT) = (uint16_t)(c0 | c1 << 8); /* una sola escritura: las dos capturas arrancan en el mismo tick */
		}
		for (uint32_t spin = 0; (REG8(R_SNDCAP0CNT) & 0x80) && spin < 4000000u; spin++)
			;
		delay(500);
		if (wr) {
			for (uint32_t c = 0; c < 16; c++)
				MEM32(SND_OUT + k * 0x400u + 0x300u + 4 * c) = REG32(CH(c));
			MEM32(SND_OUT + k * 0x400u + 0x340u) = REG8(R_SNDCAP0CNT);
			MEM32(SND_OUT + k * 0x400u + 0x344u) = REG8(R_SNDCAP1CNT);
		}
	}
	REG16(R_SOUNDCNT) = 0;
	for (uint32_t c = 0; c < 16; c++)
		REG32(CH(c)) = 0;
	MBOX->done = 1;
	for (;;)
		;
}
