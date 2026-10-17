/*
 * SalsaDS — máquina de la DS (Fase 1.2): mapa de memoria, planificador, IRQ, temporizadores, DMA, IPC,
 * cartucho, SPI, matemáticas por hardware, BIOS en HLE y arranque directo.
 *
 * Código propio, escrito desde GBATEK (documentación) y la comparación de comportamiento con melonDS; no
 * viene de ningún emulador (docs/THIRD_PARTY.md). Portable: compila en la Mac (little-endian) y con ppu-gcc
 * (big-endian); toda la memoria emulada se toca con rd16le/wr16le y compañía (util/endian.h).
 *
 * Tiempo. La base es el reloj del ARM9 (67,027964 MHz, `n->time`); el ARM7 corre a la mitad (cada ciclo del
 * ARM7 = 2 del ARM9) y el bus/temporizadores a 33,513982 MHz = ciclos del ARM7. Una línea de video son
 * 355 puntos × 6 ciclos de bus = 2130 ciclos de bus = 4260 del ARM9; un cuadro 263 líneas.
 *
 * La frontera para el ARM7 en otro hilo (Fase 4, docs/fase1.md «Planificador y frontera entre CPUs»):
 * cada CPU es dueña de su `struct nds_cpuio` (IME/IE/IF, temporizadores, DMA, registros propios). Lo único
 * que se comparte entre las dos CPUs es (1) la memoria (RAM principal, WRAM compartida), (2) el IPC
 * (IPCSYNC y las dos colas de FIFO) y (3) las líneas de IRQ que una CPU levanta en la otra
 * (`nds_irq_raise`). Todo lo demás pasa por el planificador de un solo hilo, que corre las dos CPUs en
 * tramos de `n->quantum` ciclos del ARM9 y atiende los eventos al final de cada tramo.
 */
#ifndef SALSADS_CORE_NDS_H
#define SALSADS_CORE_NDS_H

#include <stddef.h>
#include <stdint.h>

#include "cpu/acore.h"

#define NDS_CPU9 0
#define NDS_CPU7 1

#define NDS_RAM_SIZE (4u << 20)
#define NDS_SWRAM_SIZE 0x8000u
#define NDS_WRAM7_SIZE 0x10000u
#define NDS_VRAM_SIZE 0xA4000u /* 656 KB: A, B, C, D (128 KB), E (64), F, G (16), H (32), I (16) */
#define NDS_PAL_SIZE 0x800u
#define NDS_OAM_SIZE 0x800u
#define NDS_BIOS7_SIZE 0x4000u
#define NDS_BIOS9_SIZE 0x1000u
#define NDS_FW_SIZE 0x40000u

/* Tiempo (ciclos del ARM9). */
#define NDS_LINE_CYCLES 4260u
#define NDS_HBLANK_CYCLES 3072u /* el HBlank empieza en el punto 256 (256 × 6 ciclos de bus × 2) */
#define NDS_LINES 263u
#define NDS_FRAME_CYCLES (NDS_LINE_CYCLES * NDS_LINES)
#define NDS_SCREEN_W 256
#define NDS_SCREEN_H 192

/* Fuentes de IRQ (bits de IE/IF). */
#define NDS_IRQ_VBLANK (1u << 0)
#define NDS_IRQ_HBLANK (1u << 1)
#define NDS_IRQ_VCOUNT (1u << 2)
#define NDS_IRQ_TIMER(i) (1u << (3 + (i)))
#define NDS_IRQ_SIO (1u << 7) /* ARM7: RCNT/RTC */
#define NDS_IRQ_DMA(i) (1u << (8 + (i)))
#define NDS_IRQ_KEYPAD (1u << 12)
#define NDS_IRQ_GBASLOT (1u << 13)
#define NDS_IRQ_IPCSYNC (1u << 16)
#define NDS_IRQ_IPCSEND (1u << 17)
#define NDS_IRQ_IPCRECV (1u << 18)
#define NDS_IRQ_CARDDONE (1u << 19)
#define NDS_IRQ_CARDIREQ (1u << 20)
#define NDS_IRQ_GXFIFO (1u << 21) /* ARM9 */
#define NDS_IRQ_LIDOPEN (1u << 22) /* ARM7 */
#define NDS_IRQ_SPI (1u << 23)    /* ARM7 */
#define NDS_IRQ_WIFI (1u << 24)   /* ARM7 */

/* Botones (KEYINPUT, 0 = apretado) y EXTKEYIN (solo ARM7). */
#define NDS_KEY_A (1u << 0)
#define NDS_KEY_B (1u << 1)
#define NDS_KEY_SELECT (1u << 2)
#define NDS_KEY_START (1u << 3)
#define NDS_KEY_RIGHT (1u << 4)
#define NDS_KEY_LEFT (1u << 5)
#define NDS_KEY_UP (1u << 6)
#define NDS_KEY_DOWN (1u << 7)
#define NDS_KEY_R (1u << 8)
#define NDS_KEY_L (1u << 9)
#define NDS_KEY_X (1u << 10) /* aquí 1 = apretado; se invierte al exponerlos */
#define NDS_KEY_Y (1u << 11)
#define NDS_KEY_PEN (1u << 12)  /* pantalla táctil apretada */
#define NDS_KEY_LID (1u << 13)  /* tapa cerrada */

enum nds_ev {
	EV_LINE = 0, /* empieza una línea */
	EV_HBLANK,   /* empieza el HBlank de la línea */
	EV_TM9,      /* 4 eventos */
	EV_TM7 = EV_TM9 + 4,
	EV_CART = EV_TM7 + 4,
	EV_SPI,
	EV_COUNT
};
#define NDS_NEVER UINT64_MAX

struct nds;

struct nds_timer {
	uint16_t reload, ctl;
	uint32_t cnt;  /* contador al instante t0 (ciclos del ARM9) */
	uint64_t t0;
};

struct nds_dma {
	uint32_t sad, dad, cnt; /* registros: cnt = cuenta (bits 0-20) y control (bits 16-31) */
	uint32_t src, dst, left; /* estado interno de la transferencia en curso */
	uint8_t latched;
};

/* Estado que cada CPU posee (ver la frontera arriba). */
struct nds_cpuio {
	uint32_t ime, ie, ifl;
	uint16_t dispstat; /* solo los bits escribibles: 3-5 (habilitaciones) y 7-15 (LYC) */
	uint16_t keycnt;
	uint8_t key_cond;  /* última condición de KEYCNT, para disparar por flanco */
	uint16_t ipcfifocnt; /* bits escritos: 2, 10, 15 y el error (14) */
	uint16_t exmem;    /* ARM9: EXMEMCNT; ARM7: bits 0-6 propios */
	uint8_t postflg;
	struct nds_timer tm[4];
	struct nds_dma dma[4];
	uint16_t gen[0x1000]; /* registros sin comportamiento propio (0x04000000-0x04001FFF por medias palabras) */
};

struct nds_fifo {
	uint32_t q[16];
	uint32_t head, count, last;
};

struct nds_rgn {
	uint8_t *p;
	uint32_t mask;
};

struct nds_cart {
	const uint8_t *rom;
	size_t size;
	uint32_t capacity;     /* 128 KB << cabecera[0x14] */
	uint16_t auxcnt;
	uint8_t auxdata;
	uint32_t romctl;       /* bit 31 y 23 reflejan el estado vivo */
	uint8_t cmd[8];
	uint8_t buf[0x4000];
	uint32_t nwords, widx; /* palabras del bloque y siguiente a entregar */
	uint8_t drq, busy;
	uint8_t spi_hold;      /* AUXSPI sin chip de guardado: se contesta 0xFF */
};

struct nds_spi {
	uint16_t cnt;
	uint8_t data;
	uint8_t busy;
	uint8_t fw_state, fw_cmd, fw_wel, fw_cnt;
	uint32_t fw_addr;
	uint8_t pm_state, pm_reg, pm_regs[8];
	uint8_t tsc_ctl, tsc_q[2], tsc_qn;
	uint8_t cur_dev, cur_hold, data_next;
};

struct nds_rtc {
	uint16_t pins;       /* registro 0x04000138 */
	uint8_t state;       /* 0 = esperando comando, 1 = datos */
	uint8_t cmd, nbits_in, bits_in;
	uint8_t out[8], nout, outbit; /* bytes por entregar, índice de bit */
	uint8_t in[8], nin, want_in;
	uint8_t status1, status2, clkadj, freereg;
	uint8_t last_sck, sio_out, idx;
	uint32_t base_sec; /* segundos desde 2000-01-01 00:00:00 al instante 0 de la emulación */
};

/* Matemáticas por hardware del ARM9 (DIVCNT y SQRTCNT). */
struct nds_math {
	uint16_t divcnt, sqrtcnt;
	uint64_t numer, denom, quot, rem;
	uint64_t sqrt_param;
	uint32_t sqrt_res;
	uint64_t div_done, sqrt_done; /* instante (ARM9) en que termina el cálculo */
	uint8_t div0;
};

struct nds {
	struct acore a9, a7;

	/* memoria */
	uint8_t itcm[ACORE_ITCM_SIZE], dtcm[ACORE_DTCM_SIZE];
	uint8_t ram[NDS_RAM_SIZE];
	uint8_t swram[NDS_SWRAM_SIZE];
	uint8_t wram7[NDS_WRAM7_SIZE];
	uint8_t vram[NDS_VRAM_SIZE];
	uint8_t pal[NDS_PAL_SIZE], oam[NDS_OAM_SIZE];
	uint8_t bios9[NDS_BIOS9_SIZE], bios7[NDS_BIOS7_SIZE];
	uint8_t fw[NDS_FW_SIZE];
	struct nds_rgn rgn_rd[2][512], rgn_wr[2][512]; /* por CPU y por región de 8 MB: ruta rápida */
	uint8_t wramcnt;
	uint8_t vramcnt[9]; /* A..I */
	/* Vista de la VRAM por la CPU: por ventana (0 fondo A, 1 fondo B, 2 OBJ A, 3 OBJ B) y página de 16 KB,
	 * qué bancos la cubren (máscara de 9 bits) y, si es uno solo, el puntero a la página. */
	uint16_t vpg_mask[4][32];
	uint8_t *vpg_ptr[4][32];
	uint16_t vpg7_mask[16];
	uint8_t *vpg7_ptr[16];
	uint8_t vram_lcdc_mask[41]; /* bancos que existen y están como LCDC, por página del LCDC */
	uint32_t vbank_off[9];      /* desplazamiento de cada banco dentro de la ventana en que está mapeado */
	uint32_t vramstat;          /* VRAMSTAT (ARM7) */

	/* E/S por CPU */
	struct nds_cpuio io[2];
	uint16_t keyinput, extkeyin;
	uint16_t touch_x, touch_y; /* en píxeles de la pantalla de abajo */
	uint16_t powcnt1, powcnt2;
	struct nds_math math;
	uint32_t ipcsync_out[2]; /* bits 8-11 de cada CPU */
	uint8_t ipcsync_irqen[2];
	struct nds_fifo fifo[2]; /* cola que envía cada CPU */

	struct nds_cart cart;
	struct nds_spi spi;
	struct nds_rtc rtc;

	/* tiempo y eventos */
	uint64_t time;    /* ciclos del ARM9 que el planificador ya dio por vividos */
	uint64_t ev[EV_COUNT];
	uint32_t line;    /* línea que muestran los eventos (0-262) */
	uint32_t quantum; /* ciclos del ARM9 por tramo */
	uint64_t frames;  /* cuadros completos (a cada inicio del VBlank) */
	uint8_t stop, stop_on_vblank; /* el planificador sale al terminar el evento en curso (nds_run_frame) */

	/* BIOS en HLE */
	uint8_t intrwait_active[2];
	uint32_t user_cmd_swi_seen;
	uint8_t swi_warned[2][32];
	uint8_t powered_off;

	void (*log)(void *ctx, const char *msg);
	void *log_ctx;
};

/* ---- API pública -------------------------------------------------------------------------------- */

void nds_init(struct nds *n);
/* Conecta la ROM (sin copiarla: `rom` tiene que vivir mientras se use). 0 = bien. */
int nds_load_rom(struct nds *n, const uint8_t *rom, size_t len);
/* Arranque directo: carga los binarios ARM9/ARM7, arma la memoria y los registros. 0 = bien. */
int nds_boot_direct(struct nds *n);
/* Corre `arm9_cycles` ciclos del ARM9 (y la mitad del ARM7). */
void nds_run(struct nds *n, uint64_t arm9_cycles);
/* Corre hasta que empiece el siguiente VBlank (línea 192): un cuadro. */
void nds_run_frame(struct nds *n);

/* Entrada: botones NDS_KEY_*, y el toque en píxeles (0-255, 0-191) si NDS_KEY_PEN. */
void nds_set_input(struct nds *n, uint32_t keys, unsigned touch_x, unsigned touch_y);

/* Imagen de las dos pantallas en RGB555 (bit 15 = 1), 256×192 cada una. Fase 1.2: modo de visualización
 * directa de VRAM (motor A) y fondo (paleta 0) en el modo de gráficos; las capas 2D son de la Fase 2. */
void nds_screens(const struct nds *n, uint16_t *top, uint16_t *bottom);

/* Acceso de depuración a la memoria vista por una CPU (sin efectos; 0 si no hay nada). */
uint32_t nds_peek32(struct nds *n, int cpu, uint32_t addr);
uint32_t nds_peek8(struct nds *n, int cpu, uint32_t addr);

void nds_logf(struct nds *n, const char *fmt, ...);

/* IRQ que una CPU levanta en otra (o en sí misma): el único camino de IRQ entre CPUs. */
void nds_irq_raise(struct nds *n, int cpu, uint32_t mask);

/* Tiempo de una CPU en ciclos del ARM9. */
static inline uint64_t nds_cpu_now(const struct nds *n, int cpu)
{
	return cpu ? n->a7.cycles * 2u : n->a9.cycles;
}

/* Línea actual y posición dentro de ella, calculadas del instante `t` (no hace falta haber procesado los
 * eventos de línea): el cuadro empieza en t = 0 con la línea 0. */
static inline uint32_t nds_line_at(uint64_t t)
{
	return (uint32_t)((t % NDS_FRAME_CYCLES) / NDS_LINE_CYCLES);
}
static inline uint32_t nds_dot_at(uint64_t t)
{
	return (uint32_t)(t % NDS_LINE_CYCLES);
}

#endif
