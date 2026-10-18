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
SDS_GITREV ?= desconocida

ARM9_FLAGS := -marm -mcpu=arm946e-s+nofp -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
              -D__NDS__ -D__BLOCKSDS__ -DARM9 -isystem $(BLOCKSDS)/libs/libnds/include -specs=$(BLOCKSDS)/sys/crts/ds_arm9.specs
ARM9_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds9 -lc -Wl,--end-group
ARM7_FLAGS := -marm -mcpu=arm7tdmi -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
              -D__NDS__ -D__BLOCKSDS__ -DARM7 -isystem $(BLOCKSDS)/libs/libnds/include -specs=$(BLOCKSDS)/sys/crts/ds_arm7.specs
ARM7_LIBS  := -L$(BLOCKSDS)/libs/libnds/lib -Wl,--start-group -lnds7 -lc -Wl,--end-group

ROMS := tablas gx snd hola cpu9 cpuarm mem irq dma ipc bios bios13 periph demo ocio carga save gfxprobe gfxrand gfxfx gfxfx1 gfxmos0 gfxmos1 gfxmos2 gfxmos3 gfxmos4 gfxmos5 gfxmos6 gfxmos7 gfxmos8 gfxmos9 gfxmos10 gfxmos11 gfxobjmap gfxobjmap1 gfxomap00 gfxomap01 gfxomap02 gfxomap03 gfxomap10 gfxomap11 gfxomap12 gfxomap13 gfxextpal gfxextpal1 gfxextpal2 gfxtext gfxaffine gfxaffine1 gfxext16 gfxbmp8 gfxbmpd gfxobj gfxobjaff gfxobjx gfxraster gfxwin gfxwinx gfxblend0 gfxblend1 gfxblend2 gfxblend3 gfxblend4 gfxblend5 gfxmode0 gfxmode2 gfxmode3
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
SRCS_dma := src/common/sds.c src/dma/arm9.c
SRCS7_dma := src/common/sds7.c src/dma/arm7.c
SRCS_ipc := src/common/sds.c src/ipc/arm9.c
SRCS7_ipc := src/common/sds7.c src/ipc/arm7.c
SRCS_bios := src/common/sds.c src/bios/arm9.c src/bios/helpers.S
SRCS7_bios := src/common/sds7.c src/bios/arm7.c src/bios/helpers.S
SRCS_bios13 := $(SRCS_bios)
SRCS7_bios13 := $(SRCS7_bios)
FLAGS_bios13 := -DBIOS_SWI13
ARM7X_bios13 := -DBIOS_SWI13
SRCS_periph := src/common/sds.c src/periph/arm9.c
SRCS7_periph := src/common/sds7.c src/periph/arm7.c
SRCS_tablas := src/common/sds.c src/tablas/arm9.c
SRCS7_tablas := src/tablas/arm7.c
SRCS_gx := src/common/sds.c src/gx/arm9.c
SRCS_snd := src/common/sds.c src/snd/arm9.c
SRCS7_snd := src/snd/arm7.c
SRCS_demo := src/common/sds.c src/demo/arm9.c
SRCS7_demo := src/common/sds7.c src/demo/arm7.c
SRCS_ocio := src/ocio/arm9.c
SRCS7_ocio := src/ocio/arm7.c
SRCS_carga := src/common/sds.c src/carga/arm9.c
SRCS7_carga := src/carga/arm7.c
SRCS_save := src/common/sds.c src/save/arm9.c
SRCS_gfxprobe := src/common/sds.c src/gfx/probe.c
SRCS_gfxrand := src/common/sds.c src/gfx/rand.c
SRCS_gfxfx := src/common/sds.c src/gfx/fx.c
SRCS_gfxmos0 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos0 := -DMOS=0
TITLE_gfxmos0 := SalsaDS gfxmos0;mosaico 0;SalsaDS
SRCS_gfxmos1 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos1 := -DMOS=1
TITLE_gfxmos1 := SalsaDS gfxmos1;mosaico 1;SalsaDS
SRCS_gfxmos2 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos2 := -DMOS=2
TITLE_gfxmos2 := SalsaDS gfxmos2;mosaico 2;SalsaDS
SRCS_gfxmos3 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos3 := -DMOS=3
TITLE_gfxmos3 := SalsaDS gfxmos3;mosaico 3;SalsaDS
SRCS_gfxmos4 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos4 := -DMOS=4
TITLE_gfxmos4 := SalsaDS gfxmos4;mosaico 4;SalsaDS
SRCS_gfxmos5 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos5 := -DMOS=5
TITLE_gfxmos5 := SalsaDS gfxmos5;mosaico 5;SalsaDS
SRCS_gfxmos6 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos6 := -DMOS=6
TITLE_gfxmos6 := SalsaDS gfxmos6;mosaico 6;SalsaDS
SRCS_gfxmos7 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos7 := -DMOS=7
TITLE_gfxmos7 := SalsaDS gfxmos7;mosaico 7;SalsaDS
SRCS_gfxmos8 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos8 := -DMOS=8
TITLE_gfxmos8 := SalsaDS gfxmos8;mosaico 8;SalsaDS
SRCS_gfxmos9 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos9 := -DMOS=9
TITLE_gfxmos9 := SalsaDS gfxmos9;mosaico 9;SalsaDS
SRCS_gfxmos10 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos10 := -DMOS=10
TITLE_gfxmos10 := SalsaDS gfxmos10;mosaico 10;SalsaDS
SRCS_gfxextpal := src/common/sds.c src/gfx/extpal.c
SRCS_gfxextpal1 := src/common/sds.c src/gfx/extpal.c
FLAGS_gfxextpal1 := -DEXT=1
TITLE_gfxextpal := SalsaDS gfxextpal;paletas extendidas;SalsaDS
TITLE_gfxextpal1 := SalsaDS gfxextpal1;paletas normales;SalsaDS
SRCS_gfxtext := src/common/sds.c src/gfx/hand.c
FLAGS_gfxtext := -DSCENE=0
TITLE_gfxtext := SalsaDS gfxtext;fondos de texto;SalsaDS
SRCS_gfxaffine := src/common/sds.c src/gfx/hand.c
FLAGS_gfxaffine := -DSCENE=1 
TITLE_gfxaffine := SalsaDS gfxaffine;escena grafica 1;SalsaDS
SRCS_gfxaffine1 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxaffine1 := -DSCENE=12 -DAFIN=1
TITLE_gfxaffine1 := SalsaDS gfxaffine1;escena grafica 12;SalsaDS
SRCS_gfxext16 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxext16 := -DSCENE=2 
TITLE_gfxext16 := SalsaDS gfxext16;escena grafica 2;SalsaDS
SRCS_gfxbmp8 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxbmp8 := -DSCENE=3 
TITLE_gfxbmp8 := SalsaDS gfxbmp8;escena grafica 3;SalsaDS
SRCS_gfxbmpd := src/common/sds.c src/gfx/hand.c
FLAGS_gfxbmpd := -DSCENE=4 
TITLE_gfxbmpd := SalsaDS gfxbmpd;escena grafica 4;SalsaDS
SRCS_gfxobj := src/common/sds.c src/gfx/hand.c
FLAGS_gfxobj := -DSCENE=5
TITLE_gfxobj := SalsaDS gfxobj;OBJ normales;SalsaDS
SRCS_gfxobjaff := src/common/sds.c src/gfx/hand.c
FLAGS_gfxobjaff := -DSCENE=6
TITLE_gfxobjaff := SalsaDS gfxobjaff;OBJ afines;SalsaDS
SRCS_gfxwin := src/common/sds.c src/gfx/hand.c
FLAGS_gfxwin := -DSCENE=8 
TITLE_gfxwin := SalsaDS gfxwin;escena grafica 8;SalsaDS
SRCS_gfxwinx := src/common/sds.c src/gfx/hand.c
FLAGS_gfxwinx := -DSCENE=8 -DWINX=1
TITLE_gfxwinx := SalsaDS gfxwinx;escena grafica 8;SalsaDS
SRCS_gfxblend0 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend0 := -DSCENE=9 -DBLEND=0
TITLE_gfxblend0 := SalsaDS gfxblend0;escena grafica 9;SalsaDS
SRCS_gfxblend1 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend1 := -DSCENE=9 -DBLEND=1
TITLE_gfxblend1 := SalsaDS gfxblend1;escena grafica 9;SalsaDS
SRCS_gfxblend2 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend2 := -DSCENE=9 -DBLEND=2
TITLE_gfxblend2 := SalsaDS gfxblend2;escena grafica 9;SalsaDS
SRCS_gfxblend3 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend3 := -DSCENE=9 -DBLEND=3
TITLE_gfxblend3 := SalsaDS gfxblend3;escena grafica 9;SalsaDS
SRCS_gfxblend4 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend4 := -DSCENE=9 -DBLEND=4
TITLE_gfxblend4 := SalsaDS gfxblend4;escena grafica 9;SalsaDS
SRCS_gfxblend5 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxblend5 := -DSCENE=9 -DBLEND=5
TITLE_gfxblend5 := SalsaDS gfxblend5;escena grafica 9;SalsaDS
SRCS_gfxobjx := src/common/sds.c src/gfx/hand.c
FLAGS_gfxobjx := -DSCENE=7 
TITLE_gfxobjx := SalsaDS gfxobjx;escena grafica 7;SalsaDS
SRCS_gfxraster := src/common/sds.c src/gfx/hand.c
FLAGS_gfxraster := -DSCENE=13 
TITLE_gfxraster := SalsaDS gfxraster;escena grafica 13;SalsaDS
SRCS_gfxmos11 := src/common/sds.c src/gfx/mos.c
FLAGS_gfxmos11 := -DMOS=11
TITLE_gfxmos11 := SalsaDS gfxmos11;mosaico 11;SalsaDS
SRCS_gfxextpal2 := src/common/sds.c src/gfx/extpal.c
FLAGS_gfxextpal2 := -DBIT15=1
TITLE_gfxextpal2 := SalsaDS gfxextpal2;paletas extendidas bit 15;SalsaDS
SRCS_gfxobjmap := src/common/sds.c src/gfx/objmap.c
SRCS_gfxobjmap1 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxobjmap1 := -DMAP=1
TITLE_gfxobjmap := SalsaDS gfxobjmap;OBJ sin banco;SalsaDS
TITLE_gfxobjmap1 := SalsaDS gfxobjmap1;OBJ sin banco 1;SalsaDS
SRCS_gfxomap00 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap00 := -DMAP=1 -DBPP4=0 -DSHIFT=0
TITLE_gfxomap00 := SalsaDS gfxomap00;OBJ sin banco gfxomap00;SalsaDS
SRCS_gfxomap01 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap01 := -DMAP=1 -DBPP4=0 -DSHIFT=1
TITLE_gfxomap01 := SalsaDS gfxomap01;OBJ sin banco gfxomap01;SalsaDS
SRCS_gfxomap02 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap02 := -DMAP=1 -DBPP4=0 -DSHIFT=2
TITLE_gfxomap02 := SalsaDS gfxomap02;OBJ sin banco gfxomap02;SalsaDS
SRCS_gfxomap03 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap03 := -DMAP=1 -DBPP4=0 -DSHIFT=3
TITLE_gfxomap03 := SalsaDS gfxomap03;OBJ sin banco gfxomap03;SalsaDS
SRCS_gfxomap10 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap10 := -DMAP=1 -DBPP4=1 -DSHIFT=0
TITLE_gfxomap10 := SalsaDS gfxomap10;OBJ sin banco gfxomap10;SalsaDS
SRCS_gfxomap11 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap11 := -DMAP=1 -DBPP4=1 -DSHIFT=1
TITLE_gfxomap11 := SalsaDS gfxomap11;OBJ sin banco gfxomap11;SalsaDS
SRCS_gfxomap12 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap12 := -DMAP=1 -DBPP4=1 -DSHIFT=2
TITLE_gfxomap12 := SalsaDS gfxomap12;OBJ sin banco gfxomap12;SalsaDS
SRCS_gfxomap13 := src/common/sds.c src/gfx/objmap.c
FLAGS_gfxomap13 := -DMAP=1 -DBPP4=1 -DSHIFT=3
TITLE_gfxomap13 := SalsaDS gfxomap13;OBJ sin banco gfxomap13;SalsaDS
SRCS_gfxfx1 := src/common/sds.c src/gfx/fx.c
FLAGS_gfxfx1 := -DFX=1
SRCS_gfxmode0 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxmode0 := -DSCENE=11 -DDMODE=0
TITLE_gfxmode0 := SalsaDS gfxmode0;pantalla apagada;SalsaDS
SRCS_gfxmode2 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxmode2 := -DSCENE=11 -DDMODE=2
TITLE_gfxmode2 := SalsaDS gfxmode2;visualizacion de VRAM;SalsaDS
SRCS_gfxmode3 := src/common/sds.c src/gfx/hand.c
FLAGS_gfxmode3 := -DSCENE=11 -DDMODE=3
TITLE_gfxmode3 := SalsaDS gfxmode3;memoria principal por FIFO;SalsaDS
TITLE_hola := SalsaDS hola;prueba de VRAM;SalsaDS
TITLE_cpu9 := SalsaDS cpu9;pruebas de CPU ARM9;SalsaDS
TITLE_cpuarm := SalsaDS cpuarm;pruebas de CPU ARM9 y ARM7;SalsaDS
TITLE_mem := SalsaDS mem;mapa de memoria y VRAM;SalsaDS
TITLE_irq := SalsaDS irq;interrupciones, temporizadores y video;SalsaDS
TITLE_dma := SalsaDS dma;transferencias DMA;SalsaDS
TITLE_ipc := SalsaDS ipc;sincronia y FIFO entre CPUs;SalsaDS
TITLE_bios := SalsaDS bios;llamadas SWI de la BIOS;SalsaDS
TITLE_bios13 := SalsaDS bios13;SWI 0x13 Huffman;SalsaDS
TITLE_periph := SalsaDS periph;SPI, RTC, cartucho y matematicas;SalsaDS
TITLE_tablas := SalsaDS tablas;SWI 1Ah-1Ch del ARM7;SalsaDS
TITLE_gx := SalsaDS gx;FIFO de comandos de geometria;SalsaDS
TITLE_snd := SalsaDS snd;sonido: 16 canales y captura;SalsaDS
TITLE_demo := SalsaDS demo;demostracion del nucleo;SalsaDS
TITLE_ocio := SalsaDS ocio;las dos CPUs en espera;SalsaDS
TITLE_carga := SalsaDS carga;trabajo continuo en las dos CPUs;SalsaDS
TITLE_save := SalsaDS save;memoria de guardado del cartucho;SalsaDS
TITLE_gfxprobe := SalsaDS gfxprobe;desde que linea se ve un cambio;SalsaDS
TITLE_gfxrand := SalsaDS gfxrand;escenas 2D aleatorias;SalsaDS
TITLE_gfxfx := SalsaDS gfxfx;brillo del motor 2D;SalsaDS
TITLE_gfxfx1 := SalsaDS gfxfx1;mezcla alfa del motor 2D;SalsaDS

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
	@$$(CC) $$(ARM9_FLAGS) $$(FLAGS_$(1)) -DSDS_ROM_NAME=\"$(1)\" -DSDS_GITREV=\"$$(SDS_GITREV)\" -Wl,-Map,$$(OUT)/$(1).map -Wl,--gc-sections $$(SRCS_$(1)) -o $$@ $$(ARM9_LIBS)
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

# Las ROMs de exploración (gfxprobe, gfxobjmap*, gfxomap*: sondas de melonDS) no se instalan; se construyen con make
INSTALL := $(filter-out gfxprobe gfxobjmap gfxobjmap1 gfxomap%,$(ROMS))
install: all
	cp $(addprefix $(OUT)/,$(addsuffix .nds,$(INSTALL))) homebrew/
	cd homebrew && sha256sum *.nds > SHA256SUMS

clean:
	rm -rf $(OUT)
