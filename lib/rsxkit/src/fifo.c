/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — espacio en el búfer de comandos antes de cada operación.
 *
 * El búfer de comandos de libgcm va por segmentos: cuando una función de
 * PSL1GHT no cabe en el segmento en curso, llama al callback del contexto,
 * que salta al segmento siguiente y mueve context->current. En PSL1GHT
 * (f649a08) ese llamado es un `asm` sin "memory" en sus clobbers
 * (rsxContextCallback): GCC deduce que no toca memoria y la función que lo
 * llamó sigue escribiendo con el puntero viejo, más allá del fin del
 * segmento, mientras el nuevo queda con huecos. El RSX lee entonces datos
 * como comandos (en RPCS3: "FIFO error: possible desync event").
 *
 * rsxkit lo evita pidiendo el espacio antes, con un llamado correcto: cada
 * operación pública reserva lo que puede escribir, y así las funciones de
 * PSL1GHT nunca llegan a cambiar de segmento por su cuenta.
 */
#include "internal.h"

/* Llama al callback del contexto (descriptor de función de 32 bits:
 * entrada y TOC) con r3 = contexto y r4 = palabras pedidas. */
static s32 __attribute__((noinline)) fifo_callback(gcmContextData *context, u32 count)
{
	register u64 r3 __asm__("r3") = (u64)(uintptr_t)context;
	register u64 r4 __asm__("r4") = count;
	u32 opd = (u32)context->callback;
	__asm__ __volatile__(
		"stdu 1,-128(1)\n\t"
		"std 2,40(1)\n\t"
		"lwz 0,0(%2)\n\t"
		"lwz 2,4(%2)\n\t"
		"mtctr 0\n\t"
		"bctrl\n\t"
		"ld 2,40(1)\n\t"
		"addi 1,1,128\n\t"
		: "+r"(r3), "+r"(r4)
		: "b"((u64)opd)
		: "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "ctr", "lr", "xer",
		  "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return (s32)r3;
}

/* El callback espera sin tope a que el RSX deje libre el segmento
 * siguiente, y su lógica es del sistema (sin fuente: no se puede saber de
 * antemano si va a esperar). Queda anotado como espera (wait.c) para que un
 * vigía en otro hilo vea si se queda ahí. Perdido el RSX (rsxkit_lost) no
 * se llama: lo que se escriba pisa el segmento en curso y no llega. */
void rsxkit_reserve(uint32_t words)
{
	gcmContextData *ctx = rsxkit_g.ctx;
	if (!ctx || ctx->current + words <= ctx->end)
		return;
	if (rsxkit_lost()) {
		ctx->current = ctx->begin;
		return;
	}
	rsxkit_wait_begin(RSXKIT_WAIT_FIFO, "FIFO", 0, words, 0);
	fifo_callback(ctx, words);
	rsxkit_wait_end();
}
