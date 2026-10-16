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
              -D__NDS__ -D__BLOCKSDS__ -DARM9 -specs=$(BLOCKSDS)/sys/crts/ds_arm9.specs
ARM9_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds9 -lc -Wl,--end-group
ARM7_FLAGS := -marm -mcpu=arm7tdmi -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
              -D__NDS__ -D__BLOCKSDS__ -DARM7 -specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs
ARM7_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds7 -lc -Wl,--end-group

ROMS := hola cpu9
SRCS_hola := src/common/sds.c src/hola/main.c
SRCS_cpu9 := src/common/sds.c $(wildcard src/cpu9/*.c) $(wildcard src/cpu9/*.S)
TITLE_hola := SalsaDS hola;prueba de VRAM;SalsaDS
TITLE_cpu9 := SalsaDS cpu9;pruebas de CPU ARM9;SalsaDS

.PHONY: all install clean $(ROMS)
all: $(addprefix $(OUT)/,$(addsuffix .nds,$(ROMS)))

$(ROMS): %: $(OUT)/%.nds

$(OUT)/arm7min.elf: src/arm7min/main.c
	@mkdir -p $(@D)
	@echo "  ARM7    $@"
	@$(CC) $(ARM7_FLAGS) $< -o $@ $(ARM7_LIBS)

define ROM_RULES
$$(OUT)/$(1).elf: $$(SRCS_$(1)) $$(wildcard src/common/*.h src/cpu9/*.h)
	@mkdir -p $$(@D)
	@echo "  ARM9    $$@"
	@$$(CC) $$(ARM9_FLAGS) -Wl,-Map,$$(OUT)/$(1).map -Wl,--gc-sections $$(SRCS_$(1)) -o $$@ $$(ARM9_LIBS)
$$(OUT)/$(1).nds: $$(OUT)/$(1).elf $$(OUT)/arm7min.elf $$(ICON)
	@echo "  NDSTOOL $$@"
	@$$(NDSTOOL) -c $$@ -7 $$(OUT)/arm7min.elf -9 $$(OUT)/$(1).elf -b $$(ICON) "$$(TITLE_$(1))" >/dev/null
endef
$(foreach r,$(ROMS),$(eval $(call ROM_RULES,$(r))))

install: all
	cp $(addprefix $(OUT)/,$(addsuffix .nds,$(ROMS))) homebrew/
	cd homebrew && sha256sum *.nds > SHA256SUMS

clean:
	rm -rf $(OUT)
