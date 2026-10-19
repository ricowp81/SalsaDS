/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — codificación de instrucciones PowerPC (Power ISA 2.02, la del
 * PPE) para el JIT de la Fase 0.7. Nada de formas con punto, `mfcr`, `mtcrf`
 * ni desplazamientos por registro (microcódigo en el PPE; docs/fase0.md
 * §0.3), ni `isel` (el PPE no lo tiene).
 */
#ifndef SALSADS_JIT_PPCENC_H
#define SALSADS_JIT_PPCENC_H

#include <stdint.h>

#define PPC_BO_TRUE  12u
#define PPC_BO_FALSE 4u
#define PPC_LT 0u
#define PPC_GT 1u
#define PPC_EQ 2u
#define PPC_BI(crf, bit) ((crf) * 4u + (bit))

static inline uint32_t ppc_d(unsigned op, unsigned rt, unsigned ra, int32_t imm)
{
	return op << 26 | rt << 21 | ra << 16 | ((uint32_t)imm & 0xFFFFu);
}
static inline uint32_t ppc_addi(unsigned rt, unsigned ra, int32_t imm) { return ppc_d(14, rt, ra, imm); }
static inline uint32_t ppc_addis(unsigned rt, unsigned ra, int32_t imm) { return ppc_d(15, rt, ra, imm); }
static inline uint32_t ppc_li(unsigned rt, int32_t imm) { return ppc_addi(rt, 0, imm); }
static inline uint32_t ppc_lis(unsigned rt, int32_t imm) { return ppc_addis(rt, 0, imm); }
static inline uint32_t ppc_ori(unsigned ra, unsigned rs, uint32_t imm) { return ppc_d(24, rs, ra, (int32_t)imm); }
static inline uint32_t ppc_xori(unsigned ra, unsigned rs, uint32_t imm) { return ppc_d(26, rs, ra, (int32_t)imm); }
static inline uint32_t ppc_lwz(unsigned rt, unsigned ra, int32_t d) { return ppc_d(32, rt, ra, d); }
static inline uint32_t ppc_stw(unsigned rs, unsigned ra, int32_t d) { return ppc_d(36, rs, ra, d); }
/* ld y std (formas DS): desplazamiento múltiplo de 4. */
static inline uint32_t ppc_ld(unsigned rt, unsigned ra, int32_t d) { return 58u << 26 | rt << 21 | ra << 16 | ((uint32_t)d & 0xFFFCu); }
static inline uint32_t ppc_std(unsigned rs, unsigned ra, int32_t d) { return 62u << 26 | rs << 21 | ra << 16 | ((uint32_t)d & 0xFFFCu); }
static inline uint32_t ppc_stdu(unsigned rs, unsigned ra, int32_t d) { return 62u << 26 | rs << 21 | ra << 16 | ((uint32_t)d & 0xFFFCu) | 1u; }

static inline uint32_t ppc_x(unsigned op, unsigned xo, unsigned a, unsigned b, unsigned c)
{
	return op << 26 | a << 21 | b << 16 | c << 11 | xo << 1;
}
/* add rt,ra,rb ; subf rt,ra,rb = rb - ra */
static inline uint32_t ppc_add(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 266, rt, ra, rb); }
static inline uint32_t ppc_subf(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 40, rt, ra, rb); }
static inline uint32_t ppc_and(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 28, rs, ra, rb); }
static inline uint32_t ppc_or(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 444, rs, ra, rb); }
static inline uint32_t ppc_xor(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 316, rs, ra, rb); }
static inline uint32_t ppc_mr(unsigned ra, unsigned rs) { return ppc_or(ra, rs, rs); }
static inline uint32_t ppc_extsw(unsigned ra, unsigned rs) { return ppc_x(31, 986, rs, ra, 0); }
static inline uint32_t ppc_nop(void) { return ppc_ori(0, 0, 0); }

/* Cargas y almacenamientos indexados, con inversión de bytes. */
static inline uint32_t ppc_lwbrx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 534, rt, ra, rb); }
static inline uint32_t ppc_stwbrx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(31, 662, rs, ra, rb); }
static inline uint32_t ppc_lbzx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 87, rt, ra, rb); }
static inline uint32_t ppc_stbx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(31, 215, rs, ra, rb); }

/* Comparaciones en el campo crf: de 32 bits (L = 0). */
static inline uint32_t ppc_cmpw(unsigned crf, unsigned ra, unsigned rb) { return 31u << 26 | crf << 23 | ra << 16 | rb << 11; }
static inline uint32_t ppc_cmplw(unsigned crf, unsigned ra, unsigned rb) { return 31u << 26 | crf << 23 | ra << 16 | rb << 11 | 32u << 1; }
static inline uint32_t ppc_cmpdi(unsigned crf, unsigned ra, int32_t imm) { return 11u << 26 | crf << 23 | 1u << 21 | ra << 16 | ((uint32_t)imm & 0xFFFFu); }
static inline uint32_t ppc_cmpwi(unsigned crf, unsigned ra, int32_t imm) { return 11u << 26 | crf << 23 | ra << 16 | ((uint32_t)imm & 0xFFFFu); }
static inline uint32_t ppc_cmplwi(unsigned crf, unsigned ra, uint32_t imm) { return 10u << 26 | crf << 23 | ra << 16 | (imm & 0xFFFFu); }

/* rlwinm ra,rs,sh,mb,me (sin forma con punto). */
static inline uint32_t ppc_rlwinm(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
{
	return 21u << 26 | rs << 21 | ra << 16 | sh << 11 | mb << 6 | me << 1;
}
/* rldicl ra,rs,sh,mb; clrldi ra,rs,n = rldicl ra,rs,0,n. */
static inline uint32_t ppc_rldicl(unsigned ra, unsigned rs, unsigned sh, unsigned mb)
{
	return 30u << 26 | rs << 21 | ra << 16 | (sh & 31u) << 11 | (mb & 31u) << 6 | ((mb >> 5) & 1u) << 5 | ((sh >> 5) & 1u) << 1;
}
static inline uint32_t ppc_clrldi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicl(ra, rs, 0, n); }
/* sradi ra,rs,sh (sin forma con punto): XS-form, opcode 31, xo 413. */
static inline uint32_t ppc_sradi(unsigned ra, unsigned rs, unsigned sh)
{
	return 31u << 26 | rs << 21 | ra << 16 | (sh & 31u) << 11 | 413u << 2 | ((sh >> 5) & 1u) << 1;
}

/* Saltos: `off` en bytes respecto de la propia instrucción. */
static inline uint32_t ppc_b(int32_t off) { return 18u << 26 | ((uint32_t)off & 0x03FFFFFCu); }
static inline uint32_t ppc_bc(unsigned bo, unsigned bi, int32_t off) { return 16u << 26 | bo << 21 | bi << 16 | ((uint32_t)off & 0xFFFCu); }
#define PPC_BLR 0x4E800020u
static inline uint32_t ppc_mflr(unsigned rt) { return 31u << 26 | rt << 21 | 256u << 11 | 339u << 1; }
static inline uint32_t ppc_mtlr(unsigned rs) { return 31u << 26 | rs << 21 | 256u << 11 | 467u << 1; }

/* ---- Lo que agrega el recompilador de la Fase 4 (src/jit/arec*) ------------------------------------------ */

static inline uint32_t ppc_oris(unsigned ra, unsigned rs, uint32_t imm) { return ppc_d(25, rs, ra, (int32_t)imm); }
static inline uint32_t ppc_xoris(unsigned ra, unsigned rs, uint32_t imm) { return ppc_d(27, rs, ra, (int32_t)imm); }
static inline uint32_t ppc_mulli(unsigned rt, unsigned ra, int32_t imm) { return ppc_d(7, rt, ra, imm); }
static inline uint32_t ppc_subfic(unsigned rt, unsigned ra, int32_t imm) { return ppc_d(8, rt, ra, imm); }
static inline uint32_t ppc_lbz(unsigned rt, unsigned ra, int32_t d) { return ppc_d(34, rt, ra, d); }
static inline uint32_t ppc_lhz(unsigned rt, unsigned ra, int32_t d) { return ppc_d(40, rt, ra, d); }
static inline uint32_t ppc_stb(unsigned rs, unsigned ra, int32_t d) { return ppc_d(38, rs, ra, d); }
static inline uint32_t ppc_sth(unsigned rs, unsigned ra, int32_t d) { return ppc_d(44, rs, ra, d); }
static inline uint32_t ppc_cmpldi(unsigned crf, unsigned ra, uint32_t imm) { return 10u << 26 | crf << 23 | 1u << 21 | ra << 16 | (imm & 0xFFFFu); }
static inline uint32_t ppc_cmpd(unsigned crf, unsigned ra, unsigned rb) { return 31u << 26 | crf << 23 | 1u << 21 | ra << 16 | rb << 11; }
static inline uint32_t ppc_cmpld(unsigned crf, unsigned ra, unsigned rb) { return 31u << 26 | crf << 23 | 1u << 21 | ra << 16 | rb << 11 | 32u << 1; }

/* XO-form aritméticas (sin OE ni Rc): rt, ra, rb */
static inline uint32_t ppc_xo(unsigned xo, unsigned rt, unsigned ra, unsigned rb) { return 31u << 26 | rt << 21 | ra << 16 | rb << 11 | xo << 1; }
static inline uint32_t ppc_neg(unsigned rt, unsigned ra) { return ppc_xo(104, rt, ra, 0); }
static inline uint32_t ppc_mullw(unsigned rt, unsigned ra, unsigned rb) { return ppc_xo(235, rt, ra, rb); }
static inline uint32_t ppc_mulld(unsigned rt, unsigned ra, unsigned rb) { return ppc_xo(233, rt, ra, rb); }
static inline uint32_t ppc_mulhwu(unsigned rt, unsigned ra, unsigned rb) { return ppc_xo(11, rt, ra, rb); }
static inline uint32_t ppc_mulhw(unsigned rt, unsigned ra, unsigned rb) { return ppc_xo(75, rt, ra, rb); }

/* X-form lógicas: ra, rs, rb */
static inline uint32_t ppc_andc(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 60, rs, ra, rb); }
static inline uint32_t ppc_orc(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 412, rs, ra, rb); }
static inline uint32_t ppc_nor(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 124, rs, ra, rb); }
static inline uint32_t ppc_eqv(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 284, rs, ra, rb); }
static inline uint32_t ppc_slw(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 24, rs, ra, rb); }
static inline uint32_t ppc_srw(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 536, rs, ra, rb); }
static inline uint32_t ppc_sraw(unsigned ra, unsigned rs, unsigned rb) { return ppc_x(31, 792, rs, ra, rb); }
static inline uint32_t ppc_srawi(unsigned ra, unsigned rs, unsigned sh) { return ppc_x(31, 824, rs, ra, sh & 31u); }
static inline uint32_t ppc_cntlzw(unsigned ra, unsigned rs) { return ppc_x(31, 26, rs, ra, 0); }
static inline uint32_t ppc_extsb(unsigned ra, unsigned rs) { return ppc_x(31, 954, rs, ra, 0); }
static inline uint32_t ppc_extsh(unsigned ra, unsigned rs) { return ppc_x(31, 922, rs, ra, 0); }

/* Cargas y almacenamientos indexados */
static inline uint32_t ppc_lhbrx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 790, rt, ra, rb); }
static inline uint32_t ppc_sthbrx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(31, 918, rs, ra, rb); }
static inline uint32_t ppc_ldx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 21, rt, ra, rb); }
static inline uint32_t ppc_lwzx(unsigned rt, unsigned ra, unsigned rb) { return ppc_x(31, 23, rt, ra, rb); }
static inline uint32_t ppc_stwx(unsigned rs, unsigned ra, unsigned rb) { return ppc_x(31, 151, rs, ra, rb); }

/* rlwimi ra,rs,sh,mb,me y rlwnm ra,rs,rb,mb,me (sin punto) */
static inline uint32_t ppc_rlwimi(unsigned ra, unsigned rs, unsigned sh, unsigned mb, unsigned me)
{
	return 20u << 26 | rs << 21 | ra << 16 | (sh & 31u) << 11 | mb << 6 | me << 1;
}
static inline uint32_t ppc_rlwnm(unsigned ra, unsigned rs, unsigned rb, unsigned mb, unsigned me)
{
	return 23u << 26 | rs << 21 | ra << 16 | rb << 11 | mb << 6 | me << 1;
}
/* rldicr ra,rs,sh,me: sldi ra,rs,n = rldicr ra,rs,n,63-n */
static inline uint32_t ppc_rldicr(unsigned ra, unsigned rs, unsigned sh, unsigned me)
{
	return 30u << 26 | rs << 21 | ra << 16 | (sh & 31u) << 11 | (me & 31u) << 6 | ((me >> 5) & 1u) << 5 | 1u << 2 | ((sh >> 5) & 1u) << 1;
}
/* rldimi ra,rs,sh,mb */
static inline uint32_t ppc_rldimi(unsigned ra, unsigned rs, unsigned sh, unsigned mb)
{
	return 30u << 26 | rs << 21 | ra << 16 | (sh & 31u) << 11 | (mb & 31u) << 6 | ((mb >> 5) & 1u) << 5 | 3u << 2 | ((sh >> 5) & 1u) << 1;
}
static inline uint32_t ppc_sldi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicr(ra, rs, n, 63 - n); }
static inline uint32_t ppc_srdi(unsigned ra, unsigned rs, unsigned n) { return ppc_rldicl(ra, rs, 64 - n, n); }

/* Saltos con enlace y por CTR */
static inline uint32_t ppc_bl(int32_t off) { return ppc_b(off) | 1u; }
#define PPC_BCTR 0x4E800420u
#define PPC_BCTRL 0x4E800421u
static inline uint32_t ppc_mtctr(unsigned rs) { return 31u << 26 | rs << 21 | 288u << 11 | 467u << 1; }

/* Registro de condición. mfcr es microcódigo en el PPE (~34 ciclos): solo en caminos fríos. mtocrf de un campo
 * es gratis (docs/fase0.md §0.3). */
static inline uint32_t ppc_mfcr(unsigned rt) { return 31u << 26 | rt << 21 | 19u << 1; }
static inline uint32_t ppc_mtocrf(unsigned crf, unsigned rs) { return 31u << 26 | rs << 21 | 1u << 20 | (0x80u >> crf) << 12 | 144u << 1; }
/* mtcrf con varios campos (microcódigo): solo en la entrada y la salida del código traducido. */
static inline uint32_t ppc_mtcrf(unsigned fxm, unsigned rs) { return 31u << 26 | rs << 21 | (fxm & 0xFFu) << 12 | 144u << 1; }
static inline uint32_t ppc_crop(unsigned xo, unsigned bt, unsigned ba, unsigned bb) { return 19u << 26 | bt << 21 | ba << 16 | bb << 11 | xo << 1; }
static inline uint32_t ppc_crand(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(257, bt, ba, bb); }
static inline uint32_t ppc_cror(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(449, bt, ba, bb); }
static inline uint32_t ppc_crxor(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(193, bt, ba, bb); }
static inline uint32_t ppc_crnor(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(33, bt, ba, bb); }
static inline uint32_t ppc_creqv(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(289, bt, ba, bb); }
static inline uint32_t ppc_crandc(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(129, bt, ba, bb); }
static inline uint32_t ppc_crorc(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(417, bt, ba, bb); }
static inline uint32_t ppc_crnand(unsigned bt, unsigned ba, unsigned bb) { return ppc_crop(225, bt, ba, bb); }
#define PPC_SO 3u

#endif
