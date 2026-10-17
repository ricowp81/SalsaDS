/*
 * SalsaDS — ROM mem, lado ARM7: la WRAM del ARM7, la compartida vista desde el ARM7 y la VRAM que el ARM9
 * le asigna (bancos C y D). Atiende los pedidos del ARM9 por el buzón (hw.h).
 */
#include "../common/hw.h"

static uint32_t pattern(uint32_t off)
{
	return 0xC0DE0000u + off / 4;
}

static void standalone(void)
{
	sds_group("WRAM7");
	MEM32(0x03800000) = 0x71727374u;
	SDS_CHECK(MEM32(0x03810000), 0x71727374u); /* 64 KB que se repiten */
	SDS_CHECK(MEM32(0x03FF0000), 0x71727374u);
	MEM32(0x0380FFFC) = 0xFEEDF00Du;
	SDS_CHECK(MEM32(0x03FFFFFC), 0xFEEDF00Du);
	SDS_CHECK(MEM8(0x03800001), 0x73);
	MEM8(0x03800002) = 0x55;
	SDS_CHECK(MEM32(0x03800000), 0x71557374u);
	MEM16(0x03800002) = 0x1234;
	SDS_CHECK(MEM32(0x03800000), 0x12347374u);
	/* lo que es solo del ARM9 no existe aquí */
	SDS_CHECK(MEM32(0x05000000), 0);
	MEM32(0x05000000) = 0x55555555u;
	SDS_CHECK(MEM32(0x05000000), 0);
	SDS_CHECK(MEM32(0x07000000), 0);
	SDS_CHECK(MEM32(0x06800000), 0); /* el LCDC tampoco */
	SDS_CHECK(MEM32(0x040003F0), 0);
	/* la BIOS del ARM7 está en 0 y tiene vectores */
	SDS_CHECK(rd32_raw(0x00000000) != 0, 1);
	SDS_CHECK(rd32_raw(0x00004000), 0);
}

static uint32_t wram_view(uint32_t mode)
{
	/* devuelve 1 si el ARM7 ve lo esperado con ese WRAMCNT */
	uint32_t ok = 1;

	switch (mode) {
	case 0: /* nada compartido: la WRAM del ARM7 repetida */
		MEM32(0x03800040) = 0x70707070u;
		ok &= MEM32(0x03000040) == 0x70707070u;
		ok &= MEM32(0x03010040) == 0x70707070u;
		ok &= MEM32(0x037F0040) == 0x70707070u;
		break;
	case 1: /* la primera mitad */
		ok &= MEM32(0x03000000) == pattern(0);
		ok &= MEM32(0x03000008) == pattern(8);
		ok &= MEM32(0x03004008) == pattern(8);
		ok &= MEM32(0x037FC008) == pattern(8);
		break;
	case 2: /* la segunda mitad */
		ok &= MEM32(0x03000000) == pattern(0x4000);
		ok &= MEM32(0x03000008) == pattern(0x4008);
		ok &= MEM32(0x03004008) == pattern(0x4008);
		break;
	default: /* todo */
		ok &= MEM32(0x03000000) == pattern(0);
		ok &= MEM32(0x03004000) == pattern(0x4000);
		ok &= MEM32(0x03007FFC) == pattern(0x7FFC);
		ok &= MEM32(0x03008000) == pattern(0);
		ok &= MEM32(0x037F8004) == pattern(4);
		break;
	}
	/* WRAMSTAT (0x04000241) refleja WRAMCNT */
	ok &= REG8(R_WRAMSTAT) == mode;
	return ok;
}

static void service(void)
{
	for (;;) {
		const uint32_t cmd = MBOX->cmd;

		if (!cmd)
			continue;
		MBOX->cmd = 0;
		switch (cmd) {
		case 1: /* llena la WRAM compartida (el ARM9 puso modo 3) */
			for (uint32_t o = 0; o < 0x8000; o += 4)
				MEM32(0x03000000 + o) = pattern(o);
			break;
		case 2:
			MBOX->res[0] = wram_view(MBOX->arg[0]);
			break;
		case 3: /* modo 1 del ARM9: el ARM7 tiene la primera mitad */
			MEM32(0x03000010) = 0x600DF00Du;
			MEM32(0x03004010) = 0x600DF00Du;
			break;
		case 4: /* C asignado al ARM7 en 0x06000000 */
			MBOX->res[0] = MEM32(0x06000040);
			MBOX->res[1] = REG8(R_VRAMSTAT);
			MEM16(0x06000044) = 0;
			MEM8(0x06000046) = 0xCD; /* el ARM7 sí puede escribir bytes en esta VRAM */
			MEM8(0x06000047) = 0xAB;
			break;
		case 5: /* D asignado con OFS 1 en 0x06020000 */
			MBOX->res[0] = MEM32(0x06020040);
			MBOX->res[1] = REG8(R_VRAMSTAT);
			MBOX->res[2] = MEM32(0x06000040);
			MEM32(0x06020044) = 0x11223344u;
			break;
		case CMD_QUIT:
			MBOX->done = 1;
			return;
		default:
			break;
		}
		MBOX->done = 1;
	}
}

int main(void)
{
	__asm__ volatile("msr cpsr_c, #0xDF" : : : "cc");
	sds_init("MEM ARM7");
	standalone();
	service();
	sds_finish();
}
