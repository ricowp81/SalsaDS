/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — interno de src/core: lo que comparten los archivos de la máquina. No lo incluye nadie más.
 */
#ifndef SALSADS_CORE_NDS_PRIV_H
#define SALSADS_CORE_NDS_PRIV_H

#include "core/nds.h"
#include "jit/arec.h"
#include "util/endian.h"

#if defined(__GNUC__)
#define NDS_LIKELY(x) __builtin_expect(!!(x), 1)
#define NDS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NDS_LIKELY(x) (x)
#define NDS_UNLIKELY(x) (x)
#endif

/* Escritura a memoria (puntero del host `p`): si algún recompilador tiene código traducido ahí, lo invalida. Sin JIT,
 * jit_phys es 0 y no hace nada. El espacio físico (desplazamiento desde `itcm`) es el mismo para los dos: el del ARM9
 * llega hasta el final de `swram` y el del ARM7 hasta el final de `vram`. */
static inline void NDS_CODE_WRITE(struct nds *n, const uint8_t *p, unsigned size)
{
	const uintptr_t off = (uintptr_t)p - (uintptr_t)n->itcm;

	if (NDS_UNLIKELY(off < n->jit_phys)) {
		if (n->jit9 && off < n->jit_phys9)
			arec_code_write(n->jit9, (uint32_t)off, size);
		if (n->jit7)
			arec_code_write(n->jit7, (uint32_t)off, size);
	}
}
void nds_code_write_range(struct nds *n, const uint8_t *p, size_t len);
void nds_jit_wramcnt(struct nds *n);
void nds_jit_reinit(struct nds *n);

static inline struct acore *nds_core(struct nds *n, int cpu)
{
	return cpu ? &n->a7 : &n->a9;
}

/* mem.c */
extern const struct acore_bus nds_bus9, nds_bus7, nds_bus7_diag;
void nds_mem_init(struct nds *n);
void nds_mem_wramcnt(struct nds *n, uint8_t v);
void nds_vramcnt_write(struct nds *n, unsigned bank, uint8_t v);
/* Lectura/escritura de bus (sin TCM) de `sz` bytes en la dirección alineada `a`, como las ve `cpu`. */
uint32_t nds_bus_read(struct nds *n, int cpu, uint32_t a, int sz);
void nds_bus_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v);

uint32_t nds_vram_bank_offset(unsigned bank);                           /* inicio del banco en el arreglo `vram` */
uint32_t nds_vram_view_offset(const struct nds *n, unsigned win, unsigned pg); /* página de una vista (0 fondo A, 1 fondo B, 2 OBJ A, 3 OBJ B) */

/* video.c */
void nds_video_reg_changed(struct nds *n, uint32_t io_offset);
void nds_video_pal_written(struct nds *n, uint32_t off);
void nds_video_oam_written(struct nds *n, uint32_t off);
void nds_video_vram_dirty(struct nds *n, uint32_t vram_off);
void nds_video_mmem_push(struct nds *n, uint16_t px);
void nds_video_line_start(struct nds *n, uint32_t line);

/* sound.c */
uint16_t nds_snd_read(struct nds *n, uint32_t off);
void nds_snd_write(struct nds *n, uint32_t off, uint16_t v, uint16_t mask);

/* io.c */
uint32_t nds_io_read(struct nds *n, int cpu, uint32_t a, int sz);
void nds_io_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v);

/* sched.c: eventos, IRQ, temporizadores, DMA, video */
void nds_sched_init(struct nds *n);
void nds_sched_run(struct nds *n, uint64_t until);
void nds_event_set(struct nds *n, int ev, uint64_t when);
/* Una CPU sondea un registro que solo cambia con el evento `ev` (el bit de ocupado del cartucho o del SPI): su reloj avanza de
 * una vez hasta el primer evento pendiente o, si ya venció dentro del tramo en curso, hasta el final del tramo (los eventos se
 * atienden recién ahí; hasta entonces el registro no cambia y la CPU solo giraría en el bucle del juego). */
void nds_poll_skip(struct nds *n, int cpu, int ev);
void nds_irq_update(struct nds *n, int cpu);
void nds_halt_check(struct nds *n);
void nds_cpu_halt(struct nds *n, int cpu); /* HALT: espera IE & IF (ARM7) o la línea de IRQ (ARM9) */
uint16_t nds_dispstat_read(struct nds *n, int cpu);
void nds_dispstat_write(struct nds *n, int cpu, uint16_t v, uint16_t mask);
void nds_hblank_refresh(struct nds *n); /* arma el evento de HBlank de la línea en curso si se acaba de pedir */
void nds_keys_changed(struct nds *n);
void nds_keycnt_write(struct nds *n, int cpu, uint16_t v, uint16_t mask);
uint16_t nds_timer_read(struct nds *n, int cpu, int i);
void nds_timer_write(struct nds *n, int cpu, int i, int hi, uint16_t v, uint16_t mask);
void nds_dma_write(struct nds *n, int cpu, unsigned reg, uint32_t v, uint32_t mask);
void nds_dma_trigger(struct nds *n, int cpu, unsigned mode);
#define NDS_DMA_VBLANK 1
#define NDS_DMA_HBLANK 2
#define NDS_DMA_DISPSTART 3
#define NDS_DMA_MMEM 4 /* ARM9: memoria principal a la FIFO del modo 3 */
#define NDS_DMA_CART 5 /* ARM9: modo 5; ARM7: modo 2 */
#define NDS_GX_PERIOD 1024u /* ciclos del ARM9 entre dos peticiones de la IRQ 21 (por nivel) */
#define NDS_GX_POLL 256u    /* ciclos del ARM9 entre dos miradas del evento EV_GX a la FIFO (sólo con IRQ 21 por nivel o una DMA de modo 7 esperando) */
#define NDS_DMA_GXFIFO 7 /* ARM9: FIFO de comandos de geometría */

/* geom.c */
void nds_gx_init(struct nds *n);
void nds_gx_fifo_write(struct nds *n, uint32_t w);
/* Ruta corta de la CPU (ARM9) a una palabra alineada de 0x04000400-0x040005FF: las mismas reglas que nds_io_write sin pasar por el bus. */
void nds_gx_cpu_write(struct nds *n, uint32_t a, uint32_t v);
/* Ráfaga de la DMA de modo 7: pasa a la FIFO hasta `count` palabras de `src` (little-endian, sin pasar por el bus) con las mismas
 * reglas, ciclos y paradas que una palabra a la vez; para cuando la FIFO llega a la mitad. Devuelve cuántas pasaron. */
uint32_t nds_gx_dma_burst(struct nds *n, const uint8_t *src, uint32_t count);

void nds_gx_port_write(struct nds *n, uint32_t off, uint32_t w); /* off = dirección - 0x04000000, 0x440-0x5CC */
uint32_t nds_gx_read32(struct nds *n, uint32_t off);              /* off = dirección - 0x04000000, 0x600-0x6A7 */
void nds_gx_gxstat_write(struct nds *n, uint32_t v, uint32_t mask);
int nds_gx_dma_ok(struct nds *n, uint64_t t);
void nds_gx_schedule(struct nds *n, uint32_t delay);
int nds_dma_waits(const struct nds *n, int cpu, unsigned mode);
void nds_gx_event(struct nds *n, uint64_t bt);
void nds_gx_vblank(struct nds *n, uint64_t t);
void nds_video_submit_pending(struct nds *n, uint32_t line); /* video.c: cada línea; entrega el 2D pendiente en la 262 (o en r3d_defer_line del cuadro siguiente) */

/* ipc.c */
void nds_ipc_init(struct nds *n);
uint16_t nds_ipcsync_read(struct nds *n, int cpu);
void nds_ipcsync_write(struct nds *n, int cpu, uint16_t v, uint16_t mask);
uint16_t nds_ipcfifocnt_read(struct nds *n, int cpu);
void nds_ipcfifocnt_write(struct nds *n, int cpu, uint16_t v, uint16_t mask);
void nds_ipcfifo_send(struct nds *n, int cpu, uint32_t v);
uint32_t nds_ipcfifo_recv(struct nds *n, int cpu);

/* cart.c */
int nds_cart_owner(const struct nds *n);
uint32_t nds_cart_read(struct nds *n, int cpu, uint32_t off, int sz);
void nds_cart_write(struct nds *n, int cpu, uint32_t off, int sz, uint32_t v);
uint32_t nds_cart_data(struct nds *n, int cpu); /* 0x04100010 */
void nds_cart_event(struct nds *n);
uint32_t nds_cart_chipid(const struct nds_cart *c);
void nds_cart_pread(struct nds_cart *c, uint32_t off, uint8_t *dst, uint32_t len); /* binarios de arranque: sin pasar por la caché */

/* save.c */
void nds_save_default(struct nds *n);
uint8_t nds_save_xfer(struct nds *n, uint8_t in);
void nds_save_deselect(struct nds *n);

/* spi.c */
void nds_spi_init(struct nds *n);
uint32_t nds_spi_read(struct nds *n, uint32_t off, int sz);
void nds_spi_write(struct nds *n, uint32_t off, int sz, uint32_t v);
void nds_spi_event(struct nds *n);
uint16_t nds_rtc_read(struct nds *n);
void nds_rtc_write(struct nds *n, uint16_t v, uint16_t mask);
void nds_fw_make(struct nds *n);

/* math.c */
uint32_t nds_math_read(struct nds *n, uint32_t off, int sz);
void nds_math_write(struct nds *n, uint32_t off, int sz, uint32_t v);

/* bios.c / boot.c */
void nds_bios_init(struct nds *n);
int nds_swi_hook(struct acore *c, uint32_t comment);
void nds_softreset(struct nds *n, int cpu);
uint32_t nds_crc16(uint32_t crc, const uint8_t *p, size_t len);

#endif
