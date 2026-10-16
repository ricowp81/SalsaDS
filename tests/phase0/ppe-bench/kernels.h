/*
 * SalsaDS — Fase 0.3: núcleos en ensamblador del PPE para los microbenchmarks.
 *
 * Todo lo que mide el programa vive en este par de archivos para poder
 * probarlo también en el host: bajo qemu-ppc64 (tests/unit) los núcleos de
 * banderas se comparan con la referencia en C; los de memoria se ejecutan y
 * se comprueban sus resultados. En un host que no sea PowerPC64 los núcleos
 * no existen (KERNELS_AVAILABLE es 0) y las pruebas se omiten.
 *
 * Reglas del camino caliente (docs/cell-desde-salsa64.md §1.2): ninguna
 * forma con punto, ni mfcr, ni desplazamientos por registro.
 */
#ifndef SALSADS_BENCH_KERNELS_H
#define SALSADS_BENCH_KERNELS_H

#include <stdint.h>

#if defined(__powerpc64__) || defined(__PPC64__)
#define KERNELS_AVAILABLE 1
#else
#define KERNELS_AVAILABLE 0
#endif

/* ---- Memoria little-endian de la máquina emulada --------------------------
 *
 * Cada núcleo hace `iters` vueltas de un cuerpo con KMEM_UNROLL accesos
 * independientes a direcciones fijas de la misma línea de L1 (alineada a 128
 * bytes): aísla el costo de las instrucciones, sin fallos de caché. El
 * bucle va con ctr/bdnz; `mem_base` es una línea de baseline con el mismo
 * bucle y sin accesos, para restar su costo.
 *
 * Los de latencia (`chase`) recorren una permutación cíclica de 1024 entradas
 * (la tabla debe estar en el orden de bytes que lee el núcleo): cada acceso
 * da el índice del siguiente, así que miden latencia de carga + intercambio.
 */
#define KMEM_UNROLL 4
#define KCHASE_UNROLL 4
#define KCHASE_ENTRIES 1024

typedef void (*kmem_fn)(void *base, uint64_t iters);
typedef uint32_t (*kchase_fn)(const void *table, uint64_t iters);

enum kmem_kind { KM_LOAD32, KM_LOAD16, KM_STORE32, KM_STORE16 };
enum kmem_impl { KI_PLAIN, KI_BR, KI_ROT }; /* lwz/stw, lwbrx/stwbrx, lwz/stw + rotaciones */

struct kmem_desc {
	const char *name;
	enum kmem_kind kind;
	enum kmem_impl impl;
	kmem_fn fn;
};

struct kchase_desc {
	const char *name;
	int bits; /* 32 o 16 */
	enum kmem_impl impl;
	kchase_fn fn;
};

extern const struct kmem_desc kmem_table[];
extern const int kmem_count;
extern const struct kchase_desc kchase_table[];
extern const int kchase_count;
void kmem_base(void *base, uint64_t iters);

/* Llena `table` (KCHASE_ENTRIES entradas de 32 o 16 bits) con una
 * permutación cíclica, en el orden de bytes de `impl` (BE para PLAIN, LE para
 * BR y ROT: la memoria de la DS). Devuelve el índice con el que se termina
 * tras `steps` pasos desde 0, calculado en C: sirve de comprobación. */
uint32_t kchase_fill(void *table, int bits, enum kmem_impl impl, uint64_t steps);

/* ---- Banderas NZCV ---------------------------------------------------------
 *
 * Un bucle guest fijo (9 instrucciones ARM, 6–9 por vuelta según los saltos):
 *
 *   loop: ADDS r0,r0,r1 ; CMP r0,r2 ; BLT L1 ; ADDS r3,r3,r5
 *   L1:   CMP r3,r8 ; BHI L2 ; ADDS r6,r6,r7
 *   L2:   SUBS r4,r4,#1 ; BNE loop
 *
 * con valores que hacen que haya desbordamiento con signo (para BLT, N != V)
 * y acarreo (para BHI, C && !Z). Cuatro implementaciones del mismo bucle:
 *   KF_NONE   cota inferior: sin banderas (cmpw + salto directo). No es una
 *             implementación válida de ARM: sirve para saber cuánto cuestan
 *             las banderas.
 *   KF_EAGER  (a) cuatro banderas completas tras cada instrucción, empaquetadas
 *             como en el CPSR, con instrucciones sin punto.
 *   KF_LAZY   (b) perezosa: se guardan operandos y resultado, y las banderas
 *             se calculan solo al consumirlas.
 *   KF_CR     (c) N/Z con cmpwi en cr0, C con cmplw en cr1 y V en cr2; los
 *             saltos usan los bits del CR directamente (el consumidor conoce
 *             el tipo de la instrucción anterior, como en un recompilador por
 *             bloques).
 * Todas dejan el mismo estado guest final.
 */
struct kguest {
	uint32_t r[9]; /* r0..r8 del guest; r4 es el contador de vueltas */
};

typedef void (*kflags_fn)(struct kguest *g);

enum kflags_impl { KF_NONE, KF_EAGER, KF_LAZY, KF_CR, KF_COUNT };
extern const char *const kflags_name[KF_COUNT];
extern const kflags_fn kflags_table[KF_COUNT];

/* Estado inicial del bucle para `iters` vueltas. */
void kflags_init(struct kguest *g, uint32_t iters);
/* Referencia en C con la semántica de ARM; devuelve las instrucciones guest
 * ejecutadas. Deja en `g` el estado final. */
uint64_t kflags_ref(struct kguest *g);

#endif
