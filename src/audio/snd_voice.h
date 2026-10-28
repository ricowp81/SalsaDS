/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Adaptación de desmume/src/SPU.cpp y SPU.h, commit
 * fc504ee0bc9ac3434cafcaae7b129c025499a609 (TASEmulators/desmume).
 * Copyright (C) 2006 yopyop
 * Copyright (C) 2006 Theo Berkau
 * Copyright (C) 2008-2025 DeSmuME team
 * Ideas borrowed from Stephane Dallongeville's SCSP core
 *
 * This file is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this software. If not, see <https://www.gnu.org/licenses/>.
 *
 * Uso interno de snd_mix.c. Reloj entero a 16 756 991 Hz, sin tablas
 * calculadas al arrancar ni asignaciones. El bus mantiene las lecturas LE.
 */

static uint32_t voice_byte(const snd_bus *bus, snd_chan *c, uint32_t addr, const cap_run *cr)
{
	const uint32_t line = addr & ~(uint32_t)(SND_LINE - 1);
	/* Reenvío de escrituras aún agrupadas para DMA. No esperar al siguiente
	 * trabajo para oír la captura: eso cambiaba la realimentación del juego. */
	for (int i = 1; i >= 0; i--) {
		const uint32_t d = addr - cr[i].addr;
		if (d < cr[i].n) return cr[i].buf[d];
	}
	if (c->tag != line) {
		bus->fetch(bus->ctx, line, c->line);
		c->tag = line;
	}
	return c->line[addr & (SND_LINE - 1)];
}

static void voice_header(snd_chan *c, const snd_bus *bus, const cap_run *cr)
{
	const uint32_t h = voice_byte(bus, c, c->sad, cr) | voice_byte(bus, c, c->sad + 1, cr) << 8;
	c->adv = (int16_t)h;
	c->adi = (int32_t)(voice_byte(bus, c, c->sad + 2, cr) & 127u);
	if (c->adi > 88) c->adi = 88;
	c->hdr = 1;
}

static void voice_start(snd_chan *c, const snd_bus *bus, const cap_run *cr)
{
	const uint32_t fmt = c->cnt >> 29 & 3u;
	c->pos = fmt == 3 ? -1 : -3;
	c->loop_valid = 0;
	if (fmt == 2) voice_header(c, bus, cr);
	if (fmt != 3 && c->pnt + c->len == 0) c->on = c->keyon = 0;
}

static void voice_step(snd_chan *c, int idx, const snd_bus *bus, const cap_run *cr)
{
	static const uint8_t shift[4] = {2, 1, 3, 0};
	const uint32_t fmt = c->cnt >> 29 & 3u;
	const uint32_t total = (c->pnt + c->len) << shift[fmt], loop = (uint32_t)c->pnt << shift[fmt];
	const int32_t pos = c->pos;
	c->out = 0;
	if (fmt != 3 && pos >= 0 && (uint32_t)pos >= total &&
	    (!c->len || (fmt == 2 && c->pnt + c->len < 4))) return;
	if (pos >= 0) {
		if (fmt == 0) {
			c->out = (int8_t)voice_byte(bus, c, c->sad + (uint32_t)pos, cr) * 256;
		} else if (fmt == 1) {
			const uint32_t a = c->sad + (uint32_t)pos * 2;
			c->out = (int16_t)(voice_byte(bus, c, a, cr) | voice_byte(bus, c, a + 1, cr) << 8);
		} else if (fmt == 2 && pos >= 8) {
			if (c->pnt + c->len < 4 && (uint32_t)pos >= total) return;
			if ((uint32_t)pos == loop) {
				c->alv = c->adv; c->ali = c->adi; c->loop_valid = 1;
			}
			const uint32_t b = voice_byte(bus, c, c->sad + ((uint32_t)pos >> 1), cr);
			const uint32_t nib = (b >> ((pos & 1) * 4)) & 15u;
			/* DeSmuME calcula una sola división, no tres sumandos truncados. */
			const int32_t delta = ((int32_t)(2 * (nib & 7u) + 1) * ima_step[c->adi]) >> 3;
			c->adv += nib & 8u ? -delta : delta;
			if (c->adv < -32768) c->adv = -32768;
			if (c->adv > 32767) c->adv = 32767;
			c->adi += ima_adj[nib & 7u];
			if (c->adi < 0) c->adi = 0;
			if (c->adi > 88) c->adi = 88;
			c->out = c->adv;
		} else if (fmt == 3 && idx >= 8) {
			if (idx < 14) c->out = ((uint32_t)pos & 7u) <= (c->cnt >> 24 & 7u) ? 32767 : -32767;
			else {
				const uint32_t bit = c->lfsr & 1u;
				c->lfsr = (c->lfsr >> 1) ^ (bit ? 0x6000u : 0);
				c->out = bit ? -32767 : 32767;
			}
		}
	}
	c->pos++;
	if (fmt == 3) { c->pos &= 7; return; }
	if (c->pos < 0 || (uint32_t)c->pos < total) return;
	if ((c->cnt >> 27 & 3u) != 1) { c->on = c->keyon = 0; return; }
	/* Longitud cero o ADPCM demasiado corto: ocupado sin leer más allá. */
	if (!c->len || (fmt == 2 && c->pnt + c->len < 4)) return;
	c->pos = (int32_t)loop;
	c->tag = UINT32_MAX;
	if (fmt == 2) {
		if (c->loop_valid) { c->adv = c->alv; c->adi = c->ali; }
		else voice_header(c, bus, cr);
	}
}

static int32_t voice_mul7(int32_t v, uint32_t gain)
{
	return gain == 127u ? v : (v * (int32_t)gain) >> 7;
}
