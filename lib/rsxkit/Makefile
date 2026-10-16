# SPDX-License-Identifier: MIT
#
# rsxkit — biblioteca estática para PSL1GHT. Sin dependencias del proyecto
# que la contenga: solo PSL1GHT, sus herramientas (ppu-gcc, cgcomp, bin2s) y
# lo que hay en este directorio.
#
#   make                 → $(OUT)/librsxkit.a (por omisión OUT = build)
#   make OUT=/ruta       salida en otro directorio
#
# Para usarla: -I<rsxkit>/include y enlazar $(OUT)/librsxkit.a antes de
# -lrsx -lgcm_sys -lsysutil.

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
          -I$(PSL1GHT)/ppu/include -I$(HERE)/include -I$(HERE)/src

SRCS     := $(wildcard $(HERE)/src/*.c)
VSHADERS := $(wildcard $(HERE)/shaders/*.vcg)
FSHADERS := $(wildcard $(HERE)/shaders/*.fcg)
COBJS    := $(patsubst $(HERE)/src/%.c,$(OBJ)/%.o,$(SRCS))
OBJS     := $(COBJS) \
            $(patsubst $(HERE)/shaders/%.vcg,$(OBJ)/%.vpo.o,$(VSHADERS)) \
            $(patsubst $(HERE)/shaders/%.fcg,$(OBJ)/%.fpo.o,$(FSHADERS))

.PHONY: all clean
all: $(OUT)/librsxkit.a

$(OUT)/librsxkit.a: $(OBJS)
	@echo "  AR      $(notdir $@)"
	$(VERB)rm -f $@ && ppu-ar rcs $@ $^

$(OBJ)/%.o: $(HERE)/src/%.c
	@echo "  PPU CC  rsxkit/$*.c"
	@mkdir -p $(@D)
	$(VERB)ppu-gcc $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/%.vpo: $(HERE)/shaders/%.vcg
	@echo "  CGCOMP  $(notdir $<)"
	@mkdir -p $(@D)
	$(VERB)cgcomp -v $< $@

$(OBJ)/%.fpo: $(HERE)/shaders/%.fcg
	@echo "  CGCOMP  $(notdir $<)"
	@mkdir -p $(@D)
	$(VERB)cgcomp -f $(CGFLAGS) $< $@

# cgcomp compila con -fastprecision: Cg baja a fp16 las cuentas que cree que
# lo aguantan. Las del posprocesado van en texeles (hasta miles): en fp32.
$(OBJ)/rsxkit_post%.fpo: CGFLAGS := -Wcg,-nofastprecision

# bin2s exporta <archivo>_<ext> alineado a 64 bytes (fb.c declara los símbolos).
define EMBED
	@echo "  EMBED   $(notdir $<)"
	$(VERB)bin2s -a 64 $< | ppu-as -o $@
endef
$(OBJ)/%.vpo.o: $(OBJ)/%.vpo ; $(EMBED)
$(OBJ)/%.fpo.o: $(OBJ)/%.fpo ; $(EMBED)

.SECONDARY:

clean:
	rm -rf $(OUT)

-include $(COBJS:.o=.d)
