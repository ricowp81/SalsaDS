# Estado de SalsaDS

Bitácora de lo hecho, lo que quedó a medias y lo decidido. Lo más reciente
arriba. Las mediciones van en `perf-log.md`; los resultados de la Fase 0, en
`fase0.md`.

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
| SLDS00003 | `tests/phase0/udplog` |

Pendiente (preguntas abiertas del plan, de la decisión del autor):
licencia del proyecto, juegos objetivo, aplicación independiente o núcleo
libretro, nombre del ejecutable.

## Qué existe hoy de DS en la PS3

No encontré ningún emulador de DS usable en PS3 (búsqueda del 2026-10-16;
detalle y fuentes en `docs/fase0.md`, 0.2).
