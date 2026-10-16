# syntax=docker/dockerfile:1.7
#
# Pruebas big-endian en la Mac: el código compilado para Linux ppc64
# big-endian (ELFv1, la misma ABI que PSL1GHT en la PS3) y ejecutado con
# qemu-user. Es la única forma de correr en el host el código que genera el
# recompilador: RPCS3 no ejecuta código generado (docs/cell-desde-salsa64.md).
#
# Dos imágenes (tools/docker-image-ppc64.sh):
#   --target cross  salsads-ppc64-cross:dev, linux/amd64: Debian no trae
#                   gcc-powerpc64-linux-gnu para arm64; corre con la
#                   emulación de amd64 de Docker y compila en segundos.
#   --target qemu   salsads-qemu:dev, nativa (arm64): qemu-ppc64 dentro de
#                   un contenedor amd64 se cuelga; nativo es rápido.

ARG DEBIAN=debian:12-slim

FROM --platform=linux/amd64 ${DEBIAN} AS cross
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc-powerpc64-linux-gnu libc6-dev-ppc64-cross \
      binutils-powerpc64-linux-gnu make \
 && rm -rf /var/lib/apt/lists/*

FROM ${DEBIAN} AS qemu
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends qemu-user-static \
 && rm -rf /var/lib/apt/lists/*
