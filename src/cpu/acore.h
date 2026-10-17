/*
 * SalsaDS — intérpretes de CPU de la DS (Fase 1.1).
 *
 *   ARM9  ARM946E-S, ARMv5TE: ARM y Thumb, CLZ, QADD/QSUB/QDADD/QDSUB, SMLAxy y
 *         familia, LDRD/STRD, BLX, BKPT, PLD (NOP), interworking al cargar PC, CP15
 *         mínimo (registro de control, ITCM, DTCM, espera de interrupción) y
 *         las dos TCM dentro del núcleo (sin caché ni MPU).
 *   ARM7  ARM7TDMI, ARMv4T: ARM y Thumb.
 *
 * Un solo código para las dos (`struct acore`, la bandera `v5` decide lo que
 * cambia entre versiones). Código propio escrito desde el manual de
 * arquitectura de ARM y GBATEK (documentación; ver docs/THIRD_PARTY.md): no
 * viene de ningún emulador.
 *
 * Diseño de la interfaz (pensada para que el recompilador de la Fase 4
 * reemplace al intérprete sin tocar el resto):
 *
 *  - Todo el estado del procesador vive en `struct acore`, con los registros
 *    visibles primero (`r[16]`, `cpsr`) para direccionarlos por desplazamiento
 *    desde código generado. Un recompilador usa la misma estructura, las mismas
 *    funciones de memoria del bus y las mismas rutinas de excepción.
 *  - La memoria entra por `struct acore_bus` (callbacks por tamaño; direcciones
 *    siempre alineadas: la alineación, la rotación del LDR y demás reglas por
 *    versión las resuelve el núcleo). El mapa de memoria real (Fase 1.2)
 *    implementa esos callbacks; la ITCM y la DTCM las maneja el propio núcleo
 *    porque su ubicación sale de CP15. Un callback puede señalar un aborto con
 *    `acore_signal_abort`.
 *  - El tiempo es un contador de ciclos grueso (`cycles`): 1 por instrucción
 *    más los ciclos internos y de salto de la ARM7TDMI, sin esperas de bus (las
 *    suma el mapa de memoria con `c->cycles += n` desde sus callbacks).
 *    `acore_run(c, hasta)` ejecuta hasta que `cycles` llegue a `hasta`.
 *  - Interrupciones: nivel de línea con `acore_set_irq`/`acore_set_fiq`; se
 *    toman entre instrucciones si no están enmascaradas. La CPU en HALT
 *    despierta con la línea de IRQ aunque CPSR.I la enmascare (como la DS).
 *  - HLE de la BIOS: `swi_hook` (si devuelve distinto de cero, la SWI se da por
 *    atendida y no se toma la excepción).
 *
 * Convención de PC (importante para quien toque el núcleo): entre
 * instrucciones, r[15] vale la dirección de la próxima instrucción + 4 en ARM
 * (+ 2 en Thumb). Durante la ejecución, r[15] vale lo que lee el programa
 * (dirección de la instrucción + 8 en ARM, + 4 en Thumb). `acore_pc()` da la
 * dirección de la próxima instrucción.
 */
#ifndef SALSADS_CPU_ACORE_H
#define SALSADS_CPU_ACORE_H

#include <stdint.h>

/* CPSR */
#define ACORE_N 0x80000000u
#define ACORE_Z 0x40000000u
#define ACORE_C 0x20000000u
#define ACORE_V 0x10000000u
#define ACORE_Q 0x08000000u /* solo ARMv5TE */
#define ACORE_I 0x80u
#define ACORE_F 0x40u
#define ACORE_T 0x20u
#define ACORE_MODE_MASK 0x1Fu

#define ACORE_MODE_USR 0x10u
#define ACORE_MODE_FIQ 0x11u
#define ACORE_MODE_IRQ 0x12u
#define ACORE_MODE_SVC 0x13u
#define ACORE_MODE_ABT 0x17u
#define ACORE_MODE_UND 0x1Bu
#define ACORE_MODE_SYS 0x1Fu

enum acore_kind { ACORE_ARM7 = 0, ACORE_ARM9 = 1 };

/* Excepciones (el desplazamiento en la tabla de vectores es 4 * el valor). */
enum acore_exc {
	ACORE_EXC_RESET = 0,
	ACORE_EXC_UNDEF = 1,
	ACORE_EXC_SWI = 2,
	ACORE_EXC_PABT = 3,
	ACORE_EXC_DABT = 4,
	ACORE_EXC_IRQ = 6,
	ACORE_EXC_FIQ = 7,
};

enum { ACORE_ABORT_NONE = 0, ACORE_ABORT_DATA = 1, ACORE_ABORT_PREFETCH = 2 };

struct acore;

/*
 * Bus de memoria. Las direcciones llegan alineadas al tamaño del acceso. Los
 * valores de 8 y 16 bits viajan en los bits bajos. Un callback puede sumar
 * esperas con `c->cycles += n` y señalar un aborto con acore_signal_abort().
 */
struct acore_bus {
	uint32_t (*read8)(struct acore *c, uint32_t addr);
	uint32_t (*read16)(struct acore *c, uint32_t addr);
	uint32_t (*read32)(struct acore *c, uint32_t addr);
	void (*write8)(struct acore *c, uint32_t addr, uint32_t v);
	void (*write16)(struct acore *c, uint32_t addr, uint32_t v);
	void (*write32)(struct acore *c, uint32_t addr, uint32_t v);
	uint32_t (*fetch16)(struct acore *c, uint32_t addr);
	uint32_t (*fetch32)(struct acore *c, uint32_t addr);
};

#define ACORE_ITCM_SIZE 0x8000u /* 32 KB */
#define ACORE_DTCM_SIZE 0x4000u /* 16 KB */

struct acore {
	/* ---- estado visible: primero y en este orden ---- */
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t bank;           /* banco del modo actual: 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
	uint32_t spsr_bank[6];   /* SPSR por banco (el 0 no se usa) */

	/* ---- registros con banco (los de otros modos; los del modo actual están vivos en r[]) ---- */
	uint32_t sp_bank[6], lr_bank[6];
	uint32_t fiq_r8_12[5];   /* r8-r12 de FIQ cuando no es el modo actual */
	uint32_t usr_r8_12[5];   /* r8-r12 de usuario mientras el modo actual es FIQ */

	/* ---- configuración ---- */
	uint32_t v5;             /* 1 = ARMv5TE (ARM9), 0 = ARMv4T (ARM7) */
	const struct acore_bus *bus;
	void *user;              /* para el dueño del núcleo: lo usan los callbacks */
	int (*swi_hook)(struct acore *c, uint32_t comment);

	/* ---- ejecución ---- */
	uint64_t cycles;         /* contador de ciclos (modelo grueso) */
	uint64_t icount;         /* instrucciones ejecutadas */
	uint32_t irq, fiq;       /* líneas de interrupción (nivel) */
	uint32_t halted;         /* esperando una interrupción */
	uint32_t attn;           /* halted | irq | fiq: ruta lenta del bucle */
	uint32_t abort;          /* ACORE_ABORT_*: lo pone el bus, lo atiende el núcleo */

	/* ---- CP15 y TCM (solo ARM9) ---- */
	uint8_t *itcm, *dtcm;    /* memoria de las TCM (del dueño); NULL en la ARM7 */
	uint32_t cp15_ctrl;
	uint32_t cp15_dtcm_reg, cp15_itcm_reg;
	uint32_t cp15_misc[48];  /* c2, c3, c5, c6, c9 de lockdown, c13: se guardan, no se aplican */
	uint32_t vector_base;
	uint64_t itcm_rd_len, itcm_wr_len, dtcm_rd_len, dtcm_wr_len;
	uint32_t dtcm_base;
};

void acore_init(struct acore *c, enum acore_kind kind, const struct acore_bus *bus, void *user);
/* Conecta la memoria de las TCM del ARM9 (32 KB y 16 KB). */
void acore_attach_tcm(struct acore *c, uint8_t *itcm, uint8_t *dtcm);
/* Excepción de reinicio: modo SVC, I y F puestos, ARM; ARM9 con vectores altos. */
void acore_reset(struct acore *c);

/* Ejecuta hasta que c->cycles >= until (o hasta que quede en HALT sin interrupción: entonces avanza el reloj). */
void acore_run(struct acore *c, uint64_t until);
/* Una instrucción (o la entrada a una IRQ). */
void acore_step(struct acore *c);

void acore_set_irq(struct acore *c, int level);
void acore_set_fiq(struct acore *c, int level);
/* Pone la CPU en HALT (SWI Halt de la ARM7, espera de interrupción de CP15 en la ARM9). */
void acore_halt(struct acore *c);
/* Despierta la CPU de HALT sin levantar la línea de IRQ: en la DS el HALT termina cuando IE & IF != 0
 * aunque IME o el bit I del CPSR enmascaren la interrupción; la línea (acore_set_irq) es IME & IE & IF. */
void acore_wake(struct acore *c);

/* Dirección de la próxima instrucción. */
static inline uint32_t acore_pc(const struct acore *c)
{
	return c->r[15] - ((c->cpsr & ACORE_T) ? 2u : 4u);
}

/* Salta a `addr` en el estado actual (alinea según T) / con cambio de estado según el bit 0. */
void acore_set_pc(struct acore *c, uint32_t addr);
void acore_bx(struct acore *c, uint32_t addr);

/* Escribe el CPSR completo cambiando de modo con sus bancos (arranque directo, pruebas). */
void acore_set_cpsr(struct acore *c, uint32_t cpsr);
/* Registro del banco de usuario (para LDM/STM con ^ y para depurar). */
uint32_t acore_get_user_reg(const struct acore *c, unsigned n);

/* Para los callbacks del bus: señalar un aborto de datos o de captación. */
static inline void acore_signal_abort(struct acore *c, uint32_t kind)
{
	c->abort = kind;
}

/* Entrada a una excepción (la usan el núcleo y los ganchos de HLE). `lr` es el valor para r14 del modo nuevo. */
void acore_exception(struct acore *c, enum acore_exc e, uint32_t lr);

/* Accesos del programa (ven las TCM, como una instrucción de carga/almacenamiento): para la HLE de la BIOS y
 * para quien necesite leer la memoria como la CPU. Direcciones alineadas al tamaño. */
uint32_t acore_rd8(struct acore *c, uint32_t a);
uint32_t acore_rd16(struct acore *c, uint32_t a);
uint32_t acore_rd32(struct acore *c, uint32_t a);
void acore_wr8(struct acore *c, uint32_t a, uint32_t v);
void acore_wr16(struct acore *c, uint32_t a, uint32_t v);
void acore_wr32(struct acore *c, uint32_t a, uint32_t v);

/* Estado de CP15 del ARM9 (registro de control y regiones de ITCM/DTCM) y recálculo de las TCM: lo usa el
 * arranque directo para dejar el estado que deja la BIOS. */
void acore_set_tcm_state(struct acore *c, uint32_t ctrl, uint32_t itcm_reg, uint32_t dtcm_reg);

#endif
