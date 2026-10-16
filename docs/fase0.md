# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | en curso |
| 0.3 Microbenchmarks del PPE | en curso: programa listo; **faltan los números de la PS3 real** |
| 0.4 Rasterizador 3D en un SPE | pendiente |
| 0.5 Composición 2D en un SPE | pendiente |
| 0.6 Salida de dos pantallas | pendiente |
| 0.7 Prueba de JIT ARM → PPC | pendiente |

## 0.1 Esqueleto

Criterio: `tools/build.sh` compila un programa vacío de PS3 a `.pkg` y
`tools/build.sh test-host` corre una prueba en LE y en BE.

- `tools/build.sh -C tests/phase0/empty all pkg` → `build/salsads-empty/salsads-empty.pkg`;
  el `.elf` corre en RPCS3 y escribe `RESULTADO empty: OK`
  (`tools/rpcs3-run.py empty`).
- `tools/build.sh test-host` → `LE OK` y `BE OK` (12 comprobaciones cada una).
