#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Calcula en el host los resultados esperados de la ROM carga (tests/roms/src/carga/gold.h) compilando el mismo
# kernel.h. El resultado de una iteración no depende de la CPU: las dos usan las mismas semillas.
#   tools/carga-gold.sh > tests/roms/src/carga/gold.h
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
cat >"$TMP/gold.c" <<'EOC'
#include <stdio.h>
#include "iter.h"
static struct k_buf buf;
int main(void)
{
	printf("/* Generado por tools/carga-gold.sh: resultados esperados de una iteración de la ROM carga. */\n");
	printf("#ifndef CARGA_GOLD_H\n#define CARGA_GOLD_H\n\n#include \"iter.h\"\n\n");
	printf("static const uint32_t carga_gold[CARGA_SEEDS] = { ");
	for (unsigned i = 0; i < CARGA_SEEDS; i++)
		printf("0x%08Xu%s", carga_iter(&buf, carga_seed[i]), i + 1 < CARGA_SEEDS ? ", " : " };\n");
	printf("\n#endif\n");
	return 0;
}
EOC
${CC:-cc} -std=c99 -O2 -Wall -Wextra -I"$ROOT/tests/roms/src/carga" "$TMP/gold.c" -o "$TMP/gold"
"$TMP/gold"
rm -rf "$TMP"
