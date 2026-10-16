# SalsaDS — pruebas unitarias en el host. Desde la raíz: tools/build.sh test-host
# (corre en little-endian con el gcc nativo del contenedor y en big-endian con
# qemu-ppc64; ver tools/test-host.sh). Por omisión compila con AddressSanitizer
# y UBSan; SAN= los quita (lo hace la compilación cruzada).

ROOT := $(abspath ../..)
OUT  ?= $(ROOT)/build/host

CC      ?= gcc
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Werror -ffp-contract=off -fno-math-errno \
           -I$(ROOT)/src -I$(CURDIR) $(SAN) $(EXTRA_CFLAGS)
LDFLAGS := $(SAN) $(EXTRA_LDFLAGS)
LDLIBS  := -lm

TEST_SRCS := $(wildcard $(CURDIR)/*.c)
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
