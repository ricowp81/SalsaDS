/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — subida de fragment programs a la memoria del RSX.
 *
 * cgcomp (PSL1GHT f649a08, fpparser.cpp: IsPCDisablingInstruction) marca las
 * instrucciones MUL, DP2/DP3/DP4, DIV y NRM3 con el bit 31 de SRC2, que
 * desactiva la corrección de perspectiva de la entrada que leen. Esa
 * entrada llega entonces dividida entre w: con w = 1 no se nota, pero un
 * vértice de la N64 con w ≈ 2000 deja el término casi en cero (RPCS3 lo
 * emula así: tcN * gl_FragCoord.w). Ver docs/rsx-notes.md.
 *
 * Como rsxkit siempre quiere interpolación con perspectiva, el bit se
 * limpia en cada instrucción al subir el programa.
 */
#include <string.h>

#include "internal.h"

/* Cada instrucción son 4 palabras de 32 bits con sus mitades de 16 bits
 * intercambiadas (como las espera el RSX). Palabra 0: bit 0 fin del
 * programa. Palabras 1–3 (SRC0–2): bits 0–1 tipo de registro (2 =
 * constante, cuyo valor viene en los 16 bytes siguientes). */
#define FP_END          0x00000001u
#define FP_SRC_TYPE(w)  ((w) & 3u)
#define FP_SRC_CONST    2u
#define FP_DISABLE_PC   0x80000000u

static inline u32 halves(u32 w)
{
	return (w << 16) | (w >> 16);
}

static void fix_perspective(u32 *ucode, u32 size)
{
	u32 n = size / 4;
	for (u32 i = 0; i + 4 <= n;) {
		u32 *insn = ucode + i;
		u32 w0 = halves(insn[0]);
		int has_const = 0;
		for (int s = 1; s < 4; s++)
			if (FP_SRC_TYPE(halves(insn[s])) == FP_SRC_CONST)
				has_const = 1;
		insn[3] = halves(halves(insn[3]) & ~FP_DISABLE_PC);
		i += has_const ? 8 : 4;
		if (w0 & FP_END)
			break;
	}
}

/* Los programas subidos, con una suma de su copia en la memoria del RSX,
 * para ver si alguien la pisó (rsxkit_fp_verify). `src` es de donde se
 * restaura: el microcódigo del .fpo (con `fix`, se le limpia la corrección
 * de perspectiva al copiarlo) o la copia propia de un programa de
 * posprocesado, ya arreglada y con sus constantes. */
#define FP_TRACKED 64
static struct {
	const void *src;
	int fix;
	u32 *buf;
	u32 size, sum;
} tracked[FP_TRACKED];
static unsigned ntracked;

static u32 checksum(const volatile u32 *p, u32 size)
{
	u32 s = 0x811C9DC5u;
	for (u32 i = 0; i < size / 4; i++)
		s = (s ^ p[i]) * 16777619u;
	return s;
}

void rsxkit_fp_fix_perspective(void *ucode, u32 size)
{
	fix_perspective((u32 *)ucode, size);
}

void rsxkit_fp_track(void *buf, const void *src, u32 size, int fix)
{
	for (unsigned i = 0; i < ntracked; i++)
		if (tracked[i].buf == buf) {
			tracked[i].sum = checksum(buf, size);
			return;
		}
	if (ntracked >= FP_TRACKED)
		return;
	tracked[ntracked].src = src;
	tracked[ntracked].fix = fix;
	tracked[ntracked].buf = buf;
	tracked[ntracked].size = size;
	tracked[ntracked].sum = checksum(buf, size);
	ntracked++;
}

void rsxkit_fp_untrack(void *buf)
{
	for (unsigned i = 0; i < ntracked; i++)
		if (tracked[i].buf == buf) {
			tracked[i] = tracked[--ntracked];
			return;
		}
}

static void restore(unsigned i)
{
	memcpy(tracked[i].buf, tracked[i].src, tracked[i].size);
	if (tracked[i].fix)
		fix_perspective(tracked[i].buf, tracked[i].size);
}

void *rsxkit_fp_upload(rsxFragmentProgram *fp, u32 *offset)
{
	void *ucode;
	u32 size;
	rsxFragmentProgramGetUCode(fp, &ucode, &size);
	void *buf = rsxMemalign(64, size);
	if (!buf)
		return NULL;
	memcpy(buf, ucode, size);
	fix_perspective(buf, size);
	rsxAddressToOffset(buf, offset);
	rsxkit_fp_track(buf, ucode, size, 1);
	return buf;
}

int rsxkit_fp_verify(int repair)
{
	int bad = 0;
	for (unsigned i = 0; i < ntracked; i++) {
		if (checksum(tracked[i].buf, tracked[i].size) == tracked[i].sum)
			continue;
		bad++;
		if (repair)
			restore(i);
	}
	return bad;
}
