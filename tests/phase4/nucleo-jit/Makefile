# «SalsaDS JIT núcleo» (Fase 4): el mismo programa que «SalsaDS núcleo» (tests/phase1/nucleo/main.c) con el ARM9 en el
# recompilador ARMv5TE → PowerPC64 (src/jit/arec*), la caché de código de 4 MB y su prueba con respaldo de la Fase 0.7.
PROGRAM := salsads-nucleo-jit
TITLE   := SalsaDS JIT núcleo
APPID   := SLDS00009
SRCS    := ../../phase1/nucleo/main.c ../../phase0/jit-arm/rec_cache.c
COMMON  := src/platform/ps3/log.c src/platform/ps3/audio_out.c src/frontend/dspad.c src/frontend/layout.c \
           src/jit/arec.c src/jit/arec_dec.c src/jit/arec_emit.c \
           $(patsubst $(abspath ../../..)/%,%,$(wildcard $(abspath ../../..)/src/core/*.c $(abspath ../../..)/src/bios/*.c $(abspath ../../..)/src/cpu/*.c $(abspath ../../..)/src/gpu2d/gpu2d_ref.c $(abspath ../../..)/src/gpu2d/gpu2d_compose.c $(abspath ../../..)/src/gpu3d/gx.c $(abspath ../../..)/src/gpu3d/gx_exec.c $(abspath ../../..)/src/gpu3d/gx_selftest.c $(abspath ../../..)/src/gpu3d/r3d_ref.c $(abspath ../../..)/src/gpu3d/r3d_core.c $(abspath ../../..)/src/gpu3d/r3d_scene.c $(abspath ../../..)/src/audio/snd_mix.c))
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -laudio -lm
RSXKIT  := 1
SPUKIT  := 1

SPU_EXTRA_CFLAGS := -DG2D_PROF -DGX_PROF -DGX_VEC

SPU_PROGS    := gpu2d snd raster3d geom
gpu2d_SRCS   := ../../../src/spu/gpu2d/gpu2d.c ../../../src/gpu2d/gpu2d_compose.c
gpu2d_STACK_MIN := 4096
gpu2d_STACK_ARGS := --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones
raster3d_SRCS := ../../../src/spu/raster3d/raster3d.c ../../../src/gpu3d/r3d_core.c
raster3d_STACK_MIN := 8192
raster3d_STACK_ARGS := --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones
# Motor de geometría (Fase 3.5): un SPE (src/spu/geom + src/gpu3d/gx_exec.c)
geom_SRCS := ../../../src/spu/geom/geom.c ../../../src/gpu3d/gx_exec.c
geom_STACK_MIN := 4096
geom_STACK_ARGS := --edge emit_poly_inner:job_flush --edge job_flush:put_verts --edge job_flush:put_polys --edge gx_job_run:fetch --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones
snd_SRCS     := ../../../src/spu/snd/snd.c ../../../src/audio/snd_mix.c
snd_STACK_MIN := 4096
snd_STACK_ARGS := --edge snd_run:bus_fetch --edge snd_run:bus_store --ignore __call_exitprocs --ignore __do_global_ctors_aux --ignore exit --ignore register_tm_clones

DATA     := carga
carga_FILE := tests/roms/homebrew/carga.nds

# La caché de código en una cola NOBITS del segmento de texto (mk/rec-cache.ld); con -T gcc ya no agrega lv2.ld solo.
LDFLAGS := -T $(abspath ../../../mk/rec-cache.ld) -T lv2.ld

CFLAGS  := -DNUCLEO_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\" -DNUCLEO_JIT '-DNUCLEO_TITLE="$(TITLE)"' \
           '-DNUCLEO_APPID="$(APPID)"' -I$(abspath ../../phase0/jit-arm) -I$(abspath ../../phase1/nucleo)

include ../../../mk/ps3.mk
