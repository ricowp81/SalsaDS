/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — marco mínimo de pruebas unitarias. */
#ifndef SALSADS_TESTS_CHECK_H
#define SALSADS_TESTS_CHECK_H

#include <stdio.h>

extern int check_total, check_failed;

#define CHECK(cond)                                                              \
	do {                                                                     \
		check_total++;                                                   \
		if (!(cond)) {                                                   \
			check_failed++;                                          \
			printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
		}                                                                \
	} while (0)

#define CHECK_EQ(got, want)                                                      \
	do {                                                                     \
		unsigned long long g_ = (unsigned long long)(got);               \
		unsigned long long w_ = (unsigned long long)(want);              \
		check_total++;                                                   \
		if (g_ != w_) {                                                  \
			check_failed++;                                          \
			printf("FALLO %s:%d: %s = 0x%llx, esperado 0x%llx\n",    \
			       __FILE__, __LINE__, #got, g_, w_);                \
		}                                                                \
	} while (0)

#endif
