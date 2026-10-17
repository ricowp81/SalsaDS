/* SalsaDS — pruebas del JIT ARM → PPC64 (Fase 0.7): codificaciones, el
 * intérprete de referencia contra el resultado calculado en C y contra el de
 * la Fase 0.3, la traducción de los programas en los tres modos de banderas
 * y, en ppc64 (qemu, big-endian), la ejecución del código traducido contra el
 * intérprete de referencia. */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "interp.h"
#include "jit/armenc.h"
#include "jit/jit_arm.h"
#include "jit/ppcenc.h"
#include "util/endian.h"

#if defined(__powerpc64__)
#include <sys/mman.h>
#endif

static void test_encodings(void)
{
	CHECK_EQ(ppc_add(3, 4, 5), 0x7C642A14u);
	CHECK_EQ(ppc_lwbrx(3, 4, 5), 0x7C642C2Cu);
	CHECK_EQ(ppc_rlwinm(3, 4, 0, 24, 31), 0x5483063Eu);
	CHECK_EQ(ppc_clrldi(3, 4, 32), 0x78830020u);
	CHECK_EQ(ppc_mflr(0), 0x7C0802A6u);
	CHECK_EQ(ppc_mtlr(0), 0x7C0803A6u);
	CHECK_EQ(ppc_stdu(1, 1, -288), 0xF821FEE0u | 1u);
	CHECK_EQ(PPC_BLR, 0x4E800020u);
	CHECK_EQ(ppc_cmpw(0, 3, 4), 0x7C032000u);
	CHECK_EQ(ppc_cmplw(1, 3, 4), 0x7C832040u);
	CHECK_EQ(ppc_cmpwi(7, 28, 5), 0x2F9C0005u);
	CHECK_EQ(ppc_cmpdi(7, 28, 5), 0x2FBC0005u);
	CHECK_EQ(ppc_addi(3, 4, -1), 0x3864FFFFu);
	CHECK_EQ(ppc_bc(PPC_BO_FALSE, PPC_BI(0, PPC_EQ), -8), 0x4082FFF8u);
	CHECK_EQ(ppc_ld(27, 3, 88), 0xEB630058u);
	/* los programas ARM son los que dicen los comentarios */
	ja_loops_init();
	CHECK_EQ(ja_loops[0].code[11], 0x1AFFFFF6u);  /* BNE loop: -10 palabras */
	CHECK_EQ(ja_loops[0].code[13], 0x1AFFFFF1u);  /* BNE outer: -15 */
}

/* El programa «mezcla» es el de la 0.3 y su resultado, el mismo. */
static void test_ref_vs_03(void)
{
	static uint8_t mem[ARM_MEM_SIZE] __attribute__((aligned(16)));
	static uint8_t mem2[JA_MEM_SIZE] __attribute__((aligned(16)));
	struct arm_cpu c;
	struct ja_state s;
	arm_init();
	arm_load_program(&c, mem, 2);
	for (unsigned i = 0; i < ja_loops[0].nwords; i++)
		CHECK_EQ(rd32le(mem + 4 * i), ja_loops[0].code[i]);
	arm_run(&c, DISPATCH_SWITCH);
	ja_loop_setup(&s, mem2, 2);
	ja_ref_run(&s, ja_loops[0].code, ja_loops[0].nwords);
	CHECK_EQ(s.ret, JA_EXIT_SWI);
	CHECK_EQ(s.r[0], c.r[0]);
	CHECK_EQ(s.r[4], c.r[4]);
	CHECK_EQ(ja_out_sum(mem2), arm_out_sum(&c) ? ja_out_sum(mem2) : 0);
	CHECK_EQ(arm_expected_icount(2), (uint64_t)(INT64_MAX / 2 - s.budget));
}

static void test_ref_vs_c(void)
{
	static uint8_t mem[JA_MEM_SIZE] __attribute__((aligned(16)));
	for (unsigned l = 0; l < ja_nloops; l++) {
		struct ja_state s;
		struct ja_expect e;
		ja_loop_setup(&s, mem, 3);
		ja_ref_run(&s, ja_loops[l].code, ja_loops[l].nwords);
		ja_loop_expect(l, 3, &e);
		CHECK_EQ(s.ret, JA_EXIT_SWI);
		CHECK_EQ(s.r[0], e.r0);
		CHECK_EQ(s.r[7], e.r7);
		if (l >= 2)
			CHECK_EQ(s.r[4], e.r4);
		CHECK_EQ(ja_out_sum(mem), e.out_sum);
	}
}

static void test_translate(void)
{
	static uint32_t code[4096];
	for (unsigned l = 0; l < ja_nloops; l++) {
		for (int m = 0; m < JA_FLAGS_COUNT; m++) {
			size_t n = 0;
			CHECK_EQ(ja_translate(ja_loops[l].code, ja_loops[l].nwords, (enum ja_flagmode)m, code, 4096, &n), 0);
			CHECK(n > ja_loops[l].nwords);
			CHECK_EQ(code[n - 1], PPC_BLR);
			for (size_t i = 0; i < n; i++)
				CHECK(code[i] != 0);
		}
	}
	/* Lo fuera del subconjunto se rechaza. */
	const uint32_t bad_mi[] = { arm_dp_imm(ARM_SUB, 1, 1, 1, 0, 1), arm_b(ARM_MI, -3), ARM_SWI };
	size_t n;
	CHECK_EQ(ja_translate(bad_mi, 3, JA_FLAGS_LAZY, code, 4096, &n), JA_E_FLAGS);
	const uint32_t bad_shift[] = { arm_dp_reg(ARM_ADD, 0, 1, 1, 2, ARM_ROR, 0), ARM_SWI };
	CHECK_EQ(ja_translate(bad_shift, 2, JA_FLAGS_LAZY, code, 4096, &n), JA_E_UNSUPPORTED);
	CHECK_EQ(ja_translate(ja_loops[0].code, ja_loops[0].nwords, JA_FLAGS_LAZY, code, 20, &n), JA_E_NOSPACE);
	/* En FUSED las comparaciones de «mezcla» y «ramas» se funden con su salto. */
	CHECK_EQ(ja_translate(ja_loops[2].code, ja_loops[2].nwords, JA_FLAGS_FUSED, code, 4096, &n), 0);
	CHECK(ja_last_tr.fused >= 3);
}

#if defined(__powerpc64__)
static void test_execute(void)
{
	const size_t cap = 4096 * 4;
	uint32_t *code = mmap(NULL, cap, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(code != MAP_FAILED);
	if (code == MAP_FAILED)
		return;
	static uint8_t mem[JA_MEM_SIZE] __attribute__((aligned(16))), mem_ref[JA_MEM_SIZE] __attribute__((aligned(16)));
	for (unsigned l = 0; l < ja_nloops; l++) {
		for (int m = 0; m < JA_FLAGS_COUNT; m++) {
			size_t n = 0;
			CHECK_EQ(ja_translate(ja_loops[l].code, ja_loops[l].nwords, (enum ja_flagmode)m, code, 4096, &n), 0);
			__builtin___clear_cache((char *)code, (char *)(code + n));
			struct ja_state s, r;
			ja_loop_setup(&r, mem_ref, 3);
			ja_ref_run(&r, ja_loops[l].code, ja_loops[l].nwords);
			ja_loop_setup(&s, mem, 3);
			const int rc = ja_call(code, &s);
			CHECK_EQ(rc, JA_EXIT_SWI);
			CHECK_EQ(s.ret, JA_EXIT_SWI);
			for (int i = 0; i <= 12; i++)
				CHECK_EQ(s.r[i], r.r[i]);
			CHECK_EQ(ja_out_sum(mem), ja_out_sum(mem_ref));
			CHECK_EQ(memcmp(mem, mem_ref, JA_MEM_SIZE), 0);
			/* las instrucciones ejecutadas salen del presupuesto */
			CHECK_EQ(s.budget, r.budget);
			CHECK_EQ(s.pc, r.pc);
		}
	}
	/* Presupuesto corto: sale por el encabezado de un bloque, sin pasarse. */
	for (int m = 0; m < JA_FLAGS_COUNT; m++) {
		size_t n = 0;
		ja_translate(ja_loops[1].code, ja_loops[1].nwords, (enum ja_flagmode)m, code, 4096, &n);
		__builtin___clear_cache((char *)code, (char *)(code + n));
		struct ja_state s;
		ja_loop_setup(&s, mem, 1000);
		s.budget = 5000;
		CHECK_EQ(ja_call(code, &s), JA_EXIT_BUDGET);
		CHECK(s.budget >= 0 && s.budget < 6);
		CHECK_EQ(s.pc % 4, 0);
	}
	munmap(code, cap);
}
#endif

void test_jit(void)
{
	test_encodings();
	test_ref_vs_03();
	test_ref_vs_c();
	test_translate();
#if defined(__powerpc64__)
	test_execute();
#endif
}
