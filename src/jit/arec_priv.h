/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — interno del recompilador (src/jit/arec*.c). No lo incluye nadie más.
 */
#ifndef SALSADS_JIT_AREC_PRIV_H
#define SALSADS_JIT_AREC_PRIV_H

#include <stddef.h>
#include <stdint.h>

#include "jit/arec.h"

/* ---- representación intermedia de una instrucción guest ---------------------------------------------- */

enum ai_kind {
	AI_DP,    /* proceso de datos (ARM y lo de Thumb que equivale) */
	AI_MUL,   /* MUL/MLA */
	AI_MULL,  /* UMULL/UMLAL/SMULL/SMLAL */
	AI_CLZ,
	AI_MEM,   /* LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB/LDRSH */
	AI_BLOCK, /* LDM/STM, PUSH/POP */
	AI_B,     /* salto directo: B, BL, BLX inmediato, Bcond y BL de Thumb (las dos mitades juntas) */
	AI_BX,    /* BX/BLX con registro */
	AI_BLLO,  /* Thumb: la segunda mitad de BL/BLX sola (destino = lr + desplazamiento) */
	AI_CALL,  /* la ejecuta el intérprete desde el bloque */
	AI_NOP,
};

enum { O2_IMM, O2_SHI, O2_SHR };

/* banderas, en el orden del CPSR */
#define F_N 8u
#define F_Z 4u
#define F_C 2u
#define F_V 1u
#define F_ALL 15u

#define AI_COND_AL 14u

struct ai {
	uint32_t addr;     /* dirección de la instrucción */
	uint32_t raw;      /* palabra ARM o media palabra Thumb (para la llamada al intérprete) */
	uint32_t imm;      /* AI_DP O2_IMM: el operando; AI_MEM: el desplazamiento inmediato */
	uint32_t pcv;      /* lo que vale r15 al leerlo como operando */
	uint32_t target;   /* AI_B: destino con el bit 0 = Thumb */
	uint32_t lrv;      /* AI_B/AI_BX/AI_BLLO con enlace: el valor de lr (con el bit 0 en Thumb) */
	uint16_t list;     /* AI_BLOCK */
	uint8_t kind, cond, thumb, size, ninsn; /* size: bytes que ocupa; ninsn: instrucciones guest (2 en el BL fusionado) */
	uint8_t cyc;       /* ciclos fijos (con el de captación) */
	uint8_t fr, fw;    /* banderas que lee / que escribe seguro (no las que deja como estaban) */
	uint8_t ends;      /* termina el bloque */
	uint8_t op, s, rd, rn, rm, rs; /* AI_DP: op ARM 0-15; AI_MUL: rd, rn = acumulador; AI_MULL: rd = hi, rn = lo */
	uint8_t o2, sht, sha;
	int8_t ic;         /* O2_IMM: acarreo del desplazador (-1 = no cambia) */
	uint8_t load, msz, msign, pre, up, wb; /* AI_MEM / AI_BLOCK */
	uint8_t acc, sgn, link, xchg;           /* AI_MUL/AI_MULL; AI_B/AI_BX/AI_BLLO; AI_DP con rd = 15: xchg = 0 */
	uint8_t call_ro;   /* AI_CALL que no lee ni escribe banderas (CP15, PLD) */
	uint8_t fast;      /* AI_CALL con camino rápido en línea (FK_*); el intérprete queda en el camino frío */
	uint8_t pad[2];
};

/* Llamadas al intérprete que se traducen en línea en el caso común (el resto por el camino frío, con los mismos
 * ciclos: `cyc` es el total que suma el intérprete). */
enum {
	FK_NONE = 0,
	FK_CP15NOP, /* MCR p15 c7 (caché, búfer de escritura) que no sea la espera de interrupción: no-op */
	FK_MRC,     /* MRC p15 de c1,c0,0 / c9,c1,0 / c9,c1,1 a un registro */
	FK_MRS,     /* MRS rd, CPSR / SPSR */
	FK_MSR,     /* MSR CPSR_c sin cambio de modo / MSR SPSR_* con banco */
	FK_SWP,     /* SWP / SWPB por la ruta rápida */
	FK_LDRD,    /* LDRD / STRD por la ruta rápida (las dos palabras en la misma página) */
};

#define AREC_MAX_INSN 64

/* Decodifica el bloque que empieza en `pc` (bit 0 = Thumb). Devuelve las instrucciones (al menos una si hay código)
 * y deja en *phys_lo/_hi el rango físico [lo, hi) que cubre (AREC_PHYS_NONE si es de solo lectura). */
struct arec_src {
	const uint8_t *(*code_ptr)(void *ctx, uint32_t a, uint32_t *phys, uint32_t *avail);
	void *ctx;
	int v5; /* 1: ARMv5TE (ARM9); 0: ARMv4T (ARM7: sin CLZ, QADD, BLX, LDRD, CP15; LDR/LDM a pc sin cambio de estado) */
};
unsigned arec_decode(const struct arec_src *src, uint32_t pc, struct ai *out, unsigned max, uint32_t *phys_lo,
		     uint32_t *phys_hi);
/* Decodifica una instrucción (para la vista adelante de banderas) y da su desplazamiento físico. 0 si no hay código. */
int arec_decode_one(const struct arec_src *src, uint32_t pc, struct ai *out, uint32_t *phys);
/* Banderas que lee la condición. */
uint8_t arec_cond_reads(unsigned cond);

/* ---- bloques -------------------------------------------------------------------------------------------- */

#define AREC_MAX_EXITS 16
#define AREC_NO_BLOCK 0xFFFFFFFFu

struct ablock_exit {
	uint32_t site;    /* palabra del salto que se parchea (índice en la caché) */
	uint32_t stub;    /* palabra a la que salta sin enlace */
	uint32_t target;  /* pc de destino (bit 0 = Thumb) */
};

struct ablock {
	uint32_t pc;        /* con el bit 0 = Thumb */
	uint32_t entry;     /* índice de palabra en la caché */
	uint32_t words;
	uint16_t ninsn, maxcyc;
	uint32_t hot;       /* palabras de la sección caliente */
	uint32_t hnext;     /* cadena de la tabla de bloques */
	uint32_t in_links;  /* lista de enlaces entrantes (índices en el arreglo de enlaces) */
	uint32_t phys_lo, phys_hi;
	uint32_t bnext[2];  /* listas por cubeta física de 1 KB (dos cubetas como mucho) */
	uint32_t bucket[2];
	uint8_t nexits, dead, nbuckets, pad;
	struct ablock_exit exits[AREC_MAX_EXITS];
};

struct alink {
	uint32_t site;      /* palabra parcheada */
	uint32_t stub;      /* a dónde volver al desenlazar */
	uint32_t next;
};

/* Entrada de la tabla de destinos indirectos que consulta el código generado. */
struct ajl {
	uint32_t key;       /* pc | Thumb; 1 = vacía (ningún pc ARM es impar y ningún Thumb vale 1) */
	uint32_t pad;
	uint64_t code;      /* dirección del bloque */
};
#define AREC_JL_SIZE 16384u

#define AREC_HASH 16384u
#define AREC_BUCKET_SHIFT 10u

struct arec {
	struct arec_pub pub; /* primero: arec_code_write lo lee */
	struct acore *c;
	struct arec_cfg cfg;
	int can_exec;

	uint32_t *code;      /* caché de código */
	size_t code_words, code_used, code_base_used; /* code_base_used: trampolín y rutinas comunes */
	uint32_t ent_enter, ent_exit;                  /* índices de las rutinas comunes */
	uint32_t ent_rd, ent_wr, ent_call;             /* caminos lentos compartidos (llamadas con bl desde los bloques) */
	uint64_t enter_opd[3];                         /* descriptor ELFv1 de la entrada */

	struct ablock *blocks;
	uint32_t nblocks, max_blocks;
	uint32_t hash[AREC_HASH];
	struct alink *links;
	uint32_t nlinks, max_links;
	struct ajl *jl;      /* AREC_JL_SIZE entradas, alineada */

	/* tablas de la ruta rápida: rd[AREC_PAGES] seguida de wr[AREC_PAGES] (el código suma 2 MB para escribir) */
	uint8_t **pt;
	uint32_t npages;     /* páginas de 16 KB que el dueño llega a mapear (las de wr_real, wr_phys y phys_pages) */
	uint8_t **wr_real;   /* lo que tendría wr sin el código */
	uint32_t *wr_phys;   /* página física de escritura (>> 14) por página, o AREC_PHYS_NONE */
	/* por página física de 16 KB: bloques que la tocan y las páginas que la escriben */
	uint32_t nphys16;
	uint32_t *phys_blocks;
	uint32_t *phys_first, *phys_cnt, *phys_pages;
	uint32_t *bucket_head; /* por cubeta física de 1 KB: primer bloque */
	uint32_t nbuckets;

	int exit_req;          /* se invalidó código: el bloque en curso sale en la próxima instrucción */
	unsigned last_live;    /* banderas exactas en la última salida (para --lockstep) */

	uint64_t (*interp_fn)(void);
};

/* arec_emit.c: traduce un bloque decodificado. Escribe en j->code a partir de j->code_used; devuelve las
 * palabras escritas o < 0 si no cupo. Llena b (salidas, ciclos). */
int arec_emit_block(struct arec *j, const struct ai *ins, unsigned n, struct ablock *b, uint32_t block_id);
/* Rutinas comunes (entrada, salida): al principio de la caché. */
int arec_emit_common(struct arec *j);

/* Ayudantes que llama el código generado (arec.c). Las direcciones van en c->jit_fn[]. */
enum {
	AH_RD = 0,    /* uint32 (c, addr, kind) */
	AH_WR,        /* uint32 (c, addr, value, kind) -> distinto de 0: salir */
	AH_CALL,      /* uint32 (c, insn, addr | thumb) -> distinto de 0: salir (estado en memoria) */
	AH_SHIFT,     /* uint64 (type, value, amount, cin) -> carry << 32 | value */
	AH_FLAGS,     /* void (c, cr, desc_lo, desc_hi): materializa las banderas vivas desde el CR */
	AH_COUNT
};
enum { MK_W = 0, MK_H, MK_SH, MK_B, MK_SB }; /* tipos de acceso de AH_RD/AH_WR (W de lectura rota) */

#endif
