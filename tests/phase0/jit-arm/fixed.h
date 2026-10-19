/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Números con 2 decimales sin printf de flotantes (la libc de la PS3 no los trae confiables):
 * "%u.%02u" con F2(v), y «+a.b» con un signo aparte. */
#ifndef SALSADS_JIT_ARM_FIXED_H
#define SALSADS_JIT_ARM_FIXED_H

static inline unsigned fx_cent(double v)
{
	if (v < 0)
		v = -v;
	return (unsigned)(v * 100.0 + 0.5);
}
#define F2(v) (fx_cent(v) / 100), (fx_cent(v) % 100)
#define SGN(v) ((v) < 0 ? '-' : '+')

#endif
