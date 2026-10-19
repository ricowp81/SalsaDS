/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.3: los cuatro bucles de banderas NZCV (incluido por
 * kernels.c solo en PowerPC64). Registros: r14..r22 = r0..r8 del guest,
 * r23..r25 estado de banderas, r26..r30 temporales, r31 = 1.
 * Ninguna instrucción con punto ni mfcr.
 */

#define LOAD_GUEST                                                               \
	"lwz 14,0(%0)\n\t lwz 15,4(%0)\n\t lwz 16,8(%0)\n\t lwz 17,12(%0)\n\t"   \
	"lwz 18,16(%0)\n\t lwz 19,20(%0)\n\t lwz 20,24(%0)\n\t lwz 21,28(%0)\n\t" \
	"lwz 22,32(%0)\n\t li 31,1\n\t"
#define STORE_GUEST                                                              \
	"stw 14,0(%0)\n\t stw 15,4(%0)\n\t stw 16,8(%0)\n\t stw 17,12(%0)\n\t"   \
	"stw 18,16(%0)\n\t stw 19,20(%0)\n\t stw 20,24(%0)\n\t stw 21,28(%0)\n\t" \
	"stw 22,32(%0)\n\t"

#define KFLAGS(name, body)                                                       \
	static void name(struct kguest *g)                                       \
	{                                                                        \
		__asm__ volatile(LOAD_GUEST "1:\n\t" body STORE_GUEST            \
				 : : "b"(g) : CLOBBERS);                         \
	}

/* --- KF_NONE: cota inferior ------------------------------------------------ */
KFLAGS(kf_none,
       "add 14,14,15\n\t"      /* ADDS r0,r0,r1 */
       "cmpw 0,14,16\n\t"      /* CMP r0,r2 */
       "blt 0,2f\n\t"          /* BLT L1 */
       "add 17,17,19\n\t"      /* ADDS r3,r3,r5 */
       "2:\n\t"
       "cmplw 0,17,22\n\t"     /* CMP r3,r8 */
       "bgt 0,3f\n\t"          /* BHI L2 */
       "add 20,20,21\n\t"      /* ADDS r6,r6,r7 */
       "3:\n\t"
       "subf 18,31,18\n\t"     /* SUBS r4,r4,#1 */
       "cmpwi 0,18,0\n\t"
       "bne 0,1b\n\t")

/* --- KF_EAGER (a): N, Z, C y V completas en r23 (como el CPSR) --------------
 *
 * En PowerPC64 el CA de addc/subfc es el acarreo del bit 63, no del 32: no
 * sirve para el C de ARM. Los registros guest se mantienen extendidos con
 * ceros a 64 bits (clrldi al escribir) y el acarreo de 32 bits es el bit 32
 * del resultado de 64 bits (en la resta es el préstamo, negado). */
#define E_TAIL_NZ                                                                \
	"rlwinm 23,26,0,0,0\n\t"   /* N */                                       \
	"cntlzw 27,26\n\t"                                                       \
	"rlwinm 27,27,25,1,1\n\t"  /* Z: 32 (bit 5) -> 0x40000000 */             \
	"or 23,23,27\n\t"
#define E_C_ADD "rldicl 28,26,32,63\n\t"
#define E_C_SUB "rldicl 28,26,32,63\n\t xori 28,28,1\n\t"
#define E_TAIL_C                                                                 \
	"rlwinm 28,28,29,2,2\n\t" /* C -> 0x20000000 */                         \
	"or 23,23,28\n\t"
#define E_TAIL_V                                                                 \
	"rlwinm 29,29,29,3,3\n\t"  /* V -> 0x10000000 */                         \
	"or 23,23,29\n\t"
#define ADDS_E(rd, rn, rm)                                                       \
	"add 26," #rn "," #rm "\n\t" E_TAIL_NZ E_C_ADD E_TAIL_C                  \
	"xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t andc 29,29,27\n\t" E_TAIL_V \
	"clrldi " #rd ",26,32\n\t"
#define SUBS_E(rd, rn, rm)                                                       \
	"subf 26," #rm "," #rn "\n\t" E_TAIL_NZ E_C_SUB E_TAIL_C                  \
	"xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t and 29,29,27\n\t" E_TAIL_V \
	"clrldi " #rd ",26,32\n\t"
#define CMP_E(rn, rm) SUBS_E(30, rn, rm)

KFLAGS(kf_eager,
       ADDS_E(14, 14, 15)
       CMP_E(14, 16)
       /* BLT: N != V */
       "rlwinm 27,23,3,0,0\n\t xor 27,27,23\n\t cmpwi 0,27,0\n\t blt 0,2f\n\t"
       ADDS_E(17, 17, 19)
       "2:\n\t"
       CMP_E(17, 22)
       /* BHI: C && !Z */
       "rlwinm 27,23,3,30,31\n\t cmpwi 0,27,1\n\t beq 0,3f\n\t"
       ADDS_E(20, 20, 21)
       "3:\n\t"
       SUBS_E(18, 18, 31)
       /* BNE: !Z */
       "rlwinm 27,23,2,31,31\n\t cmpwi 0,27,0\n\t beq 0,1b\n\t")

/* --- KF_LAZY (b): r23 = a, r24 = b' (b o ~b), r25 = resultado -------------- */
#define ADDS_L(rd, rn, rm) "add 25," #rn "," #rm "\n\t mr 23," #rn "\n\t mr 24," #rm "\n\t mr " #rd ",25\n\t"
#define SUBS_L(rd, rn, rm) "subf 25," #rm "," #rn "\n\t mr 23," #rn "\n\t nor 24," #rm "," #rm "\n\t mr " #rd ",25\n\t"
#define CMP_L(rn, rm) "subf 25," #rm "," #rn "\n\t mr 23," #rn "\n\t nor 24," #rm "," #rm "\n\t"

KFLAGS(kf_lazy,
       ADDS_L(14, 14, 15)
       CMP_L(14, 16)
       /* BLT: N != V, con V = ((a ^ r) & ~(a ^ b')) >> 31 */
       "xor 26,23,25\n\t xor 27,23,24\n\t andc 26,26,27\n\t xor 26,26,25\n\t"
       "cmpwi 0,26,0\n\t blt 0,2f\n\t"
       ADDS_L(17, 17, 19)
       "2:\n\t"
       CMP_L(17, 22)
       /* BHI: C && !Z, con C = ((a & b') | ((a | b') & ~r)) >> 31 */
       "and 26,23,24\n\t or 27,23,24\n\t andc 27,27,25\n\t or 26,26,27\n\t"
       "cmpwi 0,25,0\n\t beq 0,4f\n\t cmpwi 0,26,0\n\t blt 0,3f\n\t"
       "4:\n\t"
       ADDS_L(20, 20, 21)
       "3:\n\t"
       SUBS_L(18, 18, 31)
       /* BNE */
       "cmpwi 0,25,0\n\t bne 0,1b\n\t")

/* --- KF_CR (c): N/Z en cr0, C en cr1 (cmplw), V en cr2 ---------------------- */
#define ADDS_C(rd, rn, rm)                                                       \
	"add 26," #rn "," #rm "\n\t cmpwi 0,26,0\n\t cmplw 1,26," #rn "\n\t"     \
	"xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t andc 29,29,27\n\t cmpwi 2,29,0\n\t" \
	"mr " #rd ",26\n\t"
#define SUBS_C(rd, rn, rm)                                                       \
	"subf 26," #rm "," #rn "\n\t cmpwi 0,26,0\n\t cmplw 1," #rn "," #rm "\n\t" \
	"xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t and 29,29,27\n\t cmpwi 2,29,0\n\t" \
	"mr " #rd ",26\n\t"
#define CMP_C(rn, rm) SUBS_C(30, rn, rm)

KFLAGS(kf_cr,
       ADDS_C(14, 14, 15)
       CMP_C(14, 16)
       /* BLT: N != V (cr0.lt ^ cr2.lt -> cr3.lt) */
       "crxor 12,0,8\n\t blt 3,2f\n\t"
       ADDS_C(17, 17, 19)
       "2:\n\t"
       CMP_C(17, 22)
       /* BHI: r3 >u r8 */
       "bgt 1,3f\n\t"
       ADDS_C(20, 20, 21)
       "3:\n\t"
       SUBS_C(18, 18, 31)
       "bne 0,1b\n\t")

/* --- KF_DOT (d): EXPLORATORIA, formas con punto y acarreo (ver kernels.h) ------
 *
 * Los registros guest viven en la mitad alta de los de 64 bits (valor << 32,
 * mitad baja en cero): addc./subfc. dan N y Z en cr0 (signo y cero de los 64
 * bits = los del guest) y el acarreo del bit 63 en XER.CA = el C de ARM. V
 * con xor/and y cmpdi en cr2. El C solo se lee (li + addze) cuando un salto
 * lo necesita (BHI). */
#define SHL_GUEST                                                                \
	"sldi 14,14,32\n\t sldi 15,15,32\n\t sldi 16,16,32\n\t sldi 17,17,32\n\t sldi 18,18,32\n\t" \
	"sldi 19,19,32\n\t sldi 20,20,32\n\t sldi 21,21,32\n\t sldi 22,22,32\n\t sldi 31,31,32\n\t"
#define SHR_GUEST                                                                \
	"srdi 14,14,32\n\t srdi 15,15,32\n\t srdi 16,16,32\n\t srdi 17,17,32\n\t srdi 18,18,32\n\t" \
	"srdi 19,19,32\n\t srdi 20,20,32\n\t srdi 21,21,32\n\t srdi 22,22,32\n\t"
#define ADDS_DOT(rd, rn, rm)                                                     \
	"addc. 26," #rn "," #rm "\n\t xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t andc 29,29,27\n\t" \
	"cmpdi 2,29,0\n\t mr " #rd ",26\n\t"
#define SUBS_DOT(rd, rn, rm)                                                     \
	"subfc. 26," #rm "," #rn "\n\t xor 27," #rn "," #rm "\n\t xor 29," #rn ",26\n\t and 29,29,27\n\t" \
	"cmpdi 2,29,0\n\t mr " #rd ",26\n\t"
#define CMP_DOT(rn, rm) SUBS_DOT(30, rn, rm)

static void kf_dot(struct kguest *g)
{
	__asm__ volatile(LOAD_GUEST SHL_GUEST "1:\n\t"
			 ADDS_DOT(14, 14, 15)
			 CMP_DOT(14, 16)
			 "crxor 12,0,8\n\t blt 3,2f\n\t"
			 ADDS_DOT(17, 17, 19)
			 "2:\n\t"
			 CMP_DOT(17, 22)
			 /* BHI: C && !Z. El C sale de XER.CA solo si Z = 0 */
			 "beq 0,4f\n\t li 27,0\n\t addze 27,27\n\t cmpwi 1,27,0\n\t bne 1,3f\n\t"
			 "4:\n\t"
			 ADDS_DOT(20, 20, 21)
			 "3:\n\t"
			 SUBS_DOT(18, 18, 31)
			 "bne 0,1b\n\t" SHR_GUEST STORE_GUEST
			 : : "b"(g) : CLOBBERS);
}

const char *const kflags_name[KF_COUNT] = {
	"sin banderas (cota)", "(a) completas a cada instr.", "(b) perezosas", "(c) N/Z cr0, C cr1, V cr2",
	"(d) add./subfc. + XER.CA (explor.)",
};
const kflags_fn kflags_table[KF_COUNT] = { kf_none, kf_eager, kf_lazy, kf_cr, kf_dot };
