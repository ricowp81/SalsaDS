/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — referencia del motor de render 3D (Fase 3.2): el cuadro entero en memoria, los polígonos en el orden de la
 * DS uno detrás de otro, cada texel leído directo de la VRAM de la instantánea. Escrita para leerse; es la «verdad»
 * contra la que se prueba el rasterizador por bandas (r3d_core.c, el código del SPE). Las reglas están en r3d_px.h.
 * Código propio (docs/fase3.md, 3.2).
 */
#include <string.h>

#include "gpu3d/r3d.h"
#include "gpu3d/r3d_px.h"

typedef struct ref_vram {
	const uint8_t *vram;
	const uint32_t *texslot, *palslot;
} ref_vram;

static uint32_t ref_rdt(void *ctx, uint32_t a)
{
	const ref_vram *v = (const ref_vram *)ctx;
	const uint32_t off = v->texslot[(a >> 17) & 3u];

	a &= 0x7FFFFu;
	return off == R3D_UNMAPPED ? 0u : v->vram[off + (a & 0x1FFFFu)];
}

static uint32_t ref_rdp(void *ctx, uint32_t a)
{
	const ref_vram *v = (const ref_vram *)ctx;
	const uint32_t slot = (a >> 14) & 7u;
	const uint32_t off = slot < R3D_PAL_SLOTS ? v->palslot[slot] : R3D_UNMAPPED;

	return off == R3D_UNMAPPED ? 0u : v->vram[off + (a & 0x3FFFu)];
}

static uint32_t ref_tex(void *ctx, const r3d_tex *tx, uint32_t s, uint32_t t)
{
	return r3d_decode(tx, s, t, ref_rdt, ref_rdp, ctx);
}

static uint32_t rcol[R3D_H][R3D_W], rdep[R3D_H][R3D_W], ratt[R3D_H][R3D_W];
static uint32_t rkey[GX_MAX_POLYS];
static uint16_t rord[GX_MAX_POLYS];

void r3d_render_ref(const gx_frame *f, const uint8_t *vram, const uint32_t texslot[R3D_TEX_SLOTS],
		    const uint32_t palslot[R3D_PAL_SLOTS], uint32_t flags, uint32_t *out)
{
	r3d_rstate rs;
	ref_vram rv = { vram, texslot, palslot };
	r3d_stats st;
	uint32_t n;

	if (!f || (flags & R3D_F_BLANK)) {
		memset(out, 0, sizeof(uint32_t) * R3D_W * R3D_H);
		return;
	}
	memset(&st, 0, sizeof st);
	r3d_rstate_init(&rs, f, flags);
	for (int y = 0; y < R3D_H; y++)
		for (int x = 0; x < R3D_W; x++) {
			rcol[y][x] = rs.clear_px;
			rdep[y][x] = rs.clear_depth;
			ratt[y][x] = rs.clear_attr;
		}
	/* orden de dibujo: inserción estable por clave */
	n = f->npolys > GX_MAX_POLYS ? GX_MAX_POLYS : f->npolys;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t k = r3d_sort_key(&f->poly[i], f->swap_params & 1u);
		uint32_t j = i;

		while (j > 0 && rkey[j - 1] > k) {
			rkey[j] = rkey[j - 1];
			rord[j] = rord[j - 1];
			j--;
		}
		rkey[j] = k;
		rord[j] = (uint16_t)i;
	}
	for (uint32_t o = 0; o < n; o++) {
		const gx_poly *gp = &f->poly[rord[o]];
		const gx_vertex *vv[GX_POLY_MAXV];
		r3d_poly p;
		int32_t y0, y1;

		if (gp->nverts < 3 || gp->nverts > GX_POLY_MAXV)
			continue;
		for (int i = 0; i < gp->nverts; i++)
			vv[i] = &f->vtx[gp->vidx[i] % GX_MAX_VERTS];
		if (!r3d_poly_setup(&p, gp, vv, &rs))
			continue;
		y0 = p.ytop < 0 ? 0 : p.ytop;
		y1 = p.ybot > R3D_H - 1 ? R3D_H - 1 : p.ybot;
		for (int32_t y = y0; y <= y1; y++) {
			r3d_row row;

			if (r3d_row_at(&p, y, &row))
				r3d_draw_row(&p, &row, &rs, rcol[y], rdep[y], ratt[y], ref_tex, &rv, &st);
		}
	}
	if ((rs.d3 & D3_EDGE) && !(flags & R3D_F_NOEDGE))
		for (int y = 0; y < R3D_H; y++)
			r3d_edge_row(&rs, rcol[y], rdep[y], ratt[y], y > 0 ? rdep[y - 1] : NULL, y > 0 ? ratt[y - 1] : NULL,
				     y < R3D_H - 1 ? rdep[y + 1] : NULL, y < R3D_H - 1 ? ratt[y + 1] : NULL);
	if ((rs.d3 & D3_FOG) && !(flags & R3D_F_NOFOG))
		for (int y = 0; y < R3D_H; y++)
			r3d_fog_row(&rs, rcol[y], rdep[y], ratt[y]);
	memcpy(out, rcol, sizeof rcol);
}

void r3d_slots_from_vramcnt(const uint8_t vramcnt[7], const uint32_t bank_off[7], uint32_t texslot[R3D_TEX_SLOTS],
			    uint32_t palslot[R3D_PAL_SLOTS])
{
	for (int i = 0; i < R3D_TEX_SLOTS; i++)
		texslot[i] = R3D_UNMAPPED;
	for (int i = 0; i < R3D_PAL_SLOTS; i++)
		palslot[i] = R3D_UNMAPPED;
	/* GBATEK, «DS Memory Control - VRAM»: A-D con MST 3 = imagen de textura en la ranura OFS; E con MST 3 = paletas
	 * 0-3 (64 KB); F y G con MST 3 = paleta (OFS.0 + OFS.1·4). Si dos bancos van a la misma ranura gana el de menor
	 * índice (como el 2D; melonDS los combina). */
	for (int b = 6; b >= 0; b--) {
		const uint8_t c = vramcnt[b];
		const unsigned mst = c & 7u, ofs = (c >> 3) & 3u;

		if (!(c & 0x80u) || mst != 3)
			continue;
		if (b < 4)
			texslot[ofs] = bank_off[b];
		else if (b == 4)
			for (unsigned k = 0; k < 4; k++)
				palslot[k] = bank_off[b] + 0x4000u * k;
		else
			palslot[(ofs & 1u) + ((ofs >> 1) & 1u) * 4u] = bank_off[b];
	}
}

uint64_t r3d_hash_image(const uint32_t *img)
{
	uint64_t h = 0xcbf29ce484222325ull;

	for (int i = 0; i < R3D_W * R3D_H; i++) {
		h ^= img[i];
		h *= 0x100000001b3ull;
	}
	return h;
}
