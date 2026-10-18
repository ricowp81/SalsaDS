/*
 * SalsaDS — mezclador de sonido de la DS (Fase 2.2): los 16 canales del ARM7, SOUNDCNT/SOUNDBIAS, la captura y el
 * remuestreo a la salida de la PS3. Código portable: corre en el host, en el PPE y en un SPE (src/spu/snd).
 *
 * Modelo (comprobado muestra a muestra contra melonDS 1.1 con la ROM `snd` y tools/snd-compare.py):
 *  - El mezclador saca una muestra cada 1024 ciclos del ARM7 (33 513 982 Hz / 1024 = 32 728,5 Hz): un «tick».
 *  - Cada canal tiene un contador de temporizador que sube 512 unidades por tick (16,757 MHz); al llegar a 0x10000
 *    pasa a TMR (y toma una muestra: PCM8/16/ADPCM, un paso de PSG o un desplazamiento del ruido). La muestra se
 *    mantiene hasta el siguiente desbordamiento.
 *  - Una escritura a un registro hecha en el intervalo del tick I se ve a partir del tick I + 1 (el inicio de un canal
 *    o de una captura se enganchan en el límite del tick); el canal empieza en la posición -3 (tres desbordamientos
 *    antes de la muestra 0; el PSG y el ruido empiezan en 0).
 *  - Volumen y balance: izquierda = (m * v * (128 - p)) >> (14 + d) y derecha = (m * v * p) >> (14 + d), con v = 128 si
 *    VOL = 127, p = 128 si PAN = 127 y d = {0, 1, 2, 4}: un solo desplazamiento al final (con redondeos intermedios
 *    cambian muestras sueltas respecto de melonDS). La suma de los canales se
 *    recorta a 16 bits: eso es lo que captura SNDCAP. El volumen general (SOUNDCNT) se aplica después, y de ahí sale
 *    el audio (la captura no lo ve).
 *
 * Salida: las muestras de 16 bits a 32,7 kHz se convierten a flotante y se remuestrean a 48 kHz. FIR polifásico
 * de 32 coeficientes (snd_fir.h, de tools/gen-snd-fir.py; pasabajos de 15 kHz): `snd_resample()` (snd_mix.c) es la única
 * función que sabe del filtro; arrastra entre trabajos las últimas 32 muestras nativas por canal (`snd_state.hist`) y
 * `frac`, y la salida va retrasada 16 muestras nativas. La opción `SND_MODE_ORIGINAL` reproduce la
 * cuantización a 10 bits con el sesgo (SOUNDBIAS) de la salida de la DS real.
 */
#ifndef SALSADS_AUDIO_SND_H
#define SALSADS_AUDIO_SND_H

#include <stdint.h>

#define SND_NCH 16
#define SND_TICK_CYCLES 1024u        /* ciclos del ARM7 por muestra del mezclador */
#define SND_TICK_ARM9 2048u          /* ... en ciclos del ARM9 (el ARM7 va a la mitad) */
#define SND_RATE_NATIVE 32728.498f   /* 33 513 982 / 1024 */
#define SND_RATE_OUT 48000.0f
#define SND_LINE 128                 /* bytes de la línea de caché de cada canal */
#define SND_LOG_MAX 2048             /* escrituras a registros por trabajo */
#define SND_TICKS_MAX 640            /* ticks por trabajo (un cuadro lleva 547; sobra para retrasos) */
#define SND_OUT_MAX 1024             /* muestras estéreo de salida por trabajo */
#define SND_HIST 32

/* modos (snd_state.mode) */
#define SND_MODE_ORIGINAL 1u         /* cuantizar a 10 bits con SOUNDBIAS, como la salida de la DS */

/* Una escritura a un registro de sonido (media palabra): `off` es el desplazamiento desde 0x04000400. */
typedef struct snd_ent {
	uint32_t tick;                   /* intervalo de tick en que se hizo (instante del ARM7 / 1024) */
	uint16_t off, val, mask, pad;
	uint32_t pad2;
} snd_ent;

typedef struct snd_chan {
	uint32_t cnt;                    /* SOUNDxCNT sin el bit 31 */
	uint32_t sad, len;               /* 27 y 22 bits */
	uint16_t tmr, pnt;
	uint8_t on, hdr, pad[2];
	int32_t pos;                     /* posición en muestras (ADPCM: nibbles; PSG: paso 0-7) */
	uint32_t tcnt;                   /* contador del temporizador (sube hasta 0x10000) */
	int32_t out;                     /* muestra actual, 16 bits con signo */
	int32_t adv, adi, alv, ali;      /* ADPCM: valor e índice, y los guardados en el punto de bucle */
	uint32_t lfsr;                   /* ruido */
	uint32_t tag;                    /* dirección de la línea de caché, o 0xFFFFFFFF */
	uint32_t pad3[3];
	uint8_t line[SND_LINE] __attribute__((aligned(16)));
} snd_chan;

typedef struct snd_cap {
	uint8_t cnt, on, pad[2];
	uint32_t dad, len;               /* destino y longitud (palabras) tal como se escribieron */
	uint32_t pos;                    /* bytes escritos */
	uint32_t base, bytes;            /* dirección y tamaño en uso (los que valían al empezar) */
} snd_cap;

typedef struct snd_state {
	snd_chan ch[SND_NCH];
	uint16_t soundcnt, bias;
	snd_cap cap[2];
	uint64_t tick;                   /* ticks ya generados */
	/* remuestreo */
	float hist[2][SND_HIST];         /* últimas SND_HIST muestras nativas de cada canal (la ventana del FIR) */
	float frac;                      /* fase dentro de la muestra nativa, [0, 1) */
	float ratio_adj;                 /* corrección de la razón de remuestreo (control de deriva), 0 = ninguna */
	uint32_t mode;
	/* resultados del último trabajo */
	uint32_t busy;                   /* bit c: canal c encendido; bit 16 y 17: captura 0 y 1 */
	uint32_t peak;                   /* pico absoluto de lo mezclado (0-32768), para el log */
	uint32_t nout;                   /* muestras estéreo que sacó */
	uint32_t ticks_run;
	uint32_t log_applied;
} snd_state;

/* El bus del sonido (el ARM7): traer una línea de SND_LINE bytes alineada y escribir los de la captura. */
typedef struct snd_bus {
	void *ctx;
	void (*fetch)(void *ctx, uint32_t addr, uint8_t *dst);                  /* addr alineada a SND_LINE */
	void (*store)(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n); /* captura (n = 1, 2 o 4... bytes) */
} snd_bus;

void snd_reset(snd_state *st);

/*
 * Genera `nticks` muestras del mezclador aplicando las escrituras `log` (en orden de `tick`; se aplican las de
 * `tick < T` antes de generar la muestra T). Devuelve cuántas entradas consumió (las demás esperan al siguiente).
 *   nat      si no es NULL: 2 * nticks muestras de 16 bits (izquierda, derecha) del mezclador con el volumen
 *            general aplicado, a 32,7 kHz
 *   out      si no es NULL: salida estéreo en flotante a 48 kHz (intercalada), hasta out_max pares; `*out_n` dice cuántos
 */
uint32_t snd_run(snd_state *st, const snd_ent *log, uint32_t nlog, uint32_t nticks, const snd_bus *bus,
		 int16_t *nat, float *out, uint32_t out_max, uint32_t *out_n);

#endif
