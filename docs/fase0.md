# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | hecho (2026-10-16) |
| 0.3 Microbenchmarks del PPE | hecho (2026-10-16): medido en la PS3 real, decisiones abajo |
| 0.4 Rasterizador 3D en un SPE | hecho (2026-10-16): medido en la PS3 real, **criterio cumplido** (4,19 ms), decisión abajo |
| 0.5 Composición 2D en un SPE | medido en la PS3 real (2026-10-16): imagen correcta, **criterio < 4 ms NO cumplido** con las dos pantallas en un SPE (4,31 / 5,59 / 8,46 ms); salidas anotadas abajo |
| 0.6 Salida de dos pantallas | **60 Hz estables en la PS3 real** a 720p y 1080p (16/16 combinaciones); sensación de los modos del puntero pendiente |
| 0.7 Prueba de JIT ARM → PPC | construido y verificado (host y qemu-ppc64, 2026-10-16); **MHz, banderas y hilos pendientes de la PS3 real** |

## 0.1 Esqueleto

Criterio: `tools/build.sh` compila un programa vacío de PS3 a `.pkg` y
`tools/build.sh test-host` corre una prueba en LE y en BE.

- `tools/build.sh -C tests/phase0/empty all pkg` → `build/salsads-empty/salsads-empty.pkg`;
  el `.elf` corre en RPCS3 y escribe `RESULTADO empty: OK`
  (`tools/rpcs3-run.py empty`).
- `tools/build.sh test-host` → `LE OK` y `BE OK` (12 comprobaciones cada una).

## 0.2 ROMs de prueba propias

Criterio: la ROM «hola» y una ROM de prueba de CPU se construyen desde el
repositorio y corren en el emulador de referencia. **Cumplido.**

- `tools/build-roms.sh` → `build/roms/hola.nds` y `cpu9.nds` (reproducibles:
  dos construcciones dan los mismos hashes; `tests/roms/homebrew/SHA256SUMS`).
- `tools/melonds-run.py build/roms/hola.nds` → `OK hola.nds: 5 comprobaciones`;
  `cpu9.nds` → `OK … 65187 comprobaciones en 9 grupos`. Control negativo: con
  una expectativa errónea del propio test la ROM dio `FALLO 168 de 65411` y
  el script lo reportó (código 1).
- El volcado de la VRAM (`--vram`) confirma que el texto y la franja de
  veredicto se dibujan en la pantalla de arriba por el modo de visualización
  directa. Detalles del marco de pruebas en `tests/roms/README.md`.

### Toolchain ARM y licencias (verificadas)

BlocksDS v1.24.0 (imagen oficial `skylyrac/blocksds:slim-latest`, fijada por
digest `sha256:779f6193…01a51` en `docker/nds.Dockerfile`; arm64 nativa),
`arm-none-eabi-gcc` 16.2.0 del toolchain Wonderful. Los textos de licencia se
leyeron de los archivos de la propia imagen el 2026-10-16:

| Componente | Licencia | Dentro de la ROM |
|---|---|---|
| libnds (`libs/libnds/licenses/LICENSE`) | zlib | sí (y FatFs, 1-cláusula BSD, sin aviso en binarios) |
| crt0 (`sys/crts/COPYING.zlib`) | zlib; los scripts `.ld`/`.mem`, MPL-2.0 | el crt0 sí; los scripts no |
| núcleo ARM7 por omisión (`sys/default_arm7`) | zlib, pero enlaza DSWiFi (MIT), Maxmod (ISC) y Mbed TLS (Apache-2.0/GPL-2.0+) | **no se usa**: las ROMs llevan un ARM7 propio mínimo (`tests/roms/src/arm7min`) |
| picolibc (libc de la toolchain) | MIT/BSD y otras permisivas por archivo (según la guía de licencias de BlocksDS; el `COPYING.picolibc` no viene en la imagen) | sí, 64 archivos objeto en `hola`; **falta copiar su aviso íntegro antes de redistribuir** (ver `tests/roms/homebrew/AVISOS.md`) |
| libgcc | GPL-3.0 con excepción de runtime | sí |
| ndstool (arma el `.nds`) | GPL-3.0 y MIT | herramienta, no entra en la ROM |

Fuente de la guía oficial: blocksds.skylyrac.net/docs/guides/licenses/
(consultada el 2026-10-16). El ícono de la cabecera es propio (el de BlocksDS
es un logotipo CC BY-ND). Registro en `docs/THIRD_PARTY.md` y avisos en
`tests/roms/homebrew/AVISOS.md`.

### Suites libres de CPU y hardware de la DS

Resultado de una búsqueda web del 2026-10-16 (no probé yo las suites):

| Suite | Licencia | Cubre | Veredicto |
|---|---|---|---|
| gba-suite (jsmolka) | MIT | ARM/Thumb ARM7TDMI (ARMv4T), memoria, PPU de GBA; se construye con FASMARM, sin binarios | Licencia clara. Útil como modelo para un ARM7 (ARMv4T) pero no corre tal cual en DS |
| RockWrestler (RockPolish) | **sin licencia visible** | ARMv5 (CLZ, saturación, BLX, multiplicaciones), IPC, división/raíz por hardware, WRAMCNT/VRAMCNT/TCM; resultado en pantalla | **No copiar ni redistribuir.** Se puede usar localmente para comparar. Pendiente: pedir licencia al autor |
| armwrestler-gba-fixed (destoer) | sin licencia visible | ARM/Thumb de GBA | No copiar |
| ARMWrestler original, ndstest, Normmatt, nocash, DS-Test | no localizadas / no verificadas | — | No usar hasta verificar |

No hay una suite de ARMv5 y hardware de DS con licencia clara confirmada: el
plan es **escribir las propias** (`cpu9` es la primera; faltan ARM7/ARMv4T,
DMA, timers, IPC, VRAM por bancos, 3D). La ROM de `cpu9` no copia nada de las
suites de terceros.

### Emulador de referencia

melonDS 1.1 (2025-11-18, GPL-3.0), zip oficial `melonDS-1.1-macOS-universal.zip`
(sha256 `79843a5e…18d`, coincide con el que publica GitHub; 24 MB), instalado
fuera del repositorio. El cask de Homebrew está deshabilitado desde
2026-09-01 (`fails_gatekeeper_check`), por eso la descarga oficial. Arranca
ROMs directamente con su BIOS libre integrada (no hacen falta archivos de la
DS).

- **Stub de GDB: sí, y el binario oficial lo trae.** Se activa con
  `Instance0.Gdb.Enabled = true` en `~/Library/Preferences/melonDS/melonDS.toml`
  (puertos 3333 ARM9 y 3334 ARM7). Se usa con Ctrl-C (`0x03`) para detener,
  `m<dir>,<largo>` para leer memoria y `c` para seguir; hay que mandar un
  acuse `+` al conectar, `qSupported` no se entiende y **`?` tumba el
  proceso**. melonDS aborta al reescribir su configuración si el TOML mínimo
  no tiene `[Instance0]` explícito.
- No hay modo sin interfaz (abre su ventana), ignora `SIGTERM` a veces y no
  respeta `HOME` para su configuración: `tools/melonds-run.py` respalda y
  restaura esa configuración, usa `SIGKILL` si hace falta y es una corrida a la
  vez.
- Trazas de instrucciones o consola de depuración: no encontradas (solo el
  stub de GDB).
- DeSmuME 0.9.13 (GPL-2.0; tiene `--arm9gdb`/`--arm7gdb` según su manual) y
  NooDS (GPL-3.0): no instalados ni probados. Nunca se copia código de ellos.

### Qué existe hoy de DS en la PS3

Búsqueda del 2026-10-16: **no encontré ningún emulador de DS usable en PS3.**
La receta de PS3 de libretro-super lista `desmume2015` con el campo de
activación en NO (no verificado qué significa exactamente), el buildbot
oficial de libretro no tiene carpeta PS3 y no hay ports de melonDS, NooDS ni
DeSmuME a PS3/Cell. Sin revisar: el foro de PSX-Place y repos de la escena.

## 0.3 Microbenchmarks del PPE (`tests/phase0/ppe-bench`)

Estado: **medido en la PS3 real** el 2026-10-16 con el paquete
`SalsaDS bench` (APPID `SLDS00001`) compilado desde `08b7ef0`; el log completo
(88 líneas, `RESULTADO ppe-bench: OK`) está en
`docs/datos/ppe-bench-08b7ef0-ps3.log`. Los números de RPCS3 no valen.

Todo se mide con el timebase (`mftb`, 79,8 MHz = 40 ciclos de 3,2 GHz por
tick). Cada medición se calibra a ≥ 60 ms, se repite 7 veces y se reporta la
**mediana y el mínimo**. Las líneas `DATO …` del log (UDP 18195, y
`/dev_hdd0/salsads/ppe-bench.log` por FTP) son las que se copian aquí y a
`perf-log.md`.

### Cómo se verifica antes de medir

Los núcleos de ensamblador no se pueden correr en la Mac, así que se prueban
bajo qemu-ppc64 (big-endian) con `tools/build.sh test-host`
(`tests/unit/test_ppe_bench.c`): los cuatro bucles de banderas dan el mismo
estado final que una referencia en C con la semántica de ARM (con
desbordamiento con signo y acarreo), las cadenas de latencia dan el índice que
calcula C en las seis variantes, y el intérprete ARM mínimo da el resultado
esperado con los tres despachos en little-endian y en big-endian. El propio
programa repite esas comprobaciones en la consola antes de medir y termina con
FALLO si algo no coincide. Se revisó el desensamblado: en los núcleos no hay
formas con punto, ni `mfcr` en los bucles (solo el que GCC pone en el prólogo
de cada función por los campos de CR que se declaran como usados), ni
desplazamientos por registro.

### 1. Memoria little-endian (`lwbrx` y compañía)

Throughput: bucles de 4 accesos independientes a una línea de L1 caliente
alineada, con `ctr`/`bdnz`; se resta el mismo bucle con 4 `ori 0,0,0` (neto).
Variantes: carga/almacenamiento normal (lo que sería la memoria big-endian),
`lwbrx`/`stwbrx`/`lhbrx`/`sthbrx` y carga normal + rotaciones (`rlwinm` +
2 `rlwimi` en 32 bits, `rlwinm` + `rlwimi` en 16). Latencia: cadena
dependiente por una permutación de 1024 entradas (índice → `rlwinm` → carga →
[swap] → índice). Si `lwbrx` fuera microcódigo se vería ≥ 11 ciclos por
acceso en throughput.

**Qué decide:** si `lwbrx` ≈ carga normal → memoria de la DS en little-endian
tal cual con `lwbrx`/`stwbrx` (y tablas de páginas de punteros); si cuesta
mucho más, o si carga + rotaciones gana en latencia, el diseño de la memoria
emulada cambia (p. ej. guardarla en orden big-endian por palabras y
reordenar bytes solo en accesos de 8/16 bits).

### 2. Banderas NZCV

Bucle guest de 9 instrucciones ARM (`ADDS, CMP, BLT, ADDS, CMP, BHI, ADDS,
SUBS, BNE`: 7,55 por vuelta en promedio) con valores que hacen que haya
desbordamiento con signo (`BLT` = N≠V) y acarreo (`BHI` = C y no Z). Cuatro
implementaciones (los registros guest viven en registros del PPE; las ramas
guest son ramas del host):

| Clave | Estrategia |
|---|---|
| sin banderas (cota) | `cmpw`/`cmplw` + salto directo. No es una implementación válida de ARM: da el costo de las banderas por diferencia |
| (a) completas | N, Z, C, V a cada instrucción, empaquetadas como el CPSR, sin formas con punto: N con `rlwinm`, Z con `cntlzw`, C como bit 32 del resultado de 64 bits (**`addc`/`subfc` no sirven en PowerPC64: su CA es el acarreo del bit 63**), V con `xor/andc`; al consumir se extraen con `rlwinm` + `cmpwi` |
| (b) perezosas | se guardan `a`, `b'` (o `~b` en las restas) y el resultado (4 instrucciones); al consumir se calculan N, Z, C, V con operaciones lógicas (Z y N son 2 instrucciones, C y V ~6) |
| (c) N/Z en CR | `cmpwi cr0,res,0` (N/Z), `cmplw cr1,…` (C) y V en `cr2` con `cmpwi`; los saltos usan los bits del CR (`blt cr0`, `bgt cr1`), `crxor` para N≠V. El consumidor conoce el tipo de la instrucción anterior (como un recompilador por bloques) |

| (d) `add.`/`subfc.` + XER.CA (**exploratoria**) | Fuera de las reglas del plan: N/Z de cr0 con `addc.`/`subfc.` y C de XER.CA (`li` + `addze` solo cuando un salto lo necesita). Para que CA sea el acarreo de 32 bits del guest, los registros guest viven en la **mitad alta** de los de 64 bits (valor << 32); así N y Z salen del signo y el cero de los 64 bits y no hace falta `clrldi`. Se agregó por una afirmación de un informe externo (no leído por mí en el Handbook): en el PPE `add.`, `subf.`, `neg.` y las formas con acarreo **no** serían microcódigo. Solo la decisión con los números de la PS3 dice si vale |

Se reporta ciclos por vuelta, por instrucción ARM y «sobre la cota». No se
modela aquí la eliminación de banderas muertas (aplica por igual a todas: en
este bucle la mitad de las instrucciones que escriben banderas las
sobrescriben antes de leerlas).

### 4. Costo de instrucciones sueltas, ramas y `mtctr`/`bctr`

Para saber qué es microcódigo de verdad en el PPE, en vez de fiarse de
documentos (la guía de Salsa64 daba las formas con punto como microcódigo; un
informe externo dice que `add.`, `subf.`, `neg.` y las formas con acarreo no
lo son, y que sí lo son las de lógica, rotación, desplazamiento, extensión,
`cntlz`, multiplicación y división, y los accesos que cruzan un sector de
32 bytes): throughput de 8 copias independientes de cada instrucción (neto
del bucle de 8 nops; las de acarreo dependen entre sí por XER), y

- **pares «instrucción + salto»** (`add`+`cmpwi`+`beq`, `add.`+`beq`,
  `subf.`+`beq`, `addc.`+`beq`, y con dos nops de separación): cuánto cuesta
  que el salto lea CR0 recién escrito;
- **`mtctr` + `bctr`** con 0, 2, 4, 8 y 12 instrucciones en medio (contra un
  salto directo): el informe dice que hacen falta ~10 ciclos de separación;
- **cruces de sector de 32 B** en la memoria (sección 1): `lwz`/`lwbrx`/`stw`/
  `stwbrx` a los offsets 30, 62, 94 y 126, `lhz`/`lhbrx` a 31, 63, 95 y 127, y
  `lwz`/`lwbrx` no alineados dentro de un sector (1, 33, 65, 97) como control.
  Importa para la memoria little-endian de la DS: los accesos guest alineados
  nunca cruzan sector, pero los no alineados sí podrían.

Las líneas son `DATO instr <grupo> <nombre> bruto … neto …`. Si `add.` y
`subf.` salen cerca de `add` (≈ 1 ciclo) y `and.`/`rlwinm.`/`cntlzw.`
cerca de 11+ ciclos, el informe tiene razón y la estrategia (d) es la
candidata (N/Z de `cr0` gratis, C de `XER.CA` sin `mfcr`); si no, vale la
guía de Salsa64 y se queda (c) o (b).

### 3. Despacho del intérprete

Intérprete mínimo de ARM (datos con desplazador, `LDR/STR`, `CMP`, `B<cond>`,
`SWI` de fin) que corre un bucle de 9 instrucciones por elemento sobre un
arreglo de 1024 palabras, con la memoria guest leída con `rd32le`. Tres
despachos que comparten los cuerpos de los manejadores: `switch` (con
`-mno-relative-jumptables`), tabla de punteros a función (en la ABI de la PS3
cada puntero es un descriptor) y `goto` calculado (código enhebrado). Se
reporta ns, MIPS guest y ciclos por instrucción guest. Las banderas del
intérprete son completas en C y su costo es común a los tres. Referencia: la
DS pide del orden de 100 MIPS (ARM9 a 67 MHz + ARM7 a 33 MHz); Salsa64 midió
8,8 → 21,2 MIPS en su intérprete (`docs/cell-desde-salsa64.md` §1.3), así que
lo esperable es que el intérprete solo no alcance y el recompilador sea
obligatorio.

### Qué mirar al correrlo en la PS3

1. La primera línea del log: `ppe-bench <versión> (SalsaDS bench, SLDS00001)`
   y `timebase 79800000 Hz`.
2. `RESULTADO ppe-bench: OK` (si dice FALLO, hay una línea `FALLO …` antes y
   los números no valen).
3. Memoria: `lwbrx`/`lhbrx`/`stwbrx`/`sthbrx` contra la carga normal, en neto
   y en latencia; y si carga + rotaciones queda por debajo en alguno.
4. Banderas: el orden de las tres estrategias en «ciclos por instrucción ARM»
   y cuánto suma cada una sobre la cota. Ojo con (c): `crxor` y los `cmp` a
   varios campos del CR podrían ser lentos si el PPE los trata como microcódigo.
5. Intérprete: MIPS de los tres despachos (el de punteros a función paga el
   descriptor ELFv1).
6. Sección 4: qué formas con punto y de acarreo son microcódigo (≈ 11+ ciclos
   por instrucción) y cuáles no; `add.`+`beq` contra `add`+`cmpwi`+`beq`; la
   separación que necesita `mtctr`→`bctr`; y los cruces de sector de 32 B.
7. Banderas: la estrategia (d) contra (a)–(c).
8. Repetir 2–3 veces si las medianas varían más de ±3 % (ruido de ventana
   conocido del PPE).

### Resultados (PS3 real, `08b7ef0`)

Ciclos del PPE (3,2 GHz; timebase 79,8 MHz = 40 ciclos por tick); mediana de 7
repeticiones, el mínimo difiere en menos de 1 % (±3 % de ruido de ventana no
se alcanzó a ver).

**Cómo leer las cifras.** Los bucles son cortos y cada vuelta cuesta de por sí
~10 ciclos de salto (`bdnz`): un bucle de 4 accesos tarda 10–11 ciclos aunque
los accesos sean gratis. Por eso el *throughput* de memoria no distingue
diferencias menores de ~2,5 ciclos por acceso, y el «neto» de la sección 4 sale
0,00 donde el bucle de referencia (ocho `ori 0,0,0`, 2,06 ciclos cada uno) es
más lento que las propias instrucciones (1,54): ahí valen las cifras
**brutas**. Lo que discrimina en memoria es la **latencia**.

**1. Memoria little-endian** (ciclos por acceso; L1 caliente)

| Acceso | Throughput | Latencia (cadena dependiente) |
|---|---|---|
| `lwz` (normal) | 2,56 | 7,16 |
| `lwbrx` | 2,56 | **7,17** |
| `lwz` + 3 rotaciones | 4,46 | 13,36 |
| `lhz` (normal) | 2,57 | 7,20 |
| `lhbrx` | 2,57 | **7,19** |
| `lhz` + 2 rotaciones | 3,63 | 11,31 |
| `stw` / `stwbrx` / rotaciones + `stw` | 2,69 / 2,70 / 4,32 | — |
| `sth` / `sthbrx` / rotaciones + `sth` | 2,70 / 2,69 / 3,82 | — |

`lwbrx`, `lhbrx`, `stwbrx` y `sthbrx` cuestan **lo mismo que las normales** (no
son microcódigo). Las rotaciones suman 0,8–1,9 ciclos de throughput y 4–6 de
latencia.

**Cruces de sector de 32 B** (neto sobre el bucle): `lwz` 57,4; `lwbrx` 57,3;
`stw` 49,9; `stwbrx` 49,8; `lhz` 51,7; `lhbrx` 51,6 ciclos por acceso: es
microcódigo (confirma el informe externo). Un acceso **no alineado dentro de un
sector** (offsets 1, 33, 65, 97) cuesta 2,57, como uno alineado.

**2. Banderas NZCV** (bucle de 7,55 instrucciones ARM por vuelta)

| Estrategia | Ciclos/vuelta | Ciclos por instr. ARM | Sobre la cota |
|---|---|---|---|
| sin banderas (cota) | 25,17 | 3,33 | — |
| (a) completas a cada instrucción | 126,12 | 16,70 | +13,37 |
| (b) perezosas | **45,37** | **6,01** | **+2,67** |
| (c) N/Z `cr0`, C `cr1`, V `cr2` | 57,40 | 7,60 | +4,27 |
| (d) `add.`/`subfc.` + XER.CA (exploratoria) | 49,49 | 6,55 | +3,22 |

**4. Instrucciones** (ciclos por instrucción, **bruto**; add sin punto = 1,54)

| Instrucción | Ciclos | ¿Microcódigo? |
|---|---|---|
| `add`, `add.`, `subf.`, `neg.` | 1,54 / 1,67 / 1,55 / 1,67 | no |
| `addc`, `subfc`, `addc.`, `subfc.` | 1,54 / 1,54 / 1,54 / 1,66 | no |
| `adde`, `subfe`, `addze` (dependen de XER) | 2,05 | no |
| `and`, `cntlzw`, `rlwinm`, `extsw` (sin punto) | 1,54 | no |
| `and.`, `andi.`, `cntlzw.`, `rlwinm.` | 14,5–14,6 (+12,5) | **sí** |
| `slw`, `sraw` (desplazamiento por registro) | 16,6 (+14,5) | **sí** |
| `mullw` | 11,48 (+9,4) | lento (no pipeline) |
| `cmpw`, `cmplw`, `crxor`, `mtocrf` (un campo) | 1,54–1,67 | no |
| `mfcr` | 26,97 (+24,9) | **sí** |
| `mtcrf 0xff` (todos los campos) | 20,72 (+18,7) | **sí** |

El informe externo acertó: las formas con punto de **aritmética** y las de
acarreo son nativas; las de lógica, rotación y `cntlz`, microcódigo (~12,5
ciclos de más, que cuadra con los «≥ 11» de la guía de Salsa64). La guía de
Salsa64 (§1.2) las daba todas por microcódigo: ver la corrección en esa
sección.

Pares «instrucción + salto» (ciclos por par): `add`+`cmpwi`+`beq` 4,12;
`add.`+`beq` 3,33; `subf.`+`beq` 3,09; `addc.`+`beq` 3,35; con dos nops entre
`add` y `cmpwi` 6,19. Un salto que lee el CR0 recién escrito por una forma con
punto **no** paga espera visible.

`mtctr`+`bctr` (contra un salto directo, 18,43 ciclos): 0 instrucciones en
medio 18,44; 2: 16,39; 4: 18,40; 8: 21,53; 12: 28,81. **No se ve penalización
por poner `bctr` justo después de `mtctr`** (el informe dice que hacen falta
~10 ciclos de separación; con este destino fijo y bien predicho no aparece;
con destinos que cambian, como en un despacho de intérprete, puede aparecer
como desvío mal predicho y habría que medirlo aparte).

**3. Despacho del intérprete ARM mínimo**

| Despacho | ns/instr. | MIPS guest | Ciclos/instr. |
|---|---|---|---|
| `switch` | 60,48 | 16,53 | 193,6 |
| tabla de punteros a función | 60,52 | 16,52 | 193,7 |
| `goto` calculado (enhebrado) | 55,92 | 17,88 | 179,0 |

### Decisiones

1. **Memoria de la DS en little-endian con `lwbrx`/`lhbrx`/`stwbrx`/`sthbrx`**
   (capa `rd/wr 16/32 le`): mismo costo que una carga normal y casi la mitad
   de latencia que carga + rotaciones (7,2 contra 13,4 ciclos en 32 bits). No
   se guarda la memoria en big-endian.
2. **Alineación de 32 bytes del host.** Un acceso que cruza un múltiplo de 32
   cuesta 50–57 ciclos. Los accesos guest alineados (ARM los exige) nunca
   cruzan; los no alineados (LDR con rotación, etc.) van por un camino lento,
   y las estructuras del núcleo (registros, tablas) se alinean. Un acceso no
   alineado que no cruza sector no cuesta de más.
3. **Banderas: perezosas (b)** como mecanismo base del intérprete y del
   recompilador. Ganó en este bucle (6,01 ciclos por instrucción ARM contra
   6,55 de (d), 7,60 de (c) y 16,70 de (a)). (a) se descarta: serializa ~14
   instrucciones dependientes. (d) no gana y exige registros guest en la mitad
   alta (desplazamientos al cargar y guardar). Para el recompilador, además,
   fusionar cuando se pueda la instrucción que fija banderas con el salto que
   las consume en el mismo bloque (`cmpw` + `b`, que es la cota) y eliminar
   banderas muertas. **Reserva:** el bucle tiene 3 consumidores por 7,55
   instrucciones; en código con más consumos (c) y (d), que consumen gratis,
   podrían acercarse: reevaluar en la Fase 4 con trazas de juegos reales.
4. **Qué evitar en el camino caliente:** formas con punto de lógica, rotación y
   `cntlz` (+12,5), desplazamientos por registro (+14,5), `mfcr` (+25),
   `mtcrf` de varios campos (+18,7) y `mullw` fuera de los MUL de ARM (+9,4). Sí
   se pueden usar `add.`, `subf.`, `neg.` y las formas con acarreo. `mtocrf` de
   un campo es gratis.
5. **El intérprete solo no alcanza:** 16,5–17,9 MIPS guest (≈ 180–194 ciclos
   por instrucción) contra ~100 MIPS que pide la DS (ARM9 + ARM7). **El
   recompilador ARM/Thumb → PPC64 es obligatorio** (como en Salsa64, donde el
   intérprete también quedó en 8,8–21,2 MIPS). El despacho cambia poco
   (`goto` calculado +8 %): no es la palanca; el costo está en el resto de la
   ruta (banderas, estado en memoria y las 26 formas con punto que GCC dejó
   en el intérprete: `andi.`, `andis.`, `rlwinm.`). Para la Fase 1 sirve
   tabla de tipos + `goto` calculado.
6. **Dónde corre el ARM7** (pregunta de la arquitectura): sin cambios por
   ahora; con ~17 MIPS por intérprete no cabe en el PPE junto al ARM9, así que
   depende del recompilador (Fase 4) y de 0.7. Se re-evalúa al cierre de la
   Fase 0.

## 0.4 Rasterizador 3D en SPEs (`tests/phase0/spe-raster3d`)

Estado: **medido en la PS3 real** el 2026-10-16 con el paquete `SalsaDS raster`
(APPID `SLDS00002`, el mismo de `tests/phase0/empty`, que solo se corrió en
RPCS3) compilado desde `255b0d2`; log completo (166 líneas,
`RESULTADO spe-raster3d: OK`) en `docs/datos/spe-raster3d-255b0d2-ps3.log`.
Los tiempos de RPCS3 no valen. **Criterio cumplido: 4,19 ms a 1× con 2048
triángulos en 2 SPEs (objetivo < 8 ms).**

Criterio del plan: ms por cuadro en la PS3 real con N = 500, 1000 y 2048
triángulos, con 1 y 2 SPEs, a 1×, 2× y 4×; objetivo **< 8 ms a 1× con 2048
triángulos en 2 SPEs** (si no, RSX para el 3D y SPEs solo para el 2D).

### Qué dibuja

Una escena sintética (`r3d_scene.c`, solo enteros: la misma en la Mac, en
qemu y en la PS3) de N triángulos ya transformados, con textura y color de
vértice, a 256×192 y a 2× (512×384) y 4× (1024×768) de resolución interna:

- área media 3·256·192/N px²: la escena cubre la pantalla 3,6–4 veces sea cual
  sea N (más que una escena típica de la DS); con N = 500 los triángulos son
  grandes y con 2048, chicos (~90 px²);
- w entre 1 y 6 por vértice (perspectiva), z de 24 bits, 25 % translúcidos
  (alfa 6–26; uno de cada 8 escribe profundidad), vértices hasta 32 px fuera
  de la pantalla;
- 12 texturas de la DS en la VRAM, little-endian como en la DS (directas
  A1BGR5, de paleta de 256 y de 16 colores; 32×32, 64×64 y 64×32; con texeles
  transparentes, repetidas o recortadas por eje, offsets alineados a 8 y no a
  16), 25 KB de VRAM y 88 KB ya decodificadas. Variante con 24 texturas
  (176 KB decodificadas) para ver la caché desbordada;
- cada triángulo en su propia capa de z (permutación al azar, poca
  pendiente): no hay cruces ni empates de profundidad, así que qué triángulo
  se ve en cada píxel no depende del redondeo y se puede comparar exacto.

### Diseño elegido y por qué

**Entrada por lotes.** El PPE publica **un trabajo de spukit por SPE y por
cuadro** (`struct r3d_job`, 88 bytes: dónde están la lista, la VRAM, el
cuadro y un área de trabajo del SPE; escala, unidad y reparto). La lista es un
arreglo de `struct r3d_tri` de 64 bytes (lo que daría el motor de geometría
de la DS: x/y en 12.4, z de 24 bits, w en 20.12, s/t en 1.11.4, color BGR555,
alfa, TEXIMAGE_PARAM y PLTT_BASE) en orden de dibujo. El SPE la trae en lotes
de 32 triángulos (2 KB) con doble búfer. Nada por triángulo pasa por el PPE.

**Setup en el SPE, no en el PPE.** El PPE es el recurso escaso de la
arquitectura (las dos CPUs de la DS no caben en el intérprete: 0.3). La
pasada 1 del SPE calcula, para cada triángulo que toca alguna de sus
unidades, las tres funciones de borde en enteros y los planos de 7 atributos
en flotante (un recíproco de 4 carriles para 1/w de los tres vértices y
1/área), y deja el registro de 192 bytes en memoria principal (área propia de
cada SPE: 384 KB) y la caja del triángulo en unidades en el local store (4 ×
2 bytes). Con 2 SPEs cada uno hace el setup de todo lo que toca sus unidades
(con bandas alternas, casi todo): se duplica a cambio de no sincronizar SPEs.

**Recorrido por unidades de 4096 píxeles.** La unidad es una **banda de
líneas** a lo ancho (256×16 a 1×, 512×8 a 2×, 1024×4 a 4×) o un **tile** de
256×16. Color (32 bits) y profundidad (24 bits en 32) de la unidad viven en
el local store mientras se dibuja; **la profundidad nunca va a memoria
principal** y el color sale con un solo DMA de 16 KB (o una lista de 16 filas
en un tile), con doble búfer. Por unidad: limpiar, armar la lista de
triángulos que la tocan en orden de dibujo (comparando 8 cajas por vez),
traer sus registros con listas de DMA en lotes de 32 (doble búfer), dibujar,
sacar. Reparto entre 2 SPEs **por filas de unidades** (bandas de líneas):
alternas (equilibra la carga sin conocer la escena) o mitades (menos setup
duplicado). Se miden las dos, y bandas contra tiles a 2× y 4× (a 4× la banda
de 4 líneas hace que cada triángulo caiga en ~3 veces más unidades:
unidad·triángulo 31 649 contra 11 018 con N = 2048).

**Qué es entero y qué es flotante.** Lo que decide qué píxel se dibuja va en
enteros: bordes con 4 bits de subpíxel y la regla de arriba-izquierda,
recortes, caja. La cobertura es exacta en el SPE, el PPE y el host. Los
atributos (z, q = 1/w, s·q, t·q y (color+1)·q) van en **flotante de 4
carriles**, un grupo de 4 píxeles alineado por vector, con un recíproco por
grupo para la corrección de perspectiva: el SPU hace 4 fma por ciclo y su
entero de 32 bits no tiene multiplicación (son 5 instrucciones); el flotante
del SPU redondea hacia cero, pero eso solo mueve el color en un texel de
borde. Las líneas se acotan con la intersección de sus tres bordes (con un
píxel de margen) y se recorren de a **dos grupos por vuelta** (dos cadenas
independientes, para llenar los dos tubos del SPU con latencias de 6–7
ciclos). Direccionamiento sin desplazamientos por píxel: s se interpola como
(s + 2048)·4 y t como (t + 2048)·4·ancho, y el desplazamiento del texel en
bytes sale con dos `and` y un `or`. Los atributos se evalúan desde una base
que depende solo del triángulo: cualquier reparto (1 o 2 SPEs, bandas o
tiles) da los mismos bits.

**Caché de texturas.** Texturas enteras **ya decodificadas a 32 bits por
texel** ((r6+1), (g6+1), (b6+1), (a5+1): la modulación y la mezcla salen con
`shufb` y `mpyu`, sin decodificar por píxel) en 28 páginas de 4 KB (112 KB);
clave = offset, tamaño, formato y transparencia del índice 0 (más la paleta);
reemplazo de la corrida de páginas usada hace más tiempo. Se trae por DMA en
trozos de 2 KB desde cualquier alineación y se decodifica al llegar (formatos
directo, paleta de 16 y de 256; los demás, sin soporte todavía, salen
blancos y se cuentan). Se vacía al empezar cada cuadro (un emulador llevaría
versión por banco de VRAM; así la medida incluye la carga de todas las
texturas en cada cuadro). Con 12 texturas cada SPE trae cada una una vez por
cuadro; con 24, como cada unidad vuelve a recorrer la lista en orden de
dibujo, el LRU se desborda en cada unidad (1178 fallos y 2,9 MB de VRAM por
cuadro a 1× en un SPE): ver «Riesgos».

**Presupuesto del local store** (cabecera de `spu_main.c`, medido al
enlazar): código 27,3 KB, datos 0,8 KB, bss 202,1 KB (caché de texturas
112, color 2 × 16, profundidad 16, cajas 16, registros 2 × 6, triángulos 2 ×
2, lista 4 KB…), pila con 25,6 KB libres; la cadena más honda (`_start →
main → r3d_frame → setup`) usa 2,5 KB: **holgura 23,1 KB** (el enlace falla
por debajo de 8 KB, `raster_STACK_MIN`). spu-gcc integra `spukit_run` y el
manejador, así que no hay llamadas por puntero que declarar.

**Instrucciones por vuelta del bucle interno** (2 grupos = 8 píxeles,
`spu-objdump` del binario; par = tubo par, impar = cargas, `shufb`, saltos):

| Variante | Instrucciones | Par | Impar |
|---|---|---|---|
| opaco, textura repetida | 196 | 119 | 65 |
| opaco, «clamp» en algún eje | 214 | 135 | 66 |
| translúcido, repetida | 248 | 160 | 77 |
| translúcido, «clamp» | 266 | 174 | 78 |

Por grupo de 4 píxeles opaco: 7 fma de atributos, recíproco (frest + fi + 1
paso de Newton), 5 fm, 6 conversiones, 4 lecturas de texel (lqx + rotqby y
ensamblado), modulación con 4 `mpyu`, prueba de z, `selb` y 2 `stqx`. Cota
de tubo par: ~60 ciclos por grupo, ~15 por píxel.

### Fidelidad con la DS (qué se aproxima)

Lo que se respeta porque cambia el costo: textura con corrección de
perspectiva, color de vértice, modulación de la DS
(((texel+1)·(vértice+1)−1)/64 en 6 bits), alfa por polígono por alfa del
texel, alfa 0 no se dibuja, mezcla de translúcidos ((fuente·(a+1) +
destino·(31−a))/32, sin mezclar sobre alfa 0, alfa = máximo), translúcidos
después de los opacos, z-buffer de 24 bits con prueba «menor que»,
translúcidos que no escriben profundidad salvo con su bandera, texturas de la
VRAM en su formato y orden de bytes.

Lo que se aproxima o falta (Fase 3): la regla de cobertura y la
interpolación exactas de la DS (la DS usa coordenadas enteras de pantalla y
su propia precisión para la perspectiva; aquí hay subpíxel y flotante), el
orden de los translúcidos por Y (aquí, el orden de la lista), w-buffer (costaría
lo mismo: w ya se calcula por píxel), espejado de texturas, los formatos
A3I5, A5I3, 4 colores y comprimido, niebla, toon/highlight, edge marking,
antialias, polígonos de sombra, reglas de id de polígono en translúcidos,
imagen de fondo y el límite de polígonos por línea.

### Verificación

- **Referencia en C** (`r3d_ref.c`): escrita para leerse, no para correr;
  recorre la caja de cada triángulo píxel a píxel con bordes en enteros de 64
  bits, atributos baricéntricos en doble y cada texel leído directo de la
  VRAM (sin caché ni decodificación previa). Misma semántica que el núcleo.
- **Prueba unitaria** (`tests/unit/test_r3d.c`, LE en la Mac con ASan/UBSan y
  BE en qemu-ppc64): el mismo núcleo que corre en el SPE, con una E/S de
  memcpy que copia cada DMA recién al esperarlo (si el núcleo leyera un búfer
  antes de esperar o lo reescribiera antes de que salga, la imagen saldría
  mal) y cuenta alineaciones y tamaños inválidos. Escenas 1× N = 500 y 2048,
  1× con 24 texturas (desalojos), 2× N = 1000 y 4× N = 500; repartos de 1, 2
  y 3 «SPEs», alternas y mitades, bandas y tiles. Resultado en LE y en BE:
  **modo id idéntico a la referencia** (bordes, regla de arriba-izquierda,
  z-buffer, orden, recortes, reparto y DMA); **color**: 99,96 % de píxeles
  idénticos, 0,02–0,04 % «lejos» (un texel vecino por redondeo en el borde
  entre texeles; tope de la prueba 0,1 %); **todos los repartos dan los
  mismos bits**; con 12 texturas, una carga por textura y SPE.
- **Autoprueba de `r3d_vec.h`**: las funciones con intrínsecos del SPU
  (recíproco, `mpyu`/`mpyh`, `shufb`, `gb`, comparaciones) no se ejercitan en
  el host; el programa las corre en cada SPE y las compara con las genéricas
  del PPE (enteros iguales; recíproco con error relativo ≤ 1e-6). La puso ahí
  un error real: `spu_re` de spu-gcc **ya es `fi(frest(x))`**, y un `si_fi`
  de más dejaba el recíproco en ~12 bits: en RPCS3, texturas con ruido (47 %
  de píxeles lejos) y capas de z invertidas en 17 píxeles. Con el arreglo:
  0,89e-7.
- **RPCS3** (`tools/rpcs3-run.py spe-raster3d`, con el candado compartido con
  Salsa64): `RESULTADO spe-raster3d: OK`; las 45 configuraciones y las 4 de
  la caché desbordada con el modo id idéntico a la referencia del PPE, color
  99,95–99,96 % idéntico (0,03–0,04 % lejos) y el mismo hash en todos los
  repartos de cada escena. Deja la imagen de 1× N = 2048 en
  `/dev_hdd0/salsads/spe-raster3d-1x-2048.ppm`.

### Estimación antes de medir **[diseño]** (contra lo medido)

Los conteos (grupos, líneas, unidad·triángulo) son exactos y salieron iguales
en el host, RPCS3 y la PS3; los ciclos por vuelta eran una suposición (~160 por
par de grupos, ~500 por setup). N = 2048, ms por cuadro:

| Escala | 1 SPE estimado | 1 SPE medido | 2 SPEs estimado | 2 SPEs medido |
|---|---|---|---|---|
| 1× banda | ~3,5 | 6,76 | ~2 | **4,19** (alternas) / 4,25 (mitades) |
| 2× banda / tile | ~8 | 15,17 / 14,33 | ~4,5 | 8,38 / 7,96 |
| 4× banda / tile | ~27 / ~24 | 42,82 / 37,64 | ~14 / ~13 | 22,20 / 19,61 |

La estimación se quedó corta ×1,6–2: **lo medido cuesta el doble.** Del
desglose por SPE (1×, N = 2048): el dibujo costó 4,42 ms con 93 188 grupos →
152 ciclos por grupo de 4 píxeles (304 por vuelta de 2 grupos, contra 160
supuestos; el bucle de 196 instrucciones no llena los dos tubos y hay esperas
por las cadenas de latencia y las cargas del texel), y el setup 1,20 ms con
1969 triángulos → ~1950 ciclos por triángulo (contra 500: la división, la
conversión de 7 atributos y los DMA de salida pesan más de lo supuesto). Aun
así el criterio se cumple con casi el doble de margen.

### Resultados (PS3 real, `255b0d2`)

ms por cuadro, mediana de 9 tras 2 de calentamiento (mínimo y máximo difieren
< 1 %, salvo un máximo de 4,52 en 1× N = 2048 alternas). Autoprueba de los dos
SPEs: enteros iguales al PPE, recíproco 0,89e-7 (igual que en RPCS3). Escena
de 12 texturas.

| Escala | N | 1 SPE banda | 2 SPEs banda alternas | 2 SPEs banda mitades | 1 SPE tile | 2 SPEs tile alternas | 2 SPEs tile mitades |
|---|---|---|---|---|---|---|---|
| 1× | 500 | 3,63 | 2,15 | 2,20 | — | — | — |
| 1× | 1000 | 4,64 | 2,80 | 2,82 | — | — | — |
| 1× | 2048 | 6,76 | **4,19** | 4,25 | — | — | — |
| 2× | 500 | 9,53 | 5,09 | 5,29 | 9,26 | 4,96 | 5,16 |
| 2× | 1000 | 11,20 | 6,08 | 6,18 | 10,69 | 5,83 | 5,91 |
| 2× | 2048 | 15,17 | 8,38 | 8,59 | 14,33 | 7,96 | 8,15 |
| 4× | 500 | 30,26 | 15,45 | 16,10 | 28,38 | 14,53 | 15,04 |
| 4× | 1000 | 33,36 | 17,16 | 17,51 | 30,10 | 15,54 | 15,82 |
| 4× | 2048 | 42,82 | 22,20 | 22,81 | 37,64 | 19,61 | 20,09 |

(A 1× el tile de 256 es la banda.) Con 24 texturas (caché desbordada), N =
2048: 1× 42,22 (1 SPE) y **22,65** (2 SPEs alternas) contra 4,19 con 12; 2×
101,84 y **52,36** contra 8,38. El mismo núcleo escalar en el PPE (1× N =
2048): 50,46 ms, 7,5× más lento que 2 SPEs; la referencia de `r3d_ref.c`
tarda 237 ms por cuadro a 1× y 930 a 2×.

Desglose por SPE, 1× N = 2048 (ms): 1 SPE, total 6,75 = setup 1,20 + listas
0,08 + dma 0,02 + texturas 0,33 + dibujo 4,42 + salida 0,02; 2 SPEs
alternas, total 4,18 = setup 1,20 + listas 0,04 + dma 0,01 + texturas 0,33 +
dibujo 2,24 + salida 0,01. 4× N = 2048 tile alternas: 19,60 = setup 1,21 +
listas 0,53 + dma 0,11 + texturas 0,33 + dibujo 16,17 + salida 0,17. Con 24
texturas a 1× (1 SPE): texturas **35,40** de 42,22 ms (1178 fallos, 2913 KB
de VRAM por cuadro).

Lectura:

1. **A 1× sobra margen:** 4,19 ms con 2048 triángulos y 2,15 con 500.
2. **A 2× va al límite con 2 SPEs** (7,96–8,38 ms con N = 2048): sirve a 60 Hz
   solo si el 3D se queda con casi todo el cuadro. **A 4× no cabe en 2
   SPEs** (19,6–22,2 ms, más que los 16,7 ms de un cuadro): harían falta ~3
   SPEs o un bucle interno más rápido.
3. **Escala casi lineal con los SPEs:** 2 SPEs dan ×1,6 a 1× (el setup,
   1,20 ms, se duplica: 29 % del tiempo) y ×1,8–1,9 a 2× y 4×.
4. **Alternas gana a mitades** por 1–4 % (mejor equilibrio; el setup que
   ahorran las mitades no se nota).
5. **Tile gana a banda** a 2× (−5 %) y a 4× (−12 %): la banda de 4 líneas a 4×
   mete cada triángulo en ~3 veces más unidades (unidad·triángulo 31 649
   contra 11 018) y las listas pesan (0,53 ms).
6. **El costo es el dibujo** (66–82 %), no el DMA (< 3 %) ni las listas; con
   el doble búfer las esperas de DMA son despreciables.

**Imagen de la consola** (`spe-raster3d-1x-2048.ppm`): contra la de RPCS3 del
mismo programa, 49 145 de 49 152 píxeles idénticos, 1 cerca y 6 lejos (texel
vecino por redondeo del SPU real contra el de RPCS3); contra la del host,
49 115 idénticos, 9 cerca y 28 lejos (0,06 %, dentro de la tolerancia de
0,5 %). La imagen es la esperada.

### Decisión

1. **El 3D en SPEs es viable:** 4,19 ms contra el objetivo de 8 a 1× con 2048
   triángulos en 2 SPEs. A 1× no hace falta el RSX para el 3D; la alternativa
   del RSX queda solo para resoluciones internas de 4× (o 2× si el 2D y el
   audio comparten los SPEs), a decidir en la Fase 5.
2. **Reparto:** bandas alternas a 1×; tiles de 256 de ancho, alternos por
   filas de unidades, a 2× y 4×.
3. **Primera decisión de la Fase 3: la caché de texturas desbordada.** Con 24
   texturas el tiempo se multiplica ×5,4 a 1× (22,65 contra 4,19 ms) y ×6,2 a
   2×; el 84 % del tiempo de 1 SPE son texturas que se vuelven a traer y
   decodificar (1178 fallos por cuadro). Es el riesgo que más amenaza el
   objetivo con juegos reales (cientos de KB de texturas). Opciones, por
   medir antes de escribir el motor 3D: (a) guardar la textura en su formato
   original con la paleta decodificada aparte (2–8× menos local store, un
   `lqx` más por texel); (b) agrupar por textura o usar unidades más altas
   para recorrer menos veces la lista; (c) decodificación vectorizada de 4
   texeles por instrucción; (d) precarga de la siguiente textura mientras se
   dibuja (hoy el DMA y la decodificación son síncronos).
4. **Palancas del bucle interno** si hace falta 2× o 4×: el dibujo cuesta 152
   ciclos por grupo, con una cota del tubo par de ~60. Candidatas: cargas del
   texel (4 `lqx` + `rotqby`), un recíproco cada dos grupos, la conversión de z
   y el setup duplicado (29 % a 1×).

### Qué mirar al correrlo en la PS3

1. La primera línea del log (`/dev_hdd0/salsads/spe-raster3d.log`, o UDP
   18195): `spe-raster3d <versión> (SalsaDS raster, SLDS00002)`, y la
   segunda: la carpeta `/dev_hdd0/salsads` existe y el log se escribe.
2. `DATO autoprueba SPE0/SPE1`: enteros iguales al PPE y el error del
   recíproco en el hardware real (en RPCS3, 0,89e-7).
3. `RESULTADO spe-raster3d: OK`. Si hay `FALLO`, la línea dice qué escena,
   qué reparto y cuántos píxeles: en modo id es un error de verdad (cobertura,
   z, DMA, reparto); en color, ver si el porcentaje lejos supera el 0,5 %.
4. **El criterio**: `DATO r3d 1x N=2048 12tex 2spe banda alternas` (y
   `mitades`): ms por cuadro, mediana de 9 tras 2 de calentamiento. Objetivo
   < 8 ms.
5. Las demás `DATO r3d …` (1×, 2× y 4×; banda y tile; 1 y 2 SPEs) y, por SPE,
   el reparto `setup + listas + dma + texturas + dibujo + salida`: si el
   setup duplicado pesa (alternas contra mitades), si la banda de 4 líneas a
   4× pierde contra el tile, si hay esperas de DMA (`dma`) o de texturas.
6. `DATO r3d 1x N=2048 PPE`: el mismo núcleo en el PPE (sin AltiVec), para
   comparar.
7. La caché desbordada (24 texturas): cuánto suman los fallos.
8. Bajar también `spe-raster3d-1x-2048.ppm` y compararla con la del host.

### Si no se alcanza el objetivo

La alternativa del plan: el 3D en el RSX (con `rsxkit_draw_triangles` y
fragment programs de `fpgen` para la modulación y la mezcla de la DS) y los
SPEs solo para el 2D, aceptando perder exactitud (las reglas de cobertura, el
z/w-buffer de 24 bits, el orden de translúcidos y el edge marking de la DS no
se reproducen bit a bit en el RSX) y pagando la lectura de vuelta para la
captura de pantalla (`DISPCAPCNT`). Antes de llegar ahí, por orden de
palanca: más SPEs para el 3D (hay 6; el 2D y el audio piden uno cada uno),
z en flotante (sin `cfltu` por píxel), saltar grupos sin cobertura, setup sin
duplicar (dividido entre SPEs) y el w-buffer.

### Riesgos que deja

- **Texturas que no caben en la caché.** Con texeles de 32 bits caben 28 K
  texeles (112 KB). Una escena de la DS con 200 KB de VRAM de texturas en 4 y
  8 bits son 0,5–1,6 MB decodificados. Con la lista recorrida en orden por
  cada unidad, un LRU desbordado vuelve a traer todo en cada unidad. Opciones
  para la Fase 3: guardar el formato original con la paleta (2–8 veces menos
  local store, a cambio de un segundo `lqx` por texel), unidades más altas
  para recorrer menos veces la lista, o decodificar vectorizado.
- La exactitud de la DS (regla de cobertura, precisión de la perspectiva, z y
  w de 24 bits) sube el costo por píxel; el flotante del SPU sirve de
  estimación inicial para una división entera exacta, pero falta medirlo.
## 0.5 Composición 2D en un SPE (`tests/phase0/spe-gpu2d`)

Criterio: < 4 ms por cuadro para las dos pantallas en un SPE, y la imagen
igual a una referencia en C en el host (LE y BE). Estado: **imagen verificada
en las tres escalas (host LE, host BE, RPCS3); el tiempo en la PS3 real queda
pendiente** (paquete «SalsaDS 2D», APPID `SLDS00003`; sale `DATO gpu2d …` en
el log). Los números de RPCS3 no valen para nada y no se usan.

### Qué se implementó (y qué no)

`src/gpu2d/` (código portable; el compositor corre en el SPE y, con kernels
escalares, en el host):

- 4 fondos en modo texto: 4 y 8 bits por píxel, los 4 tamaños (256², 512×256,
  256×512, 512²), volteos horizontal y vertical, banco de paleta, base de
  teselas y de mapa (con los desplazamientos de DISPCNT), desplazamiento.
- 128 OBJ normales: mapeo de teselas en 1D (con la granularidad de DISPCNT) y
  en 2D, las 12 formas y tamaños (8×8 … 64×64), volteos, 4 y 8 bits, banco de
  paleta, prioridad, posición con signo y salto vertical a 256.
- Prioridades 0–3 (el OBJ delante del fondo de su misma prioridad), color de
  fondo, pantalla en blanco forzada.
- **Registro de cambios por línea**: una lista ordenada de (línea, registro,
  valor) que mueve DISPCNT, BGxCNT, BGxHOFS/VOFS y escribe entradas de la
  paleta antes de dibujar una línea (los efectos de raster).
- **No están**: ventanas, mezclas alfa, mosaico, brillo maestro, fondos afines
  y de mapa de bits, paletas extendidas, OBJ afines, los modos de OBJ
  semitransparente/ventana/bitmap (se saltan), la capa 3D y captura. El OAM y
  la VRAM se leen como una foto al empezar el cuadro: las escrituras de VRAM u
  OAM a mitad de cuadro necesitarán su propio registro de cambios y **no se
  midieron**. Lo no implementado suma costo (sobre todo mezclas y ventanas, que
  añaden trabajo por píxel): el criterio de 4 ms se debe volver a mirar cuando
  existan.
- Las direcciones de teselas de OBJ en 2D con 256 colores y la envoltura de los
  bancos son mi lectura de GBATEK; **no las contrasté con melonDS ni con una DS**.
  La prueba demuestra que el compositor del SPE y la referencia (escrita por
  separado, ver abajo) dan la misma imagen con el mismo modelo, no que el modelo
  sea exacto. Para esta fase cuenta el patrón de DMA y el costo, que no dependen
  de ese detalle.

### Diseño del compositor del SPE

- **VRAM en memoria principal, vista ya resuelta**: fondos de A 512 KB, OBJ de A
  256 KB, B 128 KB + 128 KB (el núcleo tendrá que resolver los bancos A–I a esa
  vista por cuadro). Paletas (1 KB), OAM (1 KB) y registro de cambios se traen
  enteros al arrancar la pantalla (hasta 3072 cambios caben en el local store).
- **Tubería de dos etapas**: mientras se compone la línea `y`, ya se pidió por
  DMA lo que necesita la `y + 1`. Es posible porque los registros de todo el
  cuadro están en el registro de cambios. Etiquetas de DMA exactas (por fondo y
  ranura, por paridad de línea para OBJ y para la salida) para no esperar de más.
- **Caché de la fila de teselas de cada fondo**: el mapa (una fila de 64 B por
  bloque de pantalla) y las 33 teselas de la línea (lista de DMA de 33
  elementos de 32 o 64 B) solo se traen de nuevo cuando cambia algo que las
  determina; sin efectos de raster, una vez cada 8 líneas. Dos ranuras por
  fondo para que la etapa siguiente no pise la línea que se está componiendo.
  Hay una única espera intermedia por línea nueva (el mapa tiene que llegar
  antes de saber qué teselas pedir).
- **OBJ**: por línea, la fila de teselas de cada OBJ que la cruza (≤ 512 B, un
  `mfc_get`, con envoltura de la región si hace falta). Sin caché entre líneas:
  la cuenta es una orden de DMA por OBJ-línea visible.
- **Composición en el dominio del índice de paleta**: vectores de 8 píxeles de
  16 bits (0–511 = fondos y OBJ, 0xFFFF = transparente). Por tesela: expandir
  4 u 8 bits a 8 carriles con un `shuffle` y un desplazamiento por carril,
  comparar con cero, sumar el banco; el desplazamiento fino y la colocación de
  los OBJ en cualquier columna son `shuffle`s con patrones de una tabla; fundir
  es un `cmpeq` y un `sel`. El color se resuelve al final, una vez por píxel
  (paleta ya convertida a RGBA5551). Nada de bucles por píxel en las capas.
- **Salida**: una línea de 512 B por DMA, doble búfer, RGBA5551 listo para
  `rsxkit_fb_upload` o para una textura mapeada.
- **Presupuesto del local store** (medido al enlazar): código 11 848 B, datos
  800 B, `.bss` 187 024 B (el contexto `gpu2d_ctx` son 186 112 B: OBJ 128 KB,
  teselas 17 KB, cambios 24 KB, el resto OAM, paletas, línea, mapas y listas);
  ≈ 199,7 KB en total y **holgura de pila 60 976 B** (`tools/spu-stack.py`,
  mínimo 4096). Cabe un SPE por pantalla doble; no cabe más trabajo en el mismo
  SPE sin reducir los búferes de OBJ (128 KB: dos copias de 128 filas de 512 B).

### Verificación

1. **Referencia en C** (`gpu2d_ref.c`): píxel por píxel, sin compartir con el
   compositor ni la decodificación del OAM ni el cálculo de direcciones.
2. **Host, LE y BE** (`tools/build.sh test-host`, `tests/unit/test_gpu2d.c`): el
   compositor con kernels escalares contra la referencia, en las tres escenas
   fijas y en 60 escenas aleatorias por pantalla (configuración, OAM con afines,
   desactivados, formas inválidas y modos que no se dibujan, y cambios al azar
   incluidas la pantalla en blanco y la paleta); huellas FNV-1a de 64 bits de
   las escenas fijas, **idénticas en LE y BE**; control negativo (un píxel
   cambiado se detecta) y comprobación de que la imagen no es trivial. LE y BE:
   0 fallos.
3. **El SPE de verdad** (RPCS3, y la PS3 al probar): `spe-gpu2d` genera la escena
   en el PPE, la dibuja con la referencia, manda el trabajo al SPE por spukit y
   compara las dos pantallas píxel por píxel, otra vez tras repetir el trabajo N
   veces, y con 6 (RPCS3) o 24 (PS3) escenas aleatorias. Resultado en RPCS3: **0
   píxeles distintos en las tres escenas y en las 6 aleatorias**
   (`RESULTADO spe-gpu2d: OK`). Los kernels SIMD solo se prueban aquí y en la PS3.

### Escenas

| Escena | Qué estresa |
|---|---|
| `tipica` | 4 fondos de 4 bits (tamaños 256², 512×256, 256², 512²), un efecto de raster en un fondo, 40 OBJ de tamaños variados |
| `pesada` | los 4 fondos con raster en cada línea (2 de 8 bits), cambios de paleta del color de fondo en cada línea, 128 OBJ de tamaños variados, mezcla 4/8 bits |
| `extrema` | el peor caso de DMA: la fila de teselas de los 4 fondos cambia **en cada línea** (VOFS y HOFS por línea), 128 OBJ de 64×64 y 8 bits, 2 cambios de paleta por línea |

### Qué mirar en la PS3

Líneas del log (`/dev_hdd0/salsads/spe-gpu2d.log` o UDP 18195):

- `DATO gpu2d <escena> SPE por cuadro (A+B): mediana … ms, mín …, máx … [A …, B …]` y
  `criterio < 4 ms: SÍ/NO`: es la duración medida por el propio SPE
  (decrementador) de las dos pantallas juntas. El criterio es la mediana de
  `tipica` y `pesada`; `extrema` es un peor caso adrede.
- `contadores`: filas de teselas traídas, esperas por el mapa, filas de OBJ y
  OBJ-línea dibujados, cambios aplicados (explican el costo).
- `referencia en el PPE`: lo que tarda el PPE en la referencia píxel por píxel
  (solo de curiosidad).
- `RESULTADO spe-gpu2d: OK` solo exige que las imágenes coincidan, no el tiempo.

Palancas ya identificadas: caché de filas de OBJ entre
líneas, una sola espera por línea juntando la del mapa de todos los fondos que
cambian —ya lo hace— y resolver la paleta con vectores).

### Resultados en la PS3 real (2026-10-16)

Paquete «SalsaDS 2D» (`06ab2d0`); log del autor, 120 cuadros por escena,
`RESULTADO spe-gpu2d: OK` y 24 escenas aleatorias sin diferencias contra la
referencia. Duración medida por el propio SPE, mediana (mín = máx):

| Escena | A+B | A | B | Referencia en el PPE | < 4 ms |
|---|---|---|---|---|---|
| típica | **4,31 ms** | 2,19 | 2,12 | 22,7 ms | NO (por poco) |
| pesada | **5,59 ms** | 2,77 | 2,81 | 34,9 ms | NO |
| extrema | **8,46 ms** | 4,32 | 4,14 | 38,2 ms | NO |

Contadores (típica, A): 108 filas de teselas, 73 esperas por el mapa, 940
OBJ-línea; (extrema, A): 768 filas, 192 esperas, 5454 OBJ-línea.

Lectura: el criterio **no se cumple** con las dos pantallas en un SPE; con una
pantalla por SPE serían ~2,2 / 2,8 / 4,3 ms (la típica y la pesada caben en 4
ms, la extrema no por poco). La estimación [est] de abajo resultó ~3× optimista.
La latencia del DMA del mapa no se esconde: cada fila nueva de teselas espera
el mapa (73–192 esperas por pantalla) antes de poder pedir las teselas.

**Salidas anotadas (no implementadas):**
1. **Un SPE por pantalla** (A en uno, B en otro): el costo por SPE baja a ~2,2
   ms (típica), 2,8 (pesada) y 4,3 (extrema). Cuesta un SPE más (de los 6) y
   es lo más barato de hacer: el compositor ya es por pantalla (`engines` en el
   trabajo).
2. **Esconder el DMA del mapa con doble búfer**: pedir el mapa de la línea
   y + 2 (o de toda la fila de teselas siguiente) mientras se compone la y, para
   que las teselas se pidan sin esperar al mapa; quita la espera intermedia de
   `plan_line`. Combinable con la 1.

### Estimación previa a la medición [est] (errada)

Por conteo de instrucciones del ensamblador, un fondo cuesta del orden de 1100
instrucciones por línea (33 teselas expandidas y 32 fusiones) y un OBJ ~35 por
tesela: unos 0,5 ms de fondos y 0,8 ms de OBJ en la escena extrema para las dos
pantallas, más la resolución de la paleta (~0,25 ms) y las esperas. Se espera
que `tipica` y `pesada` queden bien por debajo de 4 ms y `extrema` cerca de la
mitad del margen, pero **es una estimación: lo que manda es la PS3**.

## 0.6 Salida de dos pantallas y entrada (`tests/phase0/dual-screen`)

Criterio: 60 Hz estables en RPCS3 y en la PS3 real, con el tiempo de
presentación en el log; DS3 → botones de la DS; puntero táctil. Estado:
**construido, verificado en RPCS3 a 720p y a 1080p; falta la PS3 real**
(paquete «SalsaDS pantallas», APPID `SLDS00004`).

### Salida

- Dos texturas RGBA5551 de 256×192 por cuadro en memoria principal mapeada al
  RSX (`rsxkit_map_main`; dos juegos de dos texturas de 96 KB que se turnan
  con una barrera `rsxkit_fence`), dibujadas como dos quads con
  `rsxkit_draw_triangles` (color = textura × blanco). Las texturas se
  rellenan en el PPE con patrones de prueba; en el emulador las escribirá el
  compositor 2D del SPE (0.5) directamente.
- Puntero y texto de ayuda: `rsxkit_ui` (cuadros con alfa) sobre las pantallas.
- Distribuciones (`src/frontend/layout.c`, probadas en LE y BE): **lado a lado**,
  **una sobre otra**, **grande arriba** y **grande abajo** (la chica a la derecha,
  alineada por abajo). L2 las cambia. Escala **ajustada** (la mayor, no entera,
  filtro lineal) o **entera** (múltiplo entero, filtro puntual):

| Salida | Lado a lado | Una sobre otra | Grande + chica |
|---|---|---|---|
| 720p ajustada | 2,5× (640×480 cada una) | 1,875× (480×360) | 3,33× + 1,67× |
| 720p entera | 2× (512×384) | 1× (256×192) | 3× + 2× |
| 1080p ajustada | 3,75× (960×720) | 2,81× (720×540) | 5× + 2,5× |
| 1080p entera | 3× (768×576) | 2× (512×384) | 5× + 2× |

  (A 720p una sobre otra con escala entera solo cabe 1×: es la limitación de la
  escala entera, no un error.)
- Barrido automático al arrancar: resolución × distribución × escala (16
  combinaciones, 180 cuadros cada una; 12 en RPCS3). Por combinación escribe
  `DATO pantallas <res> <distribución> <escala>: intervalo mediana/p95/máx, >20
  ms, vblanks saltados, espera del flip, trabajo del PPE por cuadro (mediana/p95),
  60 Hz estable: SÍ/NO` (estable = mediana del intervalo entre 16,18 y 17,18 ms,
  ningún vblank saltado y ningún cuadro de más de 25 ms). Tras el barrido, modo
  interactivo.
- El tiempo de presentación son tres cifras del log: el **intervalo** entre
  `rsxkit_frame_begin` consecutivos (60 Hz = 16,68 ms), la **espera del flip** (lo
  que `rsxkit_frame_begin` espera al vsync: el cuadro está listo mucho antes) y el
  **trabajo del PPE** por cuadro (rellenar texturas + ordenar el dibujo + la
  interfaz: la parte que no depende del vsync).

### Resultados en la PS3 real (2026-10-16)

Paquete «SalsaDS pantallas» (`06ab2d0`), log del autor: `ioPadSetSensorMode(ON)
= 0`, `controles.cfg` creado por omisión y **las 16 combinaciones (720p y 1080p,
4 distribuciones, escala ajustada y entera) con 60 Hz estable**: intervalo
mediana 16,66–16,67 ms, p95 ≤ 16,84, máx ≤ 16,86, 0 cuadros de más de 20 ms, 0
vblanks saltados, espera del flip ~15,6 ms y trabajo del PPE 1,05–1,07 ms por
cuadro. Criterio de 60 Hz cumplido.

**Medido con «SalsaDS pantallas giro» (2026-10-16)**: con la inclinación el eje
vertical está bien con `invertir_y = 1` (ahora el valor por omisión), σ del puntero
0,20–0,34 px con el control quieto y el giroscopio quieto entre 510 y 514 alrededor
de 512. El modo GIRO todavía no se probó en la consola (el log solo registró el
cambio a inclinación). **Un DualSense conectado a la PS3**: funcionan los botones
pero **no los sensores** (el sistema no se los pasa al homebrew por libpad; leerlos
exigiría USB crudo, sin verificar): los modos de inclinación y giro son del DS3.

El log de esa corrida terminó en «modo interactivo» sin líneas `sixaxis:`: el
`PUNTERO` era `STICK` (el valor por omisión) y esas líneas solo se escribían en
`INCLINACION`. Corregido: ahora los rangos crudos de los sensores se registran
en cualquier modo (el temblor solo vale en `INCLINACION`). La sensación de los
dos modos sigue pendiente.

### Resultados en RPCS3 (no son mediciones de rendimiento)

- Ambas resoluciones: `tools/rpcs3-run.py dual-screen` (720p) y con
  `--config tools/rpcs3/salsads-1080.yml` (1080p; RPCS3 solo ofrece una
  resolución por configuración, por eso el barrido salta la otra con un aviso).
- **8 combinaciones por resolución, todas con 60 Hz «estable»** por el criterio de
  arriba (intervalo mediana 16,63–16,80 ms, ningún vblank saltado; trabajo del
  PPE 0,6–0,8 ms por cuadro): el programa ordena un cuadro bien por debajo del
  presupuesto. En la PS3 real el vsync y el PPE son otros: falta medir.
- **Distribución verificada por píxeles**: en cada combinación se lee el búfer
  trasero y se comprueba que la marca roja de la pantalla de arriba y la azul de
  la táctil están donde dice la geometría y que la esquina libre es el color de
  fondo (0 fallos en 16 combinaciones: 8 a 720p y 8 a 1080p).
- La entrada sintética (el stick derecho da vueltas y R2 toca a ratos) recorre
  mando → mapeo → puntero → toque → dibujo → pantalla y pinta cientos de píxeles
  en la pantalla táctil; se ve en las capturas (`dual-screen-dump`, ver
  `tools/raw-png.py`).

### Entrada: DS3 → DS

| DS3 | DS | | DS3 | DS |
|---|---|---|---|---|
| ○ | A | | L1 / R1 | L / R |
| ✕ | B | | Start / Select | Start / Select |
| △ | X | | cruceta | cruceta (arriba+abajo e izquierda+derecha se anulan) |
| □ | Y | | L2 | cambia la distribución |
| R2 | **botón de toque** | | L3 | recentra (inclinación) / puntero al centro (stick) |

`src/frontend/dspad.{h,c}` no depende de PSL1GHT y se prueba en LE y BE
(`tests/unit/test_dspad.c`, `test_layout.c`).

### Táctil: puntero, botón de toque y modos

Decidido con el autor (2026-10-16): un puntero visible que solo «toca» mientras
se mantiene apretado el botón de toque; arrastrar es mover el puntero con el
botón apretado y soltarlo es levantar el lápiz. Se dibuja una **mira hueca**
cuando no toca y un **cuadro rojo lleno con aro blanco** cuando toca. El toque
que llegaría a la DS (x 0–255, y 0–191) es el del puntero sobre la pantalla
táctil, esté donde esté en la salida.

**Configuración**: `/dev_hdd0/salsads/controles.cfg` (texto, estilo del de
Salsa64). Si no existe, el programa lo crea con los valores por omisión y lo
comprueba (`controles.cfg no existía: lo creé…` en el log) para poder bajarlo por
FTP, editarlo y volver a subirlo; se lee al arrancar. Líneas (mayúsculas o
minúsculas, `#` comenta):

```
TOQUE = R2            # botón de toque; cualquier botón del DS3 (R3, L3, CRUZ…), hasta dos
RECENTRAR = L3
DISPOSICION = L2
A = CIRCULO           # los 12 botones de la DS se pueden remapear igual
PUNTERO = STICK       # o INCLINACION o GIRO
```

- **Cambiar el botón de toque a R3**: `TOQUE = R3`. (Nombres del DS3: CRUZ CIRCULO
  CUADRADO TRIANGULO L1 R1 L2 R2 L3 R3 START SELECT ARRIBA ABAJO IZQUIERDA DERECHA NADA.)
- **Modo del puntero**: `PUNTERO = STICK` o `PUNTERO = INCLINACION`. En la prueba,
  además, L1 + R1 con ✕ lo alterna en caliente (sin guardar).

**(a) Stick derecho** (`PUNTERO = STICK`): velocidad proporcional a la inclinación
del stick con zona muerta radial. `zona_muerta = 12` (% del recorrido),
`velocidad = 200` (px/s a fondo), `curva = 1.0` (1 = proporcional; > 1, más fino
cerca del centro). La zona muerta se descuenta (no hay salto al salir de ella).

**(b) Inclinación** (`PUNTERO = INCLINACION`): lee los acelerómetros del Sixaxis
(`ioPadSetSensorMode`; `SENSOR_X/Y/Z`, 10 bits, centro nominal 512). Inclinar a
los lados mueve en horizontal y cabecear, en vertical, con los ángulos sacados de
la **dirección de la gravedad** (no dependen de la escala de los sensores ni de
cómo se sostenga el control): a los lados = `atan2(lateral, √(gravedad² +
adelante²))`, cabeceo = `atan2(adelante, |gravedad|)`. La primera postura es el
centro y **L3 recentra** (la postura actual pasa a ser el centro y el puntero
vuelve al centro de la pantalla). Filtro paso bajo exponencial sobre los ángulos
(`filtro_ms = 120`), zona muerta en grados (`zona_muerta_inclinacion = 1.0`),
`sensibilidad_x/y = 4.0` px por grado, `invertir_x/y`. **El stick derecho sigue
moviendo el puntero** en este modo: su desplazamiento se suma a la posición dada
por la inclinación y no se acumula contra el borde. `giroscopio = 0` (apagado):
si se pone un valor, integra el giro sobre el eje vertical (`SENSOR_G`) con fuga
de 2 s; es un experimento, el acelerómetro no puede dar el giro de la mano en el
plano horizontal. Sin sensores (o sin el mando) el modo cae al del stick y la
interfaz lo dice.

**Medido en la consola (2026-10-16)**: con `PUNTERO = INCLINACION` el eje vertical
salía **invertido**; por eso `invertir_y = 1` es ahora el valor por omisión (el
horizontal estaba bien con `invertir_x = 0`).

**(c) Giro** (`PUNTERO = GIRO`, nuevo, relativo como un ratón o el giroscopio de
la Switch/DualSense): rotar el control mueve el puntero; STICK e INCLINACION
siguen. El DS3 solo tiene **un** giroscopio, de guiñada (`SENSOR_G` en `padData`
de PSL1GHT, 16 bits con rango 0–0x3FF y centro nominal 512, según su cabecera;
la escala en °/s no la conozco). Por eso: (1) **horizontal** = guiñada integrada
(`(g − sesgo) × giro_sensibilidad` px/s con `giro_zona_muerta`), con el sesgo
calibrado al arrancar (promedio de ~0,75 s: **dejar el control quieto al abrir
el programa o al cambiar de modo**) y recalibrado (media móvil de 1 s) mientras
está quieto (giroscopio a ±4 del sesgo y acelerómetro sin cambios); (2)
**vertical** = *cambio* del cabeceo del acelerómetro entre cuadros, filtrado
(`filtro_ms`) y con `sensibilidad_y`/`invertir_y`; (3) **trinquete** opcional
`TRINQUETE = R1`: el puntero solo se mueve mientras se mantiene ese botón (como
levantar el ratón; `NADA` = siempre); L3 recentra; el stick derecho también
suma. El signo y la escala de la guiñada no están medidos: `invertir_x` y
`giro_sensibilidad`. **Datos del log** cada 10 s en este modo: sesgo
calibrado, media y σ del giroscopio, y la **deriva horizontal** en px; con el
control quieto el log dice `giroscopio UTIL` (σ ≤ 3 unidades y deriva ≤ 40 px en
10 s) o `INSERVIBLE (… quedarse con INCLINACION)`; si el control se movió, dice
que la deriva no vale. L1 + R1 + ✕ cicla stick → inclinación → giro. Sin medir
en la consola.

**Suposición de los ejes del Sixaxis, sin verificar**: `eje_lateral = X` (a los
lados) y `eje_gravedad = Y` (el que lee la gravedad con el control plano,
suposición mía, ni contrastada con documentación ni con la consola); el
tercero (Z) es adelante. Si el puntero se mueve por el eje equivocado o al
revés, se arregla en `controles.cfg` (`eje_lateral`, `eje_gravedad`,
`invertir_x`, `invertir_y`) sin recompilar. Cada segundo el log trae los
rangos crudos de los tres ejes y del giro y los ángulos (`sixaxis: …`).

### Cómo se siente cada modo en la consola — **pendiente**

Esto solo se puede decidir con el mando en la mano y no lo pude probar (sin la
consola a mano). La tabla se llena en la PS3 real; el programa deja los datos para
no depender solo de la impresión:

| | Stick derecho | Inclinación |
|---|---|---|
| Precisión (¿se llega a un píxel de la DS?) | por probar | por probar |
| Temblor | no aplica | `temblor σ = (x, y) px` del log con el control quieto sobre la mesa |
| Latencia | un cuadro (16,7 ms) | un cuadro + retardo del filtro |
| Ajustes que dieron el mejor resultado | | |

- **Temblor**: con el control quieto sobre la mesa y el stick en reposo, el log
  imprime cada segundo la desviación típica de la posición del puntero
  (`temblor σ = (x, y) px`); una cifra de σ en píxeles de la DS que se pueda
  comparar entre ajustes de `filtro_ms` y `zona_muerta_inclinacion`. También hay
  una línea `sixaxis:` con el rango crudo (min…max) de cada eje, que es el ruido
  del sensor.
- **Latencia**: el pad se lee justo después del vsync y el cuadro sale en el
  siguiente: de la lectura a la pantalla hay un cuadro (16,7 ms) más la salida de
  video de la tele. El filtro de la inclinación añade su constante de tiempo
  (`filtro_ms`, 120 ms por omisión: el puntero alcanza el 63 % de un cambio brusco
  en 120 ms y el 95 % en ~360 ms; 0 lo apaga): es el compromiso temblor/retardo y
  el primer ajuste que hay que probar. El stick no tiene filtro.
- **Precisión**: la mejor prueba es el dibujo de la pantalla táctil (lo que un
  juego recibiría): con el botón de toque apretado se pinta cada (x, y) y se unen
  con líneas; trazar un círculo o escribir una letra muestra el temblor y la
  resolución de cada modo. L1 + R1 + □ borra.
- Qué ajustar si no gusta: `sensibilidad_x/y` (más alto = menos inclinación para
  cruzar la pantalla; 4 px/grado cruza en ±32°), `filtro_ms`, `zona_muerta_inclinacion`,
  `invertir_*`, `eje_*`, y `velocidad`/`curva`/`zona_muerta` del stick.

### Qué mirar en la PS3

`/dev_hdd0/salsads/dual-screen.log`: línea de banner con `SLDS00004`; `controles
…`; `ioPadSetSensorMode(ON) = 0; modo actual 1` (si el modo no pasa a 1, la
consola no da sensores); `salida WxH`; las 16 líneas `DATO pantallas`;
`barrido: N combinaciones, M con 60 Hz estable`; después, en el modo interactivo,
una línea `sixaxis:` por segundo con el puntero en modo inclinación.
`rsxkit_video_reconfigure(1080p) = -2` significa que la tele no da 1080p.

## 0.7 Prueba mínima del JIT ARM → PPC64 (`tests/phase0/jit-arm`)

Criterio del plan: mismo resultado y ganancia medida contra el intérprete de 0.3;
si en CFW se cuelga, el respaldo al intérprete funciona. Se agregó lo acordado
con el autor: (a) cuántos MHz de ARM9 saca el código traducido en bucles reales
contra el intérprete de 0.3 y contra la meta de 67 MHz; (b) cómo salen las
banderas NZCV en el PPE; (c) el ARM7 en el hilo 1 del PPE. Estado: **construido
y verificado en el host y en qemu-ppc64; falta medirlo en la PS3 real** (paquete
«SalsaDS JIT», APPID `SLDS00007`, log `/dev_hdd0/salsads/jit-arm.log`).

### Qué es

`src/jit/` (código portable; el traductor emite PowerPC64 sin ejecutarlo, así que
compila y se prueba en cualquier host):

- **Subconjunto ARM**: datos (AND, EOR, ORR, MOV, ADD, SUB, ADC y CMP) con
  inmediato o registro con desplazamiento inmediato (LSL, LSR, ASR, ROR) y S en
  ADD/SUB/CMP; LDR/STR/LDRB/STRB con desplazamiento inmediato; B<cond> y SWI. Lo
  demás (MUL, bloques LDM/STM, desplazamiento por registro, ejecución
  condicional de datos y las condiciones MI, PL, VS y VC) se rechaza con
  `JA_E_UNSUPPORTED` o `JA_E_FLAGS`: es una prueba de factibilidad, no el
  recompilador de la Fase 4.
- **Traductor** (`jit_arm.c`): una pasada sobre el programa; bloques básicos con
  un encabezado de 3 instrucciones que comprueba que el bloque quepa en el
  presupuesto de instrucciones hasta el próximo evento y lo descuenta (la
  regla de exactitud de tiempo de Salsa64); los r0–r12 del guest en r14–r26;
  memoria del guest en little-endian con `lwbrx`/`stwbrx`/`lbzx`/`stbx` sobre una
  RAM plana de 16 KB con la dirección enmascarada (la tabla de páginas de la
  Fase 1 sumaría su costo encima y **no está medida aquí**); resultados de 32
  bits con `clrldi` tras cada suma o resta. Sin formas con punto, `mfcr`,
  `mtcrf` ni desplazamientos por registro; solo Power ISA 2.02.
- **Tres formas de llevar las banderas** (`ja_flagmode`):
  - *perezosas*: se guardan los operandos y el resultado (2 `mr` por
    comparación) y el consumidor hace su `cmpw`/`cmplw` y el salto;
  - *CR por instrucción*: cada instrucción que fija banderas deja sus
    comparaciones en `cr0` (con signo, igualdad) y `cr1` (sin signo, acarreo) y el
    salto lee el campo; ADC sale con un salto corto sobre el bit de acarreo;
  - *fusionadas*: el `cmp` + `bcc` se funden si las banderas mueren ahí (análisis
    de vida sobre el programa) y solo se emite la comparación que el salto
    necesita; si no, perezosas.
  Como en el recompilador de Salsa64, el consumidor sabe en tiempo de traducción
  cuál fue la última instrucción que fijó las banderas (resta o suma), así que
  no se guarda un «tipo». Por eso las condiciones con signo salen exactas de
  `cmpw` tras una resta (no hace falta calcular N ni V) y solo se rechazan MI, PL,
  VS y VC, y las condiciones con signo tras una suma.
- **Intérprete de referencia** (`ref_arm.c`): banderas completas; es la verdad
  contra la que se compara el código traducido (y un intérprete más para medir).
- **Cuatro bucles** (`loops.c`): `mezcla` (el programa de la 0.3, mismo
  resultado y 9221 instr. por pasada; solo este corre en el intérprete de 0.3),
  `suma64` (ADDS + ADC y CMP/BNE), `ramas` (dos CMP + salto por elemento con
  datos casi aleatorios: HI/LS sin signo y GE/LT con signo) y `bytes`
  (LDRB/STRB, 9 instrucciones por byte).

### Caché de código y respaldo

`mk/rec-cache.ld` + `rec_cache.c`: cola NOBITS de 4 MB al final del segmento de
texto (con HEN es lo único escribible y ejecutable; el mecanismo y su medición
están en `docs/cell-desde-salsa64.md` §2; el segmento se descuenta de la
memoria de usuario). Antes de usarla, un **hilo de prueba** escribe
`mulli; addi; blr`, vacía la caché de instrucciones y lo ejecuta (22), lo
reescribe y vuelve a ejecutar (24), repite al final de la caché y escribe una
palabra por página; si no vuelve en 2 s (CFW: la escritura cuelga el hilo) o falla,
el programa sigue con los intérpretes y lo dice en el log. Un archivo
(`jit-arm.marker`, con la versión) marca la prueba y, después, el uso del código
traducido: si el proceso muere con la marca puesta, el siguiente arranque de esa
versión no usa el JIT. `jit-arm-nojit` fuerza el respaldo. En RPCS3 el texto es de
solo lectura: con el archivo de prueba automática el JIT no se intenta y solo
corre el respaldo (verificado: los intérpretes miden y el programa sale OK).

### Verificación

- **Host LE y BE** (`tools/build.sh test-host`, `tests/unit/test_jit.c`):
  codificaciones PowerPC contra valores conocidos, el intérprete de referencia
  contra el resultado calculado en C en los 4 bucles y contra el intérprete de 0.3
  en `mezcla` (mismas palabras de programa, mismos r0 y r4, mismas instrucciones),
  traducción de los 4 bucles en los 3 modos y rechazo de lo no soportado.
- **qemu-ppc64 (BE) ejecuta el código traducido**: los 4 bucles × 3 modos dan los
  mismos r0–r12, la misma memoria, el mismo pc y el mismo número de
  instrucciones (por el presupuesto) que el intérprete de referencia, y un
  presupuesto corto sale por el encabezado de un bloque sin pasarse. Esto cubre
  la corrección de lo emitido, no los tiempos. Descubierto en el camino: el
  compilador descartaba las escrituras del descriptor de función ELFv1 si solo se
  leían por el cast; hace falta una barrera de memoria antes de la llamada.
- En la PS3 el programa verifica de nuevo cada traducción contra el intérprete
  (registros, memoria y número de instrucciones) antes de medirla; si algo
  difiere, `RESULTADO … FALLO`.

### Qué mide en la PS3 y cómo leerlo

Líneas del log (`DATO …`), todas con el timebase, mediana y mínimo de 5 corridas
calibradas a ≥ 60 ms:

- `DATO jit <bucle> <implementación>`: ns por instrucción guest, MIPS, ciclos del
  PPE por instrucción guest, y los **MHz de ARM9 equivalentes** de dos maneras:
  con 1 ciclo por instrucción (= MIPS) y con un modelo simple (+2 ciclos por salto
  tomado), contra la **meta de 67 MHz**. Las implementaciones: referencia,
  intérprete de 0.3 (solo `mezcla`) y JIT con las tres formas de banderas.
- `DATO jit-traducción`: instrucciones del PPE emitidas por instrucción guest, bloques,
  cmp+salto fusionados, y el costo de traducir y de vaciar la caché de instrucciones.
- `DATO jit resumen`: ganancia contra la referencia y contra el intérprete de 0.3, y
  lo que ganan o pierden CR y fusionadas contra las perezosas. Con la tabla de
  costos de 0.3 (formas con punto ~12,5 ciclos extra, `mfcr` ~25, `add.` y `subf.`
  nativos) lo esperable es que CR y fusionadas queden cerca de la cota de
  «comparación + salto», pero **no hay números todavía**.
- Limitación de lo medido: la memoria es plana y sin tabla de páginas, no hay
  enlazado de bloques ni caché de destinos, y los bucles son cortos; los MHz son un
  techo para el código de la Fase 4, no una promesa.

**(c) El ARM7 en el hilo 1** (`threads.c`): el ARM9 corre `suma64` traducida en
trozos de ~6150 instrucciones y un segundo hilo simula el ARM7, casi siempre en
HALT: despierta, hace 2 µs de trabajo y vuelve a esperar. Se mide, contra el ARM9 sin
hilo del ARM7, la pérdida de MIPS del ARM9 y la latencia de despertarlo
(publicación → hilo corriendo) para cada forma de esperar: **girar**, **girar con
prioridad baja de SMT** (`or 1,1,1`), **dormir con `sysUsleep(50)`**, **semáforo de
lv2** (a 1000, 100 y 33 µs entre despertares) y **cola de eventos de lv2** (a 1000
y 100 µs), más el costo de publicar en el hilo del ARM9 (`sysSemPost`,
`sysEventPortSend`). Salsa64 midió ~0,13 ms de emulación perdidos por ms de
trabajo del hilo 1; esta prueba mide lo que cuesta además **esperar**.

### Pendiente

Pegar aquí las medianas de la PS3 (MHz por bucle y modo, ganancia contra 0.3,
costo de las banderas, pérdida del ARM9 y latencia por forma de esperar) y decidir:
si el JIT alcanza los 67 MHz del ARM9; qué forma de banderas se lleva a la
Fase 4; y cómo espera el hilo del ARM7 (la elección de la dirección del ARM7:
PPE hilo 1 o un SPE sigue abierta hasta tener estos números, ver «Decisiones» de
0.3 punto 6).
