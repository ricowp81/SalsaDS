# ROMs de prueba de la DS. Se ejecuta dentro de la imagen salsads-nds
# (tools/build-roms.sh). Toolchain: BlocksDS (arm-none-eabi-gcc, libnds,
# ndstool); licencias en tests/roms/homebrew/AVISOS.md.
#   make            construye build/roms/*.nds
#   make install    las copia a tests/roms/homebrew/ con SHA256SUMS
BLOCKSDS ?= /opt/wonderful/thirdparty/blocksds/core
ROOT     := $(abspath ../..)
OUT      := $(ROOT)/build/roms
NDSTOOL  := $(BLOCKSDS)/tools/ndstool/ndstool
CC       := arm-none-eabi-gcc
ICON     := src/icon.bmp

ARM9_FLAGS := -marm -mcpu=arm946e-s+nofp -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
              -D__NDS__ -D__BLOCKSDS__ -DARM9 -isystem $(BLOCKSDS)/libs/libnds/include -specs=$(BLOCKSDS)/sys/crts/ds_arm9.specs
ARM9_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds9 -lc -Wl,--end-group
ARM7_FLAGS := -marm -mcpu=arm7tdmi -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
              -D__NDS__ -D__BLOCKSDS__ -DARM7 -isystem $(BLOCKSDS)/libs/libnds/include -specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs
ARM7_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds7 -lc -Wl,--end-group

ROMS := hola cpu9 cpuarm mem irq
SRCS_hola := src/common/sds.c src/hola/main.c
SRCS_cpu9 := src/common/sds.c $(wildcard src/cpu9/*.c) $(wildcard src/cpu9/*.S)
SRCS_cpuarm := src/common/sds.c src/cpuarm/arm9.c src/cpuarm/helpers.S src/cpu9/thumb.S
SRCS7_cpuarm := src/common/sds7.c src/cpuarm/arm7.c src/cpuarm/helpers.S src/cpu9/thumb.S
SRCS_mem := src/common/sds.c src/mem/arm9.c
SRCS7_mem := src/common/sds7.c src/mem/arm7.c
# el ARM7 de mem prueba la WRAM compartida: su código y su pila tienen que quedar en la WRAM propia (64 KB)
ARM7X_mem := -Wl,--defsym=__shared_wram_size=0
SRCS_irq := src/common/sds.c src/irq/arm9.c
SRCS7_irq := src/common/sds7.c src/irq/arm7.c
TITLE_hola := SalsaDS hola;prueba de VRAM;SalsaDS
TITLE_cpu9 := SalsaDS cpu9;pruebas de CPU ARM9;SalsaDS
TITLE_cpuarm := SalsaDS cpuarm;pruebas de CPU ARM9 y ARM7;SalsaDS
TITLE_mem := SalsaDS mem;mapa de memoria y VRAM;SalsaDS
TITLE_irq := SalsaDS irq;interrupciones, temporizadores y video;SalsaDS

.PHONY: all install clean $(ROMS)
all: $(addprefix $(OUT)/,$(addsuffix .nds,$(ROMS)))

$(ROMS): %: $(OUT)/%.nds

$(OUT)/arm7min.elf: src/arm7min/main.c
	@mkdir -p $(@D)
	@echo "  ARM7    $@"
	@$(CC) $(ARM7_FLAGS) $< -o $@ $(ARM7_LIBS)

# ARM7 de cada ROM: el propio (SRCS7_<rom>) o, si no tiene, el mínimo que no hace nada
ARM7_OF = $(if $(SRCS7_$(1)),$(1)7,arm7min)
HDRS := $(wildcard src/*/*.h)

define ROM_RULES
$$(OUT)/$(1).elf: $$(SRCS_$(1)) $$(HDRS)
	@mkdir -p $$(@D)
	@echo "  ARM9    $$@"
	@$$(CC) $$(ARM9_FLAGS) -Wl,-Map,$$(OUT)/$(1).map -Wl,--gc-sections $$(SRCS_$(1)) -o $$@ $$(ARM9_LIBS)
$$(OUT)/$(1).nds: $$(OUT)/$(1).elf $$(OUT)/$(call ARM7_OF,$(1)).elf $$(ICON)
	@echo "  NDSTOOL $$@"
	@$$(NDSTOOL) -c $$@ -7 $$(OUT)/$(call ARM7_OF,$(1)).elf -9 $$(OUT)/$(1).elf -b $$(ICON) "$$(TITLE_$(1))" >/dev/null
endef
$(foreach r,$(ROMS),$(eval $(call ROM_RULES,$(r))))

define ROM7_RULES
$$(OUT)/$(1)7.elf: $$(SRCS7_$(1)) $$(HDRS)
	@mkdir -p $$(@D)
	@echo "  ARM7    $$@"
	@$$(CC) $$(ARM7_FLAGS) $$(ARM7X_$(1)) -Wl,-Map,$$(OUT)/$(1)7.map -Wl,--gc-sections $$(SRCS7_$(1)) -o $$@ $$(ARM7_LIBS)
endef
$(foreach r,$(ROMS),$(if $(SRCS7_$(r)),$(eval $(call ROM7_RULES,$(r)))))

install: all
	cp $(addprefix $(OUT)/,$(addsuffix .nds,$(ROMS))) homebrew/
	cd homebrew && sha256sum *.nds > SHA256SUMS

clean:
	rm -rf $(OUT)
