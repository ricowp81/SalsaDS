/*
 * SalsaDS — planificador de eventos, interrupciones, temporizadores, DMA y temporización del video
 * (Fase 1.2).
 *
 * Un solo hilo. El reloj base es el del ARM9 (`n->time`); las dos CPUs corren en tramos de `n->quantum`
 * ciclos acotados por el siguiente evento: el ARM9 hasta `next`, el ARM7 hasta `next / 2`. Al final de cada
 * tramo se atienden los eventos vencidos (líneas, HBlank, desbordes de temporizadores, cartucho, SPI). Una
 * CPU puede pasarse del final del tramo por lo que dura su última instrucción (y las esperas de un DMA que
 * ella disparó); ese error de pocos ciclos es el que se acepta.
 *
 * El video no tiene estado propio: la línea, la posición y las banderas se calculan del instante en que se
 * leen (nds_line_at/nds_dot_at); los eventos de línea solo sirven para las interrupciones y los disparos de
 * DMA. Los registros de E/S de cada CPU viven en `struct nds_cpuio`: lo que una CPU hace a la otra son
 * IRQ (`nds_irq_raise`), IPC y la memoria.
 *
 * Temporizadores: perezosos (contador y t0), con un evento por desborde; los encadenados (cuenta
 * ascendente) avanzan cuando desborda el anterior. DMA: la transferencia entera se hace en el instante del
 * disparo y se cobra a la CPU dueña como ciclos de su reloj (se detiene mientras dura).
 *
 * Fuente: GBATEK (interrupciones, temporizadores, DMA, video). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

/* ---- IRQ ---------------------------------------------------------------------------------------- */

void nds_irq_update(struct nds *n, int cpu)
{
	struct nds_cpuio *io = &n->io[cpu];
	struct acore *c = nds_core(n, cpu);
	const uint32_t pending = io->ie & io->ifl;

	acore_set_irq(c, (io->ime & 1u) && pending);
	/* El HALT del ARM7 termina con IE & IF != 0 aunque IME o el bit I enmascaren la IRQ (GBATEK). El WFI de
	 * CP15 del ARM9 solo con la línea (IME & IE & IF): la línea ya lo despierta en el núcleo. */
	if (cpu == NDS_CPU7 && c->halted && pending)
		acore_wake(c);
}

void nds_irq_raise(struct nds *n, int cpu, uint32_t mask)
{
	NDS_TRACE(n, NDS_DBG_IRQ, "IRQ%d +%08x (IE %08x IME %u)", cpu ? 7 : 9, mask, n->io[cpu].ie, n->io[cpu].ime & 1u);
	n->io[cpu].ifl |= mask;
	nds_irq_update(n, cpu);
}

void nds_cpu_halt(struct nds *n, int cpu)
{
	if (cpu == NDS_CPU7 && (n->io[cpu].ie & n->io[cpu].ifl))
		return; /* la condición de despertar ya se cumple */
	acore_halt(nds_core(n, cpu));
}

/* Al final de cada tramo: el ARM7 en HALT con IE & IF != 0 despierta (por si el HALT se pidió con la condición
 * ya cumplida). */
void nds_halt_check(struct nds *n)
{
	if (n->a7.halted && (n->io[NDS_CPU7].ie & n->io[NDS_CPU7].ifl))
		acore_wake(&n->a7);
}

/* ---- eventos --------------------------------------------------------------------------------------- */

void nds_event_set(struct nds *n, int ev, uint64_t when)
{
	n->ev[ev] = when;
	if (when < n->ev_min)
		n->ev_min = when;
}

/* ---- video: DISPSTAT, VCOUNT --------------------------------------------------------------------------- */

static inline uint32_t lyc_of(uint16_t ds)
{
	return ((uint32_t)(ds >> 8) & 0xFFu) | ((uint32_t)(ds & 0x80u) << 1);
}

uint16_t nds_dispstat_read(struct nds *n, int cpu)
{
	const uint64_t t = nds_cpu_now(n, cpu);
	const uint32_t line = nds_line_at(t), dot = nds_dot_at(t);
	uint16_t v = n->io[cpu].dispstat & 0xFFB8u;

	if (line >= 192 && line < 262)
		v |= 1;
	if (dot >= NDS_HBLANK_CYCLES)
		v |= 2;
	if (line == lyc_of(n->io[cpu].dispstat))
		v |= 4;
	return v;
}

void nds_dispstat_write(struct nds *n, int cpu, uint16_t v, uint16_t mask)
{
	struct nds_cpuio *io = &n->io[cpu];

	mask &= 0xFFB8u;
	io->dispstat = (uint16_t)((io->dispstat & ~mask) | (v & mask));
}

/* ---- teclas ------------------------------------------------------------------------------------------ */

static int key_cond(const struct nds *n, int cpu)
{
	const uint16_t cnt = n->io[cpu].keycnt, mask = cnt & 0x3FFu;
	const uint16_t pressed = (uint16_t)(~n->keyinput & 0x3FFu);

	if (!(cnt & 0x4000u))
		return 0;
	if (cnt & 0x8000u)
		return mask != 0 && (pressed & mask) == mask;
	return (pressed & mask) != 0;
}

static void keys_eval(struct nds *n, int cpu)
{
	const int cond = key_cond(n, cpu);

	if (cond && !n->io[cpu].key_cond)
		nds_irq_raise(n, cpu, NDS_IRQ_KEYPAD);
	n->io[cpu].key_cond = (uint8_t)cond;
}

void nds_keys_changed(struct nds *n)
{
	keys_eval(n, NDS_CPU9);
	keys_eval(n, NDS_CPU7);
}

void nds_keycnt_write(struct nds *n, int cpu, uint16_t v, uint16_t mask)
{
	struct nds_cpuio *io = &n->io[cpu];

	mask &= 0xC3FFu;
	io->keycnt = (uint16_t)((io->keycnt & ~mask) | (v & mask));
	keys_eval(n, cpu);
}

/* ---- temporizadores ------------------------------------------------------------------------------------ */

static const uint8_t tm_shift[4] = { 0, 6, 8, 10 }; /* prescalers F, F/64, F/256, F/1024 (en ciclos de bus) */

static inline unsigned tm_sh(const struct nds_timer *t)
{
	return 1u + tm_shift[t->ctl & 3u]; /* en ciclos del ARM9 */
}

/* El bit de cuenta ascendente del temporizador 0 no tiene a quién seguir: no cuenta nunca (como en melonDS). */
static inline int tm_cascade(const struct nds_timer *t, int i)
{
	(void)i;
	return (t->ctl & 4u) != 0;
}

static uint32_t tm_value(const struct nds_timer *t, int i, uint64_t now)
{
	if (!(t->ctl & 0x80u) || tm_cascade(t, i))
		return t->cnt & 0xFFFFu;
	return (uint32_t)(t->cnt + ((now - t->t0) >> tm_sh(t))) & 0xFFFFu;
}

static void tm_schedule(struct nds *n, int cpu, int i)
{
	struct nds_timer *t = &n->io[cpu].tm[i];
	const int ev = (cpu ? EV_TM7 : EV_TM9) + i;

	if (!(t->ctl & 0x80u) || tm_cascade(t, i)) {
		n->ev[ev] = NDS_NEVER;
		return;
	}
	nds_event_set(n, ev, t->t0 + ((uint64_t)(0x10000u - (t->cnt & 0xFFFFu)) << tm_sh(t)));
}

static void tm_overflow(struct nds *n, int cpu, int i, uint64_t when)
{
	struct nds_timer *t = &n->io[cpu].tm[i];

	t->cnt = t->reload;
	t->t0 = when;
	tm_schedule(n, cpu, i);
	if (t->ctl & 0x40u)
		nds_irq_raise(n, cpu, NDS_IRQ_TIMER(i));
	if (i < 3) {
		struct nds_timer *nx = &n->io[cpu].tm[i + 1];

		if ((nx->ctl & 0x84u) == 0x84u) { /* habilitado y de cuenta ascendente */
			if (++nx->cnt > 0xFFFFu)
				tm_overflow(n, cpu, i + 1, when);
		}
	}
}

uint16_t nds_timer_read(struct nds *n, int cpu, int i)
{
	return (uint16_t)tm_value(&n->io[cpu].tm[i], i, nds_cpu_now(n, cpu));
}

void nds_timer_write(struct nds *n, int cpu, int i, int hi, uint16_t v, uint16_t mask)
{
	struct nds_timer *t = &n->io[cpu].tm[i];
	const uint64_t now = nds_cpu_now(n, cpu);

	if (!hi) {
		t->reload = (uint16_t)((t->reload & ~mask) | (v & mask));
		return;
	}
	{
		const uint16_t old = t->ctl;
		const uint16_t nw = (uint16_t)(((t->ctl & ~mask) | (v & mask)) & 0xC7u);

		/* si corría, el contador queda en su valor de ahora (con el prescaler viejo) */
		if (old & 0x80u) {
			t->cnt = tm_value(t, i, now);
			t->t0 = now;
		}
		t->ctl = nw;
		if (!(old & 0x80u) && (nw & 0x80u)) { /* arranque: recarga */
			t->cnt = t->reload;
			t->t0 = now;
		}
		tm_schedule(n, cpu, i);
	}
}

/* ---- DMA -------------------------------------------------------------------------------------------- */

/* Modo de arranque en la numeración del ARM9: 0 inmediato, 1 VBlank, 2 HBlank, 3 inicio de pantalla,
 * 4 memoria principal, 5 cartucho, 6 ranura de GBA, 7 FIFO de geometría. El ARM7: 0, 1, 2 (cartucho) y 3
 * (WiFi / ranura de GBA). */
static unsigned dma_mode(int cpu, uint32_t ctl)
{
	if (cpu == NDS_CPU9)
		return (ctl >> 11) & 7u;
	switch ((ctl >> 12) & 3u) {
	case 0: return 0;
	case 1: return 1;
	case 2: return NDS_DMA_CART;
	default: return 6;
	}
}

static uint32_t dma_count_of(int cpu, int ch, uint32_t cnt)
{
	uint32_t c;

	if (cpu == NDS_CPU9) {
		c = cnt & 0x1FFFFFu;
		return c ? c : 0x200000u;
	}
	c = cnt & (ch == 3 ? 0xFFFFu : 0x3FFFu);
	return c ? c : (ch == 3 ? 0x10000u : 0x4000u);
}

static void dma_run(struct nds *n, int cpu, int ch, int one_unit)
{
	struct nds_dma *d = &n->io[cpu].dma[ch];
	struct acore *c = nds_core(n, cpu);
	const uint32_t ctl = d->cnt >> 16;
	const unsigned word = (ctl >> 10) & 1u, dctl = (ctl >> 5) & 3u, sctl = (ctl >> 7) & 3u;
	const int unit = word ? 4 : 2;
	uint32_t count = d->left;

	if (one_unit && count > 1)
		count = 1;
	const int gxmode = cpu == NDS_CPU9 && dma_mode(cpu, ctl) == NDS_DMA_GXFIFO;
	uint32_t done = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint32_t v;

		/* FIFO de geometría: la DMA sólo avanza mientras tenga menos de la mitad; cada palabra cuesta 4 ciclos del
		 * ARM9 y el motor sigue ejecutando mientras tanto */
		if (gxmode && !nds_gx_dma_ok(n, c->cycles))
			break;
		v = nds_bus_read(n, cpu, d->src & ~(uint32_t)(unit - 1), unit);

		nds_bus_write(n, cpu, d->dst & ~(uint32_t)(unit - 1), unit, v);
		if (sctl == 0 || sctl == 3)
			d->src += (uint32_t)unit;
		else if (sctl == 1)
			d->src -= (uint32_t)unit;
		if (dctl == 0 || dctl == 3)
			d->dst += (uint32_t)unit;
		else if (dctl == 1)
			d->dst -= (uint32_t)unit;
		done++;
		if (gxmode)
			c->cycles += 4u;
	}
	if (!gxmode)
		c->cycles += (uint64_t)count * (cpu == NDS_CPU9 ? 4u : 2u);
	d->left -= done;
	if (d->left)
		return;
	if ((ctl & 0x200u) && dma_mode(cpu, ctl) != 0) { /* repetir: recarga la cuenta (y el destino con 3) */
		d->left = dma_count_of(cpu, ch, d->cnt);
		if (dctl == 3)
			d->dst = d->dad;
	} else {
		d->cnt &= ~(1u << 31);
		d->latched = 0;
	}
	if (ctl & 0x4000u)
		nds_irq_raise(n, cpu, NDS_IRQ_DMA(ch));
}

void nds_dma_write(struct nds *n, int cpu, unsigned reg, uint32_t v, uint32_t mask)
{
	const int ch = (int)(reg / 3);
	struct nds_dma *d = &n->io[cpu].dma[ch];
	const uint32_t amask = cpu == NDS_CPU9 ? 0x0FFFFFFFu : (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);

	switch (reg % 3) {
	case 0:
		d->sad = ((d->sad & ~mask) | (v & mask)) & amask;
		return;
	case 1:
		d->dad = ((d->dad & ~mask) | (v & mask)) & amask;
		return;
	default: {
		const uint32_t old = d->cnt;
		uint32_t nw = (old & ~mask) | (v & mask);

		if (cpu == NDS_CPU7)
			nw &= 0xF7E0FFFFu; /* en el ARM7 no existen los bits 16-20 ni el 27 */
		d->cnt = nw;
		if (!(old & 0x80000000u) && (nw & 0x80000000u)) {
			d->src = d->sad;
			d->dst = d->dad;
			d->left = dma_count_of(cpu, ch, nw);
			d->latched = 1;
			if (dma_mode(cpu, nw >> 16) == 0 || (cpu == NDS_CPU9 && dma_mode(cpu, nw >> 16) == NDS_DMA_GXFIFO))
				dma_run(n, cpu, ch, 0); /* la FIFO de geometría siempre tiene sitio: arranca al armarse */
		} else if (!(nw & 0x80000000u)) {
			d->latched = 0;
		}
		return;
	}
	}
}

void nds_dma_trigger(struct nds *n, int cpu, unsigned mode)
{
	for (int ch = 0; ch < 4; ch++) {
		struct nds_dma *d = &n->io[cpu].dma[ch];

		if ((d->cnt & 0x80000000u) && d->latched && dma_mode(cpu, d->cnt >> 16) == mode)
			dma_run(n, cpu, ch, mode == NDS_DMA_CART);
	}
}

/* ¿Hay un canal armado esperando el aviso `mode`? */
static int dma_waits_for(const struct nds *n, int cpu, unsigned mode)
{
	for (int ch = 0; ch < 4; ch++) {
		const struct nds_dma *d = &n->io[cpu].dma[ch];

		if ((d->cnt & 0x80000000u) && d->latched && dma_mode(cpu, d->cnt >> 16) == mode)
			return 1;
	}
	return 0;
}

/* Lo usa geom.c: ¿hay un canal armado esperando el aviso `mode`? */
int nds_dma_waits(const struct nds *n, int cpu, unsigned mode)
{
	return dma_waits_for(n, cpu, mode);
}

/* ---- eventos del video ----------------------------------------------------------------------------------- */

static void ev_line(struct nds *n, uint64_t t)
{
	const uint32_t line = nds_line_at(t);

	n->line = line;
	n->ev[EV_LINE] = t + NDS_LINE_CYCLES;
	n->ev[EV_HBLANK] = t + NDS_HBLANK_CYCLES;
	for (int cpu = 0; cpu < 2; cpu++) {
		const uint16_t ds = n->io[cpu].dispstat;

		if ((ds & 0x20u) && line == lyc_of(ds))
			nds_irq_raise(n, cpu, NDS_IRQ_VCOUNT);
	}
	if (line == 0)
		nds_video_frame_begin(n);
	if (line == 192) {
		nds_gx_vblank(n, t);
		nds_video_frame_end(n);
		nds_snd_frame(n);
		n->frames++;
		for (int cpu = 0; cpu < 2; cpu++) {
			if (n->io[cpu].dispstat & 0x08u)
				nds_irq_raise(n, cpu, NDS_IRQ_VBLANK);
			nds_dma_trigger(n, cpu, NDS_DMA_VBLANK);
		}
		if (n->stop_on_vblank)
			n->stop = 1;
	}
	if (line < 192) {
		nds_video_line_start(n, line);
		nds_dma_trigger(n, NDS_CPU9, NDS_DMA_DISPSTART);
		if (dma_waits_for(n, NDS_CPU9, NDS_DMA_MMEM))
			for (int k = 0; k < 32; k++) /* modo 3: 32 avisos de 4 palabras por línea */
				nds_dma_trigger(n, NDS_CPU9, NDS_DMA_MMEM);
	}
}

static void ev_hblank(struct nds *n)
{
	n->ev[EV_HBLANK] = NDS_NEVER;
	for (int cpu = 0; cpu < 2; cpu++)
		if (n->io[cpu].dispstat & 0x10u)
			nds_irq_raise(n, cpu, NDS_IRQ_HBLANK);
	if (n->line < 192)
		nds_dma_trigger(n, NDS_CPU9, NDS_DMA_HBLANK);
}

/* ---- bucle ------------------------------------------------------------------------------------------------ */

void nds_sched_init(struct nds *n)
{
	for (int i = 0; i < EV_COUNT; i++)
		n->ev[i] = NDS_NEVER;
	n->time = 0;
	n->line = 0;
	n->ev[EV_HBLANK] = NDS_HBLANK_CYCLES;
	n->ev[EV_LINE] = NDS_LINE_CYCLES;
	n->ev_min = NDS_HBLANK_CYCLES;
	if (!n->quantum)
		n->quantum = 128;
}

static uint64_t ev_scan(const struct nds *n)
{
	uint64_t ev = NDS_NEVER;

	for (int i = 0; i < EV_COUNT; i++)
		if (n->ev[i] < ev)
			ev = n->ev[i];
	return ev;
}

static void run_events(struct nds *n)
{
	for (;;) {
		int best = -1;
		uint64_t bt = NDS_NEVER;

		for (int i = 0; i < EV_COUNT; i++)
			if (n->ev[i] <= n->time && n->ev[i] < bt) {
				bt = n->ev[i];
				best = i;
			}
		if (best < 0)
			break;
		n->st.events++;
		switch (best) {
		case EV_LINE:
			ev_line(n, bt);
			break;
		case EV_HBLANK:
			ev_hblank(n);
			break;
		case EV_CART:
			n->ev[EV_CART] = NDS_NEVER;
			nds_cart_event(n);
			break;
		case EV_SPI:
			n->ev[EV_SPI] = NDS_NEVER;
			nds_spi_event(n);
			break;
		case EV_GX:
			/* el motor de geometría: IRQ 21 por nivel (mientras GXSTAT diga «menos de la mitad» o «vacía» se
			 * vuelve a pedir aunque el programa la confirme) y avisos a la DMA de modo 7 */
			nds_gx_event(n, bt);
			break;
		default: {
			const int cpu = best >= EV_TM7;
			const int i = (best - (cpu ? EV_TM7 : EV_TM9)) & 3;

			tm_overflow(n, cpu, i, bt);
			break;
		}
		}
	}
	n->ev_min = ev_scan(n);
}

/* Una CPU en HALT sin línea de interrupción no ejecuta nada hasta que algo la despierte: su reloj solo avanza. */
static inline int cpu_idle(const struct acore *c)
{
	return c->halted && !(c->irq | c->fiq);
}

static inline void idle_to(struct acore *c, uint64_t t)
{
	if (c->cycles < t)
		c->cycles = t;
}

/* Corre las dos CPUs en tramos de `quantum` ciclos del ARM9 acotados por el siguiente evento. Si las dos están
 * ociosas (HALT / IntrWait sin interrupción) no hay nada que correr: el tiempo salta de una vez al siguiente
 * evento, sin pasos finos. Con una sola ociosa, esa solo adelanta su reloj (sin llamar al intérprete). */
void nds_sched_run(struct nds *n, uint64_t until)
{
	uint64_t (*const clk)(void) = n->prof_clock;

	n->stop = 0;
	while (n->time < until && !n->stop) {
		uint64_t next = n->ev_min;
		const int idle9 = cpu_idle(&n->a9), idle7 = cpu_idle(&n->a7);

		if (idle9 && idle7) {
			if (next > until)
				next = until;
			if (next > n->time) {
				idle_to(&n->a9, next);
				idle_to(&n->a7, (next + 1) >> 1);
				n->time = next;
				n->st.jumps++;
			}
		} else {
			uint64_t slice = n->time + n->quantum;

			if (next < slice)
				slice = next;
			if (slice > until)
				slice = until;
			if (slice > n->time) {
				uint64_t c0 = 0, c1 = 0, c2 = 0;

				if (clk)
					c0 = clk();
				if (idle9)
					idle_to(&n->a9, slice);
				else
					acore_run(&n->a9, slice);
				if (clk)
					c1 = clk();
				if (idle7)
					idle_to(&n->a7, (slice + 1) >> 1);
				else
					acore_run(&n->a7, (slice + 1) >> 1);
				if (clk) {
					c2 = clk();
					n->st.prof_a9 += c1 - c0;
					n->st.prof_a7 += c2 - c1;
				}
				n->time = slice;
				n->st.slices++;
			}
		}
		nds_halt_check(n);
		if (n->ev_min <= n->time) {
			if (clk) {
				const uint64_t e0 = clk();

				run_events(n);
				n->st.prof_ev += clk() - e0;
			} else {
				run_events(n);
			}
		}
	}
}
