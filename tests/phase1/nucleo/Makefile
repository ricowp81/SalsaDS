PROGRAM := salsads-nucleo
TITLE   := SalsaDS núcleo
APPID   := SLDS00008
SRCS    := main.c
COMMON  := src/platform/ps3/log.c src/platform/ps3/audio_out.c src/frontend/dspad.c src/frontend/layout.c \
           src/jit/arec.c src/jit/arec_dec.c src/jit/arec_emit.c \
           $(patsubst $(abspath ../../..)/%,%,$(wildcard $(abspath ../../..)/src/core/*.c $(abspath ../../..)/src/bios/*.c $(abspath ../../..)/src/cpu/*.c $(abspath ../../..)/src/gpu2d/gpu2d_ref.c $(abspath ../../..)/src/gpu2d/gpu2d_compose.c $(abspath ../../..)/src/gpu3d/gx.c $(abspath ../../..)/src/gpu3d/r3d_ref.c $(abspath ../../..)/src/gpu3d/r3d_core.c $(abspath ../../..)/src/gpu3d/r3d_scene.c $(abspath ../../..)/src/audio/snd_mix.c))
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -laudio -lm
RSXKIT  := 1
SPUKIT  := 1

# Tiempos por etapa del compositor 2D en los SPE (línea DATO spe-etapas del log)
SPU_EXTRA_CFLAGS := -DG2D_PROF

# Composición 2D: un SPE por pantalla (src/spu/gpu2d)
SPU_PROGS    := gpu2d snd raster3d
gpu2d_SRCS   := ../../../src/spu/gpu2d/gpu2d.c ../../../src/gpu2d/gpu2d_compose.c
gpu2d_STACK_MIN := 4096
gpu2d_STACK_ARGS := --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones

# Render 3D (Fase 3.2): dos SPE con bandas alternas (src/spu/raster3d + src/gpu3d/r3d_core.c)
raster3d_SRCS := ../../../src/spu/raster3d/raster3d.c ../../../src/gpu3d/r3d_core.c
raster3d_STACK_MIN := 8192
raster3d_STACK_ARGS := --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones

# Mezclador de sonido: un SPE (src/spu/snd + src/audio)
snd_SRCS     := ../../../src/spu/snd/snd.c ../../../src/audio/snd_mix.c
snd_STACK_MIN := 4096
snd_STACK_ARGS := --edge snd_run:bus_fetch --edge snd_run:bus_store --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones

# ROM de carga propia, embebida (símbolos carga_bin, carga_bin_end y carga_bin_size en carga_bin.h): se usa
# si no existe /dev_hdd0/salsads/rom.nds.
DATA     := carga
carga_FILE := tests/roms/homebrew/carga.nds

# Versión del árbol (la calcula tools/build.sh en el host) para la primera línea del log. Sin ella, «desconocida».
CFLAGS  := -DNUCLEO_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
