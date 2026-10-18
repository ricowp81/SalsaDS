/*
 * SalsaDS — ROM irq: pruebas comunes del ARM9 y el ARM7 (interrupciones, temporizadores y video).
 *
 * Referencia de tiempo: los temporizadores 2 y 3 en cascada dan un contador de 32 bits a 33,513982 MHz
 * (la frecuencia del bus, la misma de las dos CPUs). Una línea de video son 2130 de esos ciclos y un cuadro
 * 263 × 2130 = 560 190. Los tiempos son rangos amplios: la ROM pasa en melonDS y en SalsaDS.
 *
 * Quien la incluya define antes WAIT_VBLANK(), SWI_VBLANK_INTR_WAIT(), SWI_INTR_WAIT(clear, mask) y
 * CPU_HALT() (el ARM9 usa el WFI de CP15 y el ARM7 la SWI 6).
 */
#ifndef IRQT_H
#define IRQT_H

#include "../common/hw.h"

#define LINE_CYCLES 2130u
#define FRAME_CYCLES (263u * LINE_CYCLES)

static inline void ref_start(void)
{
	MEM16(R_TM_H(2)) = 0;
	MEM16(R_TM_H(3)) = 0;
	MEM16(R_TM_L(2)) = 0;
	MEM16(R_TM_L(3)) = 0;
	MEM16(R_TM_H(3)) = 0x84; /* cuenta ascendente, habilitado */
	MEM16(R_TM_H(2)) = 0x80; /* F, habilitado */
}

/* Lee el contador de 32 bits con la corrección del acarreo entre las dos mitades. */
static inline uint32_t ref_now(void)
{
	uint32_t hi, lo, hi2;

	do {
		hi = MEM16(R_TM_L(3));
		lo = MEM16(R_TM_L(2));
		hi2 = MEM16(R_TM_L(3));
	} while (hi != hi2);
	return (hi << 16) | lo;
}

/* Espera a que VCOUNT valga `line`. */
static inline void wait_line(uint32_t line)
{
	while (MEM16(R_VCOUNT) != line)
		;
}

/* Espera a que VCOUNT cambie de `line` a otra. */
static inline void wait_line_change(void)
{
	const uint32_t l = MEM16(R_VCOUNT);

	while (MEM16(R_VCOUNT) == l)
		;
}

/* Espera el inicio del siguiente VBlank mirando VCOUNT (sin IntrWait: con otras IRQ activas, melonDS entrega solo
 * una al ARM7 mientras el ARM7 espera con libnds7 swiIntrWait; ver docs/fase1.md). */
static inline void sync_vblank(void)
{
	while (MEM16(R_VCOUNT) == 192)
		;
	while (MEM16(R_VCOUNT) != 192)
		;
}

static inline void set_lyc(uint32_t lyc)
{
	MEM16(R_DISPSTAT) = (uint16_t)((MEM16(R_DISPSTAT) & 0x3F) | ((lyc & 0xFF) << 8) | ((lyc >> 8) << 7));
}


static volatile uint32_t n_vblank, n_hblank, n_vcount, n_tm[4], vc_line, last_vb_time, vb_times[16];

static void h_vblank(void)
{
	if (n_vblank < 16)
		vb_times[n_vblank] = ref_now();
	n_vblank++;
}

static void h_hblank(void)
{
	n_hblank++;
}

static void h_vcount(void)
{
	vc_line = MEM16(R_VCOUNT);
	n_vcount++;
}

static void h_tm0(void) { n_tm[0]++; }
static void h_tm1(void) { n_tm[1]++; }

static void group_video(void)
{
	uint32_t samples = 0, bad = 0, seen_hb = 0, seen_nohb = 0, seen_vb = 0, seen_match = 0;

	sds_group("VIDEO");
	/* VCOUNT sube de 0 a 262 y vuelve a 0 */
	{
		uint32_t prev = MEM16(R_VCOUNT), max = prev, wraps = 0, steps = 0, odd = 0;
		const uint32_t t0 = ref_now();

		while (ref_now() - t0 < 2 * FRAME_CYCLES) {
			const uint32_t v = MEM16(R_VCOUNT);

			if (v != prev) {
				if (v == (prev + 1)) {
					steps++;
				} else if (v == 0 && prev == 262) {
					wraps++;
				} else {
					odd++;
				}
				if (v > max)
					max = v;
				prev = v;
			}
		}
		SDS_CHECK(max, 262);
		SDS_CHECK(odd, 0);
		SDS_CHECK(wraps >= 1 && wraps <= 2, 1);
		SDS_CHECK(steps >= 2 * 262 - 4 && steps <= 2 * 263 + 4, 1);
	}
	/* banderas de DISPSTAT: VBlank en las líneas 192-261, HBlank se alterna en cada línea, coincidencia con LYC */
	set_lyc(77);
	for (int i = 0; i < 200000 && !(seen_hb && seen_nohb && seen_vb && seen_match); i++) {
		const uint32_t v0 = MEM16(R_VCOUNT), ds = MEM16(R_DISPSTAT), v1 = MEM16(R_VCOUNT);

		if (v0 != v1)
			continue;
		samples++;
		if (((ds & 1) != 0) != (v0 >= 192 && v0 < 262))
			bad++;
		if (((ds & 4) != 0) != (v0 == 77))
			bad++;
		if (ds & 2)
			seen_hb = 1;
		else
			seen_nohb = 1;
		if (ds & 1)
			seen_vb = 1;
		if (ds & 4)
			seen_match = 1;
	}
	SDS_CHECK(bad, 0);
	SDS_CHECK(samples > 1000, 1);
	SDS_CHECK(seen_hb && seen_nohb && seen_vb && seen_match, 1);
	/* LYC de 9 bits: el bit 7 de DISPSTAT es el más alto */
	set_lyc(0x105);
	SDS_CHECK((MEM16(R_DISPSTAT) >> 8) & 0xFF, 5);
	SDS_CHECK((MEM16(R_DISPSTAT) >> 7) & 1, 1);
	set_lyc(0);
	/* DISPSTAT: solo se escriben las habilitaciones (3-5) y LYC */
	MEM16(R_DISPSTAT) = 0xFFFF;
	SDS_CHECK(MEM16(R_DISPSTAT) & 0x38, 0x38);
	MEM16(R_DISPSTAT) = 0;
	SDS_CHECK(MEM16(R_DISPSTAT) & 0xFF38, 0);
}

static void group_vblank(void)
{
	uint32_t ok = 0, total = 0;

	sds_group("VBLANK IRQ");
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	n_vblank = 0;
	for (int i = 0; i < 8; i++)
		WAIT_VBLANK();
	SDS_CHECK(n_vblank >= 8 && n_vblank <= 9, 1);
	/* intervalo entre VBlank consecutivos: un cuadro */
	for (uint32_t i = 2; i < 8 && i < n_vblank; i++) {
		const uint32_t d = vb_times[i] - vb_times[i - 1];

		total++;
		ok += d >= FRAME_CYCLES - 600 && d <= FRAME_CYCLES + 600;
	}
	SDS_CHECK(ok, total);
	/* swiWaitForVBlank vuelve justo después del inicio del VBlank: VCOUNT en 192-195 */
	WAIT_VBLANK();
	{
		const uint32_t v = MEM16(R_VCOUNT);

		SDS_CHECK(v >= 192 && v <= 196, 1);
	}
	/* IntrWait y VBlankIntrWait de la BIOS (SWI 4 y 5) */
	{
		uint32_t before = n_vblank;

		SWI_VBLANK_INTR_WAIT();
		SDS_CHECK(n_vblank - before, 1);
		SDS_CHECK(MEM16(R_VCOUNT) >= 192 && MEM16(R_VCOUNT) <= 196, 1);
		before = n_vblank;
		SWI_INTR_WAIT(1, 1);
		SDS_CHECK(n_vblank - before, 1);
		before = n_vblank;
		for (int i = 0; i < 3; i++)
			SWI_INTR_WAIT(1, 1);
		SDS_CHECK(n_vblank - before, 3);
	}
	irqDisable(IRQ_VBLANK);
}

static void group_hvcount(void)
{
	uint32_t a, b;

	sds_group("HBLANK/VCOUNT");
	irqSet(IRQ_HBLANK, h_hblank);
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	sync_vblank();
	irqEnable(IRQ_HBLANK);
	n_hblank = 0;
	sync_vblank();
	irqDisable(IRQ_HBLANK);
	a = n_hblank;
	/* una IRQ de HBlank por línea, también en el VBlank: 263 por cuadro (el cuadro no es entero en la ventana) */
	SDS_CHECK(a >= 261 && a <= 265, 1);

	irqSet(IRQ_VCOUNT, h_vcount);
	set_lyc(100);
	irqEnable(IRQ_VCOUNT);
	n_vcount = 0;
	sync_vblank();
	sync_vblank();
	sync_vblank();
	irqDisable(IRQ_VCOUNT);
	b = n_vcount;
	SDS_CHECK(b >= 3 && b <= 4, 1);
	SDS_CHECK(vc_line >= 100 && vc_line <= 102, 1);
	/* LYC 262: la última línea; LYC 200 con el bit alto (200 < 256, sin él) */
	set_lyc(262);
	irqEnable(IRQ_VCOUNT);
	n_vcount = 0;
	sync_vblank();
	sync_vblank();
	irqDisable(IRQ_VCOUNT);
	SDS_CHECK(n_vcount >= 2 && n_vcount <= 3, 1);
	SDS_CHECK(vc_line >= 262 || vc_line == 0, 1);
	set_lyc(0);
	irqDisable(IRQ_VBLANK);
}

static void group_timers(void)
{
	uint32_t v, t, k;
	static const uint32_t div[4] = { 1, 64, 256, 1024 };

	sds_group("TIMERS");
	ref_start();
	/* arranque: el contador toma el valor de recarga */
	MEM16(R_TM_H(0)) = 0;
	v = MEM16(R_TM_L(0));
	MEM16(R_TM_L(0)) = 0xFFF0;
	SDS_CHECK(MEM16(R_TM_L(0)), v); /* con el temporizador parado, escribir la recarga no cambia el contador */
	MEM16(R_TM_H(0)) = 0x80;
	v = MEM16(R_TM_L(0));
	SDS_CHECK(v >= 0xFFF0, 1); /* arranca desde la recarga; si desborda, vuelve a ella */
	/* desborda y sigue desde la recarga */
	for (volatile int i = 0; i < 20; i++)
		;
	v = MEM16(R_TM_L(0));
	MEM16(R_TM_H(0)) = 0;
	/* parado: el contador queda donde está */
	{
		const uint32_t a = MEM16(R_TM_L(0)), b = MEM16(R_TM_L(0));

		SDS_CHECK(a, b);
		(void)v;
	}
	/* reanudar desde la recarga */
	MEM16(R_TM_L(0)) = 0x1234;
	MEM16(R_TM_H(0)) = 0x80;
	v = MEM16(R_TM_L(0));
	SDS_CHECK(v >= 0x1234 && v <= 0x1234 + 20, 1);
	MEM16(R_TM_L(0)) = 0x7000; /* cambiar la recarga en marcha no toca el contador */
	v = MEM16(R_TM_L(0));
	SDS_CHECK(v < 0x2000, 1);
	MEM16(R_TM_H(0)) = 0;

	/* prescalers: ticks en 100 líneas de video */
	for (uint32_t p = 0; p < 4; p++) {
		uint32_t ticks;
		const uint32_t lines = p == 0 ? 25 : 60;

		MEM16(R_TM_H(1)) = 0;
		MEM16(R_TM_L(1)) = 0;
		wait_line_change();
		MEM16(R_TM_H(1)) = (uint16_t)(0x80 | p);
		t = ref_now();
		for (k = 0; k < lines; k++)
			wait_line_change();
		ticks = MEM16(R_TM_L(1));
		t = ref_now() - t;
		MEM16(R_TM_H(1)) = 0;
		{
			const uint32_t want = t / div[p];

			/* leer el temporizador y luego la referencia cuesta varios ciclos de bus: con prescaler 1 (un tick por ciclo) la
			 * tolerancia de 2 no alcanza en una DS real (el ARM7 fallaba); con los demás, 2 ticks sobran */
			const uint32_t tol = p == 0 ? 64 : 2;
			const uint32_t ok_ = ticks + tol >= want && ticks <= want + tol;

			SDS_CHECK(ok_, 1);
			if (!ok_)
				SDS_DIVERGE_ID(ticks, want, 3000 + p); /* ticks obtenidos y esperados, al log */
			SDS_CHECK(t >= (lines - 1) * LINE_CYCLES && t <= (lines + 1) * LINE_CYCLES + 100, 1);
		}
	}

	/* IRQ de desborde: recarga 0xFC00 con F: 1024 ciclos de bus = 547 desbordes por cuadro */
	irqSet(IRQ_TIMER0, h_tm0);
	irqSet(IRQ_TIMER1, h_tm1);
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	sync_vblank();
	n_tm[0] = 0;
	MEM16(R_TM_L(0)) = 0xFC00;
	MEM16(R_TM_H(0)) = 0xC0; /* F, IRQ, habilitado */
	irqEnable(IRQ_TIMER0);
	sync_vblank();
	irqDisable(IRQ_TIMER0);
	MEM16(R_TM_H(0)) = 0;
	SDS_CHECK(n_tm[0] >= 540 && n_tm[0] <= 554, 1);

	/* cascada: el temporizador 1 cuenta los desbordes del 0 */
	n_tm[0] = n_tm[1] = 0;
	MEM16(R_TM_H(0)) = 0;
	MEM16(R_TM_H(1)) = 0;
	MEM16(R_TM_L(1)) = 0xFFF0; /* desborda a los 16 */
	MEM16(R_TM_L(0)) = 0xFC00; /* cada 1024 ciclos de bus */
	MEM16(R_TM_H(1)) = 0xC4;   /* cuenta ascendente, IRQ, habilitado */
	MEM16(R_TM_H(0)) = 0xC0;
	irqEnable(IRQ_TIMER0 | IRQ_TIMER1);
	sync_vblank();
	sync_vblank();
	irqDisable(IRQ_TIMER0 | IRQ_TIMER1);
	MEM16(R_TM_H(0)) = 0;
	MEM16(R_TM_H(1)) = 0;
	/* 2 cuadros ≈ 1094 desbordes del 0 → el 1 desbordó ⌊1094 / 16⌋ ≈ 68 veces */
	SDS_CHECK(n_tm[0] >= 1080 && n_tm[0] <= 1110, 1);
	SDS_CHECK(n_tm[1] + 2 >= n_tm[0] / 16 && n_tm[1] <= n_tm[0] / 16 + 2, 1);
	/* la cuenta del temporizador 1 avanza de uno en uno */
	{
		uint32_t steps = 0, bad = 0, prev;

		MEM16(R_TM_L(1)) = 0;
		MEM16(R_TM_L(0)) = 0xFF00;
		MEM16(R_TM_H(1)) = 0x84;
		MEM16(R_TM_H(0)) = 0x80;
		prev = MEM16(R_TM_L(1));
		for (int i = 0; i < 20000 && steps < 20; i++) {
			const uint32_t cur = MEM16(R_TM_L(1));

			if (cur != prev) {
				bad += cur != prev + 1;
				steps++;
				prev = cur;
			}
		}
		MEM16(R_TM_H(0)) = 0;
		MEM16(R_TM_H(1)) = 0;
		SDS_CHECK(steps, 20);
		SDS_CHECK(bad, 0);
	}
	/* el temporizador 0 no tiene anterior a quién seguir: con el bit de cuenta ascendente no cuenta */
	MEM16(R_TM_L(0)) = 0;
	MEM16(R_TM_H(0)) = 0x84;
	{
		const uint32_t a = MEM16(R_TM_L(0));

		for (volatile int i = 0; i < 200; i++)
			;
		/* En una DS real el bit 2 (cuenta ascendente) del temporizador 0 NO se guarda (TM0CNT_H lee 0xC3 tras escribir 0xFFFF)
		 * y el temporizador cuenta como uno normal: aquí, con prescaler 1, avanza. GBATEK, melonDS 1.1 y SalsaDS dicen que no
		 * cuenta (informativo hasta corregir el núcleo; ver docs/ds-real.md). */
		SDS_DIVERGE_M(MEM16(R_TM_L(0)) != a, 1, 0);
	}
	MEM16(R_TM_H(0)) = 0;
	/* bits de control: solo 0-2, 6 y 7 existen (GBATEK); melonDS 1.1 devuelve todo lo escrito (informativo) */
	MEM16(R_TM_H(0)) = 0xFFFF;
	/* hardware: 0xC3 (el temporizador 0 no tiene el bit 2); GBATEK y SalsaDS dicen 0xC7; melonDS 1.1 devuelve 0xFFFF */
	SDS_DIVERGE_M(MEM16(R_TM_H(0)), 0xC3, 0xFFFF);
	MEM16(R_TM_H(0)) = 0;
	/* acceso de 32 bits: recarga y control a la vez */
	MEM32(R_TM_L(0)) = 0x00800000u | 0x4321u;
	v = MEM16(R_TM_L(0));
	SDS_CHECK(v >= 0x4321 && v <= 0x4321 + 20, 1);
	MEM32(R_TM_L(0)) = 0;
	irqDisable(IRQ_VBLANK);
}

static void group_ime(void)
{
	uint32_t before;

	sds_group("IME/IE/IF");
	irqSet(IRQ_VBLANK, h_vblank);
	irqEnable(IRQ_VBLANK);
	WAIT_VBLANK();
	/* IME = 0: la IRQ no llega pero IF se levanta */
	MEM32(R_IME) = 0;
	before = n_vblank;
	MEM32(R_IF) = 1;
	SDS_CHECK(WAIT_UNTIL((MEM32(R_IF) & 1) != 0, 8000000), 1);
	SDS_CHECK(n_vblank, before);
	SDS_CHECK(MEM32(R_IME), 0);
	/* con IME = 1 se entrega enseguida */
	MEM32(R_IME) = 1;
	SDS_CHECK(WAIT_UNTIL(n_vblank != before, 100000), 1);
	/* IF: escribir 1 limpia, escribir 0 no cambia */
	MEM32(R_IME) = 0;
	SDS_CHECK(WAIT_UNTIL((MEM32(R_IF) & 1) != 0, 8000000), 1);
	MEM32(R_IF) = 0;
	SDS_CHECK(MEM32(R_IF) & 1, 1);
	MEM32(R_IF) = 1;
	SDS_CHECK(MEM32(R_IF) & 1, 0);
	/* IE enmascara: con IE = 0 IF sigue subiendo pero no hay IRQ ni despertar */
	MEM32(R_IE) = 0;
	MEM32(R_IME) = 1;
	before = n_vblank;
	{
		const uint32_t t = ref_now();

		while (ref_now() - t < FRAME_CYCLES + 10000)
			;
	}
	SDS_CHECK(n_vblank, before);
	SDS_CHECK(MEM32(R_IF) & 1, 1);
	MEM32(R_IF) = 1;
	irqEnable(IRQ_VBLANK);
	/* IE y IF: acceso por mitades y por bytes */
	MEM32(R_IME) = 0;
	MEM16(R_IE) = 0x0005;
	SDS_CHECK(MEM32(R_IE), 0x0005);
	MEM8(R_IE + 1) = 0x30; /* melonDS 1.1 ignora las escrituras de 8 bits en IE (informativo) */
	SDS_DIVERGE(MEM32(R_IE), 0x3005);
	MEM16(R_IE) = 0x3005;
	MEM16(R_IE + 2) = 0x0007;
	SDS_CHECK(MEM32(R_IE), 0x00073005u);
	MEM32(R_IE) = 0;
	irqEnable(IRQ_VBLANK);
	MEM32(R_IME) = 1;
	irqDisable(IRQ_VBLANK);
}


static void group_halt(void)
{
	uint32_t t0, t1;

	sds_group("HALT");
	irqSet(IRQ_TIMER0, h_tm0);
	MEM16(R_TM_H(0)) = 0;
	MEM32(R_IF) = IRQ_TIMER0; /* sin avisos viejos del temporizador de las pruebas anteriores */
	irqEnable(IRQ_TIMER0);
	/* esperar una IRQ de temporizador con HALT: 4096 ciclos de bus = 122 µs */
	n_tm[0] = 0;
	MEM16(R_TM_H(0)) = 0;
	MEM16(R_TM_L(0)) = 0xF000;
	t0 = ref_now();
	MEM16(R_TM_H(0)) = 0xC0;
	while (!n_tm[0])
		CPU_HALT();
	t1 = ref_now();
	MEM16(R_TM_H(0)) = 0;
	SDS_CHECK(t1 - t0 >= 4096 - 300 && t1 - t0 <= 4096 + 3000, 1);
	irqDisable(IRQ_TIMER0);
}

static void group_keys(void)
{
	sds_group("TECLAS");
	SDS_CHECK(MEM16(R_KEYINPUT) & 0x3FF, 0x3FF); /* nada apretado */
	MEM16(R_KEYCNT) = 0xC3FF;
	SDS_CHECK(MEM16(R_KEYCNT), 0xC3FF);
	MEM16(R_KEYCNT) = 0;
	SDS_CHECK(MEM16(R_KEYCNT), 0);
}

#endif
