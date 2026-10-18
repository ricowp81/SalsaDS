# SalsaDS — pruebas unitarias en el host. Desde la raíz: tools/build.sh test-host
# (corre en little-endian con el gcc nativo del contenedor y en big-endian con
# qemu-ppc64; ver tools/test-host.sh). Por omisión compila con AddressSanitizer
# y UBSan; SAN= los quita (lo hace la compilación cruzada).

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno \
           -I$(ROOT)/src -I$(CURDIR) -I$(ROOT)/tests/phase0/ppe-bench -I$(ROOT)/tests/phase0/spe-raster3d \
           $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm

# Los núcleos y el intérprete de tests/phase0/ppe-bench se prueban aquí: los
# de ensamblador solo existen en PowerPC64 (qemu); el resto corre en todo host.
# El rasterizador de tests/phase0/spe-raster3d, con la E/S de memcpy.
R3D       := $(ROOT)/tests/phase0/spe-raster3d
TEST_SRCS := $(wildcard $(CURDIR)/*.c) $(ROOT)/tests/phase0/ppe-bench/kernels.c \
             $(ROOT)/tests/phase0/ppe-bench/interp.c \
             $(wildcard $(ROOT)/src/gpu2d/*.c) $(wildcard $(ROOT)/src/gpu3d/*.c) $(wildcard $(ROOT)/src/audio/*.c) $(wildcard $(ROOT)/src/frontend/*.c) $(wildcard $(ROOT)/src/jit/*.c) \
             $(wildcard $(ROOT)/src/cpu/*.c) \
             $(R3D)/r3d_core.c $(R3D)/r3d_ref.c $(R3D)/r3d_scene.c $(R3D)/r3d_io_mem.c \
             $(R3D)/r3d_selftest.c
OBJS      := $(patsubst $(ROOT)/%.c,$(OUT)/obj/%.o,$(TEST_SRCS))

.PHONY: all run clean

all: $(OUT)/salsads-unit

run: $(OUT)/salsads-unit
	ASAN_OPTIONS=detect_leaks=0 $<

$(OUT)/salsads-unit: $(OBJS)
	@echo "  LD      $(notdir $@)"
	@$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OUT)/obj/%.o: $(ROOT)/%.c
	@echo "  CC      $*.c"
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(OUT)

-include $(OBJS:.o=.d)
