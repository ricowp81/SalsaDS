# SalsaDS — se ejecuta dentro de la imagen salsads-toolchain (tools/build.sh).
#   make            programas PS3 de la Fase 0
#   make rsxkit     lib/rsxkit sola (build/rsxkit/librsxkit.a)
#   make spukit     lib/spukit sola (build/spukit/libspukit.a)
#   make test-host  pruebas unitarias con el gcc nativo (LE); con qemu (BE):
#                   tools/build.sh test-host
PHASE0_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/phase0/*/Makefile))

.PHONY: all phase0 rsxkit spukit test-host clean $(PHASE0_DIRS)

all: phase0

rsxkit:
	$(MAKE) -C lib/rsxkit OUT=$(CURDIR)/build/rsxkit

spukit:
	$(MAKE) -C lib/spukit OUT=$(CURDIR)/build/spukit

phase0: $(PHASE0_DIRS)

$(PHASE0_DIRS):
	$(MAKE) -C $@

test-host:
	$(MAKE) -C tests/unit run

clean:
	rm -rf build
