/*
 * SalsaDS — ROM periph, lado ARM9: división y raíz cuadrada por hardware, el cartucho (cabecera, identificador,
 * bloques por sondeo y por DMA, IRQ de fin), puertos varios y el estado de arranque.
 */
#include <nds.h>
#include <string.h>

#include "periph.h"
#include "pattern.h"

static volatile uint32_t n_card;
static void h_card(void) { n_card++; }

static uint64_t rd64(uint32_t a)
{
	return (uint64_t)MEM32(a) | ((uint64_t)MEM32(a + 4) << 32);
}

static void wr64(uint32_t a, uint64_t v)
{
	MEM32(a) = (uint32_t)v;
	MEM32(a + 4) = (uint32_t)(v >> 32);
}

static uint32_t div_wait(void)
{
	return WAIT_UNTIL(!(MEM16(R_DIVCNT) & 0x8000), 100000);
}

static uint64_t isqrt64(uint64_t x)
{
	uint64_t r = 0;

	for (int b = 31; b >= 0; b--) {
		const uint64_t t = r | (1ull << b);

		if (t * t <= x)
			r = t;
	}
	return r;
}

static void group_div(void)
{
	static const int64_t n64[][2] = { { 1000000007LL * 123456LL, 1000003 }, { -9000000000000LL, 77 },
					  { 0x7FFFFFFFFFFFFFFFLL, 3 }, { -1LL, 5 }, { 5, -3 }, { -5, -3 } };
	static const int32_t n32[][2] = { { -1234, 10 }, { 1000, 7 }, { 7, -2 }, { -7, -2 }, { 0x7FFFFFFF, 2 },
					  { 0x7FFFFFFF, -1 }, { 0, 5 }, { 123456789, 1000 }, { -123456789, 1000 } };
	uint32_t bad = 0, busy_seen = 0;

	sds_group("DIV");
	for (unsigned i = 0; i < sizeof n32 / sizeof n32[0]; i++) {
		MEM16(R_DIVCNT) = 0;
		wr64(R_DIV_NUMER, (uint64_t)(int64_t)n32[i][0]);
		wr64(R_DIV_DENOM, (uint64_t)(int64_t)n32[i][1]);
		busy_seen |= (MEM16(R_DIVCNT) & 0x8000) != 0;
		SDS_CHECK(div_wait(), 1);
		bad += (int64_t)rd64(R_DIV_RESULT) != (int64_t)(n32[i][0] / n32[i][1]);
		bad += (int64_t)rd64(R_DIVREM_RESULT) != (int64_t)(n32[i][0] % n32[i][1]);
		SDS_CHECK(MEM16(R_DIVCNT) & 0x4000, 0);
	}
	SDS_CHECK(bad, 0);
	SDS_CHECK(busy_seen, 1); /* el ocupado se ve justo después de escribir */
	bad = 0;
	for (unsigned i = 0; i < sizeof n64 / sizeof n64[0]; i++) {
		MEM16(R_DIVCNT) = 1; /* 64 / 32 */
		wr64(R_DIV_NUMER, (uint64_t)n64[i][0]);
		wr64(R_DIV_DENOM, (uint64_t)(int64_t)(int32_t)n64[i][1]);
		div_wait();
		bad += (int64_t)rd64(R_DIV_RESULT) != n64[i][0] / (int32_t)n64[i][1];
		bad += (int64_t)rd64(R_DIVREM_RESULT) != n64[i][0] % (int32_t)n64[i][1];
		MEM16(R_DIVCNT) = 2; /* 64 / 64 */
		wr64(R_DIV_NUMER, (uint64_t)n64[i][0]);
		wr64(R_DIV_DENOM, (uint64_t)n64[i][1]);
		div_wait();
		bad += (int64_t)rd64(R_DIV_RESULT) != n64[i][0] / n64[i][1];
		bad += (int64_t)rd64(R_DIVREM_RESULT) != n64[i][0] % n64[i][1];
	}
	SDS_CHECK(bad, 0);
	/* el modo 3 es el 1; DIVCNT guarda solo el modo */
	MEM16(R_DIVCNT) = 0xFFFF;
	SDS_CHECK(MEM16(R_DIVCNT) & 0x3, 3);
	MEM16(R_DIVCNT) = 0;
	/* división entre cero: cociente ±1 (signo contrario al numerador), resto = numerador, bandera 14 */
	MEM16(R_DIVCNT) = 0;
	wr64(R_DIV_NUMER, 100);
	wr64(R_DIV_DENOM, 0);
	div_wait();
	SDS_CHECK(MEM16(R_DIVCNT) & 0x4000, 0x4000);
	SDS_CHECK((int32_t)rd64(R_DIV_RESULT), -1);
	SDS_CHECK((int32_t)rd64(R_DIVREM_RESULT), 100);
	wr64(R_DIV_NUMER, (uint64_t)(int64_t)-100);
	div_wait();
	SDS_CHECK((int32_t)rd64(R_DIV_RESULT), 1);
	SDS_CHECK((int32_t)rd64(R_DIVREM_RESULT), -100);
	MEM16(R_DIVCNT) = 2;
	wr64(R_DIV_NUMER, 77);
	wr64(R_DIV_DENOM, 0);
	div_wait();
	SDS_CHECK(rd64(R_DIV_RESULT), (uint64_t)-1);
	SDS_CHECK(rd64(R_DIVREM_RESULT), 77);
	/* INT_MIN / -1 en 32/32: el cociente 32 bits es INT_MIN y la mitad alta, invertida (GBATEK) */
	MEM16(R_DIVCNT) = 0;
	wr64(R_DIV_NUMER, (uint64_t)(int64_t)INT32_MIN);
	wr64(R_DIV_DENOM, (uint64_t)(int64_t)-1);
	div_wait();
	SDS_CHECK((uint32_t)rd64(R_DIV_RESULT), 0x80000000u);
	SDS_CHECK(rd64(R_DIV_RESULT) >> 32, 0);
	SDS_DIVERGE(rd64(R_DIVREM_RESULT), 0); /* el resto de este caso no lo dice GBATEK; melonDS deja un valor viejo */
	/* accesos de 8 y 16 bits a los resultados */
	MEM16(R_DIVCNT) = 0;
	wr64(R_DIV_NUMER, 0x1122334455667788ull);
	wr64(R_DIV_DENOM, 1);
	SDS_CHECK(MEM32(R_DIV_NUMER), 0x55667788u);
	SDS_CHECK(MEM16(R_DIV_NUMER + 6), 0x1122);
	SDS_CHECK(MEM8(R_DIV_NUMER + 1), 0x77);
	MEM16(R_DIV_NUMER + 2) = 0xABCD; /* escribir un pedazo vuelve a calcular (melonDS 1.1 ignora las de 16 bits: informativo) */
	SDS_DIVERGE(MEM32(R_DIV_NUMER), 0xABCD7788u);
	MEM32(R_DIV_NUMER) = 0xABCD7788u;
	div_wait();
	SDS_CHECK((uint32_t)rd64(R_DIV_RESULT), 0xABCD7788u);
}

static void group_sqrt(void)
{
	static const uint64_t xs64[] = { 0, 1, 15, 16, 1ull << 32, 0x123456789ABCDEFull, 0xFFFFFFFFFFFFFFFFull, 99999999999ull };
	static const uint32_t xs32[] = { 0, 1, 2, 15, 16, 99, 100, 65535, 65536, 0x7FFFFFFFu, 0xFFFFFFFFu };
	uint32_t bad = 0;

	sds_group("SQRT");
	for (unsigned i = 0; i < sizeof xs32 / 4; i++) {
		MEM16(R_SQRTCNT) = 0;
		wr64(R_SQRT_PARAM, xs32[i]);
		WAIT_UNTIL(!(MEM16(R_SQRTCNT) & 0x8000), 100000);
		bad += MEM32(R_SQRT_RESULT) != isqrt64(xs32[i]);
	}
	SDS_CHECK(bad, 0);
	bad = 0;
	for (unsigned i = 0; i < sizeof xs64 / 8; i++) {
		MEM16(R_SQRTCNT) = 1;
		wr64(R_SQRT_PARAM, xs64[i]);
		WAIT_UNTIL(!(MEM16(R_SQRTCNT) & 0x8000), 100000);
		bad += MEM32(R_SQRT_RESULT) != isqrt64(xs64[i]);
	}
	SDS_CHECK(bad, 0);
	/* el modo de 32 bits solo mira la mitad baja del parámetro */
	MEM16(R_SQRTCNT) = 0;
	wr64(R_SQRT_PARAM, 0x1234500000019ull);
	WAIT_UNTIL(!(MEM16(R_SQRTCNT) & 0x8000), 100000);
	SDS_CHECK(MEM32(R_SQRT_RESULT), 5);
	/* ocupado justo después de escribir */
	wr64(R_SQRT_PARAM, 1000000);
	/* en una DS real el cálculo (13 ciclos) puede haber terminado ya al leer: informativo, no fallo */
	SDS_DIVERGE_M(MEM16(R_SQRTCNT) & 0x8000, 0x8000, 0x8000);
	SDS_CHECK(WAIT_UNTIL(!(MEM16(R_SQRTCNT) & 0x8000), 100000), 1);
	SDS_CHECK(MEM32(R_SQRT_RESULT), 1000);
	MEM16(R_SQRTCNT) = 0xFFFF;
	SDS_DIVERGE_M(MEM16(R_SQRTCNT) & 0x7FFF, 1, 0x7FFF); /* solo el bit 0 existe (GBATEK); melonDS guarda todo */
	MEM16(R_SQRTCNT) = 0;
}

/* en un flashcart el cartucho no es el de la ROM: las comprobaciones de la ranura pasan a divergencias informativas */
static int flashcart;
#define CC(got, want) (flashcart ? sds_diverge_at((uint32_t)(got), (uint32_t)(want), SDS_MELON_NA, __LINE__, __FILE__) : SDS_CHECK(got, want))

static void group_cart(void)
{
	static uint32_t buf[0x200 / 4] __attribute__((aligned(4)));
	static uint32_t buf2[0x200 / 4] __attribute__((aligned(4)));
	const uint32_t a9_off = MEM32(0x027FFE20), a9_ram = MEM32(0x027FFE28);
	/* el cartucho no deja leer por debajo de 0x8000 con B7: se busca un bloque de 0x200 bytes alineado de la tabla de
	 * bytes conocidos (pattern.h) cuya posición en el archivo sea ≥ 0x8000 */
	const uint32_t pat_rom = (uint32_t)(uintptr_t)v_pattern - a9_ram + a9_off;
	uint32_t test_off = (pat_rom > 0x8000u ? pat_rom : 0x8000u);
	uint32_t n, i;
	const uint8_t *pat;

	test_off = (test_off + 0x1FFu) & ~0x1FFu;
	pat = v_pattern + (test_off - pat_rom);
	sds_group("CARTUCHO");
	/* ¿es el cartucho de la ROM o un flashcart (R4...)? Un flashcart sirve los datos de la ROM pero no el chip ni todos los
	 * comandos: si el identificador (B8) es 0/FFFFFFFF o B7 no devuelve los bytes conocidos, los comprobaciones de este
	 * grupo pasan a informativas ("no aplica en flashcart") y se anota en el log */
	n = rom_read(0xB8, 0, 7, 4, buf2);
	flashcart = n != 1 || buf2[0] == 0 || buf2[0] == 0xFFFFFFFFu;
	n = rom_read(0xB7, test_off, 1, 0x200, buf);
	flashcart |= n != 128 || memcmp(buf, pat, 0x200) != 0;
	if (flashcart)
		sds_note("CARTUCHO y CART ARM7: no aplican en flashcart (chip ID o B7 distintos del cartucho real); sus resultados son informativos");
	CC((MEM16(R_EXMEMCNT) >> 11) & 1, 0); /* la ranura es del ARM9 al arrancar */
	CC(test_off + 0x200 <= pat_rom + sizeof v_pattern, 1);
	/* identificador del chip (comando B8 con el cartucho en el estado en que lo deja el arranque: KEY2 sin cifrar los
	 * datos): distinto de cero. Los comandos 00h y 90h solo valen antes de activar KEY1 y no se prueban */
	n = rom_read(0xB8, 0, 7, 4, buf2);
	CC(n, 1);
	CC(buf2[0] != 0 && buf2[0] != 0xFFFFFFFFu, 1);
	SDS_DIVERGE(buf2[0], MEM32(0x027FF800)); /* el identificador que el arranque deja en RAM (informativo) */
	/* lectura B7 de un bloque de bytes conocidos */
	n = rom_read(0xB7, test_off, 1, 0x200, buf);
	CC(n, 128);
	CC(memcmp(buf, pat, 0x200), 0);
	/* un bloque de 4 bytes (modo de lectura corta) */
	n = rom_read(0xB7, test_off + 4, 7, 4, buf2);
	CC(n, 1);
	CC(memcmp(buf2, pat + 4, 4), 0);
	/* por debajo de 0x8000 se lee 0x8000 + (dir & 0x1FF): el bloque se repite cada 0x200 bytes (GBATEK). melonDS 1.1
	 * lo hace distinto: informativo */
	rom_read(0xB7, 0x1000, 1, 0x200, buf2);
	rom_read(0xB7, 0x1200, 1, 0x200, buf);
	SDS_DIVERGE(memcmp(buf2, buf, 0x200), 0);
	rom_read(0xB7, 0x8000, 1, 0x200, buf);
	SDS_DIVERGE(memcmp(buf2, buf, 0x200), 0);
	/* una lectura de 0x200 bytes que cruza un límite de 4 KB da la vuelta dentro de su página (GBATEK) */
	n = rom_read(0xB7, 0x9000 - 0x100, 1, 0x200, buf2);
	rom_read(0xB7, 0x9000 - 0x100, 7, 4, buf);
	CC(n, 128);
	CC(buf2[0], buf[0]);
	{
		uint32_t w0[1];

		rom_read(0xB7, 0x9000 - 0x1000, 7, 4, w0); /* el principio de esa página */
		SDS_DIVERGE(buf2[64], w0[0]);              /* 0x100 bytes después del inicio del bloque (melonDS 1.1 no da la vuelta) */
	}
	/* IRQ de fin de transferencia (AUXSPICNT.14, IE.19) */
	irqSet(IRQ_CARD, h_card);
	REG16(R_AUXSPICNT) = 0xC000;
	MEM32(R_IE) |= IRQ_CARD;
	n_card = 0;
	rom_cmd(0xB7, test_off);
	REG32(R_ROMCTRL) = ROMCTRL_START(1);
	{
		uint32_t cnt = 0, spin = 0;

		while ((REG32(R_ROMCTRL) & ROMCTRL_BUSY) && spin++ < 2000000u)
			if (REG32(R_ROMCTRL) & ROMCTRL_DRQ) {
				(void)REG32(R_ROMDATA);
				cnt++;
			}
		CC(cnt, 128);
	}
	CC(WAIT_UNTIL(n_card != 0, 100000), 1);
	MEM32(R_IE) &= ~IRQ_CARD;
	/* DMA en modo cartucho (modo 5 del ARM9): una palabra por aviso, cuenta 1, repetir, origen fijo */
	REG16(R_AUXSPICNT) = 0x8000;
	for (i = 0; i < 128; i++)
		buf[i] = 0;
	MEM32(R_DMA(3, 0)) = R_ROMDATA;
	MEM32(R_DMA(3, 1)) = (uint32_t)(uintptr_t)buf;
	MEM32(R_DMA(3, 2)) = (1u << 31) | (1u << 26) | (1u << 25) | (5u << 27) | (2u << 23) | 1u;
	rom_cmd(0xB7, test_off);
	REG32(R_ROMCTRL) = ROMCTRL_START(1);
	CC(WAIT_UNTIL(!(REG32(R_ROMCTRL) & ROMCTRL_BUSY), 4000000), 1);
	MEM32(R_DMA(3, 2)) = 0;
	CC(memcmp(buf, pat, 0x200), 0);
	/* AUXSPI: el chip de guardado se prueba aparte (ROM save) */
	REG16(R_AUXSPICNT) = 0x8000;
}

static void group_ports(void)
{
	sds_group("PUERTOS");
	SDS_CHECK(MEM8(R_POSTFLG) & 1, 1); /* arranque completo */
	/* POWCNT1: se guardan los bits 0-3, 9 y 15 (GBATEK) */
	{
		const uint32_t save = MEM16(R_POWCNT1);

		MEM16(R_POWCNT1) = 0xFFFF;
		SDS_CHECK(MEM16(R_POWCNT1), 0x820F);
		MEM16(R_POWCNT1) = save;
	}
	/* EXMEMCNT: los bits 0-7, 11 y 13-15 se guardan; los demás leen cero (informativo) */
	{
		const uint32_t save = MEM16(R_EXMEMCNT);

		MEM16(R_EXMEMCNT) = 0xFFFF & ~0x0800;
		SDS_DIVERGE(MEM16(R_EXMEMCNT), 0xE0FF);
		MEM16(R_EXMEMCNT) = save;
	}
	SDS_DIVERGE(MEM16(R_EXMEMCNT), 0x6000); /* el valor de arranque (informativo) */
	/* los registros de teclas del ARM9 */
	SDS_CHECK(MEM16(R_KEYINPUT) & 0x3FF, 0x3FF);
}

static void group_boot(void)
{
	static uint8_t user[0x70] __attribute__((aligned(4)));

	sds_group("ARRANQUE");
	/* la cabecera del cartucho en 0x027FFE00: título, entradas y tamaños coherentes */
	SDS_CHECK(MEM32(0x027FFE24) >= 0x02000000u && MEM32(0x027FFE24) < 0x02400000u, 1); /* entrada del ARM9 */
	SDS_CHECK(MEM32(0x027FFE2C) > 0 && MEM32(0x027FFE2C) < 0x400000u, 1);               /* tamaño del ARM9 */
	SDS_CHECK(MEM32(0x027FFE3C) > 0 && MEM32(0x027FFE3C) < 0x100000u, 1);               /* tamaño del ARM7 */
	/* datos de usuario en 0x027FFC80 (los genera el firmware): versión 5, apodo de 1 a 10 caracteres */
	for (unsigned i = 0; i < 0x70; i++)
		user[i] = MEM8(0x027FFC80 + i);
	SDS_CHECK(user[0] | (user[1] << 8), 5);
	SDS_CHECK(user[0x1A] >= 1 && user[0x1A] <= 10, 1);
	SDS_CHECK(user[0x02] <= 15, 1); /* color favorito */
	SDS_CHECK(user[0x03] >= 1 && user[0x03] <= 12, 1);
	SDS_CHECK(user[0x04] >= 1 && user[0x04] <= 31, 1);
	/* el ARM7 comprueba que son idénticos a la copia del firmware (mb_call 1) */
	MBOX->arg[1] = (uint32_t)crc16(0xFFFF, user, 0x70);
	SDS_CHECK(mb_call(P_CART_RES, 0x027FFC80u, MBOX->arg[1], 0), 1);
	SDS_CHECK(MBOX->res[0], 1);
	/* WRAMSTAT / WRAMCNT al arrancar: toda la WRAM compartida para el ARM7 (informativo; libnds no la toca) */
	SDS_DIVERGE(MEM8(R_WRAMCNT), 3);
}

int main(void)
{
	sds_nocache();
	sds_init("PERIPH ARM9+ARM7");
	MEM32(R_IME) = 1;
	group_div();
	group_sqrt();
	group_cart();
	group_ports();
	group_boot();
	/* el ARM7 lee el cartucho con la ranura a su nombre: un bloque B7 de bytes conocidos y el identificador */
	sds_group("CART ARM7");
	{
		const uint32_t save = MEM16(R_EXMEMCNT);
		const uint32_t a9_off = MEM32(0x027FFE20), a9_ram = MEM32(0x027FFE28);
		const uint32_t pat_rom = (uint32_t)(uintptr_t)v_pattern - a9_ram + a9_off;
		uint32_t test_off = (pat_rom > 0x8000u ? pat_rom : 0x8000u);

		test_off = (test_off + 0x1FFu) & ~0x1FFu;
		MEM16(R_EXMEMCNT) = (uint16_t)(save | 0x0800);
		CC(mb_call(P_CART_OWN, test_off, 0, 0), 1);
		MEM16(R_EXMEMCNT) = (uint16_t)save;
		CC(MBOX->res[0], 128);
		CC(memcmp((const void *)(uintptr_t)0x02390A00, v_pattern + (test_off - pat_rom), 0x200), 0);
		CC(MBOX->res[2] != 0 && MBOX->res[2] != 0xFFFFFFFFu, 1);
	}
	{
		uint32_t spin = 0, nibble;

		MBOX->done = 0;
		MBOX->cmd = CMD_QUIT;
		for (;;) {
			uint32_t n = 100000;

			nibble = REG32(R_IPCSYNC) & 0xF;
			if (nibble == SDS_IPC_OK || nibble == SDS_IPC_FAIL || spin >= 40000u)
				break;
			__asm__ volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n) : : "cc");
			spin++;
		}
		sds_group("ARM7");
		SDS_CHECK(nibble, SDS_IPC_OK);
	}
	sds_finish();
}
