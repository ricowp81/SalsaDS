/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: autoprueba de r3d_vec.h.
 *
 * Las funciones con intrínsecos del SPU no se ejercitan en el host. El PPE
 * corre esta función con la versión genérica y el SPE con la suya, y
 * main.c compara: lo entero tiene que dar lo mismo; el recíproco, con error
 * relativo pequeño (se reporta).
 */
#include "r3d.h"
#include "r3d_vec.h"

void r3d_selftest(struct r3d_selftest *o)
{
	const vf4 x = { 1.0f, 3.0f, 0.7f, 123456.0f };
	*(vf4 *)o->recip = v_recip(x);
	const vu4 a = { 65535u, 3u, 1000u, 0x12345678u }, b = { 65535u, 7u, 2000u, 0x9ABCDEF0u };
	*(vu4 *)o->mul16 = v_mul16(a, b);
	*(vi4 *)o->mul32x16 = v_mul32x16((vi4){ -7, 0x12345678, 327680, -2000000000 }, (vi4){ 4, 300, 1300, 65535 });
	const vu4 w = { 0x11223344u, 0xA1B2C3D4u, 0x00FF00FFu, 0x80000001u };
	*(vu4 *)o->bytes = v_pack4(v_byte0(w), v_byte1(w), v_byte2(w), v_byte3(w)) ^
			   v_pack4(v_byte3(w), v_byte2(w), v_byte1(w), v_byte0(w));
	*(vu4 *)o->cmp = v_gt_u(a, b) | (v_gt_s((vi4)a, (vi4)b) << 1) | (v_gt_f(x, v_splatf(2.0f)) << 2) |
			 (v_selu(a, b, v_gt_u(b, a)) & 0xF0u);
	*(vi4 *)o->cvt = v_cvt_i((vf4){ 2.99f, -2.99f, 8191.5f, 540000.75f }) + (vi4)v_cvt_u((vf4){ 0.5f, 7.9f, 65535.9f, 1e6f });
	o->bits = v_bits16((vs8){ -1, 0, -1, 0, 0, 0, 0, -1 }) | v_bits16(v_gt_s16((vs8){ 5, -5, 0, 1, 2, 3, 4, 5 }, v_splats16(1))) << 8;
}
