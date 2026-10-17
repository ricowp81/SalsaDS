# SalsaDS — herramientas del host que usan la máquina de src/core y los intérpretes de src/cpu:
#   salsads-cpurun   corre ROMs (tests/roms) y lee su veredicto; vuelca la imagen de las pantallas
#   armbench         mide los MIPS de los intérpretes
# Desde la raíz: tools/build.sh -C src/host (en el contenedor, con gcc) o
# tools/cpurun.sh (nativo en la Mac, con cc).
ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host-run
CC   ?= gcc
CFLAGS ?= -std=gnu11 -O2 -g -Wall -Wextra -Werror -I$(ROOT)/src

CPU_SRCS  := $(wildcard $(ROOT)/src/cpu/*.c)
CORE_SRCS := $(wildcard $(ROOT)/src/core/*.c) $(wildcard $(ROOT)/src/bios/*.c)
HDRS      := $(wildcard $(ROOT)/src/core/*.h $(ROOT)/src/cpu/*.h $(ROOT)/src/util/*.h)

.PHONY: all clean
all: $(OUT)/salsads-cpurun $(OUT)/armbench

$(OUT)/salsads-cpurun: cpurun.c $(CORE_SRCS) $(CPU_SRCS) $(HDRS)
	@mkdir -p $(OUT)
	@echo "  CC      salsads-cpurun"
	@$(CC) $(CFLAGS) cpurun.c $(CORE_SRCS) $(CPU_SRCS) -o $@

$(OUT)/armbench: armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c
	@mkdir -p $(OUT)
	@echo "  CC      armbench"
	@$(CC) $(CFLAGS) armbench.c $(CPU_SRCS) $(ROOT)/src/jit/loops.c -o $@

clean:
	rm -rf $(OUT)
