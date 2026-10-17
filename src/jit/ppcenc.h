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

#endif
