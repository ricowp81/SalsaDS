/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.3: intérprete ARM mínimo para medir el despacho.
 *
 * No es el intérprete del núcleo: solo lo justo para correr un bucle real de
 * ARM (proceso de datos con desplazamiento, LDR/STR, CMP, B<cond>) con tres
 * formas de despachar la instrucción ya decodificada a su manejador:
 *
 *   DISPATCH_SWITCH  tabla de 4096 entradas (clave de 12 bits de la
 *                    instrucción) -> tipo -> `switch`
 *   DISPATCH_CALL    la misma tabla de tipos -> tabla de punteros a función
 *                    (en la ABI de la PS3 cada puntero es un descriptor)
 *   DISPATCH_GOTO    la misma tabla de tipos -> `goto *` a etiquetas, con la
 *                    búsqueda de la siguiente instrucción al final de cada
 *                    manejador (código enhebrado)
 *
 * Los tres comparten los cuerpos de los manejadores (macros) y el orden
 * little-endian de la memoria (rd32le/wr32le de src/util/endian.h), de modo
 * que solo cambia el despacho. Las banderas se mantienen completas (como el
 * CPSR) en C: es un costo común a los tres.
 */
#ifndef SALSADS_BENCH_INTERP_H
#define SALSADS_BENCH_INTERP_H

#include <stdint.h>

enum arm_dispatch { DISPATCH_SWITCH, DISPATCH_CALL, DISPATCH_GOTO, DISPATCH_COUNT };
extern const char *const arm_dispatch_name[DISPATCH_COUNT];

#define ARM_MEM_SIZE 0x4000u
#define ARM_ARRAY_WORDS 1024u

struct arm_cpu {
	uint32_t r[16];
	uint32_t flags; /* N Z C V en los bits 31..28 */
	uint32_t pc;
	uint32_t halt; /* 1 = SWI, 2 = instrucción no soportada */
	uint64_t icount;
	uint8_t *mem; /* ARM_MEM_SIZE bytes, little-endian */
};

/* Arma la tabla de decodificación (una vez). */
void arm_init(void);
/* Escribe el programa de prueba y el arreglo de datos en `mem` (little-endian)
 * y deja `c` listo para correr `reps` pasadas sobre el arreglo. */
void arm_load_program(struct arm_cpu *c, uint8_t *mem, uint32_t reps);
/* Corre hasta el SWI. */
void arm_run(struct arm_cpu *c, enum arm_dispatch how);
/* Resultado esperado calculado en C sobre los datos del programa: r0 final y
 * suma de verificación de la zona de salida tras `reps` pasadas. */
void arm_expected(uint32_t reps, uint32_t *r0, uint32_t *out_sum);
uint32_t arm_out_sum(const struct arm_cpu *c);
/* Instrucciones ARM ejecutadas por una corrida de `reps` pasadas. */
uint64_t arm_expected_icount(uint32_t reps);

#endif
