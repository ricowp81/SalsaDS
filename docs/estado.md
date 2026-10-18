# Estado de SalsaDS

Bitácora de lo hecho, lo que quedó a medias y lo decidido. Lo más reciente
arriba. Las mediciones van en `perf-log.md`; los resultados de la Fase 0, en
`fase0.md`.

## 2026-10-17 — FIFO de geometría, sonido y compositor rápido (Fase 2.2)

- **New Super Mario Bros. ya entra a un nivel en el host.** El bucle de 620 883 instrucciones por cuadro no era el sonido: el
  ARM9 esperaba a `GXSTAT` (FIFO de comandos con menos de la mitad) y a la IRQ 21 para mandar su lista de geometría por DMA. Ahora
  la FIFO de geometría existe como «siempre vacía» (se traga las órdenes; IRQ 21 por nivel como en melonDS): título, menú,
  selección de archivo (táctil), intro, mapa y 1-1 con el reloj corriendo, a 91–94 MIPS. Sin 3D no se ve a Mario (Fase 3).
- **SWI 1Ah–1Ch** (tablas de seno, tono y volumen) generadas por fórmula y comprobadas contra las 1556 entradas de melonDS.
- **Sonido**: los 16 canales (PCM8/16, IMA-ADPCM, PSG, ruido), SOUNDCNT/SOUNDBIAS y las capturas, en `src/audio` (portable) con
  un SPE de mezcla (`src/spu/snd`), remuestreo lineal a 48 kHz en flotante (con el punto marcado para un FIR polifásico) y salida de
  audio de la PS3 (`src/platform/ps3/audio_out.c`). La ROM `snd` captura lo que sale del mezclador por hardware y
  `tools/snd-compare.py` lo compara con melonDS muestra a muestra: iguales, con dos divergencias documentadas (`docs/fase2.md`).
  Autoprueba del SPE contra el PPE al arrancar (0 diferencias en RPCS3).
- **2D en el SPE**: los 28 ms de la pantalla A y 8 de la B eran código escalar (multiplicación de 64 bits y división por
  software en los fondos afines; color final de un píxel por vuelta con una docena de saltos): vectorizado, y una vía rápida para
  los fondos que solo se desplazan. En RPCS3 (que cuenta instrucciones) con NSMB: A 14,8 → 4,7 ms, B 1,3 → 1,2; la PS3 lo dirá
  (`DATO spe-etapas`). Instrumentación por etapa en el SPE y contadores de DMA en el host.
- Herramientas: `--press`/`--shot` (entrada y capturas por guion), `--wav`, `--g2d-stats`, `--trace-from`, `--dump-cpu`, TCM visibles.
- Paquete «SalsaDS núcleo» (SLDS00008) con todo lo anterior: ver la ruta y el SHA-256 en `docs/fase2.md` (sección 5) y abajo.
- Pendiente: medir en la PS3 (2D por etapa, audio con faltas, 10 bits), 3D (Fase 3), FIR del remuestreo, HOLD y volumen general
  contra una DS real.

## 2026-10-17 — Motores 2D A y B (Fase 2.1)

El compositor de la 0.5 es ahora el renderizador de las dos pantallas: registro de cambios por línea en el núcleo
(`src/core/video.c`), `src/gpu2d/gpu2d_compose.c` (SPE y host, mismos píxeles), referencia independiente y un SPE por
pantalla en la PS3. Fondos de texto, afines y extendidos, OBJ de todos los tipos, ventanas, mezcla, brillo, mosaico,
paletas extendidas y modos de pantalla 0/1/2/3. Detalle, pruebas y divergencias con melonDS en `docs/fase2.md`.
Paquete «SalsaDS núcleo» (SLDS00008) con los SPE; New Super Mario Bros. llega al menú en el host. Pendiente: medirlo en la
PS3 real, la captura de pantalla, la capa 3D (Fase 3) y el sonido (2.2).

## 2026-10-17 — Cierre de la Fase 1 (1.3)

- **Cuadro ocioso**: el planificador cortaba el tiempo en 8753 tramos por cuadro aunque las dos CPUs estuvieran en HALT; eso eran los
  ≈ 2,1 ms fijos por cuadro medidos en la PS3 (2,73 ms de emulación con la demostración, 8,4 con 28–39 k instrucciones por cuadro:
  2,1 ms + 0,16 µs por instrucción). Ahora el tiempo salta al siguiente evento: en la Mac un cuadro ocioso baja de 177 a 24 µs
  (×7,3) y la demostración de 220 a 67 µs; la carga (ocupadas las dos CPUs) no cambia. Quedan ~525 eventos de video por cuadro.
  El paquete escribe `DATO desglose` y `DATO MIPS` en `nucleo.log` para repartirlo en la consola.
- **New Super Mario Bros. y Mario Kart DS**: no era la CPU ni el IPC. NSMB se apagaba porque no había chip de guardado (el ARM7 lee su estado
  por AUXSPI cada 5 ms y contestábamos FFh para siempre); MKDS repetía sin fin una escritura de la configuración de Wi-Fi en el firmware
  porque el firmware ignoraba las escrituras. Con las dos cosas, los dos configuran el video **igual que melonDS** (DISPCNT de los dos motores y
  VRAMCNT idénticos, y el guardado de NSMB, 8 KB, byte a byte igual al de melonDS). Super Mario 64 DS llega al mismo punto que melonDS.
- **Guardado del cartucho (AUXSPI)**: EEPROM de 0,5 KB, 8 KB, 64 KB y 128 KB, FRAM de 32 KB y Flash de 256 KB a 1 MB; el tipo sale del
  tamaño del archivo de guardado, de una tabla de tres códigos de juego o, si no, EEPROM de 64 KB. ROM propia `save` (siete tamaños,
  little y big endian y sanitizadores; contra melonDS con divergencias anotadas). El firmware de la consola también se puede escribir.
- **ROM de carga** (`carga`) y **ROM ociosa** (`ocio`): dos nuevas, propias, en `tests/roms/homebrew/`. `carga` mantiene las dos CPUs
  ocupadas con trabajo variado y comprueba sus resultados (145 MIPS en la Mac).
- Paquete **«SalsaDS núcleo» (SLDS00008)** del commit 9f2a177: `build/salsads-nucleo/salsads-nucleo.pkg`, SHA-256
  `8da0871ecb4e16e97569e3250055f0f4aef3d7ca8722e568dc627662408f92a4`. Lleva la ROM de carga embebida (o `rom.nds`), guardado por
  juego en `/dev_hdd0/salsads/saves/` y el desglose por cuadro. En RPCS3: OK. **Falta medirlo en la PS3.**
- Herramientas de diagnóstico nuevas en el host: trazas por `--dbg`, `--break7/9` con anillo y pila, `--watch7/9`, `--dump`,
  `tools/melonds-dump.py` (memoria y registros de melonDS por GDB). `tools/melonds-run.py` corre sobre una copia temporal.
- Higiene: cinco volcados de memoria de qemu (`*.core`, ≈ 40 MB) se habían colado en un commit anterior; se quitaron del árbol y se ignoran,
  pero siguen en la historia (si el repositorio se publica, conviene reescribirla antes).
- Pendiente (detalle en `docs/fase1.md`): medir en la PS3, SWI 1Ah–1Ch (sonido), FIFO de geometría (Fase 3), detección del chip de guardado
  sin archivo ni tabla, eventos de video perezosos si el desglose lo pide.

## 2026-10-17 — Máquina de la DS y primera imagen (Fase 1.2)

- `src/core` + `src/bios`: mapa de memoria de las dos CPUs (RAM, WRAM con WRAMCNT, VRAM por bancos con VRAMCNT,
  paleta, OAM), planificador de eventos con el ARM7 a la mitad del reloj del ARM9, IRQ por CPU, DISPSTAT/VCOUNT y
  VBlank/HBlank, temporizadores, DMA, IPC (SYNC y FIFO), teclas, cartucho (ROM desencriptada), SPI con firmware
  generado, táctil, energía y RTC, división y raíz por hardware, BIOS en HLE (bloques de BIOS propios + SWI) y
  arranque directo. Sustituye a `ndsmin`. Detalle, frontera para el ARM7 en otro hilo, tablas y divergencias en
  `docs/fase1.md`.
- **Primera imagen**: modo de visualización directa de VRAM del motor A y color de fondo del motor B. `hola` y `mem`
  dan en melonDS la misma imagen píxel a píxel y los mismos registros. `salsads-cpurun --screens` la vuelca.
- ROMs propias por paso (`mem`, `irq`, `dma`, `ipc`, `bios`, `periph`, `demo`): **todas OK** en SalsaDS (little y
  big-endian, y con tramos del planificador de 1 a 20 000 ciclos) y en melonDS 1.1; sus divergencias quedaron anotadas.
  Hallazgos de melonDS: su BIOS libre destruye r0 y r2 en el Halt del ARM7, da el resto de Div en valor absoluto, Sqrt
  0 con entradas grandes y no tiene Huffman; el DMA de HBlank y el de «inicio de pantalla» difieren de GBATEK.
- Paquete de PS3 **«SalsaDS núcleo» (SLDS00008)**: corre la máquina con la demostración propia embebida (o
  `/dev_hdd0/salsads/rom.nds`) y las dos pantallas de la 0.6. En RPCS3: OK. **Falta la PS3 real.**
- Respaldos del autor (a mano, sin capturas en el repo): Super Mario 64 DS arranca más lejos (configura video y
  VRAM y sigue con su lógica; usa la FIFO de geometría); New Super Mario Bros. configura video y se queda con los
  dos procesadores esperándose; Mario Kart DS no llega a configurar el video (el ARM7 lee el firmware sin parar).
- Decisión pendiente del autor: ninguna nueva. Pendiente técnico (guardados, SWI 1Ah–1Ch, trazas de los juegos, GX
  FIFO): lista en `docs/fase1.md`, «Qué falta para cerrar la Fase 1».

## 2026-10-17 — Intérpretes de CPU (Fase 1.1)

- `src/cpu/`: un núcleo (`struct acore`) para el **ARM9 (ARMv5TE)** y el **ARM7 (ARMv4T)**,
  ARM y Thumb, modos con banco, excepciones, HALT, líneas de IRQ/FIQ, memoria por
  callbacks con direcciones alineadas, un gancho de SWI para la HLE de la BIOS y, en la
  ARM9, CP15 mínimo con ITCM/DTCM (base, tamaño, espejo, modo de carga). Detalle,
  reglas por versión y plan de la 1.2 en `docs/fase1.md`.
- Pruebas: ≈ 8,0 M de comprobaciones unitarias en LE y BE (con mutaciones que las ponen a
  prueba) y las ROMs `hola`, `cpu9` y la nueva `cpuarm` (ARM9 y ARM7, ARM y Thumb,
  excepciones y TCM) corridas en los intérpretes (LE y BE) y en **melonDS 1.1: OK**.
  `tools/build.sh test-host` lo corre todo; `tools/cpurun.sh` corre una ROM en la Mac
  sin Docker; `tools/melonds-run.py --arm7` muestra también el bloque del ARM7, el
  registro de fallos y las divergencias.
- melonDS **diverge de la documentación** en varios casos de borde (LDRH/LDRSH impares del
  ARM7, listas vacías de LDM/STM, STM con la base en la lista, bit Q del ARM7, BKPT, modo de
  carga de las TCM): la ROM los anota aparte (no son fallos) y los intérpretes siguen
  GBATEK; ninguno está contrastado en una DS real (lista en `docs/fase1.md`).
- Rendimiento del intérprete en el **host**: 125–198 MIPS (Mac y contenedor); ≈ 1,5×
  más lento que el mínimo de la 0.3. En la PS3 sin medir (≈ 12 MIPS por escala [est]).
- Corredor `salsads-cpurun` + `src/host/ndsmin.c`: arranque directo con una máquina
  mínima (no es el mapa de la 1.2) que llega al `main` de las ROMs de BlocksDS con su
  `crt0` y libnds reales.
- Marco de pruebas de las ROMs: registro de los primeros 16 fallos, registro de
  divergencias, marcador de avance y variante para el ARM7 (`sds7.c`). `hola` y `cpu9`
  se reconstruyeron (hashes nuevos en `SHA256SUMS`; la construcción es reproducible).
- Pendiente para la 1.2: mapa de memoria con VRAM por bancos, planificador, IRQ,
  temporizadores, DMA, IPC FIFO, cartucho, SPI/firmware, BIOS en HLE, arranque directo y
  la primera imagen. Y probar el intérprete en el PPE real.

## 2026-10-17 — Prueba mínima del JIT ARM → PPC (Fase 0.7) y cierre de la Fase 0

- `tests/phase0/jit-arm` («SalsaDS JIT», SLDS00007) + `src/jit/`: traductor de un
  subconjunto de ARM (datos con S, ADC, LDR/STR/LDRB/STRB, B<cond>) a PowerPC64 con
  tres formas de banderas (perezosas, CR por instrucción, cmp+salto fusionados), un
  intérprete de referencia y cuatro bucles (el de la 0.3, suma64 con acarreo,
  ramas, bytes). **qemu-ppc64 ejecuta el código traducido** y da lo mismo que la
  referencia (registros, memoria, instrucciones).
- **Medido en la PS3 real (2026-10-17): el ARM9 cabe.** El peor bucle da 534,8 MIPS
  con CR por instrucción (8× los 67 MHz; intérprete de 0.3: 18,1 MIPS). ARM7 en el
  hilo 1: girar −31,9 %, dormir en un semáforo de lv2 −0,7 % a −9 %. **Fase 0
  cerrada**: arquitectura confirmada en `fase0.md` («Cierre de la Fase 0»).
- Caché de código: cola NOBITS de 4 MB en el texto (`mk/rec-cache.ld`), hilo de
  prueba con plazo de 2 s y marca en disco; si falla o cuelga (CFW), respaldo a los
  intérpretes. En RPCS3 el JIT no se intenta (solo lectura).
- Mide también el ARM7 en el hilo 1 del PPE: pérdida de MIPS del ARM9 y latencia de
  despertar con giro, giro de prioridad baja, `sysUsleep`, semáforo y cola de
  eventos de lv2.

## 2026-10-16 — Rasterizador 3D en SPEs (Fase 0.4)

- `tests/phase0/spe-raster3d` («SalsaDS raster», SLDS00002): escena sintética
  de 500, 1000 y 2048 triángulos con textura de la VRAM (por DMA) y color de
  vértice, z-buffer de 24 bits, dibujada por 1 o 2 SPEs a 1×, 2× y 4×, por
  bandas de líneas o tiles. Diseño, verificación y qué mirar en
  `docs/fase0.md` §0.4.
- Verificado: el mismo núcleo en el host (LE y BE) contra una referencia en C
  (modo id idéntico; color 99,96 % idéntico; todos los repartos con los mismos
  bits) y en RPCS3 (`RESULTADO … OK`).
- **Medido en la PS3 real (paquete de `255b0d2`): criterio cumplido.** 1× N =
  2048 en 2 SPEs: 4,19 ms (< 8); 2×: 7,96 ms; 4×: 19,61 ms (no cabe en 2
  SPEs). Medida ×1,6–2 sobre la estimación. Caché de texturas desbordada: ×5,4
  más lenta, primera decisión de la Fase 3. Detalle en `docs/fase0.md` §0.4,
  `perf-log.md` y `docs/datos/`. **Fase 0.4 cerrada.**
- Encontrado: `spu_re` de spu-gcc ya incluye el `fi`; uno más arruina el
  recíproco. El programa ahora autoprueba en cada SPE las funciones con
  intrínsecos contra el PPE.
- Queda como riesgo para la Fase 3: texturas que no caben decodificadas en el
  local store (con 24 texturas, la caché se vacía en cada unidad).
## 2026-10-16 — Composición 2D en un SPE y salida de dos pantallas (Fases 0.5 y 0.6)

- **0.5** (`tests/phase0/spe-gpu2d`, «SalsaDS 2D», SLDS00003): compositor 2D
  por líneas en un SPE (4 fondos de texto, 128 OBJ, VRAM por DMA, registro de
  cambios por línea) y una referencia píxel por píxel. Imágenes idénticas en el
  host (LE y BE) y en RPCS3 (SPE de verdad); el **tiempo (< 4 ms) falta medirlo
  en la PS3 real**. Detalle, límites del modelo y la estimación en `fase0.md` 0.5.
- **0.6** (`tests/phase0/dual-screen`, «SalsaDS pantallas», SLDS00004): dos
  texturas por cuadro con rsxkit en cuatro distribuciones a 720p y 1080p con
  escala entera y ajustada (distribución comprobada por píxeles en RPCS3), DS3 →
  botones de la DS y el puntero táctil (botón de toque mapeable, stick derecho o
  inclinación del Sixaxis). **Pendiente en la PS3 real**: los 60 Hz, 1080p, y
  cómo se sienten los dos modos del puntero (precisión, temblor, latencia); los
  ejes del Sixaxis son una suposición que se corrige en `controles.cfg`.
- Código nuevo reutilizable: `src/gpu2d/` (referencia, compositor, escenas),
  `src/frontend/{dspad,layout}.c` (entrada y geometría, sin PSL1GHT),
  `tools/raw-png.py` (volcados a PNG), `tools/rpcs3/salsads-1080.yml` (RPCS3 a
  1080p). El logger comprueba ahora que `/dev_hdd0/salsads` existe y lo dice.
- **APPID duplicado**: `tests/phase0/udplog` ya usaba SLDS00003. Los paquetes de
  0.5 y 0.6 siguen la asignación pedida (SLDS00003 y SLDS00004); si udplog y
  spe-gpu2d se instalan en la misma consola, el segundo reemplaza al primero.
  La Fase 0.4 (spe-raster3d) usa SLDS00002, que también tiene `empty`; el siguiente libre es SLDS00005 (pantallas giro usó SLDS00006).

## 2026-10-16 — Microbenchmarks del PPE (Fase 0.3)

- `tests/phase0/ppe-bench` (SLDS00001 «SalsaDS bench»): memoria LE con
  `lwbrx`/`stwbrx`/`lhbrx`/`sthbrx`, tres estrategias de banderas NZCV más una
  cota, y tres despachos de un intérprete ARM mínimo. Verificado contra
  referencias en C bajo qemu-ppc64 (`tools/build.sh test-host`) y arranca en
  RPCS3.
- **Medido en la PS3 real** (paquete de `08b7ef0`, subido por quien
  tiene el permiso del FTP): resultados, decisiones y log completo en
  `docs/fase0.md` §0.3, `perf-log.md` y `docs/datos/`. Decididos: memoria LE
  con `lwbrx` y compañía, banderas perezosas, y recompilador obligatorio
  (intérprete 16,5–17,9 MIPS contra ~100 que pide la DS). **Fase 0.3 cerrada.**
- Descubierto al escribirlo: en PowerPC64, el CA de `addc`/`subfc` es el
  acarreo del bit 63, no del 32; el C de ARM sale del bit 32 del resultado de
  64 bits con los operandos extendidos con ceros.

## 2026-10-16 — ROMs de prueba (Fase 0.2)

- `tests/roms`: marco de pruebas `sds` (resultado en RAM `0x02380000` y en
  pantalla), ROMs `hola` y `cpu9` (65 187 comprobaciones del ARM9), construidas
  con BlocksDS en Docker (`tools/build-roms.sh`) y validadas en melonDS 1.1
  con su stub de GDB (`tools/melonds-run.py`).
- ARM7 de las ROMs: uno propio vacío, para no enlazar DSWiFi/Maxmod/Mbed TLS.
- Falta: pruebas del ARM7 (ARMv4T), DMA, timers, IPC, VRAM por bancos; y
  copiar el aviso íntegro de picolibc antes de redistribuir los `.nds`.

## 2026-10-16 — Arranque: esqueleto (Fase 0.1)

Hecho:
- `lib/rsxkit` y `lib/spukit` entran por `git subtree --squash` desde sus
  repositorios (MIT). Cada uno en su propio commit y sin tocar otra carpeta.
- Build en Docker: `tools/build.sh` usa la imagen `salsads-toolchain:dev`, que
  se construye en segundos (`tools/docker-image.sh`) a partir de la imagen del
  toolchain de Salsa64 (ps3toolchain, PSL1GHT y cgcomp: más de una hora de
  compilación que no se repite). `docker/Dockerfile` explica cómo.
- `mk/ps3.mk` (reglas de PSL1GHT, SPU embebido, `.pkg`) y
  `src/platform/ps3/` (logger TTY + archivo + UDP) adaptados de Salsa64.
- Logger UDP en el **puerto 18195** (Salsa64 usa 18194): `tools/udplog.py`.
  Un receptor por puerto. Archivos de la consola en `/dev_hdd0/salsads/`.
- Pruebas del host en los dos órdenes de bytes: `tools/build.sh test-host`
  corre `tests/unit` con el gcc nativo del contenedor (little-endian, con
  ASan/UBSan) y con `qemu-ppc64` (big-endian, CPU `power9`; ver
  `docs/cell-desde-salsa64.md` §6.2). Cada corrida imprime `host: LE|BE` y el
  script falla si no coincide.
- `tools/rpcs3-run.py` y `tools/rpcs3/*.yml`: RPCS3 sin interfaz. Usa el
  **mismo candado** (`~/Library/Caches/rpcs3/salsa64-rpcs3.lock`) que Salsa64,
  porque el RPCS3, su `dev_hdd0` y su `TTY.log` son uno solo.
- `tools/deploy-ps3.sh` (FTP de webMAN MOD o `ps3load`; la IP va en
  `tools/ps3.conf`, ignorado por git).

APPIDs de SalsaDS: rango `SLDS`, distinto del `SLSA` de Salsa64.

| APPID | Programa |
|---|---|
| SLDS00001 | `tests/phase0/ppe-bench` («SalsaDS bench») |
| SLDS00002 | `tests/phase0/spe-raster3d` («SalsaDS raster»); también `tests/phase0/empty`, que solo se corre en RPCS3 |
| SLDS00003 | `tests/phase0/udplog` (y también `tests/phase0/spe-gpu2d`, «SalsaDS 2D»: ver el aviso de arriba) |
| SLDS00004 | `tests/phase0/dual-screen` («SalsaDS pantallas») |
| SLDS00006 | `dual-screen` con otro nombre («SalsaDS pantallas giro», modo GIRO) |
| SLDS00007 | `tests/phase0/jit-arm` (0.7) |
| SLDS00008 | `tests/phase1/nucleo` («SalsaDS núcleo», Fase 1.2) |

Pendiente (preguntas abiertas del plan, de la decisión del autor):
licencia del proyecto, juegos objetivo, aplicación independiente o núcleo
libretro, nombre del ejecutable.

## Qué existe hoy de DS en la PS3

No encontré ningún emulador de DS usable en PS3 (búsqueda del 2026-10-16;
detalle y fuentes en `docs/fase0.md`, 0.2).
