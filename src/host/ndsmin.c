/*
 * SalsaDS — máquina mínima para correr las ROMs de prueba de CPU en el host
 * (ver ndsmin.h: no es el mapa de memoria de la Fase 1.2).
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "host/ndsmin.h"
#include "util/endian.h"

#define CPU9 0
#define CPU7 1

static int cpu_of(const struct ndsmin *m, const struct acore *c)
{
	return c == &m->a7 ? CPU7 : CPU9;
}

/* Puntero a la memoria que respalda `a` para esa CPU, o NULL (E/S, sin mapear). */
static uint8_t *mem_ptr(struct ndsmin *m, int cpu, uint32_t a)
{
	switch (a >> 24) {
	case 0x02:
		return &m->ram[a & (NDSMIN_RAM_SIZE - 1)];
	case 0x03:
		if (cpu == CPU9) {
			switch (m->wramcnt) {
			case 0: return &m->swram[a & 0x7FFF];
			case 1: return &m->swram[0x4000 + (a & 0x3FFF)];
			case 2: return &m->swram[a & 0x3FFF];
			default: return NULL;
			}
		}
		if (a >= 0x03800000u)
			return &m->wram7[a & 0xFFFF];
		switch (m->wramcnt) {
		case 0: return &m->wram7[a & 0xFFFF];
		case 1: return &m->swram[a & 0x3FFF];
		case 2: return &m->swram[0x4000 + (a & 0x3FFF)];
		default: return &m->swram[a & 0x7FFF];
		}
	case 0x05:
		return cpu == CPU9 ? &m->pal[a & 0x7FF] : NULL;
	case 0x06:
		if (cpu == CPU9 && a >= 0x06800000u && a - 0x06800000u < NDSMIN_VRAM_SIZE)
			return &m->vram[a - 0x06800000u];
		return NULL;
	case 0x07:
		return cpu == CPU9 ? &m->oam[a & 0x7FF] : NULL;
	default:
		return NULL;
	}
}

/* ---- E/S -------------------------------------------------------------------- */

static uint32_t line_of(const struct ndsmin *m)
{
	return (uint32_t)((m->a9.cycles / 2130) % 263);
}

/* Valor de la palabra de 32 bits que contiene el registro. */
static uint32_t io_word(struct ndsmin *m, int cpu, uint32_t w)
{
	switch (w) {
	case 0x004: { /* DISPSTAT + VCOUNT */
		const uint32_t line = line_of(m);
		const uint32_t vblank = (line >= 192 && line < 262) ? 1u : 0u;

		return vblank | (line << 16);
	}
	case 0x180: /* IPCSYNC: lo que escribió la otra CPU abajo, lo propio en los bits 8-11 */
		return (m->ipc_out[1 - cpu] & 0xF) | ((m->ipc_out[cpu] & 0xF) << 8);
	default:
		return rd32le(&m->io[cpu][w]);
	}
}

static uint32_t io_read(struct ndsmin *m, int cpu, uint32_t a, int size)
{
	const uint32_t off = a & 0xFFFFFF, w = off & ~3u;
	uint32_t v;

	if (off >= NDSMIN_IO_SIZE)
		return 0;
	if (m->trace_io && w != 0x004 && w != 0x180 && m->io_seen_n < 64) {
		uint32_t i;

		for (i = 0; i < m->io_seen_n; i++)
			if (m->io_seen[i] == (off | (uint32_t)cpu << 31))
				break;
		if (i == m->io_seen_n) {
			m->io_seen[m->io_seen_n++] = off | (uint32_t)cpu << 31;
			fprintf(stderr, "E/S ARM%d lee %#x (%d)\n", cpu ? 7 : 9, a, size);
		}
	}
	v = io_word(m, cpu, w) >> (8 * (off & 3));
	return size == 4 ? v : size == 2 ? v & 0xFFFF : v & 0xFF;
}

/* DMA inmediata (suficiente para los arranques de las ROMs de prueba): al escribir el control con el bit
 * de habilitación y el inicio inmediato, copia o rellena y borra el bit. La Fase 1.2 trae el DMA de verdad. */
static void dma_start(struct ndsmin *m, int cpu, unsigned ch)
{
	struct acore *c = cpu ? &m->a7 : &m->a9;
	uint8_t *io = m->io[cpu] + 0xB0 + 12 * ch;
	uint32_t src = rd32le(io), dst = rd32le(io + 4), cnt = rd32le(io + 8) & 0xFFFF, ctl = rd32le(io + 8) >> 16;
	const unsigned start = cpu ? (ctl >> 12) & 3 : (ctl >> 11) & 7, word = (ctl >> 10) & 1;
	const unsigned dctl = (ctl >> 5) & 3, sctl = (ctl >> 7) & 3;
	const int32_t step = word ? 4 : 2;

	if (!(ctl & 0x8000) || start != 0)
		return;
	if (cnt == 0)
		cnt = 0x10000;
	for (uint32_t i = 0; i < cnt; i++) {
		if (word)
			c->bus->write32(c, dst & ~3u, c->bus->read32(c, src & ~3u));
		else
			c->bus->write16(c, dst & ~1u, c->bus->read16(c, src & ~1u));
		src += sctl == 0 ? (uint32_t)step : sctl == 1 ? (uint32_t)-step : 0u;
		dst += (dctl == 0 || dctl == 3) ? (uint32_t)step : dctl == 1 ? (uint32_t)-step : 0u;
	}
	wr32le(io + 8, rd32le(io + 8) & ~0x80000000u);
}

static void io_write(struct ndsmin *m, int cpu, uint32_t a, int size, uint32_t v)
{
	const uint32_t off = a & 0xFFFFFF, w = off & ~3u, sh = 8 * (off & 3);
	const uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu << sh : 0xFFu << sh;
	uint32_t raw;

	if (off >= NDSMIN_IO_SIZE)
		return;
	raw = rd32le(&m->io[cpu][w]);
	raw = (raw & ~mask) | ((v << sh) & mask);
	wr32le(&m->io[cpu][w], raw);
	if (w >= 0xB8 && w < 0xB8 + 12 * 4 && (w - 0xB8) % 12 == 0 && (mask & 0x80000000u))
		dma_start(m, cpu, (w - 0xB8) / 12);
	else if (w == 0x180)
		m->ipc_out[cpu] = (raw >> 8) & 0xF;
	else if (w == 0x244 && cpu == CPU9 && (mask & 0xFF000000u))
		m->wramcnt = (raw >> 24) & 3; /* WRAMCNT en 0x04000247 */
}

/* ---- bus -------------------------------------------------------------------------- */

#define BUS_FN(name, size, type, rdexpr)                                                \
	static uint32_t name(struct acore *c, uint32_t a)                               \
	{                                                                               \
		struct ndsmin *m = c->user;                                             \
		const int cpu = cpu_of(m, c);                                           \
		uint8_t *p;                                                             \
		(void)type;                                                             \
		if ((a >> 24) == 4)                                                     \
			return io_read(m, cpu, a, size);                                \
		p = mem_ptr(m, cpu, a);                                                 \
		return p ? rdexpr : 0;                                                  \
	}

BUS_FN(bus_r8, 1, 0, *p)
BUS_FN(bus_r16, 2, 0, rd16le(p))
BUS_FN(bus_r32, 4, 0, rd32le(p))

static void bus_w8(struct acore *c, uint32_t a, uint32_t v)
{
	struct ndsmin *m = c->user;
	const int cpu = cpu_of(m, c);
	uint8_t *p;

	if ((a >> 24) == 4) {
		io_write(m, cpu, a, 1, v);
		return;
	}
	p = mem_ptr(m, cpu, a);
	if (p)
		*p = (uint8_t)v;
}

static void bus_w16(struct acore *c, uint32_t a, uint32_t v)
{
	struct ndsmin *m = c->user;
	const int cpu = cpu_of(m, c);
	uint8_t *p;

	if ((a >> 24) == 4) {
		io_write(m, cpu, a, 2, v);
		return;
	}
	p = mem_ptr(m, cpu, a);
	if (p)
		wr16le(p, (uint16_t)v);
}

static void bus_w32(struct acore *c, uint32_t a, uint32_t v)
{
	struct ndsmin *m = c->user;
	const int cpu = cpu_of(m, c);
	uint8_t *p;

	if ((a >> 24) == 4) {
		io_write(m, cpu, a, 4, v);
		return;
	}
	p = mem_ptr(m, cpu, a);
	if (p)
		wr32le(p, v);
}

static const struct acore_bus bus = {
	bus_r8, bus_r16, bus_r32, bus_w8, bus_w16, bus_w32, bus_r16, bus_r32,
};

/* ---- BIOS en HLE (lo mínimo para arrancar las ROMs de prueba) ----------------------------- */

static uint32_t cpu_rd32(struct acore *c, uint32_t a)
{
	return c->bus->read32(c, a & ~3u);
}

static void cpu_wr32(struct acore *c, uint32_t a, uint32_t v)
{
	c->bus->write32(c, a & ~3u, v);
}

/* SWI de la BIOS. En la ARM9 solo si los vectores son los de la BIOS (0xFFFF0000): si el programa
 * instaló los suyos, la SWI es una excepción de verdad. Devuelve 1 si la atendió. */
static int swi_hook(struct acore *c, uint32_t comment)
{
	const uint32_t fn = (c->cpsr & ACORE_T) ? comment & 0xFF : (comment >> 16) & 0xFF;

	if (c->v5 && c->vector_base != 0xFFFF0000u)
		return 0;
	switch (fn) {
	case 0x03: /* WaitByLoop */
		return 1;
	case 0x09: { /* Div: r0 / r1 -> r0 cociente, r1 resto, r3 |cociente| */
		const int32_t n = (int32_t)c->r[0], d = (int32_t)c->r[1];
		int32_t q = 0, r = n;

		if (d != 0 && !(n == INT32_MIN && d == -1)) {
			q = n / d;
			r = n % d;
		}
		c->r[0] = (uint32_t)q;
		c->r[1] = (uint32_t)r;
		c->r[3] = (uint32_t)(q < 0 ? -q : q);
		return 1;
	}
	case 0x0B: /* CpuSet */
	case 0x0C: { /* CpuFastSet */
		uint32_t src = c->r[0], dst = c->r[1];
		const uint32_t len = c->r[2] & 0x1FFFFF, fill = (c->r[2] >> 24) & 1;
		const int word = fn == 0x0C || ((c->r[2] >> 26) & 1);

		for (uint32_t i = 0; i < len; i++) {
			if (word) {
				cpu_wr32(c, dst, cpu_rd32(c, src));
				dst += 4;
				src += fill ? 0 : 4;
			} else {
				const uint32_t v = c->bus->read16(c, src & ~1u);

				c->bus->write16(c, dst & ~1u, v);
				dst += 2;
				src += fill ? 0 : 2;
			}
		}
		return 1;
	}
	case 0x0D: { /* Sqrt */
		uint32_t x = c->r[0], r = 0;

		for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
			if (x >= r + bit) {
				x -= r + bit;
				r = (r >> 1) + bit;
			} else {
				r >>= 1;
			}
		}
		c->r[0] = r;
		return 1;
	}
	case 0x0F: /* IsDebugger */
		c->r[0] = 0;
		return 1;
	default:
		return 0;
	}
}

/* ---- arranque ----------------------------------------------------------------------- */

void ndsmin_init(struct ndsmin *m)
{
	memset(m, 0, sizeof *m);
	acore_init(&m->a9, ACORE_ARM9, &bus, m);
	acore_attach_tcm(&m->a9, m->itcm, m->dtcm);
	acore_init(&m->a7, ACORE_ARM7, &bus, m);
	m->a9.swi_hook = swi_hook;
	m->a7.swi_hook = swi_hook;
	m->wramcnt = 3;
}

uint32_t ndsmin_peek32(struct ndsmin *m, int cpu, uint32_t addr)
{
	const uint8_t *p = mem_ptr(m, cpu, addr & ~3u);

	return p ? rd32le(p) : 0;
}

static void put_bytes(struct ndsmin *m, int cpu, uint32_t addr, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint8_t *p = mem_ptr(m, cpu, addr + i);

		if (p)
			*p = src[i];
	}
}

int ndsmin_boot(struct ndsmin *m, const uint8_t *rom, size_t len)
{
	uint32_t a9_off, a9_entry, a9_ram, a9_size, a7_off, a7_entry, a7_ram, a7_size;

	if (len < 0x200)
		return -1;
	a9_off = rd32le(rom + 0x20);
	a9_entry = rd32le(rom + 0x24);
	a9_ram = rd32le(rom + 0x28);
	a9_size = rd32le(rom + 0x2C);
	a7_off = rd32le(rom + 0x30);
	a7_entry = rd32le(rom + 0x34);
	a7_ram = rd32le(rom + 0x38);
	a7_size = rd32le(rom + 0x3C);
	if ((uint64_t)a9_off + a9_size > len || (uint64_t)a7_off + a7_size > len)
		return -1;

	/* copia de la cabecera donde la espera libnds (0x027FFE00) */
	put_bytes(m, CPU9, 0x027FFE00u, rom, 0x170);
	put_bytes(m, CPU9, a9_ram, rom + a9_off, a9_size);
	put_bytes(m, CPU7, a7_ram, rom + a7_off, a7_size);

	acore_set_cpsr(&m->a9, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	acore_set_pc(&m->a9, a9_entry);
	acore_set_cpsr(&m->a7, ACORE_MODE_SYS | ACORE_I | ACORE_F);
	acore_set_pc(&m->a7, a7_entry);
	return 0;
}

void ndsmin_run(struct ndsmin *m, uint64_t arm9_cycles)
{
	const uint64_t end = m->a9.cycles + arm9_cycles;

	while (m->a9.cycles < end) {
		acore_run(&m->a9, m->a9.cycles + 64);
		acore_run(&m->a7, m->a9.cycles / 2);
	}
}
