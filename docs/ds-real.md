# Hallazgos en una DS real

Corridas de las ROMs de `tests/roms` en una DS Lite con flashcart R4 (log en la SD, ver `tests/roms/README.md`). Hardware
manda: donde difiere de GBATEK, de melonDS 1.1 o de SalsaDS, la ROM lo trata como verdad del hardware. Fecha de la primera
tanda: 2026-10-17 (ROMs del commit `728113c`). Lo que sigue marcado «pendiente» necesita otra corrida con las ROMs nuevas.

## Resultados por ROM

| ROM | Resultado en la DS real |
|---|---|
| `cpu9` | OK 65 187 (igual que el host y melonDS) |
| `ipc` | OK; las dos divergencias (FIFO) coinciden con la documentación |
| `bios` | OK 50 (sin las SWI 0x13 y 0x17); Halt del ARM7 conserva los registros (como GBATEK; la BIOS libre de melonDS no) |
| `bios13` | OK 54: la SWI 0x13 funciona con las dos condiciones de abajo |
| `mem` | OK 218; ranura de GBA medida en cinco situaciones (ver «Ranura de GBA») |
| `periph` | OK en lo que no toca el cartucho; SQRT y flashcart, abajo |
| `dma`, `irq`, `cpuarm` | ver «Fallos y su clasificación» |

## Hallazgos (tabla)

| Caso | Hardware | GBATEK / SalsaDS | melonDS 1.1 | Qué cambiar |
|---|---|---|---|---|
| SWI 0x13 (HuffUnCompReadByCallback) | exige que r2 apunte a un búfer de 0x200 bytes (con r2 = 0 no vuelve) y llama a Close (con Close = 0 tampoco) | la HLE no usa r2 ni llama a Close | no tiene la SWI | HLE de la BIOS: aceptar r2 como búfer del árbol y llamar a Close del callback (el resto ya lo hace); la ROM `bios13` lo cubre |
| SWI 0x17 (Diff8bitUnFilterVram) | no existe en la BIOS de la DS: la ROM cae en la excepción de instrucción indefinida | SalsaDS la implementa | la tiene | HLE de la BIOS: no implementarla (o dar excepción); `bios` ya no la llama (`-DBIOS_SWI17` para comparar emuladores) |
| MSR con el bit T (`msr cpsr_c, #0xFF`) | cambia a Thumb (la ROM ejecutaba basura) | el manual la declara impredecible; SalsaDS la ignora | la ignora | Ninguno obligatorio; la ROM ya no la ejercita (`-DMSR_T_TEST`) |
| Temporizador 0 con el bit 2 (cuenta ascendente) | el bit **no se guarda** (TM0CNT_H lee 0xC3 tras escribir 0xFFFF) y cuenta como uno normal | GBATEK: bits 0-2, 6, 7 (0xC7) y que con el bit no cuenta; SalsaDS lo copia de melonDS (no cuenta) | no cuenta; devuelve todo lo escrito (0xFFFF) | Núcleo de SalsaDS: en el temporizador 0 ignorar el bit 2 (no se guarda, no hay cascada) |
| Ranura de GBA vacía (0x08000000 y 0x0A000000) | con el ARM9 dueño de la ranura (EXMEMCNT bit 7 = 0): bus abierto, 0xFFFF en 16 bits y 0xFF en 8; **la tapa de fábrica no influye**. Con el ARM7 dueño (estado al arrancar la ROM, EXMEMCNT = 0xE080) el ARM9 lee todo 0, con o sin cartucho | GBATEK/SalsaDS: 0xFFFF y 0xFF (acierta) | 0xFE08 y 0xFF (melonDS se equivoca en 16 bits) | Núcleo: ranura vacía con ARM9 dueño = 0xFFFF/0xFF; con el ARM7 dueño, el ARM9 lee 0 |
| SQRTCNT justo tras escribir el parámetro | el bit de ocupado ya puede haber bajado (13 ciclos) | SalsaDS lo mantiene ocupado | ocupado | Núcleo: opcional modelar la latencia; la ROM ya lo trata como informativo |
| Cartucho en flashcart | el chip ID (B8) y los comandos son del R4, no de la ROM | — | — | Ninguno; la ROM marca CARTUCHO y CART ARM7 como no aplicables si lo detecta |
| IPC FIFO vacía (arm9.c:99 y :123) | coincide con la documentación | la documentación | no coincide | melonDS se equivoca; SalsaDS ya lo hace bien |
| TMxCNT_H, SQRTCNT, DMA de 14 bits del ARM7 (dma arm7.c:38 y :41) | coincide con la documentación | la documentación | guarda todo | SalsaDS ya lo hace bien |

## Fallos de la primera tanda y su clasificación

| Fallo | Evidencia | Clasificación | Acción |
|---|---|---|---|
| `irq` TIMERS L372 (ARM9) y ARM7 L372: «el temporizador 0 con el bit de cuenta ascendente no cuenta» | hw: contó (0x54B9 y 0xAFD ticks) y TM0CNT_H lee 0xC3: el bit 2 no existe en el temporizador 0 | **corregir el emulador** (SalsaDS y melonDS) | la prueba pasó a divergencia informativa con el valor del hardware; cuando el núcleo lo cambie, pasarla a `SDS_CHECK` |
| `irq` TIMERS L377 (divergencia) | hw 0xC3, doc 0xC7 | **corregir el emulador** | la divergencia lleva ya 0xC3 como valor de referencia |
| `irq` ARM7 TIMERS L304: prescaler, ticks fuera de ±2 | con prescaler 1 un tick es un ciclo y leer el temporizador y luego la referencia cuesta varios ciclos de bus en hardware | **corregir la prueba** | tolerancia de 64 ticks con prescaler 1 (2 con los demás); si vuelve a fallar, el log trae ticks y esperados (id 3000 + p). Pendiente de confirmar |
| `dma` ARM7 COPIA L86 y MODOS L115, L122, L135 (una palabra mal por prueba) | el DMA empieza ~2 ciclos tras habilitarlo y la CPU alcanza a ejecutar una o dos instrucciones: la primera lectura del destino podía adelantarse. Solo el ARM7; ninguna fallaba en el ARM9 | **corregir la prueba** (hipótesis; sin confirmar) | `dma_set` deja unos `nop` tras habilitar, y cada bucle anota el primer elemento malo (valor obtenido y esperado) como divergencia con id = línea·1000 + índice. Si sigue fallando, el log dirá cuál palabra |
| `dma` HBLANK L93 y L96 (ARM9): a la línea 200 había 300 o más transferencias (esperadas 192); a la línea 100 fuera de 100-106 | en hardware, el DMA de HBlank desde la línea 192 hizo más de una transferencia por línea o corrió también en el VBlank; GBATEK y SalsaDS dicen 192 | pendiente: faltan datos | las dos comprobaciones pasaron a divergencias y el log anota las cuentas en las líneas 20/100/190/200/230/255 (`[dato] HBLANK DMA...`) para decidir con la siguiente corrida |
| `dma` e `irq`: «ARM9 ARM7: obtenido 0xB esperado 0xA» | no es una prueba: el ARM7 avisa por IPCSYNC de que tuvo fallos (los de arriba) | consecuencia | desaparece al corregir los fallos del ARM7 |

## Qué debe cambiar SalsaDS (lista para otra ronda; esta tanda no tocó `src/core` ni `src/bios`)

1. **Núcleo, temporizadores**: el temporizador 0 no guarda el bit 2 de TMxCNT_H y no forma cascada; TM0CNT_H lee 0xC3 tras escribir
   0xFFFF. Luego pasar el caso de `irqt.h` (TIMERS) a `SDS_CHECK`.
2. **Núcleo, ranura de GBA**: vacía con el ARM9 dueño (EXMEMCNT bit 7 = 0) lee 0xFFFF (16 bits) y 0xFF (8 bits); con el ARM7 dueño, el ARM9 lee 0. Respetar el bit 7 de EXMEMCNT.
3. **Núcleo, DMA de HBlank**: decidir con las cuentas del log de `dma` (grupo HBLANK) si hay más de una transferencia por línea
   o si corre en el VBlank, y ajustarlo; hoy hace una por línea visible.
4. **Núcleo, SQRTCNT** (opcional): el bit de ocupado dura 13 ciclos; la lectura inmediata puede verlo ya bajo.
5. **HLE de la BIOS, SWI 0x13**: tomar r2 como búfer de 0x200 bytes para el árbol y llamar a Close; la ROM `bios13` lo comprueba.
6. **HLE de la BIOS, SWI 0x17**: no implementarla (en la DS es una excepción de instrucción indefinida).
7. **Intérprete, MSR con T**: impredecible en el manual; si se quiere fidelidad, un `msr cpsr_c` que ponga T cambia a Thumb.
8. Seguir con la documentación donde melonDS se equivoca y el hardware coincide con ella: FIFO de IPC vacía, TMxCNT_H/SQRTCNT con
   bits sin uso, el DMA de 14 bits del ARM7 y el Halt del ARM7 que conserva los registros.

## Escritura del log en el R4 (historial)

- Hasta `ccd1a17` el log solo llegaba a la SD tras pulsar START (`exit(0)`); al quitar `exit` el archivo no cambiaba aunque `fclose`
  y `fsync` devolvían 0. Hipótesis (sin confirmar): el DLDI del R4 solo confirma las escrituras al cerrarse el dispositivo o con
  las IRQ habilitadas, que es lo que hacía `exit`.
- Cambios para probarlo: `fatInit(1)` (caché de páginas mínima), IME/IE como los dejó libnds y bit I del CPSR limpio al escribir,
  `shutdown()` del DLDI al terminar, archivo testigo `resultados/_arranque-<rom>-NNN.txt` escrito al arrancar y verificación tras cerrar
  (reabrir, tamaño y marca `=== fin corrida N ===`); la pantalla muestra `SD OK T=n A=+` o `SD ERR ...`.
- Interpretación: `A=+` y `SD ERR` = el testigo se escribió al arrancar pero el log final no (estado de las pruebas);
  `A=-` y `SD ERR` = la escritura no funciona sin `exit` en ese flashcart; `SD OK` y el archivo sin cambios en la SD = el flashcart
  confirma la escritura solo al apagarse o reiniciarse.

## Escritura del log en el R4

- Hasta `ccd1a17` el log solo llegaba a la SD tras pulsar START (`exit(0)`). En el R4 **crear** archivos funciona y **agregar** a uno
  existente no. Solución: un archivo por corrida (`<rom>-NNN.txt`, primer número libre) más el testigo `_arranque-<rom>-NNN.txt`, con
  verificación tras reabrir (tamaño y marca final).

## Ranura de GBA

Cinco corridas de `mem` (ROMs `cc89405`) en una DS Lite: flashcard de GBA, ranura vacía, vacía con la tapa de fábrica, flashcard en modo
ROM directa y un cartucho original de GBA. Solo conclusiones (no se copian bytes de cartuchos al repositorio):

- Al arrancar la ROM el ARM7 es dueño de la ranura (EXMEMCNT = 0xE080): el ARM9 lee todo 0 en ROM y SRAM, también con cartucho.
  Tras poner el dueño al ARM9 (bit 7 = 0) y la temporización lenta (EXMEMCNT = 0xE00F) la ranura se lee bien.
- Con ranura vacía el bus abierto da 0xFFFF en 16 bits y 0xFF en 8 bits (también con la tapa de fábrica puesta, que no influye).
  GBATEK/SalsaDS aciertan; melonDS (0xFE08 en 16 bits) no.
- Flashcard, flashcard en modo ROM y cartucho original: cabecera de GBA válida (0x96 en 0x080000B2). Con cartucho o flashcard se lee
  su contenido, así que esas lecturas no son divergencias (la ROM `mem` solo las anota con la ranura vacía; la divergencia que salía
  con el cartucho original era simplemente su contenido).
