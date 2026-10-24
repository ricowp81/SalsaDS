/* SPDX-License-Identifier: GPL-2.0-or-later */
/* VRAM C/D: carga por LCDC, reasignación, espejos, solapamiento y protección de captura. */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "core/nds_priv.h"

static void cnt(struct nds *n, unsigned bank, uint8_t value)
{
	nds_bus_write(n, NDS_CPU9, 0x04000240u + bank, 1, value);
}

static void test_vram7_jit(struct nds *n);

void test_vram7(void)
{
	struct nds *n = NULL;
	CHECK_EQ(posix_memalign((void **)&n, 128, sizeof *n), 0);
	if (!n) return;
	memset(n, 0, sizeof *n);
	nds_init(n);
	for (unsigned b = 2; b <= 3; b++) for (unsigned ofs = 0; ofs < 2; ofs++) {
		const uint32_t lcdc = 0x06800000u + nds_vram_bank_offset(b);
		const uint32_t view = 0x06000000u + ofs * 0x20000u;
		cnt(n, 2, 0); cnt(n, 3, 0);
		cnt(n, b, 0x80);
		/* DMA del ARM9 a LCDC antes de ceder el banco al ARM7. */
		nds_bus_write(n, NDS_CPU9, 0x02000000, 4, 0xe3a0002au);
		nds_bus_write(n, NDS_CPU9, 0x040000b0, 4, 0x02000000);
		nds_bus_write(n, NDS_CPU9, 0x040000b4, 4, lcdc);
		nds_bus_write(n, NDS_CPU9, 0x040000b8, 4, 0x84000001);
		nds_bus_write(n, NDS_CPU9, lcdc + 0x1fffc, 4, 0xe12fff1eu);
		cnt(n, b, 0x82u | (ofs << 3));
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x04000240, 1), 1u << (b - 2));
		nds_bus_write(n, NDS_CPU7, 0x04000240, 2, 0xffff); /* solo lectura */
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x04000240, 1), 1u << (b - 2));
		for (unsigned mirror = 0; mirror < 64; mirror++) {
			CHECK_EQ(nds_bus_read(n, NDS_CPU7, view + mirror * 0x40000u, 4), 0xe3a0002au);
			CHECK_EQ(nds_bus_read(n, NDS_CPU7, view + mirror * 0x40000u + 0x1fffc, 4), 0xe12fff1eu);
		}
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, view ^ 0x20000u, 4), 0);
		nds_bus_write(n, NDS_CPU9, lcdc, 4, 0); /* ARM9 ya no posee el banco */
		nds_bus_write(n, NDS_CPU7, view + 1, 1, 0x55);
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, view, 4), 0xe3a0552au);
		cnt(n, b, 0x83); /* texturas: desaparece del ARM7 */
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x04000240, 1), 0);
		CHECK_EQ(nds_bus_read(n, NDS_CPU7, view, 4), 0);
		cnt(n, b, 0x80);
		CHECK_EQ(nds_bus_read(n, NDS_CPU9, lcdc, 4), 0xe3a0552au);
	}
	cnt(n, 2, 0x82); cnt(n, 3, 0x8a);
	nds_bus_write(n, NDS_CPU7, 0x06000000, 4, 0x11220000);
	nds_bus_write(n, NDS_CPU7, 0x06020000, 4, 0x00003344);
	CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x04000240, 1), 3);
	cnt(n, 3, 0x82);
	CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x06000000, 4), 0x11223344);
	nds_bus_write(n, NDS_CPU7, 0x06000000, 4, 0xabcdef01);
	cnt(n, 3, 0x8a);
	CHECK_EQ(nds_bus_read(n, NDS_CPU7, 0x06020000, 4), 0xabcdef01);
	/* Una captura de negro opaco no puede pisar código del ARM7 ni bancos no LCDC. */
	for (unsigned b = 0; b < 4; b++) for (unsigned mode = 0; mode < 6; mode++) {
		static const uint8_t modes[] = {0, 0x80, 0x81, 0x82, 0x83, 0x84};
		const uint32_t off = nds_vram_bank_offset(b), cap = 0x80000000u | (b << 16);
		struct nds_vset *s = &n->vid.set[0];
		cnt(n, b, modes[mode]);
		memset(n->vram + off, 0x5a, 0x20000);
		memset(&s->frame, 0, sizeof s->frame);
		s->frame.eng[0].regs[G2D_R_DISPCAP] = (uint16_t)cap;
		s->frame.eng[0].regs[G2D_R_DISPCAP + 1] = cap >> 16;
		n->io[0].gen[G2D_R_DISPCAP + 1] = cap >> 16;
		n->vid.cur = 0; n->vid.recording = 1;
		nds_video_frame_end(n);
		CHECK_EQ(rd32le(n->vram + off), (mode == 1 || (b < 2 && mode == 5)) ? 0x80008000 : 0x5a5a5a5a);
		CHECK_EQ(rd32le(n->vram + off + 0x1fffc), 0x5a5a5a5a);
		CHECK_EQ(n->io[0].gen[G2D_R_DISPCAP + 1] & 0x8000, 0);
	}
	test_vram7_jit(n);
	free(n);
}

#if defined(__powerpc64__) && defined(__linux__)
#include <sys/mman.h>
static void jit_check(struct nds *n, struct nds *r, uint32_t pc)
{
	acore_bx(&n->a7, pc); acore_bx(&r->a7, pc);
	const uint64_t until = n->a7.cycles + 20;
	arec_run(n->jit7, until);
	acore_run(&r->a7, until);
	CHECK(memcmp(n->a7.r, r->a7.r, sizeof n->a7.r) == 0);
	CHECK_EQ(n->a7.cpsr, r->a7.cpsr);
	CHECK_EQ(n->a7.cycles, r->a7.cycles);
}
static void test_vram7_jit(struct nds *n)
{
	struct nds *r = NULL;
	struct nds_jit_cfg cfg = {0};
	const size_t size = 2u << 20;
	void *code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(code != MAP_FAILED);
	if (code == MAP_FAILED) return;
	CHECK_EQ(posix_memalign((void **)&r, 128, sizeof *r), 0);
	if (!r) { munmap(code, size); return; }
	memset(r, 0, sizeof *r);
	nds_init(n); nds_init(r);
	cfg.code = code; cfg.code_bytes = size; cfg.code7_bytes = size / 2;
	cfg.arm7 = cfg.no_arm9 = cfg.strict = 1;
	CHECK_EQ(nds_jit_attach(n, &cfg), 0);
	if (!n->jit7) { free(r); munmap(code, size); return; }
	for (unsigned thumb = 0; thumb < 2; thumb++) {
		for (unsigned step = 0; step < 5; step++) {
			for (unsigned m = 0; m < 2; m++) {
				struct nds *q = m ? r : n;
				const uint32_t ins = thumb ? 0xe7fe2000u | (step + 1) : 0xe3a00000u | (step + 1);
				if (step == 0 || step == 3 || step == 4) {
					cnt(q, 2, 0x80); cnt(q, 3, 0x80);
					nds_bus_write(q, 0, 0x06840000, 4, ins);
					nds_bus_write(q, 0, 0x06840004, 4, 0xeafffffe);
					nds_bus_write(q, 0, 0x06860000, 4, ins);
					nds_bus_write(q, 0, 0x06860004, 4, 0xeafffffe);
					cnt(q, 2, step == 4 ? 0x8a : 0x82);
					cnt(q, 3, step >= 3 ? 0x82 : 0x8a);
				} else if (step == 1) {
					nds_bus_write(q, 1, 0x06040000, 1, ins & 0xffu); /* CPU por espejo */
				} else {
					nds_bus_write(q, 1, 0x03800000, 4, ins);
					nds_bus_write(q, 1, 0x040000b0, 4, 0x03800000);
					nds_bus_write(q, 1, 0x040000b4, 4, 0x06000000);
					nds_bus_write(q, 1, 0x040000b8, 4, 0x84000001); /* DMA inmediata */
				}
			}
			jit_check(n, r, 0x06000000u | thumb);
			CHECK_EQ(n->a7.r[0], step + 1);
		}
	}

	CHECK(arec_stats(n->jit7)->enters > 0);
	CHECK(arec_stats(n->jit7)->inval_blocks > 0);
	nds_jit_detach(n);
	free(r); munmap(code, size);
}
#else
static void test_vram7_jit(struct nds *n) { (void)n; }
#endif
