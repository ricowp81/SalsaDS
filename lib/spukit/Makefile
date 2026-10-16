# SPDX-License-Identifier: MIT
#
# spukit — biblioteca estática para PSL1GHT (lado PPE). El lado SPU es solo
# cabecera (include/spukit/spu.h). Sin dependencias del proyecto que la
# contenga: solo PSL1GHT y lo que hay en este directorio.
#
#   make                 → $(OUT)/libspukit.a (por omisión OUT = build)
#   make OUT=/ruta       salida en otro directorio
#
# Para usarla: -I<spukit>/include en el PPE y en el SPU, y enlazar
# $(OUT)/libspukit.a antes de -llv2.

ifeq ($(strip $(PSL1GHT)),)
$(error PSL1GHT no está definido)
endif

HERE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
OUT  ?= $(HERE)/build
OBJ  := $(OUT)/obj

ifneq ($(V),1)
  VERB := @
endif

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cell -mhard-float -fmodulo-sched \
          -ffunction-sections -fdata-sections \
          -I$(PSL1GHT)/ppu/include -I$(HERE)/include

SRCS  := $(wildcard $(HERE)/src/*.c)
OBJS  := $(patsubst $(HERE)/src/%.c,$(OBJ)/%.o,$(SRCS))

.PHONY: all clean
all: $(OUT)/libspukit.a

$(OUT)/libspukit.a: $(OBJS)
	@echo "  AR      $(notdir $@)"
	$(VERB)rm -f $@ && ppu-ar rcs $@ $^

$(OBJ)/%.o: $(HERE)/src/%.c
	@echo "  PPU CC  spukit/$*.c"
	@mkdir -p $(@D)
	$(VERB)ppu-gcc $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
