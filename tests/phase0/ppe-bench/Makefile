# SPDX-License-Identifier: GPL-2.0-or-later
PROGRAM := salsads-ppe-bench
TITLE   := SalsaDS bench
APPID   := SLDS00001
SRCS    := main.c kernels.c interp.c
COMMON  := src/platform/ps3/log.c
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2

# Versión del árbol (la calcula tools/build.sh en el host) para la primera
# línea del log. Sin ella, «desconocida».
CFLAGS  := -DBENCH_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
