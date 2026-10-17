/*
 * SalsaDS — máquina de la DS: arranque de la estructura, ROM, ejecución, entrada y la imagen de las dos
 * pantallas (Fase 1.2). Ver nds.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/nds_priv.h"

void nds_logf(struct nds *n, const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	if (!n->log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	n->log(n->log_ctx, buf);
}

void nds_tracef(struct nds *n, const char *fmt, ...)
{
	char buf[160];
	int k;
	va_list ap;

	if (!n->log)
		return;
	k = snprintf(buf, sizeof buf, "t=%llu ", (unsigned long long)n->time);
	va_start(ap, fmt);
	vsnprintf(buf + k, sizeof buf - (size_t)k, fmt, ap);
	va_end(ap);
	n->log(n->log_ctx, buf);
}

void nds_init(struct nds *n)
{
	void (*log)(void *, const char *) = n->log;
	void *log_ctx = n->log_ctx;
	const uint32_t quantum = n->quantum;
	const uint32_t dbg = n->dbg;

	memset(n, 0, sizeof *n);
	n->log = log;
	n->log_ctx = log_ctx;
	n->quantum = quantum;
	n->dbg = dbg;
	acore_init(&n->a9, ACORE_ARM9, &nds_bus9, n);
	acore_attach_tcm(&n->a9, n->itcm, n->dtcm);
	acore_init(&n->a7, ACORE_ARM7, &nds_bus7, n);
	n->a9.swi_hook = nds_swi_hook;
	n->a7.swi_hook = nds_swi_hook;
	n->keyinput = 0x03FF;
	n->extkeyin = 0x007F;
	n->rtc.status1 = 0x02;         /* formato de 24 horas */
	n->rtc.base_sec = 845553600u;  /* 2026-10-17 12:00:00 (segundos desde 2000-01-01) */
	nds_mem_init(n);
	nds_ipc_init(n);
	nds_spi_init(n);
	nds_bios_init(n);
	nds_sched_init(n);
}

int nds_load_rom(struct nds *n, const uint8_t *rom, size_t len)
{
	uint32_t a9_off, a9_size, a7_off, a7_size;
	unsigned shift;

	if (len < 0x200)
		return -1;
	a9_off = rd32le(rom + 0x20);
	a9_size = rd32le(rom + 0x2C);
	a7_off = rd32le(rom + 0x30);
	a7_size = rd32le(rom + 0x3C);
	if ((uint64_t)a9_off + a9_size > len || (uint64_t)a7_off + a7_size > len)
		return -1;
	shift = rom[0x14];
	n->cart.rom = rom;
	n->cart.size = len;
	n->cart.capacity = shift <= 14 ? 0x20000u << shift : 0;
	if (n->cart.capacity && n->cart.capacity < len)
		n->cart.capacity = 0; /* cabecera incoherente: sin espejo */
	nds_save_default(n);
	return 0;
}

void nds_run(struct nds *n, uint64_t arm9_cycles)
{
	n->stop_on_vblank = 0;
	nds_sched_run(n, n->time + arm9_cycles);
}

void nds_run_frame(struct nds *n)
{
	n->stop_on_vblank = 1;
	nds_sched_run(n, n->time + 2u * NDS_FRAME_CYCLES);
	n->stop_on_vblank = 0;
}

void nds_set_input(struct nds *n, uint32_t keys, unsigned touch_x, unsigned touch_y)
{
	uint16_t ext = 0x007F;
	const int lid_was = (n->extkeyin >> 7) & 1;

	n->keyinput = (uint16_t)(~keys & 0x3FFu);
	if (keys & NDS_KEY_X)
		ext &= (uint16_t)~1u;
	if (keys & NDS_KEY_Y)
		ext &= (uint16_t)~2u;
	if (keys & NDS_KEY_PEN)
		ext &= (uint16_t)~0x40u;
	if (keys & NDS_KEY_LID)
		ext |= 0x80;
	n->extkeyin = ext;
	n->touch_x = (uint16_t)(touch_x > 255 ? 255 : touch_x);
	n->touch_y = (uint16_t)(touch_y > 191 ? 191 : touch_y);
	if ((keys & NDS_KEY_LID) && !lid_was)
		nds_irq_raise(n, NDS_CPU7, NDS_IRQ_LIDOPEN);
	nds_keys_changed(n);
}

/* ---- imagen ------------------------------------------------------------------------------------------- */

static uint16_t bright(uint16_t c, uint16_t reg)
{
	const unsigned mode = (reg >> 14) & 3u;
	unsigned f = reg & 0x1Fu;
	unsigned ch[3] = { c & 31u, (c >> 5) & 31u, (c >> 10) & 31u };

	if (f > 16)
		f = 16;
	if (mode == 1 || mode == 2) {
		for (int i = 0; i < 3; i++)
			ch[i] = mode == 1 ? ch[i] + ((31u - ch[i]) * f) / 16u : ch[i] - (ch[i] * f) / 16u;
	}
	return (uint16_t)(0x8000u | ch[0] | (ch[1] << 5) | (ch[2] << 10));
}

static void fill(uint16_t *dst, uint16_t c)
{
	for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
		dst[i] = c;
}

/* Imagen de un motor: modo 0 = blanca; 1 = gráficos (por ahora solo el fondo, ver nds.h); 2 = banco de VRAM
 * (solo A); 3 = memoria principal (solo A; sin implementar: negra). */
static void engine(const struct nds *n, int b, uint16_t *dst)
{
	const unsigned gi = b ? 0x1000u >> 1 : 0;
	const uint32_t dispcnt = (uint32_t)n->io[NDS_CPU9].gen[gi] | ((uint32_t)n->io[NDS_CPU9].gen[gi + 1] << 16);
	const uint16_t br = n->io[NDS_CPU9].gen[(b ? 0x106C : 0x6C) >> 1];
	const unsigned mode = (dispcnt >> 16) & 3u;
	uint16_t c;

	if (mode == 0 || (mode == 1 && (dispcnt & 0x80u))) {
		fill(dst, bright(0x7FFF, br));
		return;
	}
	if (mode == 1 || (b && mode != 1)) {
		c = rd16le(n->pal + (b ? 0x400 : 0));
		fill(dst, bright(c, br));
		return;
	}
	if (mode == 2) {
		const unsigned bank = (dispcnt >> 18) & 3u;
		const uint8_t v = n->vramcnt[bank];
		const uint8_t *src = n->vram + 0x20000u * bank;

		if (!(v & 0x80u) || (v & 7u)) { /* tiene que estar habilitado y asignado al LCDC */
			fill(dst, bright(0, br));
			return;
		}
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
			dst[i] = bright(rd16le(src + 2 * i), br);
		return;
	}
	fill(dst, bright(0, br));
}

void nds_screens(const struct nds *n, uint16_t *top, uint16_t *bottom)
{
	if (n->powcnt1 & 0x8000u) {
		engine(n, 0, top);
		engine(n, 1, bottom);
	} else {
		engine(n, 1, top);
		engine(n, 0, bottom);
	}
}
