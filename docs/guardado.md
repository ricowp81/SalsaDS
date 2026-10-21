# Chip de guardado del cartucho

Cada juego de DS espera un chip de guardado concreto (EEPROM, FRAM o Flash) y se queja o no guarda si el emulador le contesta
otro. SalsaDS elige el chip solo; esta página dice cómo, qué esperar y cómo forzarlo a mano. El código está en `src/core/save.c`.

## Quién decide el chip (en este orden)

1. **`guardado.cfg`** del usuario, si tiene una línea de este juego («cfg»).
2. **El tamaño del `.sav`** que ya exista en `/dev_hdd0/salsads/saves/<código>.sav` («.sav»). Sus tamaños válidos son 512 B,
   8 KB, 32 KB, 64 KB, 128 KB, 256 KB, 512 KB y 1 MB. Una excepción: un `.sav` de 64 KB para un juego que la tabla da como Flash
   es el que dejó el chip por omisión de versiones anteriores; se conserva su contenido, se pasa al Flash correcto y se reescribe
   con el tamaño bueno (el log lo dice).
3. **La tabla por código de juego** («tabla»): los 3 primeros caracteres del código de la cabecera (4 si la región cambia el
   chip, como `ASMK`, Super Mario 64 DS coreano). Metroid Prime Hunters (`AMH`) usa Flash de 256 KB. Hoy cubre los juegos de Pokémon de Nintendo DS (Diamante, Perla, Platino, HeartGold,
   SoulSilver, Negro, Blanco, Negro 2, Blanco 2: Flash de 512 KB), Mario Kart DS (Flash de 256 KB), New Super Mario Bros. y
   Super Mario 64 DS (EEPROM de 8 KB), y los pocos que usan dirección de un byte (EEPROM de 512 B).
4. **Detección automática** («detectado»), para lo que no está en la tabla.
5. Si el juego no ha dejado claro nada: EEPROM de 64 KB provisional («defecto»), que sirve a cualquier EEPROM de hasta 64 KB
   (una de 8 KB funciona igual como de 64 KB).

El log (`nucleo.log`) lo dice siempre en una línea: `guardado: chip flash512k de 524288 bytes (tabla|detectado|cfg|.sav|defecto)`.

## Detección automática

Solo actúa mientras el chip es el provisional y antes de que el juego escriba nada que dependa de él; cuando una conversación
deja clara la clase, cambia de chip y repite esa conversación sobre el chip nuevo, de modo que lo escrito cae donde debe. Las reglas
son conservadoras: solo lo que no admite otra lectura.

| Lo que hace el juego | Chip |
|---|---|
| RDID (9Fh), borrar página (DBh), sector (D8h) o todo (C7h) | Flash de 512 KB (el ID que se devuelve es `20 40 13`) |
| lectura 03h o 0Bh de solo 2 bytes tras la orden | EEPROM de 512 B |
| solo el opcode 0Ah o 0Bh | Ambiguo: existe tanto en EEPROM pequeña como en Flash; no fija el tipo |
| escritura de exactamente 130 bytes tras la orden (dirección de 2 y página de 128) | EEPROM de 64 KB (confirma el provisional) |
| escritura de 131 a 259 bytes (dirección de 3 y página de 256) | EEPROM de 128 KB |
| escritura de más de 259 bytes (sin límite de página) | FRAM de 32 KB |

Límites que conviene saber: un Flash de 256 KB o de 1 MB solo se distingue del de 512 KB por tabla, `guardado.cfg` o `.sav` (el juego compara
el ID del chip con el que espera); EEPROM de 8 KB y de 64 KB se parecen y se queda el provisional; una FRAM con escrituras cortas se
queda como EEPROM de 64 KB (que para el juego es equivalente). Si algo de esto falla en un juego concreto, `guardado.cfg` lo arregla.

Los Flash de 2 MB (si algún juego los usa) **no caben**: `NDS_SAVE_MAX` es 1 MB (8 Mbit, el mayor que se conoce en cartuchos
comunes); un `.sav` mayor se ignora con un aviso.

## Forzar el chip a mano: `guardado.cfg`

Archivo de texto `/dev_hdd0/salsads/guardado.cfg`, una línea por juego, `CODIGO=tipo`:

```
# Pokémon Blanco 2 (cualquier región)
IRD=flash512k
# Solo la versión japonesa de un juego
ABCJ=eeprom8k
```

- `CODIGO` son 3 o 4 letras del código de juego (el nombre del `.sav` en `saves/`; mayúsculas o minúsculas). La de 4 gana a la de 3.
- `tipo`: `eeprom512b`, `eeprom8k`, `eeprom64k`, `eeprom128k`, `fram32k`, `flash256k`, `flash512k` o `flash1m`.
- `#` comenta. Una línea con un tipo desconocido se avisa en el log y se ignora.
- Gana a la tabla y al tamaño del `.sav`: un `.sav` de otro tamaño se corta o se rellena con FFh para ajustarse al chip.

Si ya existe un `.sav` equivocado, consérvalo como respaldo antes de empezar de cero. No se recuperan datos perdidos rellenándolo con FFh.

### Metroid Hunters y los archivos antiguos de 512 bytes

La detección anterior confundía el opcode 0Ah de Flash con la dirección alta de una EEPROM pequeña. Al reabrir, los 512 bytes del archivo
volvían a imponer el chip incorrecto y el juego fallaba al comprobar sus datos. `nds_save_attach` devuelve `NDS_SAVE_INCOMPATIBLE` para ese
archivo de `AMH` seleccionado por tabla, sin importar su contenido ni modificar el chip correcto. La configuración explícita del usuario sigue teniendo prioridad.

Además, 0Ah no puede borrar los bytes vecinos de una escritura parcial: M45PE20 conserva las posiciones no dirigidas, a diferencia de un
borrado explícito DBh. La implementación anterior destruía partes de la partida al actualizar otros bytes de la misma página; se corrige también
esa operación. No cambia PAGE PROGRAM (02h, AND), el borrado explícito ni la vuelta dentro de cada página.

La aplicación conserva `<código>.sav` intacto y abre o crea `<código>.flash256k.sav` por separado. Empieza una memoria Flash nueva: no afirma
recuperar una campaña del archivo incorrecto. Si el archivo alternativo existe pero es ilegible o no mide 262144 bytes, bloquea las escrituras para
no destruirlo. El corredor del host rechaza el archivo incompatible con salida 2; hay que elegir otro nombre con `--save`.

## Pruebas

`tests/unit/test_save.c` (en `tools/build.sh test-host`, LE, BE y con sanitizadores): la tabla, secuencias SPI sintéticas de EEPROM
de 0,5/8/64/128 KB, FRAM y Flash de 256 KB/512 KB/1 MB, `guardado.cfg`, el `.sav` y el determinismo. En el host,
`salsads-cpurun --save-cfg ARCHIVO` aplica un `guardado.cfg`.

`make -C tests/guardado-archivos run` prueba las funciones reales de archivos de la aplicación en un directorio temporal: conserva los 512 bytes originales,
crea y reabre la Flash alternativa y bloquea escrituras si esta resulta inválida. Extrae solo la sección de guardado; no simula RSX, mandos ni el cierre de la PS3.
