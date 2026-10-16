# syntax=docker/dockerfile:1.7
#
# Imagen de desarrollo de SalsaDS: ps3toolchain + PSL1GHT + cgcomp.
#
# No reconstruye el toolchain (más de una hora): parte de la imagen de
# Salsa64 (BASE), que ya lo trae, y le pone el nombre propio de este proyecto
# para que los dos repositorios puedan cambiar a su ritmo. Si BASE no existe,
# se construye con tools/docker-image.sh de Salsa64 (docker/Dockerfile allá;
# github.com/ricowp81/Salsa64).
#
# El Cg Toolkit de NVIDIA (dentro de BASE) es propietario: la imagen es solo
# de uso local y no se publica en ningún registro.
#
# Construir con tools/docker-image.sh.

ARG BASE=salsa64-toolchain:dev
FROM ${BASE}

LABEL org.opencontainers.image.title="salsads-toolchain" \
      org.opencontainers.image.description="ps3toolchain + PSL1GHT + cgcomp para SalsaDS (a partir de ${BASE})"

WORKDIR /src
