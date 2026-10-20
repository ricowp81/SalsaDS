/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — video de la máquina: instantáneas por cuadro y registro de cambios por línea para los motores 2D
 * (Fase 2.1). Ver el comentario de `struct nds_video` en nds.h y el encabezado de src/gpu2d/gpu2d.h.
 *
 * Línea 0 (`nds_video_frame_begin`): copia a la instantánea del cuadro las páginas de VRAM cambiadas desde la última
 * vez que se llenó ese juego, la paleta y la OAM; copia los registros de cada motor y arma las tablas de páginas
 * de las vistas (lo que VRAMCNT mapea a fondos y a OBJ de A y B), las ranuras de paletas extendidas y los bancos
 * del modo de visualización de VRAM.
 *
 * Durante las líneas visibles el ARM9 escribe registros de los motores, paleta y OAM: cada escritura se anota con
 * la primera línea en que se ve (la línea siguiente a la del instante en que se escribió: el motor dibuja una
 * línea con lo que tiene al empezarla, como hace melonDS). Línea 192 (`nds_video_frame_end`): el cuadro queda
 * cerrado y se entrega al renderizador.
 *
 * Fuente: GBATEK (mapas de E/S y de VRAM de los motores 2D). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

#define PAGE_SHIFT 14u

/* Primera línea en la que se ve una escritura hecha ahora; NDS_VIDEO_NONE si no se ve en este cuadro. Como en
 * melonDS (la sonda `gfxprobe` lo mostró), el motor dibuja cada línea al llegar su HBlank: una escritura anterior al
 * HBlank de la línea L (el punto 256) se ve en la propia línea L, una posterior en la L + 1. */
#define NDS_VIDEO_NONE 0xFFFFFFFFu

static uint32_t eff_line(const struct nds *n)
{
	uint32_t line;

	if (!n->vid.recording)
		return NDS_VIDEO_NONE;
	line = nds_line_at(n->a9.cycles);
	if (line >= 192u)
		return NDS_VIDEO_NONE;
	if (nds_dot_at(n->a9.cycles) >= NDS_HBLANK_CYCLES)
		line++;
	return line >= 192u ? NDS_VIDEO_NONE : line;
}

static void log_change(struct nds *n, int eng, uint32_t reg, uint32_t value)
{
	const uint32_t line = eff_line(n);
	struct nds_vset *s = &n->vid.set[n->vid.cur];
	gpu2d_engine *ge = &s->frame.eng[eng];

	if (line == NDS_VIDEO_NONE)
		return;
	if (ge->nchg >= NDS_VLOG_MAX) {
		s->dropped++;
		return;
	}
	s->chg[eng][ge->nchg].line = (uint16_t)line;
	s->chg[eng][ge->nchg].reg = (uint16_t)reg;
	s->chg[eng][ge->nchg].value = value;
	ge->nchg++;
}

void nds_video_reg_changed(struct nds *n, uint32_t o)
{
	int eng;
	uint32_t i;

	if (o < 0x70u) {
		eng = 0;
		i = o >> 1;
	} else if (o >= 0x1000u && o < 0x1070u) {
		eng = 1;
		i = (o - 0x1000u) >> 1;
	} else {
		return;
	}
	if (i == 2 || i == 3) /* DISPSTAT y VCOUNT (los atiende el planificador) */
		return;
	if (eng == 0 && (i == 52 || i == 53)) { /* DISP_MMEM_FIFO */
		nds_video_mmem_push(n, n->io[NDS_CPU9].gen[o >> 1]);
		return;
	}
	log_change(n, eng, i, n->io[NDS_CPU9].gen[o >> 1]);
}

void nds_video_pal_written(struct nds *n, uint32_t off)
{
	const uint32_t eng = (off >> 10) & 1u, idx = (off & 0x3FFu) >> 1;

	log_change(n, (int)eng, G2D_CHG_PAL, (idx << 16) | (rd16le(n->pal + (off & ~1u)) & 0x7FFFu));
}

void nds_video_oam_written(struct nds *n, uint32_t off)
{
	const uint32_t eng = (off >> 10) & 1u, idx = (off & 0x3FFu) >> 1;

	log_change(n, (int)eng, G2D_CHG_OAM, (idx << 16) | rd16le(n->oam + (off & ~1u)));
}

void nds_video_vram_dirty(struct nds *n, uint32_t vram_off)
{
	const uint64_t bit = 1ull << (vram_off >> PAGE_SHIFT);

	n->vid.dirty[0] |= bit;
	n->vid.dirty[1] |= bit;
}

void nds_video_mmem_push(struct nds *n, uint16_t px)
{
	struct nds_video *v = &n->vid;

	if (v->mpos < (uint32_t)(GPU2D_W * GPU2D_H)) {
		wr16le(&v->set[v->cur].mmem[v->mpos / GPU2D_W][(v->mpos % GPU2D_W) * 2], px);
		v->mpos++;
	}
}

/* Línea visible `line` empieza: la FIFO del modo 3 se alinea con ella. */
void nds_video_line_start(struct nds *n, uint32_t line)
{
	n->vid.mpos = line * GPU2D_W;
}

void nds_video_init(struct nds *n)
{
	struct nds_video *v = &n->vid;
	struct nds_video_ops ops = v->ops;
	const int use_ref = v->use_ref, use_ref3d = v->use_ref3d, vs_ref3d = v->vs_ref3d;
	const uint32_t band = v->r3d_band, units = v->r3d_units, r3d_flags = v->r3d_flags, win0 = v->r3d_win0, win1 = v->r3d_win1, texmode = v->r3d_texmode, defer = v->r3d_defer_line, hist_off = v->r3d_hist_off;
	uint64_t (*clk)(void) = v->clock;

	memset(v, 0, sizeof *v);
	v->ops = ops;
	v->use_ref = use_ref;
	v->use_ref3d = use_ref3d;
	v->vs_ref3d = vs_ref3d;
	v->r3d_band = band ? band : 16u;
	v->r3d_units = units ? units : 1u;
	v->r3d_flags = r3d_flags;
	v->r3d_win0 = win0;
	v->r3d_win1 = win1;
	v->r3d_texmode = texmode;
	v->r3d_defer_line = defer;
	v->r3d_hist_off = hist_off;
	v->clock = clk;
	v->cur = 1;
	v->last = -1;
	v->dirty[0] = v->dirty[1] = (1ull << NDS_VPAGES) - 1;
	nds_video_frame_begin(n); /* el primer cuadro empieza en el instante 0: la línea 0 no tiene evento */
}

static uint32_t ext_slot(unsigned bank, unsigned slot)
{
	return nds_vram_bank_offset(bank) + 0x2000u * slot;
}

/* ---- capa 3D (Fase 3.2) ------------------------------------------------------------------------------------- */

/* Rasteriza en el host el trabajo de `s` (con el código del SPE o con la referencia). */
static void render3d_host(struct nds *n, struct nds_vset *s)
{
	struct nds_video *v = &n->vid;
	const r3d_job *jb = &s->r3d;
	const gx_frame *f = (jb->flags & R3D_F_BLANK) ? NULL : (const gx_frame *)(uintptr_t)jb->frame_ea;
	const uint64_t t0 = v->clock ? v->clock() : 0;
	r3d_stats fb_sum;

	memset(&fb_sum, 0, sizeof fb_sum);
	if (v->use_ref3d) {
		r3d_render_ref(f, s->vram, jb->texslot, jb->palslot, jb->flags, s->img3d);
	} else {
		for (uint32_t u = 0; u < jb->nunits; u++) {
			r3d_job j = *jb;
			r3d_stats st;

			j.unit = u;
			r3d_core_render(&v->r3c, &j, &st);
			fb_sum.tex_px += st.tex_px;
			fb_sum.tex_misses += st.tex_misses;
			if (!v->r3d_win1 || (v->frames >= v->r3d_win0 && v->frames < v->r3d_win1)) {
				uint32_t *dst = (uint32_t *)(void *)&v->r3d_sum;
				const uint32_t *src = (const uint32_t *)(const void *)&st;

				for (size_t k = 0; k < sizeof(r3d_stats) / 4; k++)
					dst[k] += src[k];
			}
		}
	}
	if (v->clock)
		v->r3d_ticks += v->clock() - t0;
	if (!v->use_ref3d)
		nds_video_r3d_feedback(n, &fb_sum);
	if (!v->r3d_win1 || (v->frames >= v->r3d_win0 && v->frames < v->r3d_win1))
		v->r3d_frames++;
	if (v->vs_ref3d && !v->use_ref3d) {
		r3d_render_ref(f, s->vram, jb->texslot, jb->palslot, jb->flags, v->r3d_tmp);
		if (memcmp(v->r3d_tmp, s->img3d, sizeof v->r3d_tmp) != 0)
			v->r3d_diff_frames++;
	}
}

/* El trabajo del cuadro `set` terminó: lo que tardó cada banda reparte los cuadros siguientes (r3d_balance). */
void nds_video_r3d_hist(struct nds *n, int set, const uint32_t *ticks)
{
	struct nds_video *v = &n->vid;

	memcpy(v->r3d_hist, ticks, sizeof v->r3d_hist);
	v->r3d_hist_est = v->r3d_est[set & 1];
}

/* Caché de texturas automática (3.9): r3d_texpol_feed en gpu3d/r3d.h. */
void nds_video_r3d_feedback(struct nds *n, const r3d_stats *st)
{
	if (n->vid.r3d_texmode == 0)
		r3d_texpol_feed(&n->vid.r3d_pol, st);
}

/* Línea 0: el cuadro que el motor de geometría entregó en el último SWAP_BUFFERS se dibuja durante este (como en la DS,
 * que lo rasteriza mientras barre la pantalla), con la VRAM de texturas de la instantánea que se acaba de tomar. */
static void frame_begin_3d(struct nds *n, struct nds_vset *s)
{
	struct nds_video *v = &n->vid;
	gpu2d_engine *ga = &s->frame.eng[0];
	const uint32_t dc = g2d_dispcnt(ga);
	const gx_frame *f = gx_front(&n->gx);
	r3d_job *j = &s->r3d;
	uint32_t boff[7];

	s->has3d = (dc & G2D_DC_BG(0)) && ((dc & G2D_DC_3D) || G2D_DC_BGMODE(dc) == 6);
	ga->img3d_ea = s->has3d ? (uint64_t)(uintptr_t)s->img3d : 0;
	if (!s->has3d)
		return;
	memset(j, 0, sizeof *j);
	for (unsigned b = 0; b < 7; b++)
		boff[b] = nds_vram_bank_offset(b);
	r3d_slots_from_vramcnt(n->vramcnt, boff, j->texslot, j->palslot);
	j->frame_ea = (uint64_t)(uintptr_t)f;
	j->vram_ea = (uint64_t)(uintptr_t)s->vram;
	j->out_ea = (uint64_t)(uintptr_t)s->img3d;
	j->nunits = v->r3d_units;
	j->band = v->r3d_band;
	if (f && !v->r3d_alt)
		v->r3d_est[v->cur] = r3d_balance(f, j->band, j, v->r3d_hist_off ? NULL : v->r3d_hist, v->r3d_hist_est); /* reparto por costo para dos unidades (si hay más o menos, el alterno) */
	/* sin cuadro entregado o con el motor de render apagado (POWCNT1 bit 2): transparente */
	if (!f || !(n->powcnt1 & 4u))
		j->flags |= R3D_F_BLANK;
	j->flags |= v->r3d_flags;
	if (v->r3d_texmode == 1 || (v->r3d_texmode == 0 && !v->r3d_pol.cells))
		j->flags |= R3D_F_TEXBLOCK;
	if (v->ops.submit3d)
		v->ops.submit3d(v->ops.ctx, v->cur, j);
	else
		render3d_host(n, s);
}

void nds_video_frame_begin(struct nds *n)
{
	struct nds_video *v = &n->vid;
	struct nds_vset *s;

	v->cur ^= 1;
	s = &v->set[v->cur];
	if (v->ops.wait)
		v->ops.wait(v->ops.ctx, v->cur);

	/* VRAM: solo las páginas cambiadas desde la última vez que se llenó este juego */
	for (unsigned pg = 0; pg < NDS_VPAGES; pg++)
		if (v->dirty[v->cur] & (1ull << pg))
			memcpy(s->vram + pg * 0x4000u, n->vram + pg * 0x4000u, 0x4000u);
	v->dirty[v->cur] = 0;
	memcpy(s->pal[0], n->pal, 2 * 1024);
	memcpy(s->oam[0], n->oam, 2 * 1024);
	memset(s->vram + G2D_VRAM_SIZE, 0, G2D_VRAM_ZERO);
	s->dropped = 0;

	for (int e = 0; e < 2; e++) {
		gpu2d_engine *ge = &s->frame.eng[e];
		const unsigned wbg = e ? 1u : 0u, wobj = e ? 3u : 2u;
		const uint16_t *gen = &n->io[NDS_CPU9].gen[e ? 0x800 : 0];

		memset(ge, 0, sizeof *ge);
		ge->vram_ea = (uint64_t)(uintptr_t)s->vram;
		ge->pal_ea = (uint64_t)(uintptr_t)s->pal[e];
		ge->oam_ea = (uint64_t)(uintptr_t)s->oam[e];
		ge->chg_ea = (uint64_t)(uintptr_t)s->chg[e];
		ge->mmem_ea = e ? 0 : (uint64_t)(uintptr_t)s->mmem;
		ge->flags = e ? G2D_F_ENGINE_B : 0;
		ge->bgpages = e ? G2D_BG_PAGES_B : G2D_BG_PAGES_A;
		ge->objpages = e ? G2D_OBJ_PAGES_B : G2D_OBJ_PAGES_A;
		for (int i = 0; i < G2D_NREGS; i++)
			ge->regs[i] = gen[i];
		for (unsigned pg = 0; pg < G2D_BG_PAGES_A; pg++)
			ge->bgpg[pg] = pg < ge->bgpages ? nds_vram_view_offset(n, wbg, pg) : G2D_UNMAPPED;
		for (unsigned pg = 0; pg < G2D_OBJ_PAGES_A; pg++)
			ge->objpg[pg] = pg < ge->objpages ? nds_vram_view_offset(n, wobj, pg) : G2D_UNMAPPED;
		for (int k = 0; k < 4; k++)
			ge->extbg[k] = G2D_UNMAPPED;
		ge->extobj = G2D_UNMAPPED;
		for (int k = 0; k < 4; k++)
			ge->vdisp[k] = G2D_UNMAPPED;
	}
	{
		/* Paletas extendidas (GBATEK, «DS Memory Control - VRAM»): fondos de A en E (MST 4, ranuras 0..3) o en F/G
		 * (MST 4, OFS 0: ranuras 0-1; OFS 1: ranuras 2-3); OBJ de A en F/G (MST 5); fondos de B en H (MST 2) y OBJ
		 * de B en I (MST 3). */
		gpu2d_engine *ga = &s->frame.eng[0], *gb = &s->frame.eng[1];

		for (unsigned b = 4; b < 9; b++) {
			const uint8_t c = n->vramcnt[b];
			const unsigned mst = c & 7u, ofs = (c >> 3) & 3u;

			if (!(c & 0x80u))
				continue;
			if (b == 4 && mst == 4) {
				for (unsigned k = 0; k < 4; k++)
					ga->extbg[k] = ext_slot(b, k);
			} else if ((b == 5 || b == 6) && mst == 4) {
				for (unsigned k = 0; k < 2; k++)
					ga->extbg[(ofs & 1u) * 2 + k] = ext_slot(b, k);
			} else if ((b == 5 || b == 6) && mst == 5) {
				if (ga->extobj == G2D_UNMAPPED)
					ga->extobj = ext_slot(b, 0);
			} else if (b == 7 && (mst & 3u) == 2) {
				for (unsigned k = 0; k < 4; k++)
					gb->extbg[k] = ext_slot(b, k);
			} else if (b == 8 && (mst & 3u) == 3) {
				gb->extobj = ext_slot(b, 0);
			}
		}
		for (unsigned b = 0; b < 4; b++)
			if ((n->vramcnt[b] & 0x80u) && (n->vramcnt[b] & 7u) == 0)
				ga->vdisp[b] = nds_vram_bank_offset(b);
	}
	frame_begin_3d(n, s);
	v->mpos = 0;
	v->recording = 1;
}

/* ---- captura de pantalla (DISPCAPCNT, motor A) ----------------------------------------------------------------- */

static uint16_t cap_rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Si el cuadro empezó con la captura pedida (bit 31), la hace al terminar las líneas visibles: la imagen del motor A
 * (o solo la capa 3D) se mezcla con un banco de VRAM o con la FIFO y se escribe en la VRAM emulada; el bit 31 se apaga.
 * La imagen A se compone aquí, aparte, con el mismo código que la pantalla (referencia o compositor por líneas). */
static void capture_frame(struct nds *n, struct nds_vset *s)
{
	struct nds_video *v = &n->vid;
	const gpu2d_engine *ge = &s->frame.eng[0];
	const uint32_t cap = (uint32_t)ge->regs[G2D_R_DISPCAP] | ((uint32_t)ge->regs[G2D_R_DISPCAP + 1] << 16);
	uint16_t *cur = &n->io[NDS_CPU9].gen[0x64 >> 1];
	const unsigned srcsel = (cap >> 29) & 3u, size = (cap >> 20) & 3u;
	const unsigned eva = (cap & 31u) > 16u ? 16u : (cap & 31u), evb = ((cap >> 8) & 31u) > 16u ? 16u : ((cap >> 8) & 31u);
	const unsigned width = size == 0 ? 128u : 256u, height = size == 0 ? 128u : size == 1 ? 64u : size == 2 ? 128u : 192u;
	const int use3d = (cap >> 24) & 1u, usefifo = (cap >> 25) & 1u;
	const uint32_t dc = g2d_dispcnt(ge);
	uint8_t *wb = n->vram + nds_vram_bank_offset((cap >> 16) & 3u);
	const uint8_t *rb = n->vram + nds_vram_bank_offset(G2D_DC_VRAMBANK(dc));
	const uint32_t woff = ((cap >> 18) & 3u) * 0x8000u, roff = ((cap >> 26) & 3u) * 0x8000u;
	const uint32_t *i3 = (const uint32_t *)(uintptr_t)ge->img3d_ea;
	const uint16_t *aimg = NULL;
	gpu2d_engine e = *ge;

	/* se apaga el bit 31 si nadie lo volvió a escribir durante el cuadro */
	if (cur[1] == ge->regs[G2D_R_DISPCAP + 1])
		cur[1] &= 0x7FFFu;
	if (srcsel != 1 && !(use3d && i3)) {
		/* composición de A sin brillo maestro y con la pantalla en modo de gráficos */
		e.regs[G2D_R_DISPCNT] &= (uint16_t)~G2D_DC_BLANK;
		e.regs[G2D_R_DISPCNT + 1] = (uint16_t)((e.regs[G2D_R_DISPCNT + 1] & ~3u) | 1u);
		e.regs[G2D_R_MASTER_BRIGHT] = 0;
		e.out_ea = (uint64_t)(uintptr_t)v->capimg;
		if (v->use_ref)
			gpu2d_ref_engine(&e, v->capimg);
		else
			gpu2d_compose_engine(&v->comp, &e);
		aimg = v->capimg;
	}
	for (unsigned y = 0; y < height; y++) {
		for (unsigned x = 0; x < width; x++) {
			unsigned ar = 0, ag = 0, ab = 0, aa = 0, br = 0, bg = 0, bb = 0, ba = 0;
			unsigned r, g, b, a;
			uint16_t px;

			if (srcsel != 1) {
				if (use3d) {
					const uint32_t p = i3 ? i3[y * R3D_W + x] : 0;

					aa = R3D_PX_A(p) != 0;
					if (aa) {
						ar = R3D_PX_R(p) >> 1;
						ag = R3D_PX_G(p) >> 1;
						ab = R3D_PX_B(p) >> 1;
					}
				} else {
					const uint16_t p = aimg[y * GPU2D_W + x];

					aa = 1;
					ar = (p >> 11) & 31u;
					ag = (p >> 6) & 31u;
					ab = (p >> 1) & 31u;
				}
			}
			if (srcsel != 0) {
				uint16_t p;

				if (usefifo) {
					p = cap_rd16(&s->mmem[y][x * 2]);
					ba = 1;
				} else {
					p = cap_rd16(rb + ((roff + (y * 256u + x) * 2u) & 0x1FFFFu));
					ba = (p >> 15) & 1u;
				}
				br = p & 31u;
				bg = (p >> 5) & 31u;
				bb = (p >> 10) & 31u;
			}
			if (srcsel == 0) {
				r = ar; g = ag; b = ab; a = aa;
			} else if (srcsel == 1) {
				r = br; g = bg; b = bb; a = ba;
			} else {
				r = (ar * eva + br * evb) >> 4;
				g = (ag * eva + bg * evb) >> 4;
				b = (ab * eva + bb * evb) >> 4;
				if (r > 31) r = 31;
				if (g > 31) g = 31;
				if (b > 31) b = 31;
				a = (eva && aa) || (evb && ba);
			}
			px = (uint16_t)(r | (g << 5) | (b << 10) | (a << 15));
			{
				uint8_t *d = wb + ((woff + (y * width + x) * 2u) & 0x1FFFFu);

				d[0] = (uint8_t)px;
				d[1] = (uint8_t)(px >> 8);
			}
		}
	}
	for (uint32_t pg = 0; pg < 8; pg++)
		nds_video_vram_dirty(n, nds_vram_bank_offset((cap >> 16) & 3u) + pg * 0x4000u);
}

void nds_video_frame_end(struct nds *n)
{
	struct nds_video *v = &n->vid;
	struct nds_vset *s = &v->set[v->cur];

	if (!v->recording)
		return;
	v->recording = 0;
	if (s->frame.eng[0].regs[G2D_R_DISPCAP + 1] & 0x8000u) {
		if (s->has3d && v->ops.wait3d)
			v->ops.wait3d(v->ops.ctx, v->cur); /* la captura lee la capa 3D: aquí sí hay que esperarla */
		capture_frame(n, s);
	}
	s->powcnt1 = n->powcnt1;
	v->last = v->cur;
	v->rendered = 0;
	v->frames++;
	if (v->ops.submit && v->ops.wait3d)
		v->submit_pending = 1; /* se entrega en la línea 262 (o más tarde), cuando el render 3D de este cuadro ya terminó */
	else if (v->ops.submit)
		v->ops.submit(v->ops.ctx, v->cur, &s->frame);
}

/* Línea 262 (o, con r3d_defer_line, esa línea del cuadro siguiente): se espera el render 3D del cuadro que cerró la línea 192 y se
 * entrega su 2D. El render tuvo el cuadro entero en lugar de 192 líneas (o más, con el aplazamiento); la imagen es la misma (el 3D
 * sigue siendo el del SWAP_BUFFERS anterior, como en la DS). Aplazar no agrega latencia mientras el 2D (que el SPE compone mientras
 * se emula el resto del cuadro) esté listo cuando quien dibuja lo pide al terminar la pasada: eso lo cuida quien fija la línea.
 */
void nds_video_submit_pending(struct nds *n, uint32_t line)
{
	struct nds_video *v = &n->vid;
	const uint32_t dl = v->r3d_defer_line;

	if (!v->submit_pending)
		return;
	if (dl && v->ops.wait3d) {
		if (line == NDS_LINES - 1u && v->ops.submit_early && !v->early_done) {
			v->early_done = 1; /* la pantalla B no usa el 3D: se compone ya, mientras el 3D termina */
			v->ops.submit_early(v->ops.ctx, v->last, &v->set[v->last].frame);
			return;
		}
		/* en las líneas dl..192 del cuadro siguiente al del pendiente (la primera que lo cumpla si dl cambió entre cuadros): el
		 * pendiente se marca en la 192, así que las líneas 193..262 del mismo cuadro y las anteriores a dl no lo tocan */
		if (line < dl || line > 192u)
			return;
	} else if (line != NDS_LINES - 1u) {
		return;
	}
	v->submit_pending = 0;
	if (v->ops.wait3d)
		v->ops.wait3d(v->ops.ctx, v->last);
	if (v->ops.submit)
		v->ops.submit(v->ops.ctx, v->last, &v->set[v->last].frame);
	v->early_done = 0;
}

const gpu2d_frame *nds_video_last_frame(const struct nds *n, int *set)
{
	if (n->vid.last < 0)
		return NULL;
	if (set)
		*set = n->vid.last;
	return &n->vid.set[n->vid.last].frame;
}

/* ---- host ---------------------------------------------------------------------------------------------- */

void nds_video_render_host(struct nds *n)
{
	struct nds_video *v = &n->vid;

	if (v->last < 0 || v->rendered)
		return;
	for (int e = 0; e < 2; e++) {
		gpu2d_engine *ge = &v->set[v->last].frame.eng[e];

		ge->out_ea = (uint64_t)(uintptr_t)v->img[e];
		if (v->use_ref)
			gpu2d_ref_engine(ge, v->img[e]);
		else
			gpu2d_compose_engine(&v->comp, ge);
		{
			uint32_t *dst = (uint32_t *)(void *)&v->stat_sum[e];
			const uint32_t *src = (const uint32_t *)(const void *)&v->comp.stats;

			for (size_t k = 0; k < sizeof(gpu2d_stats) / 4; k++)
				dst[k] += src[k];
		}
	}
	v->stat_frames++;
	v->rendered = 1;
}

/* RGBA5551 → RGB555 con el bit 15 en 1 */
static uint16_t to_555(uint16_t v)
{
	return (uint16_t)(0x8000u | ((v >> 11) & 31u) | (((v >> 6) & 31u) << 5) | (((v >> 1) & 31u) << 10));
}

void nds_screens(struct nds *n, uint16_t *top, uint16_t *bottom)
{
	struct nds_video *v = &n->vid;
	int swap;

	if (v->last < 0) {
		for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++)
			top[i] = bottom[i] = 0xFFFFu;
		return;
	}
	nds_video_render_host(n);
	swap = (v->set[v->last].powcnt1 & 0x8000u) != 0;
	for (int i = 0; i < NDS_SCREEN_W * NDS_SCREEN_H; i++) {
		top[i] = to_555(v->img[swap ? 0 : 1][i]);
		bottom[i] = to_555(v->img[swap ? 1 : 0][i]);
	}
}
