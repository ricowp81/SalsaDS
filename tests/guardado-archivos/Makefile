# SPDX-License-Identifier: GPL-2.0-or-later
# Prueba en el host las funciones reales de archivos de la aplicación, sin RSX ni mandos.
ROOT := $(abspath ../..)
OUT ?= $(ROOT)/build/guardado-archivos
CC ?= gcc
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror

.PHONY: run all
all: $(OUT)/guardado-archivos
run: all
	@fixture=$$(mktemp -d /tmp/sds-save.XXXXXX); ASAN_OPTIONS=detect_leaks=0 $(OUT)/guardado-archivos "$$fixture"

$(OUT)/guardado.inc: $(ROOT)/tests/phase1/nucleo/main.c
	@mkdir -p $(OUT)
	@awk '/^\/\* ---- guardado ---/{copy=1;next} /^\/\* ---- composición 2D/{copy=0} copy' $< > $@

$(OUT)/guardado-archivos: main.c $(OUT)/guardado.inc $(ROOT)/src/core/save.c $(wildcard $(ROOT)/src/core/*.h)
	$(CC) $(CFLAGS) $(SAN) -I$(ROOT)/src -I$(OUT) main.c $(ROOT)/src/core/save.c $(SAN) -o $@
