/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — corredor de las pruebas unitarias del host.
 *
 * Imprime el orden de bytes del host para que tools/test-host.sh compruebe
 * que cada corrida fue de verdad en el orden que dice (LE en la Mac, BE en
 * qemu-ppc64), y termina con una línea «total: N comprobaciones, M fallos». */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

int check_total, check_failed;

void test_endian(void);
void test_ppe_bench(void);
void test_gpu2d(void);
void test_dspad(void);
void test_layout(void);
void test_jit(void);
void test_arec(void);
void test_r3d(void);
void test_r3d_selftest(void);
void test_acore_arm(void);
void test_acore_thumb(void);
void test_acore_sys(void);
void test_snd(void);
void test_gx(void);
void test_gx_defer(void);
void test_gx_job(void);
void test_gx_math(void);
void test_render3d(void);
void test_r3d_vrow(void);
void test_save(void);

int main(int argc, char **argv)
{
	const uint16_t probe = 0x0102;
	const char *order = *(const uint8_t *)&probe == 0x02 ? "LE" : "BE";

	printf("host: %s\n", order);
	if (argc > 1 && !strcmp(argv[1], "arec")) { /* solo el recompilador (para iterar) */
		test_arec();
		printf("total: %d comprobaciones, %d fallos\n", check_total, check_failed);
		return check_failed ? 1 : 0;
	}
	/* UNIT_ONLY=r3d: solo las pruebas del render 3D (render3d y vrow), para iterar rápido */
	if (getenv("UNIT_ONLY") && !strcmp(getenv("UNIT_ONLY"), "r3d")) {
		test_render3d();
		test_r3d_vrow();
		printf("total: %d comprobaciones, %d fallos\n", check_total, check_failed);
		return check_failed ? 1 : 0;
	}
	test_endian();
	test_ppe_bench();
	test_gpu2d();
	test_dspad();
	test_layout();
	test_jit();
	test_arec();
	test_r3d_selftest();
	test_r3d();
	test_acore_arm();
	test_acore_thumb();
	test_acore_sys();
	test_snd();
	test_gx();
	test_gx_defer();
	test_gx_job();
	test_gx_math();
	test_render3d();
	test_r3d_vrow();
	test_save();
	printf("total: %d comprobaciones, %d fallos\n", check_total, check_failed);
	return check_failed ? 1 : 0;
}
