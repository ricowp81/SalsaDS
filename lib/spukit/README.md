# spukit

> **English:** Batch jobs on the PlayStation 3's SPEs (a work queue in main memory with a mailbox doorbell, persistent workers, and LZ block compression on the PPE, the host and an SPE), for homebrew built with [PSL1GHT](https://github.com/ps3dev/PSL1GHT). MIT licensed. It was born inside [Salsa64](https://github.com/ricowp81/Salsa64), a Nintendo 64 emulator for the PS3, but knows nothing about any emulated system. The documentation below is in Spanish; the API is plain C. Third-party material: [THIRD_PARTY.md](THIRD_PARTY.md).
>
> Repositorio propio: [github.com/ricowp81/spukit](https://github.com/ricowp81/spukit). Salsa64 lo lleva en `lib/spukit` (git subtree) y los cambios se sincronizan en los dos sentidos.

Trabajos por lotes en los SPEs de la PlayStation 3 con
[PSL1GHT](https://github.com/ps3dev/PSL1GHT). Nació dentro de Salsa64 (un
emulador de N64), pero no sabe nada de ningún sistema emulado: sirve a
cualquier programa homebrew que quiera mandar trabajo grande a un SPE una
vez por cuadro (una lista de comandos, un búfer de audio, un filtro) sin
diálogos finos con el PPE. Licencia **MIT** (`LICENSE`), como `lib/rsxkit`,
para que también la puedan usar proyectos que no son GPL.

## Modelo

- Un **trabajador** es un hilo SPU persistente en su propio grupo (ocupa un
  SPE mientras vive). Duerme en su mailbox de entrada.
- El PPE **publica** trabajos en una cola en memoria principal: un
  descriptor de 128 bytes (`kind` y hasta 112 bytes de parámetros, que
  normalmente son direcciones de búferes) y un **timbre** (escribir en la
  mailbox del SPU). Publicar no espera al SPU.
- El SPU atiende todo lo publicado, en orden, y por cada trabajo escribe un
  resultado de 128 bytes (código de error, duración en ticks del
  decrementador y hasta 112 bytes propios) y después el número del último
  trabajo terminado. Antes de eso espera todas sus transferencias DMA: lo
  que el trabajo escribió en memoria principal es visible para el PPE en
  cuanto ve el trabajo como terminado.
- El PPE consulta (`spukit_done`) o espera (`spukit_wait`) cuando necesita
  el resultado.

La cola tiene `SPUKIT_QUEUE_LEN` (8) lugares; si está llena, `spukit_post`
espera a que se libere uno. El resultado de un trabajo vale hasta que se
publiquen 8 más en el mismo trabajador.

Cada publicación toca el timbre (una llamada al sistema). Un timbre de más
solo despierta al SPU para encontrar la cola vacía; uno perdido (mailbox
llena) no importa, porque los que ya esperan en la mailbox lo despiertan y
el SPU siempre relee la cola completa.

## Compilar

```bash
make                    # → build/libspukit.a (lado PPE)
make OUT=/otra/ruta
```

El lado SPU es solo cabecera. Para usarla: `-I<spukit>/include` en el PPE
y en el SPU, y enlazar `libspukit.a` en el PPE antes de `-llv2`.

## Uso

PPE:

```c
#include <spukit/spukit.h>
#include "spu_prog_bin.h"          /* la imagen ELF del SPU, embebida */

spukit_init(6);
spukit_worker *w = spukit_worker_start(spu_prog_bin, "mi_trabajo", arg);

struct params p = { .src = (uintptr_t)src, .dst = (uintptr_t)dst, .n = n };
uint32_t seq = spukit_post(w, MI_TRABAJO, &p, sizeof p);
/* ... el PPE hace otra cosa ... */
const spukit_result *r = spukit_wait(w, seq);   /* r->error, r->ticks, r->data */

spukit_worker_stop(w);
```

SPU:

```c
#include <spukit/spu.h>
#include <sys/spu_thread.h>

static int handler(void *ctx, const spukit_job *job, spukit_result *res)
{
	const struct params *p = (const void *)job->data;
	/* DMA de entrada, cálculo, DMA de salida (etiquetas 0–30) */
	return 0;  /* → res->error */
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
```

## API (PPE)

| Función | Qué hace |
|---|---|
| `int spukit_init(uint32_t spus)` | Reserva `spus` SPEs para hilos SPU (`sysSpuInitialize`). Una vez por proceso. |
| `spukit_worker *spukit_worker_start(elf, name, arg)` | Crea un grupo de un hilo con la imagen ELF embebida y lo arranca; el programa recibe `main(ctrl_ea, arg, 0, 0)`. `NULL` si falla (`spukit_last_error`). Hasta 6 trabajadores. |
| `uint32_t spukit_post(w, kind, data, size)` | Publica un trabajo y toca el timbre; devuelve su número (desde 1). |
| `int spukit_done(w, seq)` | ¿Terminó? No bloquea. |
| `const spukit_result *spukit_wait(w, seq)` | Espera y devuelve el resultado: gira ~50 µs con prioridad baja del hilo de hardware (deja el núcleo al otro hilo del PPE) y después duerme de a 20 µs. |
| `int spukit_worker_stop(w)` | Pide salir al vaciar la cola, espera al hilo y libera todo; devuelve su estado de salida. |
| `void spukit_get_stats(w, out)` | Publicados, timbres, despertares del SPU, esperas y tiempo esperando. |
| `uint64_t spukit_tick_hz(void)` | Frecuencia de los ticks de `spukit_result` (timebase: 79,8 MHz en la PS3). |

## API (SPU, `spukit/spu.h`)

| Función | Qué hace |
|---|---|
| `spukit_run(ctrl_ea, handler, ctx)` | Bucle del trabajador; vuelve cuando el PPE pide salir. |
| `spukit_dma_get / spukit_dma_put(ls, ea, size, tag)` | DMA de cualquier tamaño múltiplo de 16 (en trozos de 16 KB). No espera. |
| `spukit_dma_wait(tag_mask)` | Espera las etiquetas. |
| `spukit_get_unaligned(buf, ea, size, tag)` | Trae `size` bytes desde una dirección no alineada (copia el tramo alineado a 16 en `buf`, de `size + 32` bytes) y devuelve el puntero al primer byte. |
| `spukit_ticks()` | Decrementador (cuenta hacia abajo). |

La etiqueta DMA 31 es del bucle; el manejador usa las demás.

## Compresión LZ (`spukit/lz.h`, `lzstream.h`)

Una pieza aparte que se apoya en lo anterior: compresión sin pérdida de
bloques de 64 KB en el formato de bloque de LZ4 (cualquier descompresor de
LZ4 los lee). Sirve para escribir a disco grandes zonas de memoria (estados
guardados de un emulador, cachés) sin cargar el PPE.

- `spukit/lz.h` (solo cabecera, C99): `spukit_lz_compress` y
  `spukit_lz_decompress` para un bloque, y `spukit_lz_frame*` para el
  encabezado de 4 bytes de cada bloque en un archivo (bit 31: va sin
  comprimir). El mismo código corre en el PPE, en un SPE y en el host, y da
  los mismos bytes. Cada bloque es independiente. La descompresión valida
  todos los límites.
- `spu/lz.c`: el programa SPU (un trabajador que comprime un bloque por
  trabajo, ~150 KB de local store). Se embebe como cualquier ELF de SPU.
- `spukit/lzstream.h`: lado PPE. `spukit_lzs_open(elf)` reserva un anillo de
  ocho bloques y arranca el trabajador (con `NULL`, o si no arranca, se
  comprime en el PPE); `spukit_lzs_run(z, src, len, sink, ctx, &stats)`
  parte el búfer en bloques, el SPE comprime hasta ocho por delante y cada
  bloque, con su encabezado, llega en orden a `sink` (que suele escribir a
  disco desde un hilo de baja prioridad).

```c
spukit_lzs *z = spukit_lzs_open(spu_lz_bin);
spukit_lzs_run(z, buf, len, escribir, ctx, &stats);   // buf alineado a 16
```

Mide: `stats.spu_ticks` (con `spukit_tick_hz()`), `sink_us`, `wall_us`.

## Recursos que usa

- Un grupo de hilos SPU de un hilo por trabajador (prioridad 100).
- Por trabajador, ~2,3 KB de memoria principal (bloque de control) y
  ~0,5 KB de local store (descriptor, resultado, cabeza y estado).

## Origen del código

Código propio. Usa solo la API de PSL1GHT (MIT). No contiene código de
proyectos GPL ni de proyectos sin licencia clara. El compresor sigue la
especificación pública del formato de bloque de LZ4 (BSD-2-Clause); no usa
fuentes de LZ4.
