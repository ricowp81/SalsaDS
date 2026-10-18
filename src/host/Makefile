# SalsaDS — herramientas del host que usan la máquina de src/core y los intérpretes de src/cpu:
#   salsads-cpurun   corre ROMs (tests/roms) y lee su veredicto; vuelca la imagen de las pantallas
#   armbench         mide los MIPS de los intérpretes
# Desde la raíz: tools/build.sh -C src/host (en el contenedor, con gcc) o
# tools/cpurun.sh (nativo en la Mac, con cc).
ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host-run
CC   ?= gcc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/src

CPU_SRCS  := $(wildcard $(ROOT)/src/cpu/*.c) $(ROOT)/src/jit/arec.c $(ROOT)/src/jit/arec_dec.c $(ROOT)/src/jit/arec_emit.c
CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c) $(wildcard $(ROOT)/src/bios/*.c) $(wildcard $(ROOT)/src/gpu2d/*.c) $(wildcard $(ROOT)/src/gpu3d/*.c) $(wildcard $(ROOT)/src/audio/*.c)
HDRS      := $(wildcard $(ROOT)/src/core/*.h $(ROOT)/src/cpu/*.h $(ROOT)/src/jit/*.h $(ROOT)/src/host/*.h $(ROOT)/src/util/*.h $(ROOT)/src/gpu2d/*.h $(ROOT)/src/gpu3d/*.h $(ROOT)/src/audio/*.h)

.PHONY: all clean
all: $(OUT)/salsads-cpurun $(OUT)/armbench

$(OUT)/salsads-cpurun: cpurun.c jitrun.c $(CORE_SRCS) $(CPU_SRCS) $(HDRS)
	@mkdir -p $(OUT)
	@echo "  CC      salsads-cpurun"
	@$(CC) $(CFLAGS) cpurun.c jitrun.c $(CORE_SRCS) $(CPU_SRCS) -o $@ -lm

$(OUT)/armbench: armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c
	@mkdir -p $(OUT)
	@echo "  CC      armbench"
	@$(CC) $(CFLAGS) armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c -o $@

clean:
	rm -rf $(OUT)
