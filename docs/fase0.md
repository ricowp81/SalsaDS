# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | hecho (2026-10-16) |
| 0.3 Microbenchmarks del PPE | hecho (2026-10-16): medido en la PS3 real, decisiones abajo |
| 0.4 Rasterizador 3D en un SPE | pendiente |
| 0.5 Composición 2D en un SPE | medido en la PS3 real (2026-10-16): imagen correcta, **criterio < 4 ms NO cumplido** con las dos pantallas en un SPE (4,31 / 5,59 / 8,46 ms); salidas anotadas abajo |
| 0.6 Salida de dos pantallas | **60 Hz estables en la PS3 real** a 720p y 1080p (16/16 combinaciones); sensación de los modos del puntero pendiente |
| 0.7 Prueba de JIT ARM → PPC | pendiente |

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
PUNTERO = STICK       # o INCLINACION
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
