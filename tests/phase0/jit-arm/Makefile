# SPDX-License-Identifier: GPL-2.0-or-later
PROGRAM := salsads-jit-arm
TITLE   := SalsaDS JIT
APPID   := SLDS00007
SRCS    := main.c rec_cache.c threads.c ../ppe-bench/interp.c
COMMON  := src/platform/ps3/log.c src/jit/jit_arm.c src/jit/ref_arm.c src/jit/loops.c
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2

# La caché de código en una cola NOBITS del segmento de texto (mk/rec-cache.ld);
# con -T gcc ya no agrega lv2.ld solo: va después.
LDFLAGS := -T $(abspath ../../../mk/rec-cache.ld) -T lv2.ld

# Versión del árbol (la calcula tools/build.sh en el host) para la primera
# línea del log. Sin ella, «desconocida».
CFLAGS  := -DJIT_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\" -I../ppe-bench

include ../../../mk/ps3.mk
