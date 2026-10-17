/*
 * SalsaDS — prueba mínima del JIT ARM → PPC64 (Fase 0.7).
 *
 * Un subconjunto de ARMv5 suficiente para bucles reales (datos con
 * inmediato o registro con desplazamiento inmediato, S en suma, resta y
 * comparación, ADC, LDR/STR/LDRB/STRB con desplazamiento inmediato, B<cond>,
 * SWI) con tres cosas:
 *
 *   ja_ref_run      intérprete de referencia, con las banderas completas
 *   ja_loops        programas de prueba (src/jit/loops.c)
 *   ja_translate    traductor a PowerPC64, con tres formas de llevar las
 *                   banderas NZCV (ja_flagmode)
 *
 * Memoria del guest: JA_MEM_SIZE bytes little-endian, plana (las direcciones
 * se enmascaran: la tabla de páginas de punteros de la Fase 1 suma su costo
 * encima y no se midió). Los registros r0–r12 del guest viven en r14–r26 del
 * PPE mientras corre el código traducido.
 */
#ifndef SALSADS_JIT_JIT_ARM_H
#define SALSADS_JIT_JIT_ARM_H

#include <stddef.h>
#include <stdint.h>

#define JA_MEM_SIZE 0x4000u
#define JA_DATA 0x1000u
#define JA_DATA_WORDS 1024u
#define JA_OUT 0x2000u

/* Estado del guest; el código traducido lo lee y lo escribe por desplazamientos fijos. */
struct ja_state {
	uint32_t r[16];     /* 0 */
	uint32_t flags;     /* 64: N Z C V en los bits 31..28 (solo el intérprete de referencia) */
	uint32_t pc;        /* 68: al salir por presupuesto, el inicio del bloque que no cupo */
	uint32_t ret;       /* 72: JA_EXIT_* */
	uint32_t pad;       /* 76 */
	int64_t budget;     /* 80: instrucciones guest que quedan antes del próximo evento */
	uint8_t *mem;       /* 88 */
	uint64_t taken;     /* 96: saltos tomados (solo el intérprete de referencia) */
};
enum { JA_EXIT_SWI = 1, JA_EXIT_BUDGET = 2, JA_EXIT_BAD = 3 };

/* Intérprete de referencia: corre hasta el SWI o hasta agotar `s->budget`. */
void ja_ref_run(struct ja_state *s, const uint32_t *prog, uint32_t nwords);

/* ---- Programas de prueba ----------------------------------------------- */
struct ja_loop {
	const char *name;
	const char *what;
	const uint32_t *code;
	uint32_t nwords;
	int in_interp03;  /* es el programa del intérprete de la Fase 0.3 */
};
extern const struct ja_loop ja_loops[];
extern const unsigned ja_nloops;
/* Arma las palabras de los programas (una vez); llamar antes de usar ja_loops[].code. */
void ja_loops_init(void);
/* Datos de entrada, salida a cero y r6 = reps. */
void ja_loop_setup(struct ja_state *s, uint8_t *mem, uint32_t reps);
/* Resultado esperado calculado en C: r0, r4, r7 y la suma de verificación de
 * la zona de salida (JA_OUT .. JA_OUT + 0x1000) tras `reps` pasadas. */
struct ja_expect {
	uint32_t r0, r4, r7;
	uint32_t out_sum;
};
void ja_loop_expect(unsigned loop, uint32_t reps, struct ja_expect *e);
uint32_t ja_out_sum(const uint8_t *mem);

/* ---- Traductor -------------------------------------------------------------- */
enum ja_flagmode {
	JA_FLAGS_LAZY,   /* guardar operandos y resultado; calcular al consumir (0.3, (b)) */
	JA_FLAGS_CR,     /* comparaciones al campo del CR en cada instrucción que fija banderas (0.3, (c)) */
	JA_FLAGS_FUSED,  /* cmp + salto fusionados si las banderas mueren ahí; si no, perezosas */
	JA_FLAGS_COUNT,
};
extern const char *const ja_flagmode_name[JA_FLAGS_COUNT];

enum { JA_E_UNSUPPORTED = -1, JA_E_NOSPACE = -2, JA_E_FLAGS = -3 };
/* Escribe el código PowerPC64 de la función `int f(struct ja_state *)` en
 * `code` (hasta `cap` palabras). Devuelve 0 o JA_E_*, y deja en *nwords lo
 * escrito. La función devuelve en s->ret JA_EXIT_*. */
int ja_translate(const uint32_t *prog, uint32_t nwords, enum ja_flagmode mode, uint32_t *code, size_t cap,
		 size_t *nwords_out);

/* Estadísticas de la última traducción (para el log). */
struct ja_tr_stats {
	uint32_t guest_instr, host_instr, blocks, fused;
};
extern struct ja_tr_stats ja_last_tr;

/* Llama a código traducido en `code` (descriptor ELFv1 propio). */
int ja_call(const uint32_t *code, struct ja_state *s);

#endif
