/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — motor de geometría de la DS (Fase 3.1): FIFO de comandos, matrices, vértices, iluminación, recorte y
 * viewport. Produce, por cuadro, una lista de polígonos en coordenadas de pantalla que rasteriza la Fase 3.2.
 *
 * Código propio, escrito desde GBATEK (documentación) y la comparación de comportamiento con melonDS ejecutándolo
 * (docs/fase3.md); no viene de ningún emulador. Portable: C99 con aritmética entera de 64 bits, sin coma flotante,
 * sin dependencias del resto del núcleo (corre en el host, en el PPE big-endian y se prueba con qemu). Las
 * estructuras de salida (`gx_vertex`, `gx_poly`, `gx_frame`) están pensadas para que un SPE las lea por DMA:
 * alineadas a 16 bytes, sin punteros, índices de 16 bits.
 *
 * Tiempo. Todo instante es un contador de ciclos del ARM9 (67,03 MHz); el motor de geometría corre a la mitad
 * (33,5 MHz, el reloj del bus), así que cada ciclo del motor son dos del ARM9. El motor es perezoso: no tiene hilo ni
 * evento propio, `gx_advance(now)` ejecuta los comandos cuyo comienzo cae antes de `now`. Quien lo use tiene que
 * llamarlo antes de mirar cualquier estado (el núcleo lo hace en cada acceso a los registros y en el evento EV_GX).
 *
 * Dos mitades (Fase 3.5). Este módulo es el motor de tiempo: FIFO, tubería, coste de cada orden, GXSTAT, pilas (solo sus
 * punteros y el error) y SWAP_BUFFERS. Lo que las órdenes hacen (matrices, vértices, recorte, lista de polígonos) es
 * gx_exec.c. En modo directo (por omisión) cada orden se ejecuta al empezar; en modo diferido las órdenes que empiezan se
 * anotan en un registro y un ejecutor externo (un SPE) las ejecuta por lotes: el motor de tiempo solo espera a que
 * termine cuando la CPU lee un resultado que depende de la ejecución (o en el SWAP_BUFFERS). El resultado es el mismo
 * bit a bit.
 *
 * Formatos de punto fijo (GBATEK) y sus anchos:
 *   matriz   1.19.12  (s32, 12 bits fraccionarios); el producto de matrices suma en 64 bits y trunca (>> 12)
 *   vértice  1.3.12   (s16) y VTX_10 1.3.6 (se lleva a 1.3.12 con << 6), VTX_DIFF 10 bits con signo sumados tal cual (1/4096)
 *   normal / luz  1.0.9 (s10)
 *   textura  1.11.4   (s16)
 *   color    RGB555 (5 bits por canal); en la salida se expande a 6 bits: c6 = (c5 << 1) + ((c5 + 31) >> 5)
 *   profundidad  15 bits << 9 (0 a 0xFFFE00)
 */
#ifndef SALSADS_GPU3D_GX_H
#define SALSADS_GPU3D_GX_H

#include <stdint.h>

#include "gpu3d/gx_exec.h"

typedef struct gx_qent {
	uint32_t param;
	uint8_t cmd;
	uint64_t t; /* instante en que llegó */
} gx_qent;

typedef struct gx_stats {
	uint64_t cmds;          /* comandos ejecutados */
	uint64_t polys;         /* polígonos que llegaron a la lista */
	uint64_t culled;        /* descartados por cara */
	uint64_t clipped;       /* recortados (con vértices nuevos) */
	uint64_t rejected;      /* fuera del volumen de vista o cruzan el plano lejano sin permiso (suma de las tres siguientes) */
	uint64_t rej_outside, rej_far, rej_empty; /* enteros fuera de un plano / cruzan el lejano sin el bit 12 / el recorte los dejó sin área */
	uint64_t dropped_poly;  /* sin sitio en la RAM de polígonos */
	uint64_t dropped_vert;  /* sin sitio en la RAM de vértices */
	uint64_t swaps;
	uint64_t fifo_overflow; /* escrituras perdidas (FIFO llena con el motor parado en SWAP_BUFFERS) */
	uint64_t cycles;        /* ciclos del motor ocupados (ARM9, /2 = ciclos del motor) */
	uint64_t unknown_cmds;
	uint64_t frame_polys, frame_verts; /* suma de lo entregado por los SWAP_BUFFERS (para el promedio por cuadro) */
	uint32_t max_polys, max_verts;     /* máximos de un cuadro */
	uint64_t host_ticks;               /* tiempo del host gastado ejecutando órdenes (con `clock`) */
	uint64_t by_cmd[0x80];             /* órdenes ejecutadas, por código */
} gx_stats;

typedef struct gx gx_t;

/* Registro de órdenes: palabras `cmd, p0..pn-1` (como las lee gx_exec_run). */
#define GX_LOG_WORDS 32768u
#define GX_KICK_WORDS 4096u /* a partir de cuántas palabras anotadas se publica un trabajo sin esperar */


/* Ejecutor externo (modo diferido). `post` publica un trabajo: ejecutar log[0..nwords) escribiendo en el cuadro `f`, y dejar
 * `g->ex.out` al día al terminar; devuelve un número de secuencia. `wait` espera a ese trabajo (y a los anteriores). */
typedef struct gx_exec_ops {
	void *ctx;
	uint32_t (*post)(void *ctx, gx_t *g, uint32_t flags, const uint32_t *log, uint32_t nwords, gx_frame *f);
	void (*wait)(void *ctx, gx_t *g, uint32_t seq);
} gx_exec_ops;

struct gx {
	/* registro de órdenes del modo diferido: alineado para DMA */
	uint32_t log[GX_LOG_WORDS] __attribute__((aligned(128)));
	uint32_t log_wr, log_sub;  /* palabras anotadas / publicadas desde el inicio del registro */
	uint32_t last_seq;         /* último trabajo publicado */
	int deferred;              /* 1 = ejecutor externo */
	int dirty;                 /* hay órdenes empezadas cuyo resultado no está en `ex.out` */
	int dirty_box, dirty_test; /* ... y entre ellas un BOX_TEST / un POS_TEST o VEC_TEST */
	int mats_valid;            /* `ex.out.clip/vec` corresponden a la ejecución completa */
	int newframe_pending;      /* el próximo trabajo empieza un cuadro nuevo */
	gx_exec_ops ops;

	/* cola de entradas (FIFO de 256 + tubería de 4) */
	gx_qent q[GX_QRING];
	uint32_t qhead, qcount;
	/* decodificador de palabras empaquetadas de GXFIFO */
	uint32_t pk_word;
	uint32_t pk_idx;  /* siguiente byte de orden de la palabra (4 = palabra agotada) */
	uint32_t pk_left; /* parámetros que faltan de la orden en curso */
	uint8_t pk_cmd;

	/* tiempo */
	uint64_t cycle_free;  /* instante (ARM9) en que el motor termina lo que ejecuta */
	uint64_t stack_busy_until, test_busy_until;
	uint32_t test_pending; /* BOX/POS/VEC_TEST en la cola, sin ejecutar */
	uint64_t now;          /* el instante más tardío observado */
	uint64_t nxt;          /* caché de gx_next_start: hasta ese instante gx_advance no tiene nada que ejecutar */

	/* estado de GXSTAT y lo que el tiempo necesita saber de las órdenes (se sigue aquí, sin ejecutarlas) */
	uint32_t err, irqmode;
	gx_sp fe;               /* modo de matriz y punteros de las pilas */
	uint32_t fe_polyattr;   /* POLYGON_ATTR pendiente (para el coste de NORMAL) */

	/* ejecución: en modo directo es la real; en el diferido solo `ex.out` vale (la copia que devuelve el ejecutor) */
	gx_exec ex;

	/* cuadros */
	gx_frame frame[3];         /* en construcción, entregado (`front`) y el anterior, que el render 3D puede seguir leyendo hasta la línea 262 */
	int bld;                   /* cuadro en construcción */
	int front;                 /* último cuadro entregado por SWAP_BUFFERS, o -1 */
	uint32_t swaps;            /* cuántos */
	int swap_pending;          /* SWAP_BUFFERS ejecutado: el motor espera al VBlank */
	uint32_t swap_param;
	gx_stats st;
	uint64_t (*clock)(void); /* opcional: reloj del host para medir `st.host_ticks` */
};

/* Pone el motor en su estado inicial (conserva el reloj de medición y el ejecutor). */
void gx_init(gx_t *g);
/* Modo diferido: las órdenes que empiezan se ejecutan en `ops` (ver gx_exec_ops). Llamar justo después de gx_init. */
void gx_set_executor(gx_t *g, const gx_exec_ops *ops);
/* Espera a que el ejecutor termine todo lo anotado y deja `ex.out` al día (con las matrices si `mats`). Sin efecto en modo
 * directo. */
void gx_sync(gx_t *g, int mats);
/* Publica lo anotado sin esperar (modo diferido). */
void gx_kick(gx_t *g);
/* Contadores al día (en modo diferido sincroniza). */
const gx_stats *gx_stats_get(gx_t *g);

/* Escritura de una palabra de 32 bits a GXFIFO (0x04000400-0x0400043F); empaquetada. Devuelve el instante hasta el que
 * hay que frenar a la CPU (>= now): una FIFO llena la detiene hasta que el motor libere sitio. */
uint64_t gx_fifo_write(gx_t *g, uint64_t now, uint32_t w);
/* Ráfaga de la DMA de modo 7 hacia GXFIFO: palabras little-endian de `src` (hasta `count`), una por 4 ciclos del ARM9 desde
 * `*cycles`, mientras la FIFO tenga menos de la mitad; es gx_advance + comprobación del nivel + gx_fifo_write por palabra (la vía
 * de siempre), sin las llamadas intermedias. Con `on` = 0 las palabras se consumen sin escribirse (motor apagado). Después de cada
 * palabra escrita, si `*ev_gx` es GX_NEVER (no hay mirada de EV_GX pendiente) llama a `sched(ctx, t)` con el instante de esa
 * palabra (antes de sumar sus 4 ciclos), que puede agendar una. Deja en `*cycles` el reloj final y devuelve cuántas palabras pasaron. */
uint32_t gx_dma_burst(gx_t *g, uint64_t *cycles, const uint8_t *src, uint32_t count, int on, const uint64_t *ev_gx,
		      void (*sched)(void *ctx, uint64_t t), void *ctx);
/* Escritura a un puerto de orden (0x04000440-0x040005CC, `off` = dirección - 0x04000000). Idem. */
uint64_t gx_port_write(gx_t *g, uint64_t now, uint32_t off, uint32_t w);

/* Ejecuta los comandos que ya empezaron antes de `now`. */
void gx_advance_run(gx_t *g, uint64_t now);
static inline void gx_advance(gx_t *g, uint64_t now)
{
	if (now > g->now)
		g->now = now;
	if (g->nxt > now) /* lo normal: ninguna orden empieza todavía (se llama en cada acceso a la FIFO) */
		return;
	gx_advance_run(g, now);
}
/* Instante en que empieza el siguiente comando de la cola, o GX_NEVER (cola vacía, incompleta o parada en un SWAP). */
uint64_t gx_next_start(const gx_t *g);

/* GXSTAT completo en el instante `now` (llama a gx_advance). */
uint32_t gx_gxstat(gx_t *g, uint64_t now);
/* Escritura a GXSTAT: `mask` dice qué bytes (bit 15 borra el error; bits 30-31 son el modo de la IRQ). */
void gx_gxstat_write(gx_t *g, uint32_t v, uint32_t mask);
/* Entradas en la FIFO (sin la tubería) y condición de la IRQ 21 según el modo de GXSTAT. */
static inline uint32_t gx_fifo_level(const gx_t *g)
{
	return g->qcount > GX_PIPE_ENTRIES ? g->qcount - GX_PIPE_ENTRIES : 0u;
}
int gx_irq_level(const gx_t *g);
/* RAM_COUNT (0x04000604) */
uint32_t gx_ram_count(gx_t *g);
/* Lectura de 32 bits de un registro del motor en 0x04000600-0x040006FF (`off` = dirección - 0x04000000). */
uint32_t gx_read32(gx_t *g, uint64_t now, uint32_t off);

/* SWAP_BUFFERS: ¿hay un cambio de búfer esperando al VBlank? `gx_swap` lo hace en el instante `now` (el núcleo llama a
 * gx_advance antes), toma la foto de los registros del 3D y deja que el motor siga con lo que había detrás del
 * SWAP_BUFFERS; devuelve el cuadro entregado. */
int gx_swap_pending(const gx_t *g);
const gx_frame *gx_swap(gx_t *g, uint64_t now, const gx_regs *regs);
/* Último cuadro entregado (o NULL). */
const gx_frame *gx_front(const gx_t *g);

#endif
