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
#include "gpu2d/gpu2d.h"
#include "gpu2d/gpu2d_compose.h"
#include "audio/snd.h"
#include "gpu3d/gx.h"

#define NDS_CPU9 0
#define NDS_CPU7 1

#define NDS_RAM_SIZE (4u << 20)
#define NDS_SWRAM_SIZE 0x8000u
#define NDS_WRAM7_SIZE 0x10000u
#define NDS_SAVE_MAX 0x100000u /* guardado: hasta 1 MB (EEPROM, FRAM o Flash) */
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
	EV_GX, /* el motor de geometría: empieza una orden (baja el nivel de la FIFO) o toca repetir la IRQ 21 por nivel */
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
	uint64_t spi_done;     /* AUXSPI: instante (ARM9) en que termina el byte en curso (para el bit de ocupado) */
};

/* Chip de guardado del cartucho (AUXSPI): ver save.c. */
enum { NDS_SAVE_EEPROM512 = 1, NDS_SAVE_EEPROM16, NDS_SAVE_FRAM, NDS_SAVE_EEPROM24, NDS_SAVE_FLASH };
struct nds_save {
	uint8_t type;
	uint8_t state, cmd, a8, wel, wp, srwd, first, wrote, dirty;
	uint8_t addr_len;
	uint32_t size, page, addr, nbytes;
};

struct nds_spi {
	uint16_t cnt;
	uint8_t data;
	uint8_t busy;
	uint8_t fw_state, fw_cmd, fw_wel, fw_cnt, fw_first;
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

/* ---- Video (Fase 2.1): instantáneas por cuadro para los motores 2D ----------------------------------------
 * Al empezar un cuadro (línea 0) se toma una instantánea de lo que los motores 2D necesitan (VRAM por páginas con
 * su marca de «sucia», paleta, OAM, registros, tablas de páginas de las vistas) y, hasta la línea 192, se anotan
 * por línea los cambios de registros, paleta y OAM que escribe el ARM9 (efectos de raster). En la línea 192 el
 * cuadro queda listo: o se entrega a un renderizador (`ops.submit`, p. ej. los SPEs de la PS3) o se dibuja a
 * demanda en el host (`nds_screens`). Hay dos juegos de instantáneas, que se turnan, para que un renderizador
 * asíncrono lea el del cuadro anterior mientras se graba el siguiente. */
#define NDS_VLOG_MAX 8192 /* cambios por motor y cuadro; los que sobran se descartan (cuenta en `dropped`) */
#define NDS_VPAGES 41     /* páginas de 16 KB de los 656 KB de VRAM */

struct nds_vset {
	uint8_t vram[G2D_VRAM_SIZE + G2D_VRAM_ZERO] __attribute__((aligned(128))); /* + 128 bytes en cero al final */
	uint8_t pal[2][1024] __attribute__((aligned(128)));
	uint8_t oam[2][1024] __attribute__((aligned(128)));
	uint8_t mmem[GPU2D_H][512] __attribute__((aligned(128))); /* modo 3: FIFO de memoria principal, por línea */
	gpu2d_change chg[2][NDS_VLOG_MAX] __attribute__((aligned(128)));
	gpu2d_frame frame;
	uint16_t powcnt1; /* al final del cuadro */
	uint32_t dropped; /* cambios que no cupieron */
};

struct nds_video_ops {
	/* El cuadro `set` (con `frame` ya lleno salvo las salidas) está listo: el renderizador pone `out_ea` de cada motor
	 * y lo dibuja (puede hacerlo en segundo plano). NULL = dibujar a demanda en el host. */
	void (*submit)(void *ctx, int set, gpu2d_frame *frame);
	/* Antes de volver a llenar la instantánea `set`: el renderizador tiene que haber terminado con ella (opcional). */
	void (*wait)(void *ctx, int set);
	void *ctx;
};

struct nds_video {
	struct nds_vset set[2];
	uint64_t dirty[2]; /* páginas de VRAM cambiadas desde la última instantánea de cada juego */
	int cur;           /* juego que graba el cuadro en curso */
	int recording;     /* 1 entre la línea 0 y la 192 */
	int last;          /* último juego completo (línea 192 pasada), o -1 */
	int rendered;      /* el host ya dibujó `last` en `img` */
	uint32_t mpos;     /* posición (píxel del cuadro) de la FIFO de memoria principal */
	uint32_t frames;
	struct nds_video_ops ops;
	uint16_t img[2][GPU2D_W * GPU2D_H]; /* host: RGBA5551 de los motores A y B del cuadro `last` */
	int use_ref;        /* host: dibujar con la referencia píxel por píxel (1) o con el compositor por líneas (0) */
	gpu2d_ctx comp;     /* espacio de trabajo del compositor del host */
	gpu2d_stats stat_sum[2]; /* host: suma de los contadores del compositor por motor (diagnóstico) */
	uint32_t stat_frames;
};

/* ---- Sonido (Fase 2.2): el mezclador de src/audio recibe cada cuadro las escrituras a los registros de sonido del
 * ARM7 con su instante y genera las muestras del cuadro. Hay dos trabajos que se turnan; el resultado de uno se recoge
 * al cerrar el cuadro siguiente (un cuadro de latencia, igual en el host y con el SPE de la PS3). */
struct nds_snd_rgn {
	uint64_t rd, wr; /* dirección del inicio de la región (lectura y escritura) o 0 */
	uint32_t mask;
	uint32_t pad;
};

struct nds_snd_job {
	snd_ent log[SND_LOG_MAX] __attribute__((aligned(128)));
	int16_t nat[2 * SND_TICKS_MAX] __attribute__((aligned(128)));
	float out[2 * SND_OUT_MAX] __attribute__((aligned(128)));
	struct nds_snd_rgn rgn[4]; /* regiones de 8 MB 4..7 del ARM7 (RAM principal y WRAM) */
	uint32_t nlog, nticks, out_n, used;
	uint32_t mode;      /* SND_MODE_* */
	float ratio_adj;    /* corrección de la razón de remuestreo de este trabajo */
};

struct nds_snd_ops {
	/* Manda el trabajo `set` al mezclador (PS3: a un SPE) ; NULL = el núcleo lo corre en el acto. */
	void (*submit)(void *ctx, int set, struct nds_snd_job *job, snd_state *st);
	/* Espera a que el trabajo `set` termine (si submit no es NULL). */
	void (*wait)(void *ctx, int set);
	/* Entrega lo que salió del trabajo recogido: la salida estéreo a 48 kHz y las muestras nativas. */
	void (*sink)(void *ctx, const float *out, uint32_t n, const int16_t *nat, uint32_t nticks);
	void *ctx;
};

struct nds_snd {
	snd_state st __attribute__((aligned(128)));
	struct nds_snd_job job[2];
	int cur, prev; /* trabajo que se arma / el que está en vuelo (-1 = ninguno) */
	snd_ent plog[SND_LOG_MAX]; /* escrituras todavía sin mandar */
	uint32_t nplog, dropped;
	uint64_t tick_sub; /* ticks ya mandados */
	/* lo que lee el ARM7 de vuelta */
	uint32_t cnt[SND_NCH]; /* SOUNDxCNT sin el bit 31 */
	uint32_t busy;         /* bit c: canal c encendido; 16 y 17: capturas (lo último que dijo el mezclador + arranques) */
	uint16_t soundcnt, bias;
	uint8_t capcnt[2];
	struct nds_snd_ops ops;
	uint32_t mode;   /* SND_MODE_* (se pasa en cada trabajo, no vive en el estado que el mezclador reescribe) */
	float ratio_adj; /* idem */
	uint64_t tasks, applied;
};

struct nds {
	struct acore a9, a7;

	/* memoria */
	uint8_t itcm[ACORE_ITCM_SIZE], dtcm[ACORE_DTCM_SIZE];
	uint8_t ram[NDS_RAM_SIZE] __attribute__((aligned(128)));    /* alineadas: el SPE de audio las lee por DMA */
	uint8_t swram[NDS_SWRAM_SIZE] __attribute__((aligned(128)));
	uint8_t wram7[NDS_WRAM7_SIZE] __attribute__((aligned(128)));
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
	/* Motor de geometría (Fase 3.1): src/gpu3d/gx.c; el pegamento con la máquina, en src/core/geom.c. */
	gx_t gx;
	uint64_t gx_frames; /* cuadros entregados por SWAP_BUFFERS */
	uint64_t gx_irq_t;  /* instante de la última petición de la IRQ 21 (se repite cada NDS_GX_PERIOD mientras dure el nivel) */
	uint32_t ipcsync_out[2]; /* bits 8-11 de cada CPU */
	uint8_t ipcsync_irqen[2];
	struct nds_fifo fifo[2]; /* cola que envía cada CPU */

	struct nds_cart cart;
	struct nds_save save;
	uint8_t savemem[NDS_SAVE_MAX];
	struct nds_spi spi;
	struct nds_rtc rtc;

	/* tiempo y eventos */
	uint64_t time;    /* ciclos del ARM9 que el planificador ya dio por vividos */
	uint64_t ev[EV_COUNT];
	uint64_t ev_min;  /* caché: cota inferior del próximo evento (se afina al procesar eventos) */
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
	/* Contadores del planificador (los reinicia el dueño) y reloj opcional para repartir el tiempo de `nds_run*`:
	 * con `prof_clock` distinto de NULL se acumulan en `prof_*` los ticks que pasan dentro de cada ARM, de los
	 * eventos y fuera de ellos (el planificador). El reloj lo pone el dueño (en la PS3, el timebase). */
	struct {
		uint64_t slices, jumps, events;     /* tramos corridos, saltos de tiempo ocioso, eventos atendidos */
		uint64_t prof_a9, prof_a7, prof_ev; /* ticks dentro del ARM9, del ARM7 y de los eventos */
	} st;
	uint64_t (*prof_clock)(void);
	struct nds_video vid;
	struct nds_snd snd;
	uint32_t dbg; /* NDS_DBG_*: trazas de depuración por `log` (0 = ninguna; solo el corredor del host las activa) */
};

#define NDS_DBG_SPI 1u
#define NDS_DBG_IPC 2u
#define NDS_DBG_IRQ 4u
#define NDS_DBG_CART 8u
#define NDS_DBG_IO 16u /* todas las escrituras a la E/S */
#define NDS_DBG_GX 32u /* lecturas del ARM9 a los registros del motor de geometría (0x04000600-0x040006FF) */

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

/* Sonido (src/core/sound.c). `nds_snd_frame` se llama al cerrar cada cuadro de video. */
void nds_snd_init(struct nds *n);
void nds_snd_frame(struct nds *n);
/* Modo del mezclador (SND_MODE_*) y corrección de la razón de remuestreo (control de deriva). */
void nds_snd_set_mode(struct nds *n, uint32_t mode);
void nds_snd_set_ratio_adj(struct nds *n, float adj);

/* Guardado del cartucho. El tipo por omisión sale del código de juego (nds_load_rom); `nds_save_attach` carga el
 * contenido de un archivo y fija el tipo por su tamaño (0 = bien, -1 = tamaño que no es de ningún chip).
 * `nds_save_take_dirty` dice si el juego escribió desde la última vez; `nds_save_data` da el contenido actual. */
int nds_save_attach(struct nds *n, const uint8_t *data, size_t len);
const uint8_t *nds_save_data(const struct nds *n, size_t *len);
int nds_save_take_dirty(struct nds *n);

/* Entrada: botones NDS_KEY_*, y el toque en píxeles (0-255, 0-191) si NDS_KEY_PEN. */
void nds_set_input(struct nds *n, uint32_t keys, unsigned touch_x, unsigned touch_y);

/* Imagen de las dos pantallas del último cuadro completo, en RGB555 (bit 15 = 1), 256×192 cada una, con el
 * intercambio de POWCNT1 aplicado. En el host dibuja el cuadro a demanda con el renderizador 2D de src/gpu2d;
 * si hay `vid.ops.submit` (la PS3), las imágenes las dibuja ese renderizador y esto no se usa. */
void nds_screens(struct nds *n, uint16_t *top, uint16_t *bottom);

/* ---- Video (src/core/video.c) ------------------------------------------------------------------------- */
void nds_video_init(struct nds *n);
/* Línea 0: toma la instantánea del cuadro y empieza a anotar cambios. Línea 192: cierra el cuadro. */
void nds_video_frame_begin(struct nds *n);
void nds_video_frame_end(struct nds *n);
/* Último cuadro cerrado (o NULL si aún no hay): sus descriptores, sin `out_ea` si se dibuja a demanda. */
const gpu2d_frame *nds_video_last_frame(const struct nds *n, int *set);
/* Dibuja en el host el último cuadro cerrado (a las imágenes `vid.img`); no hace nada si ya está dibujado. */
void nds_video_render_host(struct nds *n);

/* Último cuadro de geometría entregado por SWAP_BUFFERS (lista de polígonos en pantalla para la rasterización 3D), o NULL. */
const gx_frame *nds_gx_front(const struct nds *n);

/* Acceso de depuración a la memoria vista por una CPU (sin efectos; 0 si no hay nada). */
uint32_t nds_peek32(struct nds *n, int cpu, uint32_t addr);
uint32_t nds_peek8(struct nds *n, int cpu, uint32_t addr);

void nds_logf(struct nds *n, const char *fmt, ...);
/* Traza de depuración: solo si `n->dbg` tiene el bit. Lleva el instante (ciclos del ARM9). */
#define NDS_TRACE(n, bit, ...) \
	do { \
		if (__builtin_expect((n)->dbg & (bit), 0)) \
			nds_tracef((n), __VA_ARGS__); \
	} while (0)
void nds_tracef(struct nds *n, const char *fmt, ...);

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
