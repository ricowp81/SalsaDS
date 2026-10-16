# Registro de rendimiento

Cada optimización lleva medición antes/después **en la PS3 real** (RPCS3 y el
host no reproducen los tiempos del Cell). Una fila por medición, la más
reciente arriba. Formato: fecha, commit, prueba, entorno, antes, después,
notas.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| 2026-10-16 | 08b7ef0 | **ppe-bench: memoria LE** (`lwbrx`/`stwbrx`/`lhbrx`/`sthbrx` contra normal y contra rotaciones) | PS3 real con HEN (paquete «SalsaDS bench», SLDS00001; log en `docs/datos/ppe-bench-08b7ef0-ps3.log`) | carga normal: 7,16 ciclos de latencia (32 bits), 2,56 de throughput | `lwbrx`: 7,17 y 2,56 (igual); carga + rotaciones: 13,36 y 4,46 | `lhbrx` 7,19 contra 7,20; rotaciones +4–6 de latencia. Cruce de sector de 32 B: 50–57 ciclos (microcódigo); no alineado dentro del sector: gratis. Decisión: memoria LE con `*brx` |
| 2026-10-16 | 08b7ef0 | **ppe-bench: banderas NZCV** en un bucle ARM de 7,55 instr./vuelta | PS3 real con HEN | cota sin banderas: 3,33 ciclos por instr. ARM | (a) completas 16,70; **(b) perezosas 6,01**; (c) N/Z `cr0`, C `cr1`, V `cr2` 7,60; (d) `add.`/`subfc.` + XER.CA 6,55 | Decisión: perezosas (b) + fusión flags/salto y banderas muertas en el recompilador. Reevaluar (c)/(d) con trazas reales (Fase 4) |
| 2026-10-16 | 08b7ef0 | **ppe-bench: costo de instrucciones** (throughput, bruto) | PS3 real con HEN | `add` 1,54 ciclos | `add.` 1,67, `subf.` 1,55, `neg.` 1,67, `addc`/`subfc` 1,54, `adde`/`subfe`/`addze` 2,05 (nativas); `and.`/`andi.`/`cntlzw.`/`rlwinm.` 14,5; `slw`/`sraw` 16,6; `mullw` 11,5; `mfcr` 27,0; `mtcrf 0xff` 20,7; `mtocrf` 1,55 | Los netos 0,00 son artefacto de un bucle de referencia más lento (nops de 2,06 ciclos) que las instrucciones: valen los brutos. Corrige la guía de Salsa64: la aritmética con punto y el acarreo no son microcódigo. `mtctr`+`bctr`: sin penalización con 0 instrucciones en medio (destino fijo) |
| 2026-10-16 | 08b7ef0 | **ppe-bench: despacho de un intérprete ARM mínimo** (bucle de 9 instr. por elemento) | PS3 real con HEN | `switch`: 16,53 MIPS guest (193,6 ciclos/instr.) | tabla de punteros 16,52 MIPS; `goto` calculado 17,88 MIPS (179,0) | Lejos de los ~100 MIPS de la DS: el recompilador es obligatorio. El despacho cambia ≤ 8 % |
