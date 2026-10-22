/* SPDX-License-Identifier: GPL-2.0-or-later */
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
	const struct nds_video_ops vops = n->vid.ops;
	const int use_ref = n->vid.use_ref, use_ref3d = n->vid.use_ref3d, vs_ref3d = n->vid.vs_ref3d;
	const int capture_classic = n->vid.capture_classic;
	const uint32_t r3d_band = n->vid.r3d_band, r3d_units = n->vid.r3d_units;
	const int r3d_alt = n->vid.r3d_alt;
	const uint32_t r3d_flags = n->vid.r3d_flags, r3d_win0 = n->vid.r3d_win0, r3d_win1 = n->vid.r3d_win1, r3d_texmode = n->vid.r3d_texmode, r3d_defer = n->vid.r3d_defer_line, r3d_hist_off = n->vid.r3d_hist_off;
	uint64_t (*vclock)(void) = n->vid.clock;
	const uint32_t quantum = n->quantum;
	const uint32_t dbg = n->dbg;
	const uint8_t gx_lento = n->gx_lento;
	const struct nds_snd_ops sops = n->snd.ops;
	const uint32_t smode = n->snd.mode;
	struct arec *const jit9 = n->jit9, *const jit7 = n->jit7;
	const struct nds_jit_cfg jcfg = n->jit_cfg;
	void *const jit_user = n->jit_user;

	memset(n, 0, sizeof *n);
	n->log = log;
	n->log_ctx = log_ctx;
	n->quantum = quantum;
	n->dbg = dbg;
	n->gx_lento = gx_lento;
	n->vid.ops = vops;
	n->vid.use_ref = use_ref;
	n->vid.capture_classic = capture_classic;
	n->vid.use_ref3d = use_ref3d;
	n->vid.vs_ref3d = vs_ref3d;
	n->vid.r3d_band = r3d_band;
	n->vid.r3d_units = r3d_units;
	n->vid.r3d_alt = r3d_alt;
	n->vid.r3d_flags = r3d_flags;
	n->vid.r3d_win0 = r3d_win0;
	n->vid.r3d_win1 = r3d_win1;
	n->vid.r3d_texmode = r3d_texmode;
	n->vid.r3d_defer_line = r3d_defer;
	n->vid.r3d_hist_off = r3d_hist_off;
	n->vid.clock = vclock;
	n->snd.ops = sops;
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
	nds_video_init(n);
	nds_snd_init(n);
	nds_gx_init(n);
	n->snd.mode = smode;
	n->jit_user = jit_user;
	if (jit9 || jit7) {
		n->jit9 = jit9;
		n->jit7 = jit7;
		n->jit_cfg = jcfg;
		nds_jit_reinit(n);
	}
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
	nds_rom_free(n);
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
