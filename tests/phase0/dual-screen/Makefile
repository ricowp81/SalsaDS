# SPDX-License-Identifier: GPL-2.0-or-later
PROGRAM := salsads-dual-screen
TITLE   := SalsaDS pantallas
APPID   := SLDS00004
SRCS    := main.c
COMMON  := src/platform/ps3/log.c src/frontend/dspad.c src/frontend/layout.c
LIBS    := -lrsx -lgcm_sys -lio -lsysutil -lnet -lsysmodule -lrt -llv2 -lm
RSXKIT  := 1

# Versión del árbol (la calcula tools/build.sh en el host) para la primera
# línea del log. Sin ella, «desconocida».
CFLAGS  := -DSCREENS_VERSION=\"$(or $(SALSADS_VERSION),desconocida)\"

include ../../../mk/ps3.mk
