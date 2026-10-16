PROGRAM := salsads-spe-gpu2d
TITLE   := SalsaDS 2D
APPID   := SLDS00003
SRCS    := main.c ../../../src/gpu2d/gpu2d_ref.c ../../../src/gpu2d/gpu2d_scene.c
COMMON  := src/platform/ps3/log.c
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2
SPUKIT  := 1

SPU_PROGS    := gpu2d
gpu2d_SRCS   := spu/gpu2d.c ../../../src/gpu2d/gpu2d_compose.c
gpu2d_STACK_MIN := 4096
# Las llamadas indirectas de la salida del proceso (exit y los destructores
# de la biblioteca de C) no las alcanza el trabajo: el SPE termina con
# spu_thread_exit.
gpu2d_STACK_ARGS := --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones

# Versión del árbol (la calcula tools/build.sh en el host) para la primera
# línea del log. Sin ella, «desconocida».
CFLAGS  := -DGPU2D_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
