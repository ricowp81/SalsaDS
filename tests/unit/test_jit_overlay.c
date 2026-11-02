/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Código reemplazado cerca del final de una página de escritura rápida. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "core/nds_priv.h"

#if defined(__linux__) && defined(__powerpc64__)
#include <sys/mman.h>

static void run_at(struct nds *n, uint32_t pc)
{
	acore_bx(&n->a9, pc);
	arec_run(n->jit9, n->a9.cycles + 24);
}

static void replace_code(void)
{
	struct nds *n = NULL;
	const size_t size = 2u << 20;
	void *code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(code != MAP_FAILED);
	if (code == MAP_FAILED) return;
	CHECK_EQ(posix_memalign((void **)&n, 128, sizeof *n), 0);
	if (!n) { munmap(code, size); return; }
	memset(n, 0, sizeof *n);
	nds_init(n);
	struct nds_jit_cfg cfg = {0};
	cfg.code = code; cfg.code_bytes = size; cfg.no_link = 1;
	CHECK_EQ(nds_jit_attach(n, &cfg), 0);
	if (!n->jit9) { free(n); munmap(code, size); return; }
	acore_set_cpsr(&n->a9, ACORE_MODE_SYS);
	static const uint32_t offsets[] = {0x3f80, 0x3f90, 0x3ffc, 0x4000, 0x7f90};
	static const uint32_t stores[] = {0xe5c21000, 0xe1c210b0, 0xe5821000}; /* STRB, STRH, STR r1,[r2] */
	for (unsigned k = 0; k < sizeof offsets / sizeof *offsets; k++)
		for (unsigned width = 0; width < 3; width++) {
			const uint32_t target = 0x02000000u + offsets[k], writer = 0x02010000u;
			nds_bus_write(n, 0, target, 4, 0xe3a00001); /* MOV r0,#1 */
			nds_bus_write(n, 0, target + 4, 4, 0xeafffffe);
			nds_bus_write(n, 0, writer, 4, stores[width]);
			nds_bus_write(n, 0, writer + 4, 4, 0xeafffffe);
			arec_flush(n->jit9);
			run_at(n, target);
			CHECK_EQ(n->a9.r[0], 1);
			const uint64_t invalidated = arec_stats(n->jit9)->inval_blocks;
			n->a9.r[1] = width == 2 ? 0xe3a00002u : 2;
			n->a9.r[2] = target;
			run_at(n, writer);
			CHECK_EQ(nds_peek32(n, 0, target), 0xe3a00002u);
			CHECK(arec_stats(n->jit9)->inval_blocks > invalidated);
			run_at(n, target);
			CHECK_EQ(n->a9.r[0], 2);
		}
	nds_jit_detach(n);
	free(n);
	munmap(code, size);
}
#endif

void test_jit_overlay(void)
{
	/* La página guest y la física deben tener el mismo desplazamiento bajo. */
	#define PAGE_OFFSET(member) ((offsetof(struct nds, member) - offsetof(struct nds, itcm)) & 0x3fffu)
	CHECK_EQ(PAGE_OFFSET(dtcm), 0);
	CHECK_EQ(PAGE_OFFSET(ram), 0);
	CHECK_EQ(PAGE_OFFSET(swram), 0);
	CHECK_EQ(PAGE_OFFSET(wram7), 0);
	CHECK_EQ(PAGE_OFFSET(vram), 0);
	#undef PAGE_OFFSET
#if defined(__linux__) && defined(__powerpc64__)
	replace_code();
#endif
}
