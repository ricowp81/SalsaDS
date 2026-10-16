# Fase 0 — Entorno y factibilidad

Resultados de las pruebas de factibilidad del plan del proyecto. Cada prueba
vive en `tests/phase0/<nombre>`, su resultado va aquí y la medición, en
`perf-log.md`. Hasta cerrar la fase no se escribe el núcleo.

| Prueba | Estado |
|---|---|
| 0.1 Esqueleto del repositorio | hecho (2026-10-16) |
| 0.2 ROMs de prueba propias | hecho (2026-10-16) |
| 0.3 Microbenchmarks del PPE | hecho (2026-10-16): medido en la PS3 real, decisiones abajo |
| 0.4 Rasterizador 3D en un SPE | construido y verificado en host y RPCS3 (2026-10-16); falta medir en la PS3 real |
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

Estado: construido y verificado en el host (LE y BE) y en RPCS3; **pendiente
de medir en la PS3 real** con el paquete `SalsaDS raster` (APPID
`SLDS00002`, el mismo de `tests/phase0/empty`, que solo se corrió en RPCS3).
Los tiempos de RPCS3 no valen.

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

### Estimación antes de medir **[diseño]**

Solo para saber qué esperar y detectar cifras absurdas: los conteos son
exactos (los mismos en el host, RPCS3 y la PS3), los ciclos por vuelta son
una suposición (~160 por par de grupos: 119 del tubo par más esperas; ~60 por
línea; ~300 por triángulo y unidad; ~500 por setup).

| Escala, N = 2048 | Grupos | Líneas | Unidad·tri | 1 SPE (est.) | 2 SPEs (est.) |
|---|---|---|---|---|---|
| 1× | 93 188 | 29 426 | 3 596 | ~3,5 ms | ~2 ms |
| 2× banda / tile | 270 568 / 271 570 | 58 706 / 60 090 | 9 149 / 5 799 | ~8 ms | ~4,5 ms |
| 4× banda / tile | 867 184 / 871 962 | 117 224 / 125 729 | 31 649 / 11 018 | ~27 / ~24 ms | ~14 / ~13 ms |

Si la consola da algo de ese orden, el objetivo a 1× se cumple con margen y
4× no cabe en 2 SPEs a 60 Hz (haría falta 3–4, o más trabajo en el bucle).

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

### Resultados (PS3 real)

Pendiente de correr en la consola. Las cifras van aquí y en `perf-log.md`.
