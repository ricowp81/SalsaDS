# SalsaDS — herramientas del host que usan los intérpretes de src/cpu:
#   salsads-cpurun   corre ROMs de prueba de CPU (tests/roms) y lee su veredicto
#   armbench         mide los MIPS de los intérpretes
# Desde la raíz: tools/build.sh -C src/host (en el contenedor, con gcc) o
# tools/cpurun.sh (nativo en la Mac, con cc).
ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host-run
CC   ?= gcc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/src

CPU_SRCS := $(wildcard $(ROOT)/src/cpu/*.c)

.PHONY: all clean
all: $(OUT)/salsads-cpurun $(OUT)/armbench

$(OUT)/salsads-cpurun: cpurun.c ndsmin.c $(CPU_SRCS) ndsmin.h
	@mkdir -p $(OUT)
	@echo "  CC      salsads-cpurun"
	@$(CC) $(CFLAGS) cpurun.c ndsmin.c $(CPU_SRCS) -o $@

$(OUT)/armbench: armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c
	@mkdir -p $(OUT)
	@echo "  CC      armbench"
	@$(CC) $(CFLAGS) armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c -o $@

clean:
	rm -rf $(OUT)
