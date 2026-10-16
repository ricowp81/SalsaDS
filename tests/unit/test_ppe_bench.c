/* Núcleos y intérprete de tests/phase0/ppe-bench. Los de ensamblador solo
 * existen en PowerPC64 (qemu-ppc64, big-endian); el intérprete ARM corre en
 * todos los hosts y de paso prueba la memoria little-endian en los dos
 * órdenes de bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "interp.h"
#include "kernels.h"
#include "util/endian.h"

static void test_flags_reference(void)
{
	struct kguest g;

	kflags_init(&g, 1);
	CHECK_EQ(kflags_ref(&g), 8); /* ADDS CMP BLT ADDS CMP BHI ADDS SUBS BNE: 9 - saltos tomados */
	/* Una vuelta con r0 = 0x13579BDF: sin desbordamiento, r0 < r2, BLT tomado. */
	CHECK_EQ(g.r[0], 0x13579BDF);
	CHECK_EQ(g.r[4], 0);
	/* Diez mil vueltas: tiene que haber pasado por desbordamiento (r0 envuelve) y por BHI. */
	kflags_init(&g, 10000);
	uint64_t n = kflags_ref(&g);
	CHECK(n > 10000 * 6 && n < 10000 * 9);
	CHECK_EQ(g.r[4], 0);
	CHECK(g.r[3] != 0 && g.r[6] != 0);
}

static void test_flags_kernels(void)
{
#if KERNELS_AVAILABLE
	for (uint32_t iters = 1; iters <= 20000; iters = iters * 7 + 1) {
		struct kguest ref;
		kflags_init(&ref, iters);
		kflags_ref(&ref);
		for (int k = 0; k < KF_COUNT; k++) {
			struct kguest got;
			kflags_init(&got, iters);
			kflags_table[k](&got);
			if (memcmp(&ref, &got, sizeof ref))
				printf("  banderas %s con %u vueltas\n", kflags_name[k], iters);
			CHECK(!memcmp(&ref, &got, sizeof ref));
		}
	}
#endif
}

static void test_mem_kernels(void)
{
#if KERNELS_AVAILABLE
	uint8_t *buf = aligned_alloc(128, 65536);
	memset(buf, 0x11, 65536);
	for (int i = 0; i < kmem_count; i++)
		kmem_table[i].fn(buf, 1000);
	kmem_base(buf, 1000);
	for (int i = 0; i < kinstr_count; i++) {
		kinstr_table[i].fn(1000);
		if (kinstr_table[i].base)
			kinstr_table[i].base(1000);
	}
	kinstr_base_nops(1000);
	CHECK(1); /* llegaron sin caerse */

	/* Los de latencia dan el índice que calcula C, en las seis variantes. */
	uint8_t *table = aligned_alloc(128, 4096);
	for (int i = 0; i < kchase_count; i++) {
		const struct kchase_desc *d = &kchase_table[i];
		uint32_t want = kchase_fill(table, d->bits, d->impl, 4 * 777);
		CHECK_EQ(d->fn(table, 777), want);
	}
	free(table);
	free(buf);
#endif
}

static void test_chase_fill(void)
{
	/* El ciclo visita las 1024 entradas antes de repetir. */
	uint8_t table[4096];
	uint32_t seen = 0;
	for (int bits = 16; bits <= 32; bits += 16) {
		kchase_fill(table, bits, KI_BR, 0);
		uint8_t visited[KCHASE_ENTRIES] = { 0 };
		uint32_t idx = 0;
		for (int s = 0; s < KCHASE_ENTRIES; s++) {
			CHECK(!visited[idx]);
			visited[idx] = 1;
			seen++;
			idx = bits == 32 ? rd32le(table + idx * 4) : rd16le(table + idx * 2);
		}
		CHECK_EQ(idx, 0);
	}
	CHECK_EQ(seen, 2 * KCHASE_ENTRIES);
}

static void test_interp(void)
{
	uint8_t *mem = aligned_alloc(128, ARM_MEM_SIZE);
	struct arm_cpu cpu;

	arm_init();
	for (int h = 0; h < DISPATCH_COUNT; h++) {
		for (uint32_t reps = 1; reps <= 3; reps++) {
			uint32_t want_r0, want_out;
			arm_expected(reps, &want_r0, &want_out);
			arm_load_program(&cpu, mem, reps);
			arm_run(&cpu, (enum arm_dispatch)h);
			CHECK_EQ(cpu.halt, 1);
			CHECK_EQ(cpu.r[0], want_r0);
			CHECK_EQ(arm_out_sum(&cpu), want_out);
			CHECK_EQ(cpu.icount, arm_expected_icount(reps));
			CHECK_EQ(cpu.r[6], 0);
		}
	}
	free(mem);
}

void test_ppe_bench(void)
{
	printf("ppe-bench: núcleos de ensamblador %s\n", KERNELS_AVAILABLE ? "presentes (ppc64)" : "omitidos (no es ppc64)");
	test_flags_reference();
	test_flags_kernels();
	test_mem_kernels();
	test_chase_fill();
	test_interp();
}
