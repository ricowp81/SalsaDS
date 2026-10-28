# SPDX-License-Identifier: GPL-2.0-or-later
PROGRAM := salsads-spe-audio
TITLE   := SalsaDS audio prueba
APPID   := SLDS00033
SRCS    := main.c
COMMON  := src/platform/ps3/log.c src/audio/snd_mix.c
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm
SPUKIT  := 1

SPU_PROGS := snd
snd_SRCS := ../../../src/spu/snd/snd.c ../../../src/audio/snd_mix.c
snd_STACK_MIN := 8192
snd_STACK_ARGS := --edge snd_run:bus_fetch --edge snd_run:bus_store --edge chan_step_old:bus_fetch --edge voice_byte.isra.0:bus_fetch --edge cap_flush.isra.0:bus_store \
                  --ignore exit --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore register_tm_clones
CFLAGS := -DAUDIO_TEST_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
