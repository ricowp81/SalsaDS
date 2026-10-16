# syntax=docker/dockerfile:1.7
#
# Toolchain ARM libre para las ROMs de prueba de la DS: BlocksDS
# (github.com/blocksds/sdk; arm-none-eabi-gcc del toolchain Wonderful, libnds,
# ndstool). Se parte de la imagen oficial de Docker Hub fijada por digest para
# que las ROMs se reconstruyan igual: v1.24.0 de BlocksDS, GCC 16.2.0.
# Licencias de lo que acaba dentro de una ROM: docs/fase0.md (0.2) y
# tests/roms/homebrew/AVISOS.md.
#
# Construir con tools/docker-image-nds.sh.
ARG BLOCKSDS=skylyrac/blocksds:slim-latest@sha256:779f6193d1030c62e60d1f0f39c3e32cf4045c0bd224a45c0ceadf9d9ab01a51
FROM ${BLOCKSDS}
LABEL org.opencontainers.image.title="salsads-nds" \
      org.opencontainers.image.description="BlocksDS para las ROMs de prueba de SalsaDS"
ENV BLOCKSDS=/opt/wonderful/thirdparty/blocksds/core \
    WONDERFUL_TOOLCHAIN=/opt/wonderful \
    PATH=/opt/wonderful/toolchain/gcc-arm-none-eabi/bin:/opt/wonderful/bin:$PATH
WORKDIR /src
