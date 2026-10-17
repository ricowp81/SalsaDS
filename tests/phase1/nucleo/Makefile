PROGRAM := salsads-nucleo
TITLE   := SalsaDS núcleo
APPID   := SLDS00008
SRCS    := main.c
COMMON  := src/platform/ps3/log.c src/frontend/dspad.c src/frontend/layout.c \
           $(patsubst $(abspath ../../..)/%,%,$(wildcard $(abspath ../../..)/src/core/*.c $(abspath ../../..)/src/bios/*.c $(abspath ../../..)/src/cpu/*.c))
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm
RSXKIT  := 1

# ROM de demostración propia, embebida (símbolos demo_bin, demo_bin_end y demo_bin_size en demo_bin.h): se usa
# si no existe /dev_hdd0/salsads/rom.nds.
DATA     := demo
demo_FILE := tests/roms/homebrew/demo.nds

# Versión del árbol (la calcula tools/build.sh en el host) para la primera línea del log. Sin ella, «desconocida».
CFLAGS  := -DNUCLEO_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
