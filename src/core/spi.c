/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — bus SPI del ARM7 (Fase 1.2): firmware, pantalla táctil, gestión de energía y el reloj de tiempo
 * real (RTC).
 *
 * El firmware es una imagen de 256 KB que genera el emulador (nada del firmware real): una cabecera con lo
 * justo y las dos copias de los datos de usuario (0x3FE00 y 0x3FF00) con nuestro apodo, calibración del
 * táctil y CRC16. La calibración se arma de forma que la fórmula del manual dé exactamente el píxel que el
 * emulador convierte a ADC: ADC = origen + píxel × paso (x: 0x120 + 14 px; y: 0x0B8 + 19 px).
 *
 * SPI: cada byte escrito en SPIDATA ocupa el bus 8 bits × el divisor de reloj y termina con un evento
 * (EV_SPI) que limpia ocupado y, si SPICNT.14, levanta la IRQ 23. Chip select: se mantiene mientras el bit
 * de retener (11) esté puesto; al soltarlo, el dispositivo vuelve a esperar un comando.
 *
 * El firmware se puede escribir (WREN, página 0Ah, programar 02h, borrar página DBh y sector D8h) y se lee de vuelta
 * como cambió, solo en la memoria de la emulación: algunos juegos (Mario Kart DS) escriben su configuración de
 * Wi-Fi al arrancar y la verifican leyéndola; si lo escrito no vuelve, repiten sin fin.
 *
 * Fuente: GBATEK (SPI, firmware y datos de usuario, TSC, gestión de energía, RTC). Código propio.
 */
#include <string.h>

#include "core/nds_priv.h"

#define TSC_X0 0x120
#define TSC_KX 14
#define TSC_Y0 0x0B8
#define TSC_KY 19

uint32_t nds_crc16(uint32_t crc, const uint8_t *p, size_t len)
{
	crc &= 0xFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xA001u : crc >> 1;
	}
	return crc;
}

/* ---- firmware generado ------------------------------------------------------------------------------ */

static void put_user_block(uint8_t *b, uint16_t counter)
{
	static const char nick[] = "SalsaDS";
	const uint16_t x1 = (uint16_t)(TSC_X0 + 31 * TSC_KX), y1 = (uint16_t)(TSC_Y0 + 31 * TSC_KY);
	const uint16_t x2 = (uint16_t)(TSC_X0 + 223 * TSC_KX), y2 = (uint16_t)(TSC_Y0 + 159 * TSC_KY);

	memset(b, 0xFF, 0x100);
	memset(b, 0, 0x70);
	wr16le(b + 0x00, 5);   /* versión */
	b[0x02] = 7;           /* color favorito */
	b[0x03] = 1;           /* cumpleaños: mes y día */
	b[0x04] = 1;
	for (int i = 0; nick[i]; i++)
		wr16le(b + 0x06 + 2 * i, (uint16_t)nick[i]);
	wr16le(b + 0x1A, 7);   /* longitud del apodo */
	wr16le(b + 0x50, 0);   /* mensaje vacío */
	wr16le(b + 0x58, x1);  /* calibración: punto 1 (ADC) */
	wr16le(b + 0x5A, y1);
	b[0x5C] = 32;          /* punto 1 (pantalla, desde 1) */
	b[0x5D] = 32;
	wr16le(b + 0x5E, x2);  /* punto 2 (ADC) */
	wr16le(b + 0x60, y2);
	b[0x62] = 224;         /* punto 2 (pantalla, desde 1) */
	b[0x63] = 160;
	wr16le(b + 0x64, 0xEC01); /* inglés; opciones «configuradas» */
	b[0x66] = 26;          /* año (2000 + 26) */
	wr16le(b + 0x70, counter);
	wr16le(b + 0x72, (uint16_t)nds_crc16(0xFFFF, b, 0x70));
	wr16le(b + 0xFE, (uint16_t)nds_crc16(0xFFFF, b + 0x74, 0x8A));
}

void nds_fw_make(struct nds *n)
{
	uint8_t *f = n->fw;

	memset(f, 0xFF, NDS_FW_SIZE);
	memset(f, 0, 0x200);
	memcpy(f + 0x08, "SDS\x01", 4);
	wr16le(f + 0x14, 0x2000);     /* tamaño del chip: 2 × 128 KB */
	f[0x1D] = 0xFF;               /* consola: DS */
	wr16le(f + 0x20, 0x3FE00 / 8); /* datos de usuario */
	f[0x36] = 0x02;               /* MAC de administración local: 02:00:00:00:00:01 */
	f[0x3B] = 0x01;
	put_user_block(f + 0x3FE00, 0);
	put_user_block(f + 0x3FF00, 1);
}

/* ---- dispositivos ------------------------------------------------------------------------------------ */

static uint8_t fw_xfer(struct nds *n, uint8_t in)
{
	struct nds_spi *s = &n->spi;
	static const uint8_t jedec[3] = { 0x20, 0x40, 0x12 }; /* ST M45PE20: 256 KB */

	switch (s->fw_state) {
	case 0: /* comando */
		s->fw_cmd = in;
		s->fw_addr = 0;
		s->fw_cnt = 0;
		if (in == 0x06) {
			s->fw_wel = 1;
			s->fw_state = 4;
		} else if (in == 0x04) {
			s->fw_wel = 0;
			s->fw_state = 4;
		} else if (in == 0x03 || in == 0x0B || in == 0x0A || in == 0x02 || in == 0xDB || in == 0xD8) {
			s->fw_state = 1;
			s->fw_cnt = 0;
		} else {
			s->fw_state = 3;
		}
		return 0;
	case 1: /* dirección de 3 bytes */
		s->fw_addr = (s->fw_addr << 8) | in;
		if (++s->fw_cnt == 3) {
			s->fw_addr &= NDS_FW_SIZE - 1u;
			s->fw_state = s->fw_cmd == 0x0B ? 2 : 3;
			s->fw_first = 1;
			if (s->fw_cmd == 0xDB && s->fw_wel) /* borrar página de 256 bytes */
				memset(n->fw + (s->fw_addr & ~0xFFu), 0xFF, 0x100);
			else if (s->fw_cmd == 0xD8 && s->fw_wel) /* borrar sector de 64 KB */
				memset(n->fw + (s->fw_addr & ~0xFFFFu), 0xFF, 0x10000);
		}
		return 0;
	case 2: /* byte ficticio de la lectura rápida */
		s->fw_state = 3;
		return 0;
	case 3:
		switch (s->fw_cmd) {
		case 0x03:
		case 0x0B:
			return n->fw[(s->fw_addr++) & (NDS_FW_SIZE - 1)];
		case 0x05:
			return (uint8_t)(s->fw_wel << 1);
		case 0x9F:
			return jedec[(s->fw_cnt++) % 3];
		case 0x0A: /* escribir página: borra la página y programa (la dirección da la vuelta dentro de ella) */
		case 0x02: /* programar página: solo baja bits (Y de lo que había) */
			if (s->fw_wel) {
				const uint32_t base = s->fw_addr & ~0xFFu;

				if (s->fw_first && s->fw_cmd == 0x0A)
					memset(n->fw + base, 0xFF, 0x100);
				s->fw_first = 0;
				if (s->fw_cmd == 0x0A)
					n->fw[s->fw_addr] = in;
				else
					n->fw[s->fw_addr] &= in;
				s->fw_addr = base | ((s->fw_addr + 1u) & 0xFFu);
			}
			return 0;
		default: /* borrados y el resto: nada que devolver */
			return 0;
		}
	default:
		return 0;
	}
}

static unsigned tsc_value(const struct nds *n, unsigned ch)
{
	const int pen = !(n->extkeyin & 0x40u);

	switch (ch) {
	case 1: return pen ? (unsigned)(TSC_Y0 + n->touch_y * TSC_KY) : 0xFFFu;
	case 5: return pen ? (unsigned)(TSC_X0 + n->touch_x * TSC_KX) : 0u;
	case 3: return pen ? 0x400u : 0u; /* Z1 */
	case 4: return pen ? 0x800u : 0u; /* Z2 */
	case 6: return 0x800u;            /* AUX (micrófono) en reposo */
	case 0: return 0x2F0u;            /* temperatura 0 */
	case 7: return 0x300u;            /* temperatura 1 */
	default: return 0;                /* batería (GND) */
	}
}

static uint8_t tsc_xfer(struct nds *n, uint8_t in)
{
	struct nds_spi *s = &n->spi;
	uint8_t out = 0;

	if (s->tsc_qn) {
		out = s->tsc_q[0];
		s->tsc_q[0] = s->tsc_q[1];
		s->tsc_qn--;
	}
	if (in & 0x80u) { /* bit de arranque: empieza una conversión nueva */
		unsigned v = tsc_value(n, (in >> 4) & 7u);

		if (in & 0x08u) { /* 8 bits */
			v >>= 4;
			s->tsc_q[0] = (uint8_t)(v >> 1);
			s->tsc_q[1] = (uint8_t)((v & 1u) << 7);
		} else {
			s->tsc_q[0] = (uint8_t)(v >> 5);
			s->tsc_q[1] = (uint8_t)((v & 0x1Fu) << 3);
		}
		s->tsc_qn = 2;
	}
	return out;
}

static uint8_t pm_xfer(struct nds *n, uint8_t in)
{
	struct nds_spi *s = &n->spi;

	if (s->pm_state == 0) {
		s->pm_reg = in;
		s->pm_state = 1;
		return 0;
	}
	if (s->pm_reg & 0x80u)
		return s->pm_regs[s->pm_reg & 7u];
	s->pm_regs[s->pm_reg & 7u] = in;
	if ((s->pm_reg & 0x7Fu) == 0 && (in & 0x40u)) {
		n->powered_off = 1;
		NDS_TRACE(n, NDS_DBG_SPI, "PM: apagar la consola (registro 0 = %02x)", in);
	}
	return 0;
}

static void dev_deselect(struct nds_spi *s, unsigned dev)
{
	if (dev == 0) {
		s->pm_state = 0;
	} else if (dev == 1) {
		/* WEL lo apagan WRDI y el final de una escritura o borrado, no soltar el chip select */
		if (s->fw_state == 3 && (s->fw_cmd == 0x0A || s->fw_cmd == 0x02 || s->fw_cmd == 0xDB || s->fw_cmd == 0xD8))
			s->fw_wel = 0;
		s->fw_state = 0;
	} else {
		s->tsc_qn = 0;
	}
}

/* ---- bus -------------------------------------------------------------------------------------------------- */

void nds_spi_init(struct nds *n)
{
	memset(&n->spi, 0, sizeof n->spi);
	n->spi.pm_regs[0] = 0x0D;  /* amplificador y las dos luces de fondo */
	n->spi.pm_regs[4] = 0x4B;  /* brillo máximo, alimentación externa, 0x4 en los bits altos */
	nds_fw_make(n);
}

uint32_t nds_spi_read(struct nds *n, uint32_t off, int sz)
{
	const struct nds_spi *s = &n->spi;
	uint8_t img[4] = { 0 };

	if (s->busy && off < 0x1C2u) /* sondeo de SPICNT.7: hasta el evento que termina la transferencia (nds_poll_skip) */
		nds_poll_skip(n, NDS_CPU7, EV_SPI);
	wr16le(img, (uint16_t)((s->cnt & 0xCF03u) | ((unsigned)s->busy << 7)));
	img[2] = s->data;
	if (off < 0x1C0u || off + (uint32_t)sz > 0x1C4u)
		return 0;
	{
		uint32_t v = 0;

		for (int i = 0; i < sz; i++)
			v |= (uint32_t)img[off - 0x1C0u + (uint32_t)i] << (8 * i);
		return v;
	}
}

void nds_spi_write(struct nds *n, uint32_t off, int sz, uint32_t v)
{
	struct nds_spi *s = &n->spi;

	for (int i = 0; i < sz; i++) {
		const uint32_t o = off + (uint32_t)i - 0x1C0u;
		const uint8_t b = (uint8_t)(v >> (8 * i));

		if (o == 0) {
			s->cnt = (uint16_t)((s->cnt & ~0x0003u) | (b & 0x03u));
		} else if (o == 1) {
			s->cnt = (uint16_t)((s->cnt & 0x00FFu) | ((b & 0xCFu) << 8));
		} else if (o == 2) {
			unsigned dev = (s->cnt >> 8) & 3u;
			unsigned cycles;

			if (!(s->cnt & 0x8000u) || s->busy)
				continue;
			s->cur_dev = (uint8_t)dev;
			s->cur_hold = (uint8_t)((s->cnt >> 11) & 1u);
			s->data_next = dev == 0 ? pm_xfer(n, b) : dev == 1 ? fw_xfer(n, b) : dev == 2 ? tsc_xfer(n, b) : 0;
			NDS_TRACE(n, NDS_DBG_SPI, "SPI dev %u hold %u out %02x in %02x", dev, (unsigned)s->cur_hold, b, s->data_next);
			s->busy = 1;
			cycles = 8u * (8u << (s->cnt & 3u)); /* bits × ciclos de bus por bit (4 MHz = 8,4) */
			nds_event_set(n, EV_SPI, nds_cpu_now(n, NDS_CPU7) + (uint64_t)cycles * 2u);
		}
	}
}

void nds_spi_event(struct nds *n)
{
	struct nds_spi *s = &n->spi;

	s->busy = 0;
	s->data = s->data_next;
	if (!s->cur_hold)
		dev_deselect(s, s->cur_dev);
	if (s->cnt & 0x4000u)
		nds_irq_raise(n, NDS_CPU7, NDS_IRQ_SPI);
}

/* ---- RTC ---------------------------------------------------------------------------------------------------- */

static uint8_t bcd(unsigned v)
{
	return (uint8_t)(((v / 10) << 4) | (v % 10));
}

/* Hora actual de la emulación en campos (desde 2000-01-01, sábado). */
static void rtc_now(const struct nds *n, unsigned f[7])
{
	static const unsigned mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const uint64_t secs = n->rtc.base_sec + n->time / 67027964u;
	uint64_t days = secs / 86400u;
	const unsigned sod = (unsigned)(secs % 86400u);
	unsigned year = 0, mon = 0;

	f[3] = (unsigned)((days + 6) % 7); /* 2000-01-01 fue sábado; 0 = domingo */
	for (;;) {
		const unsigned ylen = (year % 4 == 0) ? 366u : 365u;

		if (days < ylen)
			break;
		days -= ylen;
		year++;
	}
	for (mon = 0; mon < 12; mon++) {
		unsigned ml = mdays[mon] + ((mon == 1 && year % 4 == 0) ? 1u : 0u);

		if (days < ml)
			break;
		days -= ml;
	}
	f[0] = year % 100;
	f[1] = mon + 1;
	f[2] = (unsigned)days + 1;
	f[4] = sod / 3600;
	f[5] = (sod / 60) % 60;
	f[6] = sod % 60;
}

static uint8_t hour_byte(const struct nds_rtc *r, unsigned h)
{
	if (r->status1 & 2u) /* 24 horas: el bit 6 indica PM */
		return (uint8_t)(bcd(h) | (h >= 12 ? 0x40u : 0u));
	return (uint8_t)(bcd(h % 12) | (h >= 12 ? 0x40u : 0u));
}

static void rtc_prepare_read(struct nds *n, unsigned idx)
{
	struct nds_rtc *r = &n->rtc;
	unsigned f[7];

	rtc_now(n, f);
	r->nout = 0;
	r->outbit = 0;
	switch (idx) {
	case 0: /* estado 1: los indicadores 4-7 se borran al leer */
		r->out[0] = r->status1;
		r->status1 &= 0x0Fu;
		r->nout = 1;
		break;
	case 4:
		r->out[0] = r->status2;
		r->nout = 1;
		break;
	case 2:
		r->out[0] = bcd(f[0]);
		r->out[1] = bcd(f[1]);
		r->out[2] = bcd(f[2]);
		r->out[3] = (uint8_t)f[3];
		r->out[4] = hour_byte(r, f[4]);
		r->out[5] = bcd(f[5]);
		r->out[6] = bcd(f[6]);
		r->nout = 7;
		break;
	case 6:
		r->out[0] = hour_byte(r, f[4]);
		r->out[1] = bcd(f[5]);
		r->out[2] = bcd(f[6]);
		r->nout = 3;
		break;
	case 3:
		r->out[0] = r->clkadj;
		r->nout = 1;
		break;
	case 7:
		r->out[0] = r->freereg;
		r->nout = 1;
		break;
	case 1:
	case 5:
		r->out[0] = r->out[1] = r->out[2] = 0;
		r->nout = idx == 1 ? 1 : 3;
		break;
	}
}

static unsigned rtc_write_len(unsigned idx)
{
	switch (idx) {
	case 2: return 7;
	case 6: return 3;
	case 5: return 3;
	default: return 1;
	}
}

static void rtc_apply_write(struct nds *n)
{
	struct nds_rtc *r = &n->rtc;

	switch (r->idx) {
	case 0:
		if (r->in[0] & 1u) { /* reinicio */
			r->status1 = 0;
			r->status2 = 0;
		}
		r->status1 = (uint8_t)((r->status1 & 0xF0u) | (r->in[0] & 0x0Eu));
		break;
	case 4: r->status2 = r->in[0] & 0x7Fu; break;
	case 3: r->clkadj = r->in[0]; break;
	case 7: r->freereg = r->in[0]; break;
	default: break; /* fecha, hora y alarmas: se ignoran (el reloj corre solo) */
	}
}

uint16_t nds_rtc_read(struct nds *n)
{
	const struct nds_rtc *r = &n->rtc;
	uint16_t v = r->pins;

	if (!(v & 0x10u)) /* dirección de lectura: el bit de datos lo pone el chip */
		v = (uint16_t)((v & ~1u) | (r->sio_out & 1u));
	return v;
}

void nds_rtc_write(struct nds *n, uint16_t v, uint16_t mask)
{
	struct nds_rtc *r = &n->rtc;
	const uint16_t old = r->pins;
	const uint16_t nw = (uint16_t)((old & ~mask) | (v & mask));
	const unsigned cs0 = (old >> 2) & 1u, cs = (nw >> 2) & 1u;
	const unsigned sck0 = (old >> 1) & 1u, sck = (nw >> 1) & 1u;
	const unsigned sio = nw & 1u;

	r->pins = nw;
	if (!cs) { /* sin chip select: reinicia la conversación */
		r->state = 0;
		r->nbits_in = 0;
		r->bits_in = 0;
		r->nin = 0;
		r->outbit = 0;
		return;
	}
	if (!cs0)
		r->state = 0, r->nbits_in = 0, r->bits_in = 0;
	if (sck && !sck0) { /* flanco de subida: el chip muestrea */
		if (r->state == 0) {
			r->bits_in |= (uint8_t)(sio << r->nbits_in);
			if (++r->nbits_in == 8) {
				const uint8_t cmd = r->bits_in;

				r->nbits_in = 0;
				r->bits_in = 0;
				if ((cmd & 0x0Fu) == 0x06u) {
					r->idx = (cmd >> 4) & 7u;
					if (cmd & 0x80u) {
						rtc_prepare_read(n, r->idx);
						r->state = 2;
					} else {
						r->want_in = (uint8_t)rtc_write_len(r->idx);
						r->nin = 0;
						r->state = 1;
					}
				} else {
					r->state = 3; /* comando desconocido: se ignora */
				}
			}
		} else if (r->state == 1) {
			r->bits_in |= (uint8_t)(sio << r->nbits_in);
			if (++r->nbits_in == 8) {
				r->in[r->nin++] = r->bits_in;
				r->nbits_in = 0;
				r->bits_in = 0;
				if (r->nin == r->want_in) {
					rtc_apply_write(n);
					r->state = 3;
				}
			}
		}
	} else if (!sck && sck0) { /* flanco de bajada: el chip presenta el siguiente bit */
		if (r->state == 2 && (r->outbit >> 3) < r->nout) {
			r->sio_out = (r->out[r->outbit >> 3] >> (r->outbit & 7u)) & 1u;
			r->outbit++;
		}
	}
}
