PROGRAM := salsads-spe-raster3d
TITLE   := SalsaDS raster
APPID   := SLDS00002
SRCS    := main.c r3d_core.c r3d_ref.c r3d_scene.c r3d_io_mem.c r3d_selftest.c
COMMON  := src/platform/ps3/log.c
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm
SPUKIT  := 1

# Programa SPU: el mismo núcleo (r3d_core.c) con la E/S por DMA de spu_main.c.
# El enlace falla si entre la cadena de llamadas más honda y el final del
# programa quedan menos de 8 KB (tools/spu-stack.py). spu-gcc integra
# spukit_run y su manejador (no queda llamada por puntero); lo que sí llama
# por puntero es el arranque y la salida de la biblioteca de C.
SPU_PROGS        := raster
raster_SRCS      := spu_main.c r3d_core.c r3d_selftest.c
raster_STACK_MIN := 8192
raster_STACK_ARGS := --ignore exit --ignore __call_exitprocs --ignore __do_global_ctors_aux \
                     --ignore __do_global_dtors_aux --ignore register_tm_clones --ignore deregister_tm_clones
SPU_EXTRA_CFLAGS := -ffp-contract=fast

# Versión del árbol (la calcula tools/build.sh en el host) para la primera
# línea del log. Sin ella, «desconocida».
CFLAGS  := -DR3D_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\" -ffp-contract=off

include ../../../mk/ps3.mk
