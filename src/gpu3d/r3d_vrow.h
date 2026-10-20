/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — línea del render 3D en vectores de 4 píxeles (Fase 3.4). Es la misma regla de r3d_px.h (`r3d_draw_row`, que
 * sigue siendo la referencia y la que usa r3d_ref.c), escrita para el SIMD del SPE: cuatro píxeles por vuelta, sin
 * saltos por píxel (todo con máscaras de carril), sin divisiones por píxel y con multiplicaciones de 16 bits donde
 * caben. Da los mismos bits que la escalar: se comprueba en las pruebas unitarias (todas las escenas, LE y BE), en
 * `--vs-ref3d` con los juegos y en la autoprueba de la PS3 (sondas contra el PPE).
 *
 * Portable: vectores genéricos de GCC/clang (`vector_size(16)`); solo las multiplicaciones de 16 bits, las
 * conversiones de coma flotante y la recíproca llevan intrínsecos del SPU. Los resultados no dependen del redondeo
 * de la coma flotante: toda estimación va con una corrección entera (como r3d_pfac).
 *
 * Qué se hace igual que en la escalar y qué cambia:
 *   - las interpolaciones lineales exactas `a + piso((b - a)·k / L)` se evalúan por píxel como `a + q·k + piso(r·k / L)`
 *     (q y r por línea: cociente y resto de b - a entre L), en lugar de con el DDA incremental;
 *   - el factor en perspectiva es r3d_pfac en vectores (cociente en coma flotante y corrección entera);
 *   - los saltos `continue` de la escalar son máscaras de carril; las escrituras son selecciones.
 * La línea entra en grupos de 4 píxeles alineados (los búferes de línea están alineados a 16), con los carriles
 * fuera de [x0, x1) apagados.
 */
#ifndef SALSADS_GPU3D_R3D_VROW_H
#define SALSADS_GPU3D_R3D_VROW_H

#include <stdint.h>

#include "gpu3d/r3d_core.h"
#include "gpu3d/r3d_px.h"

#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

typedef int32_t v4i __attribute__((vector_size(16)));
typedef uint32_t v4u __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));
typedef v4u v4u_ma __attribute__((may_alias));

/* Caminos lentos de la textura (r3d_core.c): algún carril fuera de la celda (el bloque) del texel anterior */
struct r3d_ctx;
struct r3d_tex;
static v4u vr_tex_cell_slow(struct r3d_ctx *c, const struct r3d_tex *tx, v4u sv, v4u tv, v4u M);
static v4u vr_tex_block_slow(struct r3d_ctx *c, const struct r3d_tex *tx, v4u id, v4u ix, v4u M);

R3D_INLINE v4u vs(uint32_t x)
{
	return (v4u){ x, x, x, x };
}

/* máscara (todo unos) -> b; si no a */
R3D_INLINE v4u vsel(v4u a, v4u b, v4u m)
{
	return (a & ~m) | (b & m);
}

R3D_INLINE v4u vgts(v4u a, v4u b) /* a > b con signo */
{
	return (v4u)((v4i)a > (v4i)b);
}

R3D_INLINE v4u vgtu(v4u a, v4u b) /* a > b sin signo */
{
	return (v4u)(a > b);
}

R3D_INLINE v4u veq(v4u a, v4u b)
{
	return (v4u)(a == b);
}

/* ¿algún carril encendido? */
R3D_INLINE int vany(v4u m)
{
#ifdef __SPU__
	return spu_extract(spu_gather(m), 0) != 0;
#else
	return (m[0] | m[1] | m[2] | m[3]) != 0;
#endif
}

#ifdef __SPU__
R3D_INLINE v4u vmulu16(v4u a, v4u b) /* (a & 0xFFFF) · (b & 0xFFFF) */
{
	return (v4u)spu_mulo((vec_ushort8)a, (vec_ushort8)b);
}
R3D_INLINE v4u vmuls16(v4u a, v4u b) /* a16 · b16 con signo */
{
	return (v4u)spu_mulo((vec_short8)a, (vec_short8)b);
}
R3D_INLINE v4f vitof(v4u a)
{
	return spu_convtf((vec_int4)a, 0);
}
R3D_INLINE v4u vftoi(v4f a) /* trunca hacia cero */
{
	return (v4u)spu_convts(a, 0);
}
R3D_INLINE v4f vrcp(v4f x)
{
	const vec_float4 r = spu_re(x);

	return spu_madd(spu_nmsub(r, x, spu_splats(1.0f)), r, r);
}
#else
R3D_INLINE v4u vmulu16(v4u a, v4u b)
{
	return (a & vs(0xFFFFu)) * (b & vs(0xFFFFu));
}
R3D_INLINE v4u vmuls16(v4u a, v4u b)
{
	const v4i x = (v4i)(a << 16) >> 16, y = (v4i)(b << 16) >> 16;

	return (v4u)(x * y);
}
R3D_INLINE v4f vitof(v4u a)
{
	return __builtin_convertvector((v4i)a, v4f);
}
R3D_INLINE v4u vftoi(v4f a)
{
	return (v4u)__builtin_convertvector(a, v4i);
}
R3D_INLINE v4f vrcp(v4f x)
{
	return (v4f){ 1.0f, 1.0f, 1.0f, 1.0f } / x;
}
#endif

R3D_INLINE v4f vsf(float x)
{
	return (v4f){ x, x, x, x };
}

/* ---- aritmética exacta ------------------------------------------------------------------------------------------ */

/* Cociente (piso) y resto de n entre L (0 < L < 2^15, |n| < 2^22) con una estimación en coma flotante y una corrección
 * entera: no depende del redondeo de la coma flotante. */
R3D_INLINE void vr_fdiv(int32_t n, uint32_t L, float rcp, int32_t *q, uint32_t *r)
{
	int32_t qq = (int32_t)((float)n * rcp);
	int32_t rem = n - qq * (int32_t)L;

	if (rem < 0) {
		qq--;
		rem += (int32_t)L;
	} else if (rem >= (int32_t)L) {
		qq++;
		rem -= (int32_t)L;
	}
	*q = qq;
	*r = (uint32_t)rem;
}

/* piso(n / L) por carril, con 0 <= n < 2^17 y 0 < L < 2^15 (los cocientes caben en 16 bits) */
R3D_INLINE v4u vr_ffloor(v4u n, v4f rcpv, v4u Lv)
{
	v4u q = vftoi(vitof(n) * rcpv);
	const v4u rem = n - vmulu16(q, Lv);
	const v4u neg = vgts(vs(0), rem);
	const v4u big = ~neg & ~vgts(Lv, rem);

	return q + neg - big;
}

/* Factor de interpolación con corrección por w (r3d_pfac en vectores): piso(k·w0·2^9 / ((L - k)·w1 + k·w0)). k < L;
 * los carriles apagados traen k = 0. w0, w1 < 2^16. */
R3D_INLINE v4u vr_pfac(v4u K, v4u Lv, v4u w0, v4u w1)
{
	const v4u a = vmulu16(K, w0);
	const v4u d = vmulu16(Lv - K, w1) + a;
	const v4u f = vftoi(vitof(a) * vsf((float)(1u << R3D_PS)) * vrcp(vitof(d)));
	const v4u r = (a << R3D_PS) - f * d; /* módulo 2^32; el valor real cabe en 32 bits con signo */
	const v4u neg = vgts(vs(0), r);
	const v4u big = ~neg & ~vgtu(d, r);

	return f + neg - big;
}

/* a + piso((b - a)·k / L) = a + q·k + piso(r·k / L) */
typedef struct vr_lin {
	v4u a, q, r;
} vr_lin;

R3D_INLINE vr_lin vr_lin_init(int32_t a, int32_t b, uint32_t L, float rcp)
{
	int32_t q;
	uint32_t r;

	vr_fdiv(b - a, L, rcp, &q, &r);
	return (vr_lin){ vs((uint32_t)a), vs((uint32_t)q), vs(r) };
}

R3D_INLINE v4u vr_lin16(const vr_lin *l, v4u K, v4f rcpv, v4u Lv) /* |q| < 2^15 (colores) */
{
	return l->a + vmuls16(l->q, K) + vr_ffloor(vmulu16(l->r, K), rcpv, Lv);
}

R3D_INLINE v4u vr_lin32(const vr_lin *l, v4u K, v4f rcpv, v4u Lv)
{
	return l->a + l->q * K + vr_ffloor(vmulu16(l->r, K), rcpv, Lv);
}

/* Coordenada de texel según repetir/espejo/recorte (r3d_wrap), por carril */
R3D_INLINE v4u vr_wrap(v4u c, uint32_t l, uint32_t rep, uint32_t flip)
{
	const uint32_t n = 1u << l;
	const v4u lo = c & vs(n - 1u);

	if (!rep) {
		const v4u neg = vgts(vs(0), c), big = vgts(c, vs(n - 1u));

		return vsel(vsel(c, vs(n - 1u), big), vs(0), neg);
	}
	if (flip) {
		const v4u odd = veq((v4u)((v4i)c >> l) & vs(1), vs(1));

		return vsel(lo, vs(n - 1u) - lo, odd);
	}
	return lo;
}

/* Prueba de profundidad por carril (r3d_depth_ok) */
R3D_INLINE v4u vr_dok(uint32_t dequal, uint32_t wbuf, uint32_t front, v4u d, v4u dst, v4u att)
{
	if (dequal) {
		const v4u mg = vs(wbuf ? 0xFFu : 0x200u);

		return ~vgtu(dst, d + mg) & ~vgtu(d, dst + mg); /* d + m >= dst && d <= dst + m */
	}
	if (front) /* un frontal gana el empate sobre una profundidad de espalda */
		return vgtu(dst, d) | (~vgtu(d, dst) & veq(att & vs(AT_BACK), vs(AT_BACK)));
	return vgtu(dst, d);
}

/* ---- una línea ---------------------------------------------------------------------------------------------------- */

/* Este archivo lo incluye solo r3d_core.c, después de definir el contexto y `tc_get` (el texel que no está en el bloque
 * anterior). */

R3D_INLINE v4u vr_mod6(v4u a, v4u b)
{
	return (vmulu16(a + vs(1), b + vs(1)) - vs(1)) >> 6;
}

/* Dibuja la línea del polígono p en col/dep/att (256 palabras alineadas a 16 cada uno). LIN: las w de la línea son
 * iguales (interpolación lineal exacta); TEX: el polígono lleva textura. `c` solo se usa con TEX. */
R3D_INLINE void r3d_vrow(r3d_ctx *c, const r3d_rstate *rs, r3d_stats *st, const r3d_poly *p, const r3d_row *row,
			 uint32_t *col, uint32_t *dep, uint32_t *att, const int LIN, const int TEX)
{
	const int32_t L = row->den, xl = row->xl, xo = row->xo;
	const uint32_t uL = L > 0 ? (uint32_t)L : 1u;
	const int32_t x0 = xl < 0 ? 0 : xl, x1 = row->xr > R3D_W ? R3D_W : row->xr;
	const uint32_t wl = row->l.w, wr = row->r.w;
	const uint32_t zl = row->l.z, zr = row->r.z;
	const uint32_t wbuf = rs->wbuf, d3 = rs->d3;
	const uint32_t mode = p->mode, shadow = mode == 3, wire = p->wire, pid = p->id;
	const uint32_t front = p->front, dequal = (p->attr & PA_DEQUAL) != 0, tdepth = (p->attr & PA_TDEPTH) != 0, pfog = (p->attr & PA_FOG) != 0;
	const uint32_t atest = (d3 & D3_ALPHATEST) != 0, blend = (d3 & D3_BLEND) != 0, highlight = (d3 & D3_HIGHLIGHT) != 0;
	const float rcp = 1.0f / (float)uL;
	const v4f rcpv = vsf(rcp);
	const v4u Lv = vs(uL), wlv = vs(wl), wrv = vs(wr), zlv = vs(zl);
	const v4i xl_v = (v4i){ xo, xo, xo, xo }, x0_v = (v4i){ x0, x0, x0, x0 }, x1_v = (v4i){ x1, x1, x1, x1 };
	const v4i xle_v = (v4i){ row->xle, row->xle, row->xle, row->xle }, xrs_v = (v4i){ row->xrs, row->xrs, row->xrs, row->xrs };
	const v4u alledge = row->alledge ? vs(~0u) : vs(0);
	uint32_t zq = 0, zr_ = 0, zneg = 0;
	vr_lin lr = { vs(0), vs(0), vs(0) }, lg = lr, lb = lr, ls = lr, lt = lr;
	v4u wacc = vs(0), tacc = vs(0), hacc = vs(0);
	int32_t gx;

	if (x0 >= x1)
		return;
	st->pixels += (uint32_t)(x1 - x0);
	if (!wbuf) {
		const uint32_t adz = zr >= zl ? zr - zl : zl - zr;

		zneg = zr < zl;
		zq = adz / uL;
		zr_ = adz - zq * uL;
	}
	if (LIN) {
		lr = vr_lin_init(row->l.r, row->r.r, uL, rcp);
		lg = vr_lin_init(row->l.g, row->r.g, uL, rcp);
		lb = vr_lin_init(row->l.b, row->r.b, uL, rcp);
		if (TEX) {
			ls = vr_lin_init(row->l.s, row->r.s, uL, rcp);
			lt = vr_lin_init(row->l.t, row->r.t, uL, rcp);
		}
	}
	for (gx = x0 & ~3; gx < x1; gx += 4) {
		const v4i X = (v4i){ gx, gx + 1, gx + 2, gx + 3 };
		const v4u inr = (v4u)((X >= x0_v) & (X < x1_v));
		const v4u edge = (v4u)((X < xle_v) | (X >= xrs_v)) | alledge;
		const v4u K = (v4u)(X - xl_v) & inr; /* k = x - xl en los carriles vivos, 0 en los apagados */
		v4u M = inr, f, d, dst, a0, att_old, att_new;
		v4u *pcol = (v4u *)(void *)(col + gx), *pdep = (v4u *)(void *)(dep + gx), *patt = (v4u *)(void *)(att + gx);

		if (wire)
			M &= edge;
		if (!vany(M))
			continue;
		/* factor f (0..2^9) */
		if (LIN)
			f = vr_ffloor(K << R3D_PS, rcpv, Lv);
		else
			f = vr_pfac(K, Lv, wlv, wrv);
		/* profundidad */
		if (wbuf) {
			const v4u w = (vmulu16(wlv, vs(1u << R3D_PS) - f) + vmulu16(wrv, f)) >> R3D_PS << p->wshift;

			d = vsel(w, vs(0xFFFFFFu), vgtu(w, vs(0xFFFFFFu)));
		} else {
			const v4u zu = vs(zq) * K + vr_ffloor(vmulu16(vs(zr_), K), rcpv, Lv);

			d = zneg ? zlv - zu : zlv + zu;
		}
		dst = *(v4u_ma *)pdep;
		att_old = a0 = *(v4u_ma *)patt;
		att_new = att_old;
		if (shadow) {
			if (pid == 0) { /* máscara de sombra: marca donde la prueba de profundidad FALLA */
				*(v4u_ma *)patt = vsel(att_old, att_old | vs(AT_STENCIL), M & ~vr_dok(dequal, wbuf, front, d, dst, att_old));
				continue;
			}
			{
				const v4u sten = veq(a0 & vs(AT_STENCIL), vs(AT_STENCIL)) & M;
				const v4u a0c = a0 & ~vs(AT_STENCIL);

				att_new = vsel(att_old, a0c, sten);
				a0 = a0c;
				M = sten & ~veq(a0 & vs(63u), vs(pid));
			}
		}
		/* prueba de profundidad */
		M &= vr_dok(dequal, wbuf, front, d, dst, a0);
		if (!vany(M)) {
			if (shadow)
				*(v4u_ma *)patt = att_new;
			continue;
		}
		/* color del vértice (6 bits) */
		{
			v4u vr, vg, vb, tex = vs(0), pxr, pxg, pxb, pxa, cc;

			if (LIN) {
				vr = vr_lin16(&lr, K, rcpv, Lv) >> 3;
				vg = vr_lin16(&lg, K, rcpv, Lv) >> 3;
				vb = vr_lin16(&lb, K, rcpv, Lv) >> 3;
			} else {
				const v4u fc = f >> (R3D_PS - R3D_PC), ifc = vs(1u << R3D_PC) - fc;

				vr = (v4u)((v4i)(vmuls16(vs((uint32_t)row->l.r), ifc) + vmuls16(vs((uint32_t)row->r.r), fc)) >> R3D_PC) >> 3;
				vg = (v4u)((v4i)(vmuls16(vs((uint32_t)row->l.g), ifc) + vmuls16(vs((uint32_t)row->r.g), fc)) >> R3D_PC) >> 3;
				vb = (v4u)((v4i)(vmuls16(vs((uint32_t)row->l.b), ifc) + vmuls16(vs((uint32_t)row->r.b), fc)) >> R3D_PC) >> 3;
			}
			if (TEX) {
				const r3d_tex *tx = &p->tex;
				v4u sv, tv, bx, by, id, ix;
				const uint32_t csh = tx->sl < 6 ? tx->sl : 6u, rsh = 8u - csh;
				if (LIN) {
					sv = (v4u)((v4i)vr_lin32(&ls, K, rcpv, Lv) >> 4);
					tv = (v4u)((v4i)vr_lin32(&lt, K, rcpv, Lv) >> 4);
				} else {
					const v4u nf = vs(1u << R3D_PS) - f;

					sv = (v4u)((v4i)(vmuls16(vs((uint32_t)row->l.s), nf) + vmuls16(vs((uint32_t)row->r.s), f)) >> (R3D_PS + 4));
					tv = (v4u)((v4i)(vmuls16(vs((uint32_t)row->l.t), nf) + vmuls16(vs((uint32_t)row->r.t), f)) >> (R3D_PS + 4));
				}
				sv = vr_wrap(sv, tx->sl, tx->rs, tx->fs);
				tv = vr_wrap(tv, tx->tl, tx->rt, tx->ft);
				tacc += M >> 31;
				if (c->cellmode) { /* celdas de 4×4 (3.9) */
					const v4u cid = ((tv >> 2) << 8) | (sv >> 2), cix = ((tv & vs(3u)) << 2) | (sv & vs(3u));

					if (!vany(M & ~veq(cid, vs(c->last_cid)))) {
						const uint32_t *lc = c->last_cell;

						tex = (v4u){ lc[cix[0]], lc[cix[1]], lc[cix[2]], lc[cix[3]] };
						hacc += M >> 31;
					} else {
						tex = vr_tex_cell_slow(c, tx, sv, tv, M);
					}
				} else {
					bx = sv >> csh;
					by = tv >> rsh;
					id = (by << 4) | bx;
					ix = ((tv & vs((1u << rsh) - 1u)) << csh) | (sv & vs((1u << csh) - 1u));
					if (!vany(M & ~veq(id, vs(c->last_id)))) {
						const uint32_t *lastblk = c->last_blk;

						tex = (v4u){ lastblk[ix[0] & 255u], lastblk[ix[1] & 255u], lastblk[ix[2] & 255u], lastblk[ix[3] & 255u] };
						hacc += M >> 31;
					} else {
						tex = vr_tex_block_slow(c, tx, id, ix, M);
					}
				}
			}
			/* modo del polígono */
			{
				const v4u ppa = vs(p->alpha);
				const v4u tr = tex & vs(63u), tg = (tex >> 8) & vs(63u), tb = (tex >> 16) & vs(63u), ta = (tex >> 24) & vs(31u);

				pxr = vr;
				pxg = vg;
				pxb = vb;
				pxa = ppa;
				if (mode == 1) { /* decal */
					if (TEX) {
						const v4u ita = vs(31u) - ta;
						const v4u br = (vmulu16(tr, ta) + vmulu16(vr, ita)) >> 5, bg = (vmulu16(tg, ta) + vmulu16(vg, ita)) >> 5,
							  bb = (vmulu16(tb, ta) + vmulu16(vb, ita)) >> 5;
						const v4u opaque = veq(ta, vs(31u)), clear = veq(ta, vs(0));

						pxr = vsel(vsel(br, tr, opaque), vr, clear);
						pxg = vsel(vsel(bg, tg, opaque), vg, clear);
						pxb = vsel(vsel(bb, tb, opaque), vb, clear);
					}
				} else if (mode == 2) { /* toon o highlight */
					const v4u ti = (vr >> 1) & vs(31u);
					const v4u tc = (v4u){ rs->toon[ti[0]], rs->toon[ti[1]], rs->toon[ti[2]], rs->toon[ti[3]] };
					const v4u tcr = tc & vs(63u), tcg = (tc >> 8) & vs(63u), tcb = (tc >> 16) & vs(63u);

					if (highlight) {
						pxr = pxg = pxb = vr;
					} else {
						pxr = tcr;
						pxg = tcg;
						pxb = tcb;
					}
					if (TEX) {
						pxr = vr_mod6(tr, pxr);
						pxg = vr_mod6(tg, pxg);
						pxb = vr_mod6(tb, pxb);
						pxa = (vmulu16(ta + vs(1), ppa + vs(1)) - vs(1)) >> 5;
					}
					if (highlight) {
						const v4u c63 = vs(63u);

						pxr += tcr;
						pxg += tcg;
						pxb += tcb;
						pxr = vsel(pxr, c63, vgtu(pxr, c63));
						pxg = vsel(pxg, c63, vgtu(pxg, c63));
						pxb = vsel(pxb, c63, vgtu(pxb, c63));
					}
				} else if (TEX) { /* modulate (y sombra) */
					pxr = vr_mod6(tr, vr);
					pxg = vr_mod6(tg, vg);
					pxb = vr_mod6(tb, vb);
					pxa = (vmulu16(ta + vs(1), ppa + vs(1)) - vs(1)) >> 5;
				}
			}
			cc = pxr | (pxg << 8) | (pxb << 16) | (pxa << 24);
			/* alfa 0: no se dibuja; alpha test */
			{
				v4u mw = M & ~veq(pxa, vs(0));
				v4u opq, trn, dc, cnew, anew, dnew;
				const v4u dcol = *(v4u_ma *)pcol;

				if (atest)
					mw &= vgtu(pxa, vs(rs->alpha_ref));
				if (shadow)
					opq = vs(0);
				else
					opq = mw & veq(pxa, vs(31u));
				trn = mw & ~opq;
				/* opacos: color, profundidad, id, niebla y borde */
				cnew = vsel(dcol, cc, opq);
				anew = vsel(att_new, (a0 & vs(AT_STENCIL)) | vs(pid | (pfog ? AT_FOG : 0u) | (front ? 0u : AT_BACK)) | (edge & vs(AT_EDGE)), opq);
				dnew = vsel(dst, d, opq);
				/* translúcidos: no se mezclan dos veces con el mismo id */
				if (vany(trn)) {
					v4u da, sa, ia, br, bg, bb, bl, blm, skip;

					skip = veq(a0 & vs(AT_TRANS), vs(AT_TRANS)) & veq((a0 >> 6) & vs(63u), vs(pid));
					trn &= ~skip;
					da = (dcol >> 24) & vs(31u);
					sa = pxa + vs(1);
					ia = vs(31u) - pxa;
					br = (vmulu16(pxr, sa) + vmulu16(dcol & vs(63u), ia)) >> 5;
					bg = (vmulu16(pxg, sa) + vmulu16((dcol >> 8) & vs(63u), ia)) >> 5;
					bb = (vmulu16(pxb, sa) + vmulu16((dcol >> 16) & vs(63u), ia)) >> 5;
					bl = br | (bg << 8) | (bb << 16) | (vsel(da, pxa, vgtu(pxa, da)) << 24);
					blm = blend ? trn & ~veq(da, vs(0)) : vs(0);
					dc = vsel(cc, bl, blm);
					cnew = vsel(cnew, dc, trn);
					if (tdepth)
						dnew = vsel(dnew, d, trn);
					anew = vsel(anew,
						    (a0 & ~vs(AT_FOG | (63u << 6) | (tdepth ? AT_BACK : 0u))) | vs(AT_TRANS | (pid << 6) | ((tdepth && !front) ? AT_BACK : 0u)) |
							    (veq(a0 & vs(AT_FOG), vs(AT_FOG)) & vs(pfog ? AT_FOG : 0u)),
						    trn);
				}
				wacc += (opq | trn) >> 31;
				*(v4u_ma *)pcol = cnew;
				*(v4u_ma *)pdep = dnew;
				*(v4u_ma *)patt = anew;
			}
		}
	}
	st->written += wacc[0] + wacc[1] + wacc[2] + wacc[3];
	if (TEX) {
		st->tex_px += tacc[0] + tacc[1] + tacc[2] + tacc[3];
		st->tex_hits += hacc[0] + hacc[1] + hacc[2] + hacc[3];
	}
}

#endif
