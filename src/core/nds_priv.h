/*
 * SalsaDS — interno de src/core: lo que comparten los archivos de la máquina. No lo incluye nadie más.
 */
#ifndef SALSADS_CORE_NDS_PRIV_H
#define SALSADS_CORE_NDS_PRIV_H

#include "core/nds.h"
#include "util/endian.h"

#if defined(__GNUC__)
#define NDS_LIKELY(x) __builtin_expect(!!(x), 1)
#define NDS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NDS_LIKELY(x) (x)
#define NDS_UNLIKELY(x) (x)
#endif

/* Sitio donde la Fase 4 invalidará bloques recompilados cuando una escritura toque código. Hoy no hace nada. */
#define NDS_CODE_WRITE(n, a, size) ((void)(n), (void)(a), (void)(size))

static inline struct acore *nds_core(struct nds *n, int cpu)
{
	return cpu ? &n->a7 : &n->a9;
}

/* mem.c */
extern const struct acore_bus nds_bus9, nds_bus7;
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

/* io.c */
uint32_t nds_io_read(struct nds *n, int cpu, uint32_t a, int sz);
void nds_io_write(struct nds *n, int cpu, uint32_t a, int sz, uint32_t v);

/* sched.c: eventos, IRQ, temporizadores, DMA, video */
void nds_sched_init(struct nds *n);
void nds_sched_run(struct nds *n, uint64_t until);
void nds_event_set(struct nds *n, int ev, uint64_t when);
void nds_irq_update(struct nds *n, int cpu);
void nds_halt_check(struct nds *n);
void nds_cpu_halt(struct nds *n, int cpu); /* HALT: espera IE & IF (ARM7) o la línea de IRQ (ARM9) */
uint16_t nds_dispstat_read(struct nds *n, int cpu);
void nds_dispstat_write(struct nds *n, int cpu, uint16_t v, uint16_t mask);
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
#define NDS_DMA_GXFIFO 7 /* ARM9: FIFO de comandos de geometría */

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
