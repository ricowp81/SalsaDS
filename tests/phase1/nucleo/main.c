/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — «SalsaDS núcleo» (Fases 1 y 2.1): el núcleo de la DS en la PS3, con las dos pantallas.
 *
 * Corre la máquina de src/core (ARM9 y ARM7 con intérpretes, mapa de memoria, IRQ, temporizadores, DMA, IPC,
 * cartucho, SPI, BIOS en HLE) sobre una ROM y muestra las dos pantallas con la salida de la Fase 0.6 (rsxkit).
 * Los motores 2D A y B (Fase 2.1) los dibuja un SPE por pantalla (src/spu/gpu2d, spukit): al cerrar cada cuadro
 * (línea 192) el núcleo entrega la instantánea al callback `vid_submit`, que publica los dos trabajos con la salida
 * directa en la textura del RSX; mientras el PPE emula el cuadro siguiente los SPE componen, y el cuadro se dibuja
 * al terminar la emulación del siguiente (un cuadro de latencia). Si los SPE no arrancan, el PPE dibuja con el
 * compositor del host.
 *
 * Selector de ROM: al abrir la aplicación lista los *.nds de /dev_hdd0/salsads/ (por nombre; cruceta o stick izquierdo para
 * moverse, L1/R1 o izquierda/derecha saltan de página, X abre, O sale de la aplicación). Recuerda la última elegida en
 * /dev_hdd0/salsads/ultima-rom.txt. Si no hay ninguna, ofrece la ROM de carga propia (tests/roms/src/carga: las dos CPUs
 * ocupadas sin parar, con sus resultados comprobados), embebida en el ejecutable. Dentro del juego, SELECT + START del DS3
 * mantenidos 1 s cierran el juego y vuelven al selector. Con la prueba automática (RPCS3) no hay selector: corre rom.nds si
 * existe (o la ROM de carga) tantas veces como diga ciclos.txt, abriendo y cerrando el juego, y comprueba que la memoria no crece.
 *
 * Memoria entre juegos (docs/memory-budget.md): lo grande de tamaño fijo se reserva una vez al arrancar (la estructura `nds`,
 * las texturas del RSX, los seis SPE, la caché de código y las tablas del recompilador) y se reutiliza; lo que depende de la ROM
 * (la ROM entera o la caché de páginas del modo por fragmentos) sale de páginas de 1 MB del sistema, fuera del heap, y se
 * devuelve al cerrar. Cada cierre escribe en el log la memoria libre y la diferencia frente al cierre anterior.
 *
 * Guardado: el chip de guardado del cartucho (AUXSPI) se guarda en /dev_hdd0/salsads/saves/<código de juego>.sav
 * (uno por juego); se carga al arrancar si existe y se escribe a los pocos segundos de que el juego escriba y al salir.
 *
 * Entrada (src/frontend/dspad.h, /dev_hdd0/salsads/controles.cfg): el DS3 a los botones de la DS; el puntero del
 * táctil con el stick derecho (R2 toca); L2 cambia la distribución de las pantallas; L3 + R3 muestra u oculta la
 * línea de estado. Se emula un cuadro de la DS por cada cuadro del RSX (60 Hz).
 *
 * Log: /dev_hdd0/salsads/nucleo.log, con la versión (commit) y el APPID en la primera línea. Cada ~2 s escribe
 * tres líneas DATO: el tiempo de emulación por cuadro y las instrucciones por cuadro de cada CPU; el desglose del
 * cuadro (ARM9, ARM7, eventos, planificador, pantallas, conversión, dibujo, esperas del RSX) y los MIPS de cada CPU.
 * Los cuadros pares se miden sin reloj de perfil dentro del planificador y los impares con él (timebase), así el
 * tiempo de emulación «limpio» no lleva el costo de medirlo.
 *
 * Con el archivo de prueba automática (RPCS3) corre 150 cuadros con entrada sintética, comprueba que los dos
 * procesadores corrieron y que la imagen de la demostración es la esperada, y sale.
 */
#include <dirent.h>
#include <io/pad.h>
#include <malloc.h>
#include <math.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include "core/nds.h"
#include "frontend/dspad.h"
#include "spu/gpu2d/job.h"
#include "spu/snd/job.h"
#include "spu/raster3d/job.h"
#include "spu/geom/job.h"
#include "gpu3d/gx_selftest.h"
#include "gpu3d/r3d_scene.h"
#include "spukit/spukit.h"
#include "spu_gpu2d_bin.h"
#include "spu_snd_bin.h"
#include "spu_raster3d_bin.h"
#include "spu_geom_bin.h"
#include "frontend/layout.h"
#include "platform/ps3/audio_out.h"
#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "rsxkit/rsxkit.h"
#include "rsxkit/text.h"
#include "rsxkit/ui.h"

/* La geometría se ejecuta en el sexto SPE por omisión (el SPE vectorizado tarda ~7,9 ms por cuadro en Mario Kart DS contra
 * ~2,8 del PPE, pero libera al hilo principal de la geometría; antes de la vectorización el SPE escalar tardaba 35 ms).
 * Con este archivo vuelve al PPE. */
#define GEOM_PPE_FLAG SALSADS_DATA_DIR "/geometria-ppe"
/* Con este archivo las bandas del render 3D vuelven al reparto alterno (u, u+2, ...) en lugar del reparto por costo. */
#define R3D_ALT_FLAG SALSADS_DATA_DIR "/reparto-alterno"
/* Rutas cortas de la FIFO de geometría (rama gx-rapido; docs/fase4.md, «Cuarta ronda»): ráfagas de la DMA de modo 7 y escrituras de
 * la CPU a GXFIFO y a los puertos de órdenes sin pasar por el bus (con el recompilador, tampoco por la ruta lenta general). Con este
 * archivo vuelve a la forma anterior, para comparar en la misma escena. */
#define GX_LENTO_FLAG SALSADS_DATA_DIR "/gx-lento"

#ifdef NUCLEO_JIT
/* «SalsaDS JIT núcleo» (Fase 4): el ARM9 con el recompilador (src/jit/arec*); caché de código y prueba con respaldo de la
 * Fase 0.7 (tests/phase0/jit-arm/rec_cache.c). Archivos: NOJIT_FLAG fuerza el intérprete; JIT_MARKER marca la prueba y el
 * uso del código traducido (si la aplicación muere con la marca puesta, el siguiente arranque de esta versión no usa el JIT). */
#include "jit/arec.h"
#include "rec_cache.h"
#define NOJIT_FLAG SALSADS_DATA_DIR "/nucleo-jit-nojit"
/* Con este archivo solo el ARM7 vuelve al intérprete (el ARM9 sigue con el recompilador). El ARM7 comparte la marca de seguridad
 * del recompilador (JIT_MARKER): si la aplicación muere con código traducido de cualquiera de las dos CPUs, el siguiente arranque
 * de esta versión usa el intérprete en las dos. */
#define ARM7_NOJIT_FLAG SALSADS_DATA_DIR "/arm7-nojit"
#define JIT_MARKER SALSADS_DATA_DIR "/nucleo-jit.marker"
/* Caché del recompilador pedida a PS3MAPI (fase52, experimental): solo con este archivo; si no, la de 4 MB del segmento
 * de texto. Con HEN 3.3.0 funciona apartando las páginas (rec_cache.c), pero en NSMB no ganó nada (los vaciados no eran
 * por falta de espacio) y las llamadas a C por CTR cuestan ~6 % del ARM9. Su propia marca por si cuelga la consola. */
#define JIT_MAPI_MARKER SALSADS_DATA_DIR "/nucleo-jit-ps3mapi.marker"
#define JIT_MAPI_FLAG SALSADS_DATA_DIR "/jit-ps3mapi"
/* Tramo del planificador con el recompilador (ciclos del ARM9 entre los cambios de CPU; el intérprete usa 128). Cada
 * tramo es una entrada al código generado, y lo que cuesta en la PS3 es el cambio de CPU (cachés), no las instrucciones:
 * con tramos más largos hay menos. El número en JIT_QUANTUM_FILE (de 16 a 20000) lo cambia sin compilar otra vez. */
#define JIT_QUANTUM 1024u
#define JIT_QUANTUM_FILE SALSADS_DATA_DIR "/nucleo-jit-tramo"
/* Interruptores para medir A/B en la consola las mejoras del recompilador (rama arm9-perf; docs/fase4.md, «Tercera ronda»): cada
 * archivo vuelve a la forma anterior de una (AREC_OPT_*). JIT_TIMES_FLAG mide dónde se va el tiempo del ARM9 (DATO jit-tiempo). */
static const struct {
	const char *file;
	unsigned opt;
	const char *what;
} jit_opt_files[] = {
	{ SALSADS_DATA_DIR "/jit-banderas-memoria", AREC_OPT_FLAGS_MEM, "banderas entre bloques en memoria" },
	{ SALSADS_DATA_DIR "/jit-sin-pistas", AREC_OPT_NO_HINTS, "sin pistas estáticas de salto" },
	{ SALSADS_DATA_DIR "/jit-salidas-copiadas", AREC_OPT_JL_INLINE, "búsqueda de destinos copiada en cada salida" },
	{ SALSADS_DATA_DIR "/jit-sin-ventanas", AREC_OPT_NO_SPLIT, "código frío junto al caliente" },
	{ SALSADS_DATA_DIR "/jit-sin-esperas", AREC_OPT_NO_IDLE, "sin saltar los bucles de espera" },
};
#define JIT_TIMES_FLAG SALSADS_DATA_DIR "/jit-tiempos"
static int jit_on;
static int jit_times; /* JIT_TIMES_FLAG: reloj (timebase) en el despachador, los accesos lentos y las llamadas al intérprete */
#endif
#include "carga_bin.h"
#include "../../roms/src/carga/shared.h"
#include "../../roms/src/snd/adpcm.h"
#include "util/endian.h"

#ifndef NUCLEO_VERSION
#define NUCLEO_VERSION "desconocida"
#endif
#ifndef NUCLEO_TITLE
#define NUCLEO_TITLE "SalsaDS núcleo"
#define NUCLEO_APPID "SLDS00008"
#endif
#define ROM_PATH SALSADS_DATA_DIR "/rom.nds"
#define CFG_PATH SALSADS_DATA_DIR "/controles.cfg"
#define AUDIO_CFG_PATH SALSADS_DATA_DIR "/audio.cfg"

SYS_PROCESS_PARAM(1001, 0x100000);

#define TEX_W 256
#define TEX_H 192
#define TEX_PITCH 512
#define TEX_BYTES (TEX_PITCH * TEX_H)
#define UI_W 1280.0f
#define UI_H 720.0f
#define CLEAR_RGB 0x101010u
#define AUTOTEST_FRAMES 150
#define STATS_FRAMES 120
#define SAVE_DIR SALSADS_DATA_DIR "/saves"
#define SAVE_CFG_PATH SALSADS_DATA_DIR "/guardado.cfg" /* «CODIGO=tipo»: fuerza el chip de guardado de un juego (docs/guardado.md) */
#define SAVE_FLUSH_FRAMES 120

static volatile int quit;
static int autotest, failures;

static struct nds *nds;

static volatile uint32_t vid_frames_seen; /* copia de frame_no para el hilo vigía */
/* Guardia de la caché de PS3MAPI: las dos primeras palabras de las rutinas comunes del recompilador. Si cambian, otra
 * reserva recibió la misma dirección y reemplazó la página: se apaga el JIT (el intérprete sigue) en vez de saltar a
 * memoria ajena. */
static const volatile uint32_t *jit_guard_ptr;
static uint32_t jit_guard_val[2];
#ifdef NUCLEO_JIT
/* Hilo vigía del JIT con la caché de PS3MAPI (diagnóstico de la fase52: con ella el juego se quedaba en negro sin
 * dejar rastro). Cada medio segundo, durante 40 s, anota los contadores del recompilador y el último bloque al que
 * entró; si el hilo principal se queda trabado dentro del código generado, las últimas líneas dicen dónde. */
static void jit_watch_thread(void *arg)
{
	(void)arg;
	for (int k = 0; k < 80 && !quit; k++) {
		sysUsleep(500000);
		if (!nds || !nds->jit9)
			continue;
		{
			const struct arec_pub *pb = (const struct arec_pub *)(const void *)nds->jit9;

			if (jit_guard_ptr)
				salsads_log("vigía jit %d: rutinas [0] 0x%08x [1] 0x%08x [0x77] 0x%08x", k, (unsigned)jit_guard_ptr[0],
					    (unsigned)jit_guard_ptr[1], (unsigned)jit_guard_ptr[0x77]);
			salsads_log("vigía jit %d: entradas %llu, bloques %llu, instr. traducidas %llu, intérprete %llu, "
				    "llamadas %llu, lentas %llu/%llu; último bloque pc 0x%08x en 0x%llx; ARM9 r15 0x%08x; cuadros %u",
				    k, (unsigned long long)pb->st.enters, (unsigned long long)pb->st.blocks_made,
				    (unsigned long long)pb->st.guest_made, (unsigned long long)pb->st.interp_steps,
				    (unsigned long long)pb->st.callouts, (unsigned long long)pb->st.slow_rd,
				    (unsigned long long)pb->st.slow_wr, (unsigned)pb->last_pc, (unsigned long long)pb->last_code,
				    (unsigned)nds->a9.r[15], (unsigned)vid_frames_seen);
		}
	}
	sysThreadExit(0);
}
#endif
static uint8_t *rom_buf;
static int rom_buf_pages; /* rom_buf sale de las páginas del sistema (big_alloc) o es la cabecera de 4 KB del modo por fragmentos */
static int rom_buf_owned; /* rom_buf se suelta al cerrar (no es la ROM de carga embebida) */
/* ROM por fragmentos (docs/memory-budget.md): si el archivo pasa de ROM_WHOLE_MAX_MB (o existe ROM_PAGED_FLAG) se queda abierto y
 * el núcleo lee páginas de ROM_PAGE_KB bajo demanda; rom_buf pasa a ser solo la cabecera (4 KB). */
#ifndef ROM_WHOLE_MAX_MB
#define ROM_WHOLE_MAX_MB 64
#endif
#ifndef ROM_PAGE_KB
#define ROM_PAGE_KB 256
#endif
#ifndef ROM_CACHE_MB
#define ROM_CACHE_MB 32
#endif
#define ROM_PAGED_FLAG SALSADS_DATA_DIR "/rom-paged"
static FILE *rom_file;

static unsigned rom_page_shift(void)
{
	unsigned s = 10;

	while ((1u << (s - 10)) < ROM_PAGE_KB)
		s++;
	return s;
}

static int rom_file_rd(void *user, uint64_t off, uint8_t *dst, size_t len)
{
	FILE *f = user;

	if (fseek(f, (long)off, SEEK_SET) != 0)
		return -1;
	return (int)fread(dst, 1, len, f);
}
static size_t rom_len;
static char rom_name[200];

static struct sds_padcfg cfg;
static struct sds_input input;
static struct sds_keys keys;
static struct sds_ds3 ds3;
static int pad_present;

/* Composición 2D en los SPE: un trabajador por pantalla */
static int spu_on;
static spukit_worker *wk[2];
static struct {
	int valid, set, vset; /* juego de texturas, juego de instantáneas del núcleo */
	uint32_t seq[2];
	job_result res[2];
	int done;
	uint32_t wait_us;
} sub, prev;
static uint32_t sub_count;
static uint32_t prof_buf[2][12][STATS_FRAMES]; /* tiempos por etapa de las dos pantallas (ticks del SPE), para la línea DATO spe-etapas */
static uint32_t prof_n;
static int autotest_verify;
static int last_dset = -1; /* juego de texturas del último cuadro dibujado */

static uint8_t *tex_mem;
static uint32_t tex_off;
static uint32_t set_fence[2];
static int set_used[2];
static uint32_t frame_no;

static enum sds_layout layout = SDS_LAYOUT_SIDE;
static int show_osd = 1;
static int cur_w, cur_h;

static inline uint64_t now_us(void)
{
	return sysGetSystemTime();
}

static void fail(const char *what)
{
	failures++;
	salsads_log("FALLO %s", what);
}

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
	(void)param;
	(void)usrdata;
	if (status == SYSUTIL_EXIT_GAME) {
		quit = 1;
#ifdef NUCLEO_JIT
		/* Salir desde el XMB es a propósito: se quita ya la marca del JIT, por si el sistema cierra la aplicación
		 * antes de que game_close la quite (si no, el siguiente arranque se queda con el intérprete). */
		if (jit_on)
			salsads_rec_marker(JIT_MARKER, NUCLEO_VERSION, 0);
#endif
	}
}

static bool wait_hook(void *ctx, const rsxkit_wait_info *info)
{
	(void)ctx;
	(void)info;
	sysUtilCheckCallback();
	return !quit;
}

static void core_log(void *ctx, const char *msg)
{
	(void)ctx;
	salsads_log("núcleo: %s", msg);
}

/* ---- memoria grande fuera del heap ------------------------------------------------------------------------------------
 *
 * La ROM entera (hasta 64 MB) y la caché de páginas del modo por fragmentos (32 MB) se piden al sistema en páginas de 1 MB y se
 * devuelven al cerrar cada juego. En el heap de newlib, cualquier cosa que el primer juego reserve «para siempre» a mitad de
 * la partida queda por encima de la ROM: al soltarla el heap no puede devolver el hueco (el tope está ocupado) y la ROM grande
 * siguiente ya no cabe aunque sobre memoria (la fuga entre juegos de Salsa64). Las páginas del sistema no dependen de eso. */
#define BIG_MAX 4
static struct {
	void *p;
	size_t bytes;
} big[BIG_MAX];

static void *big_alloc(size_t len)
{
	const size_t bytes = (len + (1u << 20) - 1) & ~(((size_t)1 << 20) - 1);
	sys_mem_addr_t addr = 0;

	for (int i = 0; i < BIG_MAX; i++)
		if (!big[i].p && sysMemoryAllocate(bytes, SYS_MEMORY_PAGE_SIZE_1M, &addr) == 0) {
			big[i].p = (void *)(uintptr_t)addr;
			big[i].bytes = bytes;
			return big[i].p;
		}
	salsads_log("memoria: sin páginas de 1 MB del sistema para %u MB: va en el heap", (unsigned)(bytes >> 20));
	return memalign(128, len);
}

static void big_free(void *p)
{
	if (!p)
		return;
	for (int i = 0; i < BIG_MAX; i++)
		if (big[i].p == p) {
			sysMemoryFree((sys_mem_addr_t)(uintptr_t)p);
			big[i].p = NULL;
			big[i].bytes = 0;
			return;
		}
	free(p);
}

static size_t big_bytes(void)
{
	size_t t = 0;

	for (int i = 0; i < BIG_MAX; i++)
		t += big[i].bytes;
	return t;
}

/* ---- ROM ------------------------------------------------------------------------------------------- */

/* Abre la ROM `path` (NULL = la ROM de carga embebida). 0 = bien; si falla no deja nada reservado. */
static int rom_open(const char *path)
{
	FILE *f;

	rom_buf = NULL;
	rom_buf_pages = rom_buf_owned = 0;
	rom_len = 0;
	if (!path) {
		rom_buf = (uint8_t *)carga_bin;
		rom_len = carga_bin_size;
		snprintf(rom_name, sizeof rom_name, "carga embebida");
		salsads_log("ROM de carga propia embebida: %u bytes", (unsigned)rom_len);
		return 0;
	}
	f = fopen(path, "rb");
	if (!f) {
		salsads_log("no pude abrir %s", path);
		return -1;
	}
	long n;

	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	struct stat pst;
	const int forced = stat(ROM_PAGED_FLAG, &pst) == 0;

	if (n >= 0x200 && n <= 0x20000000L && (forced || n > (long)ROM_WHOLE_MAX_MB * 1024L * 1024L)) {
		rom_buf = memalign(128, 4096);
		if (rom_buf) {
			memset(rom_buf, 0xFF, 4096);
			if (fread(rom_buf, 1, n < 4096 ? (size_t)n : 4096u, f) >= 0x200) {
				rom_file = f; /* queda abierto hasta rom_close */
				rom_len = (size_t)n;
				rom_buf_owned = 1;
				snprintf(rom_name, sizeof rom_name, "%s", path);
				salsads_log("ROM de %s: %u bytes, por fragmentos (páginas de %u KB, caché de %u MB)%s", path, (unsigned)rom_len,
					    ROM_PAGE_KB, ROM_CACHE_MB, forced ? " [forzado por " ROM_PAGED_FLAG "]" : "");
				return 0;
			}
			free(rom_buf);
			rom_buf = NULL;
		}
		salsads_log("no pude leer %s", path);
	} else if (n >= 0x200 && n <= 0x20000000L) {
		rom_buf = big_alloc((size_t)n);
		if (rom_buf && fread(rom_buf, 1, (size_t)n, f) == (size_t)n) {
			rom_len = (size_t)n;
			rom_buf_owned = rom_buf_pages = 1;
			snprintf(rom_name, sizeof rom_name, "%s", path);
			fclose(f);
			salsads_log("ROM de %s: %u bytes", path, (unsigned)rom_len);
			return 0;
		}
		big_free(rom_buf);
		rom_buf = NULL;
		salsads_log("no pude leer %s (%ld bytes)", path, n);
	} else {
		salsads_log("%s no parece una ROM de la DS (%ld bytes)", path, n);
	}
	fclose(f);
	return -1;
}

/* Suelta lo que dependía de la ROM (el archivo, la ROM entera o su cabecera). */
static void rom_close(void)
{
	if (rom_file) {
		fclose(rom_file);
		rom_file = NULL;
	}
	if (rom_buf_owned) {
		if (rom_buf_pages)
			big_free(rom_buf);
		else
			free(rom_buf);
	}
	rom_buf = NULL;
	rom_buf_owned = rom_buf_pages = 0;
	rom_len = 0;
}

/* ---- pantallas ----------------------------------------------------------------------------------------- */

static uint16_t *tex_px(int set, int screen)
{
	return (uint16_t *)(void *)(tex_mem + (size_t)(set * 2 + screen) * TEX_BYTES);
}

static uint32_t tex_offset(int set, int screen)
{
	return tex_off + (uint32_t)((set * 2 + screen) * TEX_BYTES);
}

static uint32_t vb_offset(int set)
{
	return tex_off + 0x80000u + (uint32_t)set * 0x1000u;
}

static rsxkit_vertex *vb_ptr(int set)
{
	return (rsxkit_vertex *)(void *)(tex_mem + 0x80000u + (size_t)set * 0x1000u);
}

static void make_quad(rsxkit_vertex *v, const struct sds_rect *r)
{
	const float x0 = (float)r->x, y0 = (float)r->y, x1 = (float)(r->x + r->w), y1 = (float)(r->y + r->h);
	static const float uv[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

	for (int i = 0; i < 6; i++) {
		v[i].pos[0] = uv[i][0] ? x1 : x0;
		v[i].pos[1] = uv[i][1] ? y1 : y0;
		v[i].pos[2] = 0.0f;
		v[i].pos[3] = 1.0f;
		v[i].st[0] = uv[i][0];
		v[i].st[1] = uv[i][1];
		v[i].rgba[0] = v[i].rgba[1] = v[i].rgba[2] = v[i].rgba[3] = 255;
		v[i].pad = 0;
	}
}

/* r[0] es la pantalla de arriba; la textura de A (0) o de B (1) según el intercambio de POWCNT1 */
static void draw_screens(int set, const struct sds_rect r[2], int swap)
{
	rsxkit_vertex *vb = vb_ptr(set);

	make_quad(vb, &r[0]);
	make_quad(vb + 6, &r[1]);
	rsxkit_bind_frame((float)cur_w, (float)cur_h);
	rsxkit_vertices(vb_offset(set), RSXKIT_MEM_MAIN);
	for (int s = 0; s < 2; s++) {
		rsxkit_draw d;

		memset(&d, 0, sizeof d);
		d.blend = RSXKIT_BLEND_OFF;
		d.cull = RSXKIT_CULL_NONE;
		d.ntex = 1;
		d.scissor[2] = (float)cur_w;
		d.scissor[3] = (float)cur_h;
		d.tex[0].offset = tex_offset(set, swap ? s : 1 - s);
		d.tex[0].pitch = TEX_PITCH;
		d.tex[0].width = TEX_W;
		d.tex[0].height = TEX_H;
		d.tex[0].location = RSXKIT_MEM_MAIN;
		d.tex[0].format = RSXKIT_RGBA5551;
		d.tex[0].wrap_s = d.tex[0].wrap_t = RSXKIT_WRAP_CLAMP;
		d.tex[0].filter = RSXKIT_FILTER_LINEAR;
		d.tex[0].xf[0] = d.tex[0].xf[1] = 1.0f;
		for (int c = 0; c < 4; c++)
			d.k[RSXKIT_K_T0][1][c] = 1.0f;
		rsxkit_draw_triangles(&d, (uint32_t)s * 6, 6);
	}
}

static void draw_pointer(const struct sds_rect *touch, float k)
{
	const float cx = ((float)touch->x + (input.x + 0.5f) * touch->scale) * k;
	const float cy = ((float)touch->y + (input.y + 0.5f) * touch->scale) * k;

	if (input.touching) {
		rsxkit_ui_rect(cx - 9, cy - 9, 18, 18, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 7, cy - 7, 14, 14, 0xE02020FF);
	} else {
		rsxkit_ui_rect(cx - 14, cy - 2, 28, 4, 0x000000C0);
		rsxkit_ui_rect(cx - 2, cy - 14, 4, 28, 0x000000C0);
		rsxkit_ui_rect(cx - 13, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx + 2, cy - 1, 11, 2, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy - 13, 2, 11, 0xFFFFFFFF);
		rsxkit_ui_rect(cx - 1, cy + 2, 2, 11, 0xFFFFFFFF);
	}
}

/* Salir del juego al selector: SELECT + START del DS3 mantenidos EXIT_HOLD_US (1 s). La DS reinicia con L+R+START+SELECT, y las
 * dos solas se usan poco juntas; el medidor aparece pasados los primeros 250 ms para no tapar la pantalla en los golpes sueltos. */
#define EXIT_HOLD_US 1000000u
#define EXIT_SHOW_US 250000u
static uint64_t exit_t0;       /* instante en que se apretó la combinación (0 = suelta) */
static int exit_game;          /* la combinación se mantuvo el tiempo: volver al selector */
static float exit_progress;    /* 0 a 1, solo para dibujar */

struct stats {
	uint32_t emu_us, frame_us;
	double mips9, mips7;
};

static void draw_ui(const struct sds_rect r[2], const struct stats *st)
{
	const float k = UI_W / (float)cur_w;
	char line[160];

	rsxkit_ui_begin(UI_W, UI_H);
	draw_pointer(&r[1], k);
	if (show_osd) {
		rsxkit_ui_rect(6, 6, 600, 52, 0x000000A0);
		snprintf(line, sizeof line, "%s %s  %s", NUCLEO_TITLE, NUCLEO_VERSION, sds_layout_name(layout));
		rsxkit_ui_text(12, 10, line, 1.4f, 0xFFFFFFFF, 0);
		snprintf(line, sizeof line, "emulacion %u.%02u ms  cuadro %u.%02u ms  ARM9 %u.%u MIPS  ARM7 %u.%u MIPS",
			 st->emu_us / 1000, st->emu_us % 1000 / 10, st->frame_us / 1000, st->frame_us % 1000 / 10,
			 (unsigned)st->mips9, (unsigned)(st->mips9 * 10) % 10, (unsigned)st->mips7, (unsigned)(st->mips7 * 10) % 10);
		rsxkit_ui_text(12, 32, line, 1.2f, 0xC0FFC0FF, 0);
	}
	if (exit_progress > 0.0f) {
		const float p = exit_progress > 1.0f ? 1.0f : exit_progress;

		rsxkit_ui_rect(UI_W / 2 - 160, UI_H - 70, 320, 44, 0x000000C0);
		rsxkit_ui_text(UI_W / 2 - 150, UI_H - 66, "Saliendo al selector...", 1.4f, 0xFFFFFFFF, 0);
		rsxkit_ui_rect(UI_W / 2 - 150, UI_H - 40, 300, 8, 0x404040FF);
		rsxkit_ui_rect(UI_W / 2 - 150, UI_H - 40, 300 * p, 8, 0xFFC040FF);
	}
	rsxkit_ui_end();
}

/* ---- entrada ---------------------------------------------------------------------------------------------- */

static struct sds_ds3 neutral_pad(void)
{
	struct sds_ds3 p;

	memset(&p, 0, sizeof p);
	p.lx = p.ly = p.rx = p.ry = 128;
	p.accel[0] = p.accel[1] = p.accel[2] = 512;
	p.gyro = 512;
	return p;
}

static void convert_pad(const padData *d, struct sds_ds3 *s)
{
	s->buttons = (uint32_t)d->BTN_CROSS << SDS_DS3_CROSS | (uint32_t)d->BTN_CIRCLE << SDS_DS3_CIRCLE |
		     (uint32_t)d->BTN_SQUARE << SDS_DS3_SQUARE | (uint32_t)d->BTN_TRIANGLE << SDS_DS3_TRIANGLE |
		     (uint32_t)d->BTN_L1 << SDS_DS3_L1 | (uint32_t)d->BTN_R1 << SDS_DS3_R1 |
		     (uint32_t)d->BTN_L2 << SDS_DS3_L2 | (uint32_t)d->BTN_R2 << SDS_DS3_R2 |
		     (uint32_t)d->BTN_L3 << SDS_DS3_L3 | (uint32_t)d->BTN_R3 << SDS_DS3_R3 |
		     (uint32_t)d->BTN_START << SDS_DS3_START | (uint32_t)d->BTN_SELECT << SDS_DS3_SELECT |
		     (uint32_t)d->BTN_UP << SDS_DS3_UP | (uint32_t)d->BTN_DOWN << SDS_DS3_DOWN |
		     (uint32_t)d->BTN_LEFT << SDS_DS3_LEFT | (uint32_t)d->BTN_RIGHT << SDS_DS3_RIGHT;
	s->lx = (uint8_t)d->ANA_L_H;
	s->ly = (uint8_t)d->ANA_L_V;
	s->rx = (uint8_t)d->ANA_R_H;
	s->ry = (uint8_t)d->ANA_R_V;
	s->accel[0] = (uint16_t)d->SENSOR_X;
	s->accel[1] = (uint16_t)d->SENSOR_Y;
	s->accel[2] = (uint16_t)d->SENSOR_Z;
	s->gyro = (uint16_t)d->SENSOR_G;
	s->sensors_ok = (d->SENSOR_X | d->SENSOR_Y | d->SENSOR_Z) != 0;
}

static void load_config(void)
{
	sds_padcfg_default(&cfg);
	FILE *f = fopen(CFG_PATH, "r");

	if (f) {
		char text[4096], err[96];
		const size_t n = fread(text, 1, sizeof text - 1, f);

		fclose(f);
		const int bad = sds_padcfg_parse(&cfg, text, n, err, sizeof err);

		salsads_log("%s leído (%u bytes)%s%s", CFG_PATH, (unsigned)n, bad ? "; no entendí líneas: " : "", bad ? err : "");
	} else {
		salsads_log("sin %s: controles por omisión (el programa de pantallas de la Fase 0 lo crea)", CFG_PATH);
	}
}

static uint32_t combo_prev;


/* Lee el DS3 (el selector y el juego) y lo deja en `ds3`. */
static void pad_read(void)
{
	static struct sds_ds3 last;
	padInfo info;
	padData data;

	if (ioPadGetInfo(&info) == 0 && info.status[0]) {
		if (!pad_present)
			salsads_log("mando 1 conectado");
		pad_present = 1;
		if (ioPadGetData(0, &data) == 0 && data.len > 0)
			convert_pad(&data, &last);
		ds3 = last;
	} else {
		pad_present = 0;
		ds3 = neutral_pad();
	}
}

/* Lee el DS3 y devuelve los botones de la DS y el toque para el núcleo. */
static uint32_t poll_input(float dt, unsigned *tx, unsigned *ty)
{
	int x, y, down;
	uint32_t k;

	pad_read();
	if (autotest) {
		/* RPCS3 sin mando: cruceta y A a ratos, y toque en el centro de la pantalla táctil durante los cuadros 40-110 */
		const uint32_t f = frame_no;

		ds3.buttons = ((f / 20u) & 1u ? (1u << SDS_DS3_RIGHT) : (1u << SDS_DS3_DOWN)) | ((f / 13u) & 1u ? (1u << SDS_DS3_CIRCLE) : 0u) |
			      (f >= 40 && f < 110 ? (1u << SDS_DS3_R2) : 0u);
		ds3.rx = ds3.ry = 128;
	}
	{
		const uint32_t both = (1u << SDS_DS3_SELECT) | (1u << SDS_DS3_START);

		if (!autotest && (ds3.buttons & both) == both) {
			const uint64_t t = now_us();

			if (!exit_t0)
				exit_t0 = t;
			exit_progress = t - exit_t0 > EXIT_SHOW_US ? (float)(t - exit_t0) / (float)EXIT_HOLD_US : 0.0f;
			if (t - exit_t0 >= EXIT_HOLD_US)
				exit_game = 1;
		} else {
			exit_t0 = 0;
			exit_progress = 0.0f;
		}
	}
	sds_pad_map(&cfg, &input, &ds3, &keys);
	sds_pointer_update(&cfg, &input, &ds3, dt, &keys);
	if (keys.layout)
		layout = (enum sds_layout)((layout + 1) % SDS_LAYOUT_COUNT);
	{
		const uint32_t b = ds3.buttons, both = (1u << SDS_DS3_L3) | (1u << SDS_DS3_R3);
		const uint32_t now = (b & both) == both;

		if (now && !combo_prev)
			show_osd = !show_osd;
		combo_prev = now;
	}
	sds_touch_state(&input, &x, &y, &down);
	k = keys.keys & 0x0FFFu; /* mismo orden de bits que NDS_KEY_* */
	if (down) {
		k |= NDS_KEY_PEN;
		*tx = (unsigned)x;
		*ty = (unsigned)y;
	} else {
		*tx = *ty = 0;
	}
	return k;
}

/* ---- guardado ----------------------------------------------------------------------------------------------- */

static char save_path[96];
static uint32_t save_last_frame;

static void save_name(void)
{
	char code[5];

	for (int i = 0; i < 4; i++) {
		const uint8_t c = rom_buf[0x0C + i];

		code[i] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? (char)c : '_';
	}
	code[4] = 0;
	snprintf(save_path, sizeof save_path, "%s/%s.sav", SAVE_DIR, code);
}

/* Carga el archivo de guardado del juego, si existe (su tamaño dice qué chip es). */
static void save_load(void)
{
	FILE *f;

	save_name();
	mkdir(SALSADS_DATA_DIR, 0777);
	mkdir(SAVE_DIR, 0777);
	f = fopen(SAVE_CFG_PATH, "rb");
	if (f) {
		static char cfg[4096];
		const size_t cn = fread(cfg, 1, sizeof cfg, f);

		fclose(f);
		if (nds_save_cfg(nds, cfg, cn))
			salsads_log("guardado: %s fuerza el chip de este juego", SAVE_CFG_PATH);
	}
	f = fopen(save_path, "rb");
	if (f) {
		static uint8_t buf[NDS_SAVE_MAX];
		const size_t n = fread(buf, 1, sizeof buf, f);

		fclose(f);
		if (nds_save_attach(nds, buf, n) == 0)
			salsads_log("guardado: %s, %u bytes (chip tipo %u)", save_path, (unsigned)n, (unsigned)nds->save.type);
		else
			salsads_log("guardado: %s tiene %u bytes, que no es el tamaño de ningún chip: se ignora", save_path, (unsigned)n);
	} else {
		salsads_log("guardado: sin %s; chip por omisión tipo %u de %u bytes (se crea cuando el juego escriba)", save_path,
			    (unsigned)nds->save.type, (unsigned)nds->save.size);
	}
}

/* Escribe el guardado si el juego lo cambió; con `force` también antes de que pasen SAVE_FLUSH_FRAMES. */
static void save_flush(int force)
{
	size_t len;
	const uint8_t *d;
	char tmp[100];
	FILE *f;

	if (!nds->save.dirty || (!force && frame_no - save_last_frame < SAVE_FLUSH_FRAMES))
		return;
	save_last_frame = frame_no;
	d = nds_save_data(nds, &len);
	snprintf(tmp, sizeof tmp, "%s.tmp", save_path);
	f = fopen(tmp, "wb");
	if (f && fwrite(d, 1, len, f) == len && fclose(f) == 0) {
		remove(save_path);
		if (rename(tmp, save_path) == 0) {
			nds->save.dirty = 0;
			salsads_log("guardado escrito: %s, %u bytes", save_path, (unsigned)len);
			return;
		}
	} else if (f) {
		fclose(f);
	}
	salsads_log("no pude escribir el guardado en %s", save_path);
}

/* ---- composición 2D en los SPE ------------------------------------------------------------------------------ */

/* Cierre del cuadro (línea 192): las dos pantallas van a dos SPE y la salida cae directo en las texturas del RSX. */
static void vid_submit(void *ctx, int vset, gpu2d_frame *f)
{
	const int set = (int)(sub_count & 1u);

	(void)ctx;
	if (set_used[set])
		rsxkit_fence_wait(set_fence[set]); /* el RSX ya terminó de leer esta textura */
	sub.valid = 1;
	sub.done = 0;
	sub.set = set;
	sub.vset = vset;
	for (int e = 0; e < 2; e++) {
		job_compose jc;

		f->eng[e].out_ea = (uint64_t)(uintptr_t)tex_px(set, e);
		jc.frame_ea = (uint64_t)(uintptr_t)f;
		jc.engine = (uint32_t)e;
		sub.seq[e] = spukit_post(wk[e], JOB_COMPOSE, &jc, sizeof jc);
	}
	sub_count++;
}

/* Espera los trabajos de un cuadro (si no se esperaron) y deja sus resultados. 0 = bien. */
static int vid_collect(typeof(sub) *q)
{
	if (!q->valid || q->done)
		return 0;
	{
		const uint64_t t0 = now_us();

		for (int e = 0; e < 2; e++) {
			const spukit_result *r = spukit_wait(wk[e], q->seq[e]);

			if (r->error != 0) {
				salsads_log("FALLO el SPE de la pantalla %c devolvió error %d", 'A' + e, r->error);
				q->done = 1;
				return -1;
			}
			memcpy(&q->res[e], r->data, sizeof q->res[e]);
		}
		q->wait_us = (uint32_t)(now_us() - t0);
	}
	q->done = 1;
	return 0;
}

/* Antes de reescribir una instantánea: si todavía la lee un trabajo, esperarlo */
static void vid_wait(void *ctx, int vset)
{
	(void)ctx;
	if (sub.valid && !sub.done && sub.vset == vset)
		vid_collect(&sub);
	if (prev.valid && !prev.done && prev.vset == vset)
		vid_collect(&prev);
}

/* Autoprueba: la imagen de los SPE contra la referencia del PPE sobre la misma instantánea */
static void vid_verify(const typeof(sub) *q)
{
	static uint16_t ref[TEX_W * TEX_H];

	for (int e = 0; e < 2; e++) {
		const uint16_t *spe = tex_px(q->set, e);
		int diffs = 0, first = -1;

		gpu2d_ref_engine(&nds->vid.set[q->vset].frame.eng[e], ref);
		for (int y = 0; y < TEX_H; y++)
			for (int x = 0; x < TEX_W; x++) {
				if (spe[y * (TEX_PITCH / 2) + x] != ref[y * TEX_W + x]) {
					if (first < 0)
						first = y * TEX_W + x;
					diffs++;
				}
			}
		if (diffs) {
			salsads_log("FALLO el SPE de la pantalla %c difiere de la referencia en %d píxeles (primero en %d,%d)", 'A' + e,
				    diffs, first % TEX_W, first / TEX_W);
			failures++;
		}
	}
}

/* ---- sonido: el mezclador en un SPE y la salida de audio ------------------------------------------------------------- */

static int snd_on;
static spukit_worker *wk_snd;
static uint32_t snd_seq[2];
static struct {
	uint64_t jobs, ticks, mix_ticks, wait_us, out_n;
	uint32_t ticks_max, mix_max, errors;
} snd_acc;

static void snd_submit(void *ctx, int set, struct nds_snd_job *job, snd_state *st)
{
	job_snd js;

	(void)ctx;
	memset(&js, 0, sizeof js);
	js.state_ea = (uint64_t)(uintptr_t)st;
	js.log_ea = (uint64_t)(uintptr_t)job->log;
	js.out_ea = (uint64_t)(uintptr_t)job->out;
	js.nat_ea = (uint64_t)(uintptr_t)job->nat;
	js.nlog = job->nlog;
	js.nticks = job->nticks;
	js.out_max = SND_OUT_MAX;
	js.mode = job->mode;
	js.ratio_adj = job->ratio_adj;
	for (int i = 0; i < 4; i++) {
		js.rgn_ea[i] = job->rgn[i].rd;
		js.rgn_mask[i] = job->rgn[i].mask;
	}
	snd_seq[set] = spukit_post(wk_snd, JOB_SND, &js, sizeof js);
}

static void snd_wait(void *ctx, int set)
{
	const uint64_t t0 = now_us();
	const spukit_result *r = spukit_wait(wk_snd, snd_seq[set]);
	job_snd_result jr;

	(void)ctx;
	snd_acc.wait_us += now_us() - t0;
	memcpy(&jr, r->data, sizeof jr);
	if (r->error != 0) {
		snd_acc.errors++;
		nds->snd.job[set].out_n = 0;
		return;
	}
	nds->snd.job[set].out_n = jr.out_n;
	snd_acc.jobs++;
	snd_acc.ticks += jr.ticks;
	snd_acc.mix_ticks += jr.mix_ticks;
	snd_acc.out_n += jr.out_n;
	if (jr.ticks > snd_acc.ticks_max)
		snd_acc.ticks_max = jr.ticks;
	if (jr.mix_ticks > snd_acc.mix_max)
		snd_acc.mix_max = jr.mix_ticks;
}

static void snd_sink(void *ctx, const float *out, uint32_t n, const int16_t *nat, uint32_t nticks)
{
	(void)ctx;
	(void)nat;
	(void)nticks;
	audio_out_push(out, n);
}

/* Autoprueba del SPE de audio: el mismo trabajo en el SPE y en el PPE (mismo código C) con una memoria de prueba propia:
 * las muestras de 16 bits, el estado y lo capturado tienen que ser idénticos; la salida flotante, igual salvo el redondeo
 * del SPU. Dos trabajos seguidos (el estado se arrastra). Devuelve 0 si coincide. */
#define ST_MEM 0x10000u
static uint8_t st_mem_spe[ST_MEM] __attribute__((aligned(128))), st_mem_ppe[ST_MEM] __attribute__((aligned(128)));
static snd_ent st_log[SND_LOG_MAX] __attribute__((aligned(128)));
static int16_t st_nat_spe[2 * SND_TICKS_MAX] __attribute__((aligned(128))), st_nat_ppe[2 * SND_TICKS_MAX];
static float st_out_spe[2 * SND_OUT_MAX] __attribute__((aligned(128))), st_out_ppe[2 * SND_OUT_MAX];
static snd_state st_spe __attribute__((aligned(128))), st_ppe __attribute__((aligned(128)));

static void st_bus_fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	(void)ctx;
	if ((addr >> 23) == 4)
		memcpy(dst, st_mem_ppe + (addr & (ST_MEM - 1)), SND_LINE);
	else
		memset(dst, 0, SND_LINE);
}

static void st_bus_store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	(void)ctx;
	if ((addr >> 23) == 4)
		memcpy(st_mem_ppe + (addr & (ST_MEM - 1)), src, n);
}

static uint32_t st_n;

static void st_w(uint32_t off, uint32_t v)
{
	st_log[st_n++] = (snd_ent){ .tick = 0, .off = (uint16_t)off, .val = (uint16_t)v, .mask = 0xFFFF };
}

static void st_w32(uint32_t off, uint32_t v)
{
	st_w(off, v & 0xFFFFu);
	st_w(off + 2, v >> 16);
}

static int snd_selftest(void)
{
	job_snd js;
	uint32_t t0 = 0, bad_nat = 0, bad_state = 0, bad_mem = 0, bad_out = 0;
	const uint32_t D16 = 0x02000100u, D8 = 0x02000500u, DAD = 0x02000700u, CAP = 0x02002000u;

	memset(st_mem_spe, 0, sizeof st_mem_spe);
	for (uint32_t i = 0; i < 256; i++) {
		wr16le(st_mem_spe + 0x100 + 2 * i, (uint16_t)(((int)i - 128) * 200));
		st_mem_spe[0x500 + i] = (uint8_t)(i * 3);
	}
	memcpy(st_mem_spe + 0x700, adpcm_data, sizeof adpcm_data);
	memcpy(st_mem_ppe, st_mem_spe, sizeof st_mem_ppe);
	snd_reset(&st_spe);
	snd_reset(&st_ppe);
	for (int pass = 0; pass < 2; pass++) {
		const uint32_t nt = 300u + 77u * (uint32_t)pass;
		uint32_t n_ppe = 0, applied;
		snd_bus bus = { NULL, st_bus_fetch, st_bus_store };

		st_n = 0;
		if (pass == 0) {
			st_w(0x100, 0x807F);
			st_w32(0x00 + 4, D16 & 0x07FFFFFFu);
			st_w(0x08, (uint16_t)(0x10000u - 700u));
			st_w32(0x0C, 128);
			st_w32(0x00, 0x20000000u | (1u << 27) | 127u | (60u << 16) | 0x80000000u);
			st_w32(0x10 + 4, D8);
			st_w(0x10 + 8, 0xFE00u);
			st_w32(0x10 + 12, 64);
			st_w32(0x10, (1u << 27) | 100u | (1u << 8) | (20u << 16) | 0x80000000u);
			st_w32(0x20 + 4, DAD);
			st_w(0x20 + 8, 0xFE00u);
			st_w(0x20 + 10, 1);
			st_w32(0x20 + 12, 3);
			st_w32(0x20, (1u << 27) | (2u << 29) | 127u | (80u << 16) | 0x80000000u);
			st_w32(0x80, (3u << 29) | 127u | (64u << 16) | (3u << 24) | 0x80000000u);
			st_w(0x88, 0xFF00u);
			st_w32(0xE0, (3u << 29) | 127u | (64u << 16) | 0x80000000u);
			st_w(0xE8, 0xFC00u);
			st_w32(0x110, CAP & 0x07FFFFFFu);
			st_w(0x114, 64);
			st_w(0x108, 0x84);
		}
		memset(&js, 0, sizeof js);
		js.state_ea = (uint64_t)(uintptr_t)&st_spe;
		js.log_ea = (uint64_t)(uintptr_t)st_log;
		js.out_ea = (uint64_t)(uintptr_t)st_out_spe;
		js.nat_ea = (uint64_t)(uintptr_t)st_nat_spe;
		js.nlog = st_n;
		js.nticks = nt;
		js.out_max = SND_OUT_MAX;
		js.mode = (uint32_t)pass;
		js.ratio_adj = 0.003f * (float)pass;
		js.rgn_ea[0] = (uint64_t)(uintptr_t)st_mem_spe;
		js.rgn_mask[0] = ST_MEM - 1;
		{
			const uint32_t seq = spukit_post(wk_snd, JOB_SND, &js, sizeof js);
			const spukit_result *r = spukit_wait(wk_snd, seq);
			job_snd_result jr;

			if (r->error != 0) {
				salsads_log("FALLO autoprueba del audio: el SPE devolvió error %d", r->error);
				return -1;
			}
			memcpy(&jr, r->data, sizeof jr);
			applied = jr.applied;
			t0 = jr.out_n;
		}
		st_ppe.mode = js.mode;
		st_ppe.ratio_adj = js.ratio_adj;
		(void)snd_run(&st_ppe, st_log, st_n, nt, &bus, st_nat_ppe, st_out_ppe, SND_OUT_MAX, &n_ppe);
		if (applied != st_n)
			bad_state++;
		for (uint32_t i = 0; i < 2 * nt; i++)
			bad_nat += st_nat_spe[i] != st_nat_ppe[i];
		if (t0 != n_ppe)
			bad_out += 1000;
		else
			for (uint32_t i = 0; i < 2 * n_ppe; i++) {
				const float d = st_out_spe[i] - st_out_ppe[i];

				bad_out += d > 0.0005f || d < -0.0005f;
			}
		if (st_spe.busy != st_ppe.busy || st_spe.tick != st_ppe.tick || st_spe.peak != st_ppe.peak)
			bad_state++;
		for (int c = 0; c < SND_NCH; c++)
			if (st_spe.ch[c].pos != st_ppe.ch[c].pos || st_spe.ch[c].out != st_ppe.ch[c].out ||
			    st_spe.ch[c].tcnt != st_ppe.ch[c].tcnt || st_spe.ch[c].lfsr != st_ppe.ch[c].lfsr ||
			    st_spe.ch[c].adv != st_ppe.ch[c].adv)
				bad_state++;
		bad_mem += memcmp(st_mem_spe, st_mem_ppe, sizeof st_mem_ppe) != 0;
	}
	salsads_log("autoprueba del audio: SPE contra PPE, 2 trabajos: %u muestras distintas, %u del estado, %u de la memoria "
		    "capturada, %u de la salida flotante", (unsigned)bad_nat, (unsigned)bad_state, (unsigned)bad_mem,
		    (unsigned)bad_out);
	return bad_nat || bad_state || bad_mem || bad_out ? -1 : 0;
}

static void snd_load_cfg(void)
{
	FILE *f = fopen(AUDIO_CFG_PATH, "r");
	char text[256];
	size_t len;

	if (!f)
		return;
	len = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	text[len] = '\0';
	/* una clave por línea: «original = si» cuantiza a 10 bits con el sesgo, como la salida de la DS */
	if (strstr(text, "original = si") || strstr(text, "original=si")) {
		nds_snd_set_mode(nds, SND_MODE_ORIGINAL);
		salsads_log("audio: salida «como el original» (10 bits con SOUNDBIAS) por %s", AUDIO_CFG_PATH);
	}
}

/* ---- un cuadro ------------------------------------------------------------------------------------------------ */

/* Reloj de perfil del planificador: el timebase (79,8 MHz en la PS3). */
static uint64_t tb_clock(void)
{
	uint64_t t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static uint64_t tb_hz;
#ifdef NUCLEO_JIT
static uint64_t jit_clock_ns(void)
{
	return tb_hz ? tb_clock() * 1000000000ull / tb_hz : 0;
}

/* Los relojes del recompilador de las dos CPUs (al conectar y cada vez que nds_init lo recrea con un juego nuevo). */
static void jit_clocks(void)
{
	if (nds->jit9) {
		((struct arec_pub *)(void *)nds->jit9)->clock_ns = jit_clock_ns;
		((struct arec_pub *)(void *)nds->jit9)->tick = jit_times ? tb_clock : NULL;
	}
	if (nds->jit7) {
		((struct arec_pub *)(void *)nds->jit7)->clock_ns = jit_clock_ns;
		((struct arec_pub *)(void *)nds->jit7)->tick = jit_times ? tb_clock : NULL;
	}
}
#endif

static inline uint32_t tb_us(uint64_t ticks)
{
	return (uint32_t)(ticks * 1000000u / tb_hz);
}

/* Muestras por cuadro. Los cuadros pares se miden sin reloj de perfil en el planificador; los impares con él. */
enum {
	M_EMU,    /* nds_run_frame en los cuadros pares: sin reloj de perfil, el tiempo «limpio» */
	M_EMU_P,  /* nds_run_frame en los impares: con el reloj de perfil */
	M_A9,     /* tiempo dentro del ARM9 (solo cuadros impares) */
	M_A7,     /* ídem ARM7 */
	M_EV,     /* ídem eventos */
	M_SCR,    /* nds_screens */
	M_CONV,   /* espera de la textura anterior + conversión RGB555 → RGBA5551 */
	M_DRAW,   /* distribución, vértices, dibujo de las dos pantallas y de la interfaz */
	M_INPUT,  /* lectura del DS3 y entrada a la DS */
	M_BEGIN,  /* rsxkit_frame_begin: espera del vsync y del RSX */
	M_END,    /* rsxkit_frame_end */
	M_IV,     /* intervalo entre cuadros */
	M_SPEA,   /* SPE de la pantalla A: duración de la composición del cuadro que se dibuja (decrementador) */
	M_SPEB,   /* ídem, pantalla B */
	M_WAIT,   /* el PPE esperando a los SPE (spukit_wait) del cuadro que se dibuja */
	M_R3D0,   /* render 3D: duración en el SPE 0 del 3D (decrementador) */
	M_R3D1,   /* ídem, SPE 1 */
	M_R3DW,   /* el PPE esperando al render 3D en el VBlank */
	M_COUNT
};
static uint32_t buf[M_COUNT][STATS_FRAMES];
static uint32_t nbuf[M_COUNT];
static uint32_t stat_n;
static uint64_t last_begin_us;
static struct stats cur_stats;
static uint64_t acc_gx_ticks, acc_gx_cycles, acc_gx_polys, acc_gx_verts, acc_gx_swaps, acc_frames;
static uint64_t acc_slices, acc_jumps, acc_events, acc_i9, acc_i7, acc_emu_ticks, acc_a9_ticks, acc_a7_ticks, acc_prof_n;
static uint64_t acc_i9p, acc_i7p; /* instrucciones de los cuadros con perfil (para los MIPS dentro de cada CPU) */
static spukit_worker *wk_geom;
static int geom_on;
static uint32_t geom_collect_seq = 1, geom_last_seq;
static struct {
	uint64_t jobs, ticks, cmds, puts, wait_us, waits, posts, prof[5];
	uint32_t ticks_max, errors;
} geom_acc;
static uint64_t acc_evn[EV_COUNT], acc_evt[EV_COUNT]; /* eventos del planificador por tipo: cuántos y ticks (cuadros con perfil) */
static uint64_t acc_evn_all[EV_COUNT];                /* ídem, cuántos, en todos los cuadros */
static r3d_stats acc_r3d;           /* render 3D: suma de los contadores de los dos SPE */
static uint32_t acc_r3d_frames;
/* latencia del render 3D: envío (línea 0) -> fin de cada SPE, visto desde el PPE que espera en el VBlank */
static uint64_t r3d_post_us, acc_r3d_pre_us, acc_r3d_lat_us[2];
static uint32_t acc_r3d_early;

static void sort_u32(uint32_t *a, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		const uint32_t v = a[i];
		int j = (int)i - 1;

		while (j >= 0 && a[j] > v) {
			a[j + 1] = a[j];
			j--;
		}
		a[j + 1] = v;
	}
}

static void put(int m, uint32_t v)
{
	if (nbuf[m] < STATS_FRAMES)
		buf[m][nbuf[m]++] = v;
}

/* Percentil p (0-100) de una serie en µs ya ordenada; 0 si está vacía. */
static uint32_t pct(int m, unsigned p)
{
	return nbuf[m] ? buf[m][(nbuf[m] - 1) * p / 100] : 0;
}

#define MS(v) (unsigned)((v) / 1000), (unsigned)((v) % 1000 / 10)

static uint64_t prev_i9, prev_i7, prev_silent;
#ifdef NUCLEO_JIT
static struct arec_stats prev_js, prev_js7;
#endif

static void log_stats(void)
{
	uint32_t over20 = 0;
	const uint64_t i9 = nds->a9.icount, i7 = nds->a7.icount;
	const uint32_t n = stat_n;
	uint32_t sched;
	double mips9_cpu = 0, mips7_cpu = 0, mips9_tot = 0, mips7_tot = 0;
	const double secs_a9 = acc_prof_n ? (double)acc_a9_ticks / (double)tb_hz : 0;
	const double secs_a7 = acc_prof_n ? (double)acc_a7_ticks / (double)tb_hz : 0;

	for (uint32_t i = 0; i < nbuf[M_IV]; i++)
		over20 += buf[M_IV][i] > 20000;
	for (int m = 0; m < M_COUNT; m++)
		sort_u32(buf[m], nbuf[m]);
	salsads_log("DATO nucleo: %u cuadros; emulación de un cuadro (sin reloj de perfil): mediana %u.%02u ms, p95 %u.%02u, "
		    "máx %u.%02u; intervalo del RSX: mediana %u.%02u ms, p95 %u.%02u; cuadros de más de 20 ms: %u; "
		    "instrucciones por cuadro: ARM9 %u, ARM7 %u; cuadros de la DS %u",
		    n, MS(pct(M_EMU, 50)), MS(pct(M_EMU, 95)), MS(pct(M_EMU, 100)), MS(pct(M_IV, 50)), MS(pct(M_IV, 95)),
		    over20, (unsigned)((i9 - prev_i9) / n), (unsigned)((i7 - prev_i7) / n), (unsigned)nds->frames);
	/* Desglose: el tiempo de emulación con el reloj de perfil se reparte entre el ARM9, el ARM7, los eventos y el
	 * resto (el planificador: elegir el tramo, comprobar eventos y HALT, saltos ociosos). Medianas de cuadros
	 * impares; el resto es lo que no cae en ninguna de las cuatro. */
	{
		/* la suma de las medianas no es la mediana de la suma: el planificador sale de la resta de totales */
		const uint32_t emu_p = pct(M_EMU_P, 50), a9 = pct(M_A9, 50), a7 = pct(M_A7, 50), ev = pct(M_EV, 50);

		sched = emu_p > a9 + a7 + ev ? emu_p - a9 - a7 - ev : 0;
		salsads_log("DATO desglose: por cuadro, mediana en ms (cuadros con reloj de perfil): emulación %u.%02u = ARM9 %u.%02u + "
			    "ARM7 %u.%02u + eventos %u.%02u + planificador %u.%02u; resto del cuadro: entrada %u.%02u, pantallas %u.%02u, "
			    "conversión %u.%02u, dibujo %u.%02u, frame_begin %u.%02u, frame_end %u.%02u; por cuadro (promedio): %u tramos, "
			    "%u saltos ociosos, %u eventos",
			    MS(emu_p), MS(a9), MS(a7), MS(ev), MS(sched), MS(pct(M_INPUT, 50)), MS(pct(M_SCR, 50)), MS(pct(M_CONV, 50)),
			    MS(pct(M_DRAW, 50)), MS(pct(M_BEGIN, 50)), MS(pct(M_END, 50)), (unsigned)(acc_slices / n),
			    (unsigned)(acc_jumps / n), (unsigned)(acc_events / n));
	}
	if (acc_prof_n) {
		static const char *const evn[EV_COUNT] = { "línea", "hblank", "tm9.0", "tm9.1", "tm9.2", "tm9.3", "tm7.0", "tm7.1",
							   "tm7.2", "tm7.3", "cartucho", "spi", "gx" };
		char line[512];
		size_t o = 0;

		for (int k = 0; k < EV_COUNT && o < sizeof line - 80; k++) {
			if (!acc_evn_all[k])
				continue;
			o += (size_t)snprintf(line + o, sizeof line - o, " %s %.1f (%.2f ms)", evn[k], (double)acc_evn_all[k] / (double)acc_frames,
					      (double)acc_evt[k] * 1000.0 / (double)tb_hz / (double)acc_prof_n);
		}
		salsads_log("DATO eventos: por cuadro, cuántos (ms con reloj de perfil, promedio de %u cuadros; incluye lo que el evento ejecuta, "
			    "p. ej. la geometría en «gx»):%s", (unsigned)acc_prof_n, line);
	}
	if (nds->cart.paged) {
		uint64_t ra_acc, ra_miss, ra_ra;

		nds_rom_stats(nds, &ra_acc, &ra_miss, &ra_ra);
		salsads_log("DATO rom: por fragmentos, %llu ventanas de 4 KB, %llu fallos de caché (%.3f por cuadro), %llu páginas leídas por adelantado, "
			    "%llu errores de lectura", (unsigned long long)ra_acc, (unsigned long long)ra_miss,
			    (double)ra_miss / (double)(nds->frames ? nds->frames : 1), (unsigned long long)ra_ra, (unsigned long long)nds->cart.pg.errs);
	}
	if (spu_on)
		salsads_log("DATO spe: composición 2D por pantalla y por cuadro (decrementador del SPE), mediana / p95 / máx en ms: "
			    "A %u.%02u / %u.%02u / %u.%02u, B %u.%02u / %u.%02u / %u.%02u; el PPE esperando a los SPE: mediana %u.%02u, "
			    "p95 %u.%02u, máx %u.%02u; cuadros dibujados %u",
			    MS(pct(M_SPEA, 50)), MS(pct(M_SPEA, 95)), MS(pct(M_SPEA, 100)), MS(pct(M_SPEB, 50)), MS(pct(M_SPEB, 95)),
			    MS(pct(M_SPEB, 100)), MS(pct(M_WAIT, 50)), MS(pct(M_WAIT, 95)), MS(pct(M_WAIT, 100)), nbuf[M_SPEA]);
	if (spu_on && prof_n) {
		static const char *const nm[12] = { "init", "plan", "(texto)", "(afín)", "(OBJ)", "(esperas)", "compone", "(espera)", "(capas)",
						    "(final)", "(put)", "espera final" };
		char line[700];
		int k = 0;

		for (int e = 0; e < 2; e++) {
			k += snprintf(line + k, sizeof line - (size_t)k, "%s%c:", e ? "; " : "", 'A' + e);
			for (int q = 0; q < 12; q++) {
				uint64_t us;

				sort_u32(prof_buf[e][q], prof_n);
				us = (uint64_t)prof_buf[e][q][prof_n / 2] * 1000000ull / spukit_tick_hz();

				k += snprintf(line + k, sizeof line - (size_t)k, " %s %u.%02u", nm[q], (unsigned)(us / 1000), (unsigned)(us % 1000 / 10));
			}
		}
		salsads_log("DATO spe-etapas (ms por pantalla y cuadro, mediana de %u cuadros; entre paréntesis lo que está dentro de la etapa anterior): %s",
			    prof_n, line);
		prof_n = 0;
	}
	{
		struct audio_out_stats as;

		audio_out_stats(&as, 1);
		if (snd_on && snd_acc.jobs)
			salsads_log("DATO audio: mezcla en el SPE por cuadro (decrementador): trabajo %u.%02u ms (máx %u.%02u), de eso snd_run "
				    "%u.%02u ms (máx %u.%02u); el PPE esperando %u.%02u ms por cuadro; %u muestras de 48 kHz por cuadro; "
				    "errores %u; salida: %s, anillo %u muestras, %u faltas (%u muestras de silencio en este lapso), %u bloques "
				    "tarde, pico %.0f dBFS",
				    MS(snd_acc.ticks * 1000000ull / snd_acc.jobs / spukit_tick_hz()),
				    MS((uint64_t)snd_acc.ticks_max * 1000000ull / spukit_tick_hz()),
				    MS(snd_acc.mix_ticks * 1000000ull / snd_acc.jobs / spukit_tick_hz()),
				    MS((uint64_t)snd_acc.mix_max * 1000000ull / spukit_tick_hz()),
				    MS(snd_acc.wait_us / snd_acc.jobs), (unsigned)(snd_acc.out_n / snd_acc.jobs),
				    (unsigned)snd_acc.errors, audio_out_is_open() ? "abierta" : "sin audio", as.level, as.underruns,
				    (unsigned)(as.silent - prev_silent), as.late, as.peak ? 20.0 * log10(as.peak / 32768.0) : -99.0);
		prev_silent = as.silent;
		memset(&snd_acc, 0, sizeof snd_acc);
	}
	if (secs_a9 > 0)
		mips9_cpu = (double)acc_i9p / secs_a9 / 1e6;
	if (secs_a7 > 0)
		mips7_cpu = (double)acc_i7p / secs_a7 / 1e6;
	if (acc_emu_ticks) {
		mips9_tot = (double)acc_i9 / ((double)acc_emu_ticks / (double)tb_hz) / 1e6;
		mips7_tot = (double)acc_i7 / ((double)acc_emu_ticks / (double)tb_hz) / 1e6;
	}
	{
		const uint64_t gx_us = acc_prof_n ? acc_gx_ticks * 1000000u / tb_hz / acc_prof_n : 0;

		salsads_log("DATO geometria: %u cuadros, %llu SWAP_BUFFERS; por cuadro entregado %u polígonos y %u vértices; motor de geometría "
			    "ocupado %u%% del tiempo de la DS; en el PPE, con reloj: %u.%03u ms por cuadro (%u cuadros con perfil)",
			    (unsigned)acc_frames, (unsigned long long)acc_gx_swaps,
			    acc_gx_swaps ? (unsigned)(acc_gx_polys / acc_gx_swaps) : 0u,
			    acc_gx_swaps ? (unsigned)(acc_gx_verts / acc_gx_swaps) : 0u,
			    acc_frames ? (unsigned)(100u * acc_gx_cycles / (acc_frames * NDS_FRAME_CYCLES)) : 0u,
			    (unsigned)(gx_us / 1000u), (unsigned)(gx_us % 1000u), (unsigned)acc_prof_n);
		if (geom_on) {
			const double nf = acc_frames ? (double)acc_frames : 1.0, hz = (double)spukit_tick_hz();

			salsads_log("DATO geometria-spe: por cuadro: %.1f trabajos (%.0f órdenes, %.1f subidas DMA), SPE %.2f ms (máx por trabajo %.2f ms), "
				    "el PPE esperando al SPE %.2f ms (%.1f esperas); errores %u; SPE por etapa (ms por cuadro): matrices %.2f, vértices %.2f "
				    "(de eso armado de polígonos %.2f), normales y luces %.2f, otras %.2f",
				    (double)geom_acc.jobs / nf, (double)geom_acc.cmds / nf, (double)geom_acc.puts / nf,
				    (double)geom_acc.ticks * 1000.0 / hz / nf, (double)geom_acc.ticks_max * 1000.0 / hz,
				    (double)geom_acc.wait_us / 1000.0 / nf, (double)geom_acc.waits / nf, geom_acc.errors,
				    (double)geom_acc.prof[0] * 1000.0 / hz / nf, (double)geom_acc.prof[1] * 1000.0 / hz / nf,
				    (double)geom_acc.prof[4] * 1000.0 / hz / nf, (double)geom_acc.prof[2] * 1000.0 / hz / nf,
				    (double)geom_acc.prof[3] * 1000.0 / hz / nf);
			memset(&geom_acc, 0, sizeof geom_acc);
		}
	}
	if (acc_r3d_frames) {
		const r3d_stats *r = &acc_r3d;
		const double n = (double)acc_r3d_frames, hz = (double)spukit_tick_hz() / 1000.0;

		salsads_log("DATO raster3d: %u cuadros con 3D; por cuadro y SPE (decrementador), mediana / p95 / máx en ms: SPE0 %u.%02u / %u.%02u / %u.%02u, "
			    "SPE1 %u.%02u / %u.%02u / %u.%02u; espera del PPE en el VBlank %u.%02u / %u.%02u; por cuadro: %.0f polígonos, %.0f banda·polígono, "
			    "%.0f líneas, %.0f píxeles, %.0f escritos; texeles %.0f, aciertos de la caché %.2f %%, %.1f bloques decodificados, %.1f desalojos, %.1f líneas de VRAM; "
			    "ms por cuadro (suma de los dos SPE): orden %.2f, listas %.2f, DMA %.2f, dibujo %.2f (de eso texturas %.2f; polígonos %.2f, aristas %.2f, píxeles %.2f), posproceso %.2f, salida %.2f",
			    (unsigned)acc_r3d_frames, MS(pct(M_R3D0, 50)), MS(pct(M_R3D0, 95)), MS(pct(M_R3D0, 100)), MS(pct(M_R3D1, 50)),
			    MS(pct(M_R3D1, 95)), MS(pct(M_R3D1, 100)), MS(pct(M_R3DW, 50)), MS(pct(M_R3DW, 95)), r->polys / n / 2.0,
			    r->band_polys / n, r->rows / n, r->pixels / n, r->written / n, r->tex_px / n,
			    r->tex_px ? 100.0 * (double)r->tex_hits / (double)(r->tex_hits + r->tex_misses) : 100.0, r->tex_misses / n, r->tex_evict / n,
			    r->line_miss / n, r->t_sort / n / hz, r->t_list / n / hz, r->t_fetch / n / hz, r->t_raster / n / hz, r->t_tex / n / hz,
			    r->t_setup / n / hz, r->t_rows / n / hz, r->t_pix / n / hz, r->t_post / n / hz, r->t_out / n / hz);
		salsads_log("DATO raster3d-latencia: por cuadro, desde el envío (línea 0) hasta: llegada del PPE a la línea 262 (la 192 si el cuadro captura) %.2f ms, fin del SPE0 %.2f ms, "
			    "fin del SPE1 %.2f ms (si el SPE ya había terminado cuenta la llegada); SPE ya terminados al llegar: %.2f de 2",
			    (double)acc_r3d_pre_us / 1000.0 / n, (double)acc_r3d_lat_us[0] / 1000.0 / n, (double)acc_r3d_lat_us[1] / 1000.0 / n,
			    (double)acc_r3d_early / n);
		acc_r3d_pre_us = acc_r3d_lat_us[0] = acc_r3d_lat_us[1] = 0;
		acc_r3d_early = 0;
		memset(&acc_r3d, 0, sizeof acc_r3d);
		acc_r3d_frames = 0;
	}
#ifdef NUCLEO_JIT
	if (jit_on && nds->jit9) {
		const struct arec_stats *js = arec_stats(nds->jit9), *p = &prev_js;
		uint32_t live;
		size_t used, cap;

		arec_usage(nds->jit9, &live, &used, &cap);
		salsads_log("DATO jit: por cuadro (promedio de %u): %llu entradas, %llu bloques traducidos (%llu instr., %.1f palabras "
			    "PPC por instr., %u.%03u ms traduciendo), %llu enlaces nuevos; salidas al despachador: %llu por pc, %llu enlazables, "
			    "%llu con estado, %llu por presupuesto, %llu tras un acceso lento; caídas al intérprete: %llu llamadas desde el "
			    "bloque, %llu pasos del despachador (%llu sin código traducible); ruta lenta: %llu lecturas, %llu escrituras; "
			    "%llu escrituras a líneas con código, %llu bloques invalidados; en total: %u bloques vivos, código %u de %u KB, "
			    "%llu vaciados, %llu bloques descartados por el análisis de banderas, %llu banderas en frío",
			    n, (unsigned long long)((js->enters - p->enters) / n), (unsigned long long)((js->blocks_made - p->blocks_made) / n),
			    (unsigned long long)((js->guest_made - p->guest_made) / n),
			    js->guest_made > p->guest_made ? (double)(js->host_words - p->host_words) / (double)(js->guest_made - p->guest_made) : 0.0,
			    (unsigned)((js->compile_ns - p->compile_ns) / n / 1000000u), (unsigned)((js->compile_ns - p->compile_ns) / n / 1000u % 1000u),
			    (unsigned long long)((js->links - p->links) / n), (unsigned long long)((js->exits_pc - p->exits_pc) / n),
			    (unsigned long long)((js->exits_link - p->exits_link) / n), (unsigned long long)((js->exits_state - p->exits_state) / n),
			    (unsigned long long)((js->exits_budget - p->exits_budget) / n), (unsigned long long)((js->mem_exits - p->mem_exits) / n),
			    (unsigned long long)((js->callouts - p->callouts) / n), (unsigned long long)((js->interp_steps - p->interp_steps) / n),
			    (unsigned long long)((js->no_code - p->no_code) / n), (unsigned long long)((js->slow_rd - p->slow_rd) / n),
			    (unsigned long long)((js->slow_wr - p->slow_wr) / n), (unsigned long long)((js->code_wr - p->code_wr) / n),
			    (unsigned long long)((js->inval_blocks - p->inval_blocks) / n), (unsigned)live, (unsigned)(used >> 10),
			    (unsigned)(cap >> 10), (unsigned long long)js->flushes, (unsigned long long)js->bad_blocks,
			    (unsigned long long)js->cold_flags);
		{
			/* las caídas al intérprete por clase (lo que queda después de los caminos rápidos en línea) */
			char cl[512];
			size_t o = 0;

			for (unsigned k = 0; k < AREC_CALL_KINDS && o < sizeof cl - 48; k++)
				if (js->call_by[k] != p->call_by[k])
					o += (size_t)snprintf(cl + o, sizeof cl - o, "%s%s %llu", o ? ", " : "", arec_call_kind_name[k],
							      (unsigned long long)((js->call_by[k] - p->call_by[k]) / n));
			cl[o] = 0;
			salsads_log("DATO jit-llamadas: por cuadro y clase: %s", o ? cl : "ninguna");
		}
		{
			/* accesos lentos por región, por cuadro: cuántos (y, con JIT_TIMES_FLAG, µs cada uno) */
			static const struct {
				unsigned rg;
				const char *name;
			} rgs[] = { { 0, "GXFIFO" }, { 4, "E/S" }, { 2, "RAM" }, { 3, "WRAM" }, { 5, "paleta" }, { 6, "VRAM" }, { 7, "OAM" },
				    { 15, "BIOS" } };
			char sl[640];
			size_t o = 0;
			const double us = tb_hz ? 1e6 / (double)tb_hz : 0.0;

			for (unsigned k = 0; k < sizeof rgs / sizeof rgs[0] && o < sizeof sl - 80; k++) {
				const unsigned r = rgs[k].rg;
				const uint64_t nr = js->slow_rd_by[r] - p->slow_rd_by[r], nw = js->slow_wr_by[r] - p->slow_wr_by[r];

				if (!nr && !nw)
					continue;
				if (jit_times)
					o += (size_t)snprintf(sl + o, sizeof sl - o, "%s%s %llu lect. (%.2f µs) / %llu escr. (%.2f µs)", o ? ", " : "",
							      rgs[k].name, (unsigned long long)(nr / n),
							      nr ? (double)(js->t_rd_by[r] - p->t_rd_by[r]) * us / (double)nr : 0.0,
							      (unsigned long long)(nw / n),
							      nw ? (double)(js->t_wr_by[r] - p->t_wr_by[r]) * us / (double)nw : 0.0);
				else
					o += (size_t)snprintf(sl + o, sizeof sl - o, "%s%s %llu / %llu", o ? ", " : "", rgs[k].name,
							      (unsigned long long)(nr / n), (unsigned long long)(nw / n));
			}
			sl[o] = 0;
			{
				static uint32_t prev_idle9, prev_idle7;
				static uint64_t prev_dma_w, prev_dma_b, prev_cpu_w;
				char gx[320];
				const uint64_t ngx = js->gx_wr - p->gx_wr;
				uint64_t dw, db, cw;

				if (nds->st.gx_dma_words < prev_dma_w || nds->st.gx_cpu_words < prev_cpu_w) /* (juego nuevo: nds_init puso a cero) */
					prev_dma_w = prev_dma_b = prev_cpu_w = 0;
				dw = nds->st.gx_dma_words - prev_dma_w;
				db = nds->st.gx_dma_bursts - prev_dma_b;
				cw = nds->st.gx_cpu_words - prev_cpu_w;

				/* lo que ya no va por la ruta lenta: escrituras de la CPU a GXFIFO por la ruta corta del recompilador (con su
				 * costo si hay jit-tiempos), las palabras de la CPU por el bus corto (el intérprete desde los bloques: LDM/STM) y
				 * la DMA de modo 7 por ráfagas (su tiempo está dentro de las escrituras a la E/S que la arrancan y de los eventos «gx») */
				if (jit_times)
					snprintf(gx, sizeof gx, "GXFIFO por la ruta corta %llu escr. (%.2f µs), %llu palabras de la CPU en total, "
							      "%llu palabras de DMA en %llu ráfagas",
						 (unsigned long long)(ngx / n), ngx ? (double)(js->t_gx_wr - p->t_gx_wr) * us / (double)ngx : 0.0,
						 (unsigned long long)(cw / n), (unsigned long long)(dw / n), (unsigned long long)(db / n));
				else
					snprintf(gx, sizeof gx, "GXFIFO por la ruta corta %llu escr., %llu palabras de la CPU en total, "
							      "%llu palabras de DMA en %llu ráfagas",
						 (unsigned long long)(ngx / n), (unsigned long long)(cw / n), (unsigned long long)(dw / n),
						 (unsigned long long)(db / n));
				salsads_log("DATO jit-lentos: ARM9, accesos por la ruta lenta por cuadro y región (lecturas / escrituras): %s; "
					    "ya no van por la ruta lenta (por cuadro): %s%s; "
					    "bucles de espera saltados por cuadro: ARM9 %u, ARM7 %u",
					    o ? sl : "ninguno", gx, nds->gx_lento ? " [gx-lento: nada por las rutas cortas]" : "",
					    (unsigned)((nds->a9.jit_idle - prev_idle9) / n), (unsigned)((nds->a7.jit_idle - prev_idle7) / n));
				prev_idle9 = nds->a9.jit_idle;
				prev_idle7 = nds->a7.jit_idle;
				prev_dma_w = nds->st.gx_dma_words;
				prev_dma_b = nds->st.gx_dma_bursts;
				prev_cpu_w = nds->st.gx_cpu_words;
			}
			if (jit_times) {
				uint64_t rd = 0, wr = 0;
				const double ms = tb_hz ? 1e3 / (double)tb_hz / (double)n : 0.0;

				for (unsigned r = 0; r < 16; r++) {
					rd += js->t_rd_by[r] - p->t_rd_by[r];
					wr += js->t_wr_by[r] - p->t_wr_by[r];
				}
				wr += js->t_gx_wr - p->t_gx_wr; /* (las escrituras a GXFIFO por la ruta corta siguen contando como accesos lentos aquí: comparables con gx-lento) */
				{
					const uint64_t run = js->t_run - p->t_run, code = js->t_code - p->t_code, call = js->t_call - p->t_call;
					const uint64_t net = code > call + rd + wr ? code - call - rd - wr : 0;

					salsads_log("DATO jit-tiempo: ARM9 por cuadro (promedio de %u, ms): dentro del recompilador %.2f = código generado %.2f "
						    "+ accesos lentos %.2f (lecturas %.2f, escrituras %.2f) + intérprete desde los bloques %.2f + despachador, "
						    "interrupciones y traducción %.2f; entradas %llu (%.2f µs de código generado por entrada)",
						    n, (double)run * ms, (double)net * ms, (double)(rd + wr) * ms, (double)rd * ms, (double)wr * ms,
						    (double)call * ms, run > code ? (double)(run - code) * ms : 0.0,
						    (unsigned long long)((js->enters - p->enters) / n),
						    js->enters > p->enters ? (double)net * ms * 1000.0 * (double)n / (double)(js->enters - p->enters) : 0.0);
				}
			}
		}
		prev_js = *js;
	}
	if (jit_on && nds->jit7) {
		const struct arec_stats *js = arec_stats(nds->jit7), *p = &prev_js7;
		uint32_t live;
		size_t used, cap;

		arec_usage(nds->jit7, &live, &used, &cap);
		salsads_log("DATO jit7: ARM7, por cuadro (promedio de %u): %llu entradas, %llu bloques traducidos (%llu instr., %u.%03u ms "
			    "traduciendo); salidas: %llu por pc, %llu por presupuesto, %llu con estado, %llu tras un acceso lento; caídas al "
			    "intérprete: %llu llamadas desde el bloque, %llu pasos del despachador (%llu sin código traducible); ruta lenta: "
			    "%llu lecturas, %llu escrituras; %llu escrituras a líneas con código, %llu bloques invalidados; en total: %u bloques "
			    "vivos, código %u de %u KB, %llu vaciados, %llu descartados",
			    n, (unsigned long long)((js->enters - p->enters) / n), (unsigned long long)((js->blocks_made - p->blocks_made) / n),
			    (unsigned long long)((js->guest_made - p->guest_made) / n),
			    (unsigned)((js->compile_ns - p->compile_ns) / n / 1000000u), (unsigned)((js->compile_ns - p->compile_ns) / n / 1000u % 1000u),
			    (unsigned long long)((js->exits_pc - p->exits_pc) / n), (unsigned long long)((js->exits_budget - p->exits_budget) / n),
			    (unsigned long long)((js->exits_state - p->exits_state) / n), (unsigned long long)((js->mem_exits - p->mem_exits) / n),
			    (unsigned long long)((js->callouts - p->callouts) / n), (unsigned long long)((js->interp_steps - p->interp_steps) / n),
			    (unsigned long long)((js->no_code - p->no_code) / n), (unsigned long long)((js->slow_rd - p->slow_rd) / n),
			    (unsigned long long)((js->slow_wr - p->slow_wr) / n), (unsigned long long)((js->code_wr - p->code_wr) / n),
			    (unsigned long long)((js->inval_blocks - p->inval_blocks) / n), (unsigned)live, (unsigned)(used >> 10),
			    (unsigned)(cap >> 10), (unsigned long long)js->flushes, (unsigned long long)js->bad_blocks);
		prev_js7 = *js;
	}
#endif
	salsads_log("DATO MIPS: ARM9 %.2f, ARM7 %.2f (instrucciones entre el tiempo dentro de cada CPU: %u cuadros con perfil); "
		    "sobre el tiempo total de emulación: ARM9 %.2f, ARM7 %.2f, juntas %.2f",
		    mips9_cpu, mips7_cpu, (unsigned)acc_prof_n, mips9_tot, mips7_tot, mips9_tot + mips7_tot);
	prev_i9 = i9;
	prev_i7 = i7;
	stat_n = 0;
	for (int m = 0; m < M_COUNT; m++)
		nbuf[m] = 0;
	acc_gx_ticks = acc_gx_cycles = acc_gx_polys = acc_gx_verts = acc_gx_swaps = acc_frames = 0;
	acc_slices = acc_jumps = acc_events = acc_i9 = acc_i7 = acc_emu_ticks = acc_a9_ticks = acc_a7_ticks = acc_prof_n = 0;
	acc_i9p = acc_i7p = 0;
	memset(acc_evn, 0, sizeof acc_evn);
	memset(acc_evt, 0, sizeof acc_evt);
	memset(acc_evn_all, 0, sizeof acc_evn_all);
}

/* ---- motor de geometría en un SPE (Fase 3.5) ------------------------------------------------------------------------- */

/* El motor de tiempo (FIFO, GXSTAT, IRQ) sigue en el PPE; las órdenes que empiezan se anotan en un registro y el SPE las ejecuta
 * por lotes con el mismo código (src/gpu3d/gx_exec.c) y escribe los vértices y polígonos en el cuadro de salida. El PPE espera
 * al SPE solo cuando la CPU lee un resultado de la ejecución y en el SWAP_BUFFERS (docs/fase3.md, 3.5). */

/* recoge el resultado de los trabajos ya terminados (los resultados duran SPUKIT_QUEUE_LEN trabajos más) */
static void geom_collect(int block_to)
{
	while (geom_collect_seq <= geom_last_seq && (block_to >= (int)geom_collect_seq || spukit_done(wk_geom, geom_collect_seq))) {
		const spukit_result *r = spukit_wait(wk_geom, geom_collect_seq);
		job_geom_result jr;

		memcpy(&jr, r->data, sizeof jr);
		if (r->error != 0 || jr.rc != 0) {
			if (geom_acc.errors++ < 4)
				salsads_log("FALLO el SPE de geometría devolvió error %d / %d", r->error, jr.rc);
		}
		geom_acc.jobs++;
		geom_acc.ticks += r->ticks;
		geom_acc.cmds += jr.cmds;
		geom_acc.puts += jr.puts;
		for (int k = 0; k < 5; k++)
			geom_acc.prof[k] += jr.prof[k];
		if (r->ticks > geom_acc.ticks_max)
			geom_acc.ticks_max = r->ticks;
		geom_collect_seq++;
	}
}

static uint32_t geom_post(void *ctx, gx_t *g, uint32_t flags, const uint32_t *log, uint32_t nwords, gx_frame *f)
{
	job_geom j;

	(void)ctx;
	j.log_ea = (uint64_t)(uintptr_t)log;
	j.vtx_ea = (uint64_t)(uintptr_t)f->vtx;
	j.poly_ea = (uint64_t)(uintptr_t)f->poly;
	j.out_ea = (uint64_t)(uintptr_t)&g->ex.out;
	j.nwords = nwords;
	j.flags = flags;
	geom_collect(0);
	geom_last_seq = spukit_post(wk_geom, JOB_GEOM, &j, sizeof j);
	geom_acc.posts++;
	return geom_last_seq;
}

static void geom_wait(void *ctx, gx_t *g, uint32_t seq)
{
	const uint64_t t0 = now_us();

	(void)ctx;
	(void)g;
	if (!spukit_done(wk_geom, seq)) {
		geom_acc.waits++;
		spukit_wait(wk_geom, seq);
		geom_acc.wait_us += now_us() - t0;
	}
	geom_collect((int)seq);
}

static const gx_exec_ops geom_ops = { NULL, geom_post, geom_wait };

/* Autoprueba al arrancar: registros de órdenes pseudoaleatorios ejecutados en el PPE (cuadro entero) y en el SPE (dos trabajos,
 * ventanas pequeñas por DMA): vértices, polígonos, resultados y matrices tienen que ser idénticos. Devuelve 0 si coinciden. */
#define GST_WORDS 60000u
static uint32_t gst_log[GST_WORDS] __attribute__((aligned(128)));
static gx_frame gst_fa __attribute__((aligned(128))), gst_fb __attribute__((aligned(128)));
static gx_exec gst_ref __attribute__((aligned(128)));
static gx_exec_out gst_out __attribute__((aligned(128)));

static int geom_selftest(void)
{
	static const uint32_t seeds[3] = { 1, 2, 3 };
	const spukit_result *r;
	job_geom_result ir;
	uint32_t q;
	int bad = 0;
	job_geom j;

	memset(&j, 0, sizeof j);
	q = spukit_post(wk_geom, JOB_GEOM_INFO, &j, sizeof j);
	r = spukit_wait(wk_geom, q);
	memcpy(&ir, r->data, sizeof ir);
	if (r->error != 0) {
		salsads_log("autoprueba geometría: el SPE devolvió %d", r->error);
		return 1;
	}
	salsads_log("geometría: contexto del SPE %u bytes", ir.ctx_bytes);
	for (unsigned k = 0; k < 3; k++) {
		const uint32_t n = gx_selftest_gen(seeds[k], gst_log, GST_WORDS, k == 0 ? 6000u : 2500u);
		uint32_t cut = 0, diffs_v = 0, diffs_p = 0;
		uint64_t t0, tppe, tspe;

		while (cut < n / 2)
			cut += 1u + gx_cmd_np(gst_log[cut]);
		t0 = now_us();
		gx_exec_init(&gst_ref);
		gx_exec_newframe(&gst_ref);
		gst_ref.vtx = gst_fa.vtx;
		gst_ref.poly = gst_fa.poly;
		gx_exec_run(&gst_ref, gst_log, n);
		gx_exec_flush_mats(&gst_ref);
		tppe = now_us() - t0;
		memset(&gst_out, 0, sizeof gst_out);
		memset(gst_fb.vtx, 0, sizeof(gx_vertex) * 64);
		t0 = now_us();
		j.log_ea = (uint64_t)(uintptr_t)gst_log;
		j.vtx_ea = (uint64_t)(uintptr_t)gst_fb.vtx;
		j.poly_ea = (uint64_t)(uintptr_t)gst_fb.poly;
		j.out_ea = (uint64_t)(uintptr_t)&gst_out;
		j.nwords = cut;
		j.flags = GX_JOB_RESET;
		spukit_post(wk_geom, JOB_GEOM, &j, sizeof j);
		j.log_ea = (uint64_t)(uintptr_t)(gst_log + cut);
		j.nwords = n - cut;
		j.flags = GX_JOB_MATS;
		q = spukit_post(wk_geom, JOB_GEOM, &j, sizeof j);
		r = spukit_wait(wk_geom, q);
		tspe = now_us() - t0;
		if (r->error != 0) {
			salsads_log("autoprueba geometría: el SPE devolvió %d", r->error);
			return 1;
		}
		for (uint32_t i = 0; i < gst_ref.out.nverts; i++)
			diffs_v += memcmp(&gst_fa.vtx[i], &gst_fb.vtx[i], sizeof gst_fa.vtx[0]) != 0;
		for (uint32_t i = 0; i < gst_ref.out.npolys; i++)
			diffs_p += memcmp(&gst_fa.poly[i], &gst_fb.poly[i], sizeof gst_fa.poly[0]) != 0;
		{
			const int out_diff = memcmp(&gst_out, &gst_ref.out, sizeof gst_out) != 0;

			salsads_log("autoprueba geometría: registro %u (%u palabras): %u polígonos y %u vértices (%u recortados), vértices distintos %u, "
				    "polígonos distintos %u, resultados %s; SPE %u us, PPE %u us",
				    seeds[k], (unsigned)n, (unsigned)gst_ref.out.npolys, (unsigned)gst_ref.out.nverts,
				    (unsigned)gst_ref.out.st.clipped, (unsigned)diffs_v, (unsigned)diffs_p, out_diff ? "DISTINTOS" : "iguales",
				    (unsigned)tspe, (unsigned)tppe);
			bad |= diffs_v || diffs_p || out_diff || gst_ref.out.npolys < 100;
		}
	}
	return bad;
}

/* ---- render 3D en dos SPE (Fase 3.2) ------------------------------------------------------------------------------- */

/* Línea 0: el cuadro de geometría entregado en el último SWAP_BUFFERS se reparte en bandas (por costo) entre dos SPE; la capa
 * 3D sale en la instantánea del cuadro (set->img3d), que el SPE del motor A lee al componer. Línea 262 (r3d_wait, antes de entregar el 2D; la 192 si
 * el cuadro captura la pantalla): hay que esperarlo. docs/fase3.md, 3.2. */
static spukit_worker *wk3d[2];
static int r3d_on;
static struct {
	int valid, set;
	uint32_t seq[2];
	r3d_job job;
} r3q;
static uint32_t r3d_fail;

static void r3d_spe_submit(void *ctx, int set, r3d_job *j)
{
	(void)ctx;
	r3q.valid = 1;
	r3q.set = set;
	r3q.job = *j;
	r3d_post_us = now_us();
	for (uint32_t u = 0; u < 2; u++) {
		r3d_job jj = *j;

		jj.nunits = 2;
		jj.unit = u;
		r3q.seq[u] = spukit_post(wk3d[u], JOB_RASTER3D, &jj, sizeof jj);
	}
}

/* Autoprueba durante la prueba automática: la capa 3D de los SPE contra la referencia del PPE sobre el mismo cuadro */
static void r3d_spe_verify(int set)
{
	static uint32_t ref[R3D_W * R3D_H] __attribute__((aligned(128)));
	const struct nds_vset *s = &nds->vid.set[set];
	const r3d_job *j = &r3q.job;
	int diffs = 0, first = -1;

	r3d_render_ref((j->flags & R3D_F_BLANK) ? NULL : (const gx_frame *)(uintptr_t)j->frame_ea, s->vram, j->texslot, j->palslot,
		       j->flags, ref);
	for (int i = 0; i < R3D_W * R3D_H; i++)
		if (ref[i] != s->img3d[i]) {
			if (first < 0)
				first = i;
			diffs++;
		}
	if (diffs) {
		salsads_log("FALLO el render 3D de los SPE difiere de la referencia del PPE en %d píxeles (primero en %d,%d)", diffs,
			    first % R3D_W, first / R3D_W);
		failures++;
	}
}

static void r3d_spe_wait(void *ctx)
{
	const uint64_t t0 = now_us();

	(void)ctx;
	if (!r3q.valid)
		return;
	for (uint32_t u = 0; u < 2; u++) {
		const int was_done = spukit_done(wk3d[u], r3q.seq[u]);
		const spukit_result *r = spukit_wait(wk3d[u], r3q.seq[u]);
		job_r3d_result res;

		if (u == 0)
			acc_r3d_pre_us += t0 - r3d_post_us;
		acc_r3d_lat_us[u] += now_us() - r3d_post_us;
		acc_r3d_early += (uint32_t)was_done;

		memcpy(&res, r->data, sizeof res);
		if (r->error != 0 || res.rc != 0) {
			if (r3d_fail++ < 4)
				salsads_log("FALLO el SPE %u del render 3D devolvió error %d / %d", u, r->error, res.rc);
			continue;
		}
		put(u ? M_R3D1 : M_R3D0, (uint32_t)((uint64_t)r->ticks * 1000000ull / spukit_tick_hz()));
		{
			uint32_t *dst = (uint32_t *)(void *)&acc_r3d;
			const uint32_t *src = (const uint32_t *)(const void *)&res.st;

			for (size_t k = 0; k < sizeof(r3d_stats) / 4; k++)
				dst[k] += src[k];
		}
	}
	put(M_R3DW, (uint32_t)(now_us() - t0));
	acc_r3d_frames++;
	r3q.valid = 0;
	if (autotest_verify && frame_no % 30u == 12u)
		r3d_spe_verify(r3q.set);
}

/* Imagen de la autoprueba para comparar en la Mac (solo cuando falla) */
static void r3d_dump_ppm(uint32_t seed, const char *who, const uint32_t *img)
{
	char path[128];
	FILE *f;

	snprintf(path, sizeof path, SALSADS_DATA_DIR "/r3d-%u-%s.ppm", (unsigned)seed, who);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n256 192\n255\n");
	for (int i = 0; i < R3D_W * R3D_H; i++) {
		const uint32_t q = img[i];
		const uint8_t rgb[3] = { (uint8_t)(R3D_PX_R(q) << 2), (uint8_t)(R3D_PX_G(q) << 2), (uint8_t)(R3D_PX_B(q) << 2) };

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	salsads_log("autoprueba 3D: imagen en %s", path);
}

/* Autoprueba al arrancar: escenas sintéticas (el motor de geometría real arma el cuadro) en los dos SPE contra la
 * referencia del PPE. 0 = iguales. */
static int r3d_spe_selftest(void)
{
	static gx_t g;
	static uint8_t vram[R3D_VRAM_ZERO_OFF + 128] __attribute__((aligned(128)));
	static uint32_t ref[R3D_W * R3D_H] __attribute__((aligned(128))), out[R3D_W * R3D_H] __attribute__((aligned(128)));
	static const uint32_t seeds[] = { 3, 8, 21, 40 };
	int bad = 0;

	/* primero la aritmética de r3d_px.h, función por función */
	{
		const uint32_t seed = 12345;
		const uint32_t q = spukit_post(wk3d[0], JOB_RASTER3D_PROBE, &seed, sizeof seed);
		const spukit_result *r = spukit_wait(wk3d[0], q);
		job_r3d_result res;

		memcpy(&res, r->data, sizeof res);
		for (uint32_t k = 0; k < R3D_NPROBES; k++) {
			const uint32_t ppe = r3d_math_probe(seed, k);

			if (res.probe[k] != ppe) {
				/* 4 y 5 (desplazamientos de 64 bits con signo) son informativas: r3d_px.h no los usa */
				salsads_log("autoprueba 3D: la sonda de aritmética %u da %08x en el SPE y %08x en el PPE%s", k, res.probe[k], ppe,
					    (k == 4 || k == 5) ? " (informativa)" : "");
				if (k != 4 && k != 5)
					bad = 1;
			}
		}
		if (!bad)
			salsads_log("autoprueba 3D: sondas de aritmética iguales en el SPE y el PPE");
	}

	for (unsigned k = 0; k < sizeof seeds / sizeof seeds[0]; k++) {
		r3d_job j;
		const gx_frame *f;
		uint32_t ticks[2] = { 0, 0 }, diffs = 0;
		uint64_t tp;

		memset(&j, 0, sizeof j);
		f = r3d_scene_build(&g, seeds[k], k == 0 ? 2000u : 300u, vram, j.texslot, j.palslot);
		tp = now_us();
		r3d_render_ref(f, vram, j.texslot, j.palslot, 0, ref);
		tp = now_us() - tp;
		j.frame_ea = (uint64_t)(uintptr_t)f;
		j.vram_ea = (uint64_t)(uintptr_t)vram;
		j.out_ea = (uint64_t)(uintptr_t)out;
		j.band = 16;
		j.flags = f ? 0u : R3D_F_BLANK;
		memset(out, 0xA5, sizeof out);
		r3d_spe_submit(NULL, 0, &j);
		for (uint32_t u = 0; u < 2; u++) {
			const spukit_result *r = spukit_wait(wk3d[u], r3q.seq[u]);
			job_r3d_result res;

			memcpy(&res, r->data, sizeof res);
			ticks[u] = r->ticks;
			if (r->error || res.rc) {
				salsads_log("autoprueba 3D: el SPE %u devolvió %d / %d", u, r->error, res.rc);
				bad = 1;
			}
			if (k == 0 && u == 0)
				salsads_log("render 3D: contexto del SPE %u bytes", res.ctx_bytes);
		}
		r3q.valid = 0;
		for (int i = 0; i < R3D_W * R3D_H; i++)
			diffs += out[i] != ref[i];
		salsads_log("autoprueba 3D: escena %u (%u polígonos): %u píxeles distintos; SPE0 %u us, SPE1 %u us, referencia del PPE %u us",
			    seeds[k], f ? f->npolys : 0u, diffs, (unsigned)((uint64_t)ticks[0] * 1000000ull / spukit_tick_hz()),
			    (unsigned)((uint64_t)ticks[1] * 1000000ull / spukit_tick_hz()), (unsigned)tp);
		if (diffs) {
			int shown = 0;

			bad = 1;
			for (int i = 0; i < R3D_W * R3D_H && shown < 6; i++)
				if (out[i] != ref[i]) {
					salsads_log("autoprueba 3D: (%d,%d) SPE %08x, PPE %08x", i % R3D_W, i / R3D_W, out[i], ref[i]);
					shown++;
				}
			r3d_dump_ppm(seeds[k], "spe", out);
			r3d_dump_ppm(seeds[k], "ppe", ref);
		}
	}
	return bad;
}

static void render_frame(void)
{
	unsigned tx, ty;
	uint32_t k;
	const int set = (int)(frame_no & 1u);
	const int profiled = (int)(frame_no & 1u);
	struct sds_rect r[2];
	uint64_t i9a, i7a, tb0, tb1, tb2, tb3, tb4, tb5, tb6, tb7;

	sysUtilCheckCallback();
	tb0 = tb_clock();
	rsxkit_frame_begin(CLEAR_RGB);
	tb1 = tb_clock();
	{
		const uint64_t t = now_us();
		const uint32_t iv = last_begin_us ? (uint32_t)(t - last_begin_us) : 16667;

		last_begin_us = t;
		cur_stats.frame_us = iv;
		put(M_IV, iv);
	}
	{
		float dt = cur_stats.frame_us ? (float)cur_stats.frame_us / 1e6f : 1.0f / 60.0f;

		if (dt < 1.0f / 120.0f)
			dt = 1.0f / 120.0f;
		if (dt > 1.0f / 20.0f)
			dt = 1.0f / 20.0f;
		k = poll_input(dt, &tx, &ty);
	}
	nds_set_input(nds, k, tx, ty);
	if (snd_on)
		nds_snd_set_ratio_adj(nds, audio_out_ratio_adj());
	i9a = nds->a9.icount;
	i7a = nds->a7.icount;
	{
		const uint64_t s0 = nds->st.slices, j0 = nds->st.jumps, e0 = nds->st.events;
		const uint64_t pa9 = nds->st.prof_a9, pa7 = nds->st.prof_a7, pev = nds->st.prof_ev;
		uint64_t evn0[EV_COUNT], evt0[EV_COUNT];

		memcpy(evn0, nds->st.ev_n, sizeof evn0);
		memcpy(evt0, nds->st.ev_t, sizeof evt0);

		const uint64_t gt0 = nds->gx.st.host_ticks, gc0 = nds->gx.st.cycles, gs0 = nds->gx.st.swaps;
		const uint64_t gp0 = nds->gx.st.frame_polys, gv0 = nds->gx.st.frame_verts;

		nds->prof_clock = profiled ? tb_clock : NULL;
		nds->gx.clock = profiled ? tb_clock : NULL; /* motor de geometría: su tiempo sale en «DATO geometria» */
		tb2 = tb_clock();
		nds_run_frame(nds);
		tb3 = tb_clock();
		nds->prof_clock = NULL;
		nds->gx.clock = NULL;
		acc_gx_ticks += nds->gx.st.host_ticks - gt0;
		acc_gx_cycles += nds->gx.st.cycles - gc0;
		acc_gx_swaps += nds->gx.st.swaps - gs0;
		acc_gx_polys += nds->gx.st.frame_polys - gp0;
		acc_gx_verts += nds->gx.st.frame_verts - gv0;
		acc_frames++;
		for (int k = 0; k < EV_COUNT; k++) {
			acc_evn_all[k] += nds->st.ev_n[k] - evn0[k];
			if (profiled) {
				acc_evn[k] += nds->st.ev_n[k] - evn0[k];
				acc_evt[k] += nds->st.ev_t[k] - evt0[k];
			}
		}
		acc_slices += nds->st.slices - s0;
		acc_jumps += nds->st.jumps - j0;
		acc_events += nds->st.events - e0;
		put(profiled ? M_EMU_P : M_EMU, tb_us(tb3 - tb2));
		if (profiled) {
			put(M_A9, tb_us(nds->st.prof_a9 - pa9));
			put(M_A7, tb_us(nds->st.prof_a7 - pa7));
			put(M_EV, tb_us(nds->st.prof_ev - pev));
			acc_a9_ticks += nds->st.prof_a9 - pa9;
			acc_a7_ticks += nds->st.prof_a7 - pa7;
			acc_i9p += nds->a9.icount - i9a;
			acc_i7p += nds->a7.icount - i7a;
			acc_prof_n++;
		}
	}
	cur_stats.emu_us = tb_us(tb3 - tb2);
	cur_stats.mips9 = (double)(nds->a9.icount - i9a) / (double)(cur_stats.emu_us ? cur_stats.emu_us : 1);
	cur_stats.mips7 = (double)(nds->a7.icount - i7a) / (double)(cur_stats.emu_us ? cur_stats.emu_us : 1);
	acc_i9 += nds->a9.icount - i9a;
	acc_i7 += nds->a7.icount - i7a;
	acc_emu_ticks += tb3 - tb2;
	tb4 = tb_clock();
	{
		int swap = 1, show = 1, dset = set;

		if (spu_on) {
			/* el 2D de esta pasada se entregó en la línea 262, casi al final de la emulación: se recoge el de la pasada
			 * anterior (los SPE lo compusieron mientras se emulaba esta) y el de esta se recoge en la siguiente */
			show = prev.valid;
			if (show) {
				vid_collect(&prev);
				dset = prev.set;
				swap = (nds->vid.set[prev.vset].powcnt1 & 0x8000u) != 0;
				for (int e = 0; e < 2; e++) {
					const uint32_t *pp = (const uint32_t *)(const void *)&prev.res[e].prof;

					if (prof_n < STATS_FRAMES)
						for (int q = 0; q < 12; q++)
							prof_buf[e][q][prof_n] = pp[q];
				}
				if (prof_n < STATS_FRAMES)
					prof_n++;
				put(M_SPEA, (uint32_t)((uint64_t)prev.res[0].ticks * 1000000ull / spukit_tick_hz()));
				put(M_SPEB, (uint32_t)((uint64_t)prev.res[1].ticks * 1000000ull / spukit_tick_hz()));
				put(M_WAIT, prev.wait_us);
				if (autotest_verify && frame_no % 30u == 20u)
					vid_verify(&prev);
			}
			prev = sub;
			sub.valid = 0;
		} else {
			/* sin SPE: el compositor del host dibuja el cuadro y se copia a la textura */
			nds_video_render_host(nds);
			if (set_used[set])
				rsxkit_fence_wait(set_fence[set]);
			memcpy(tex_px(set, 0), nds->vid.img[0], TEX_W * TEX_H * 2);
			memcpy(tex_px(set, 1), nds->vid.img[1], TEX_W * TEX_H * 2);
			if (nds->vid.last >= 0)
				swap = (nds->vid.set[nds->vid.last].powcnt1 & 0x8000u) != 0;
		}
		tb5 = tb_clock();
		tb6 = tb_clock();
		sds_layout_compute(layout, SDS_SCALE_FIT, cur_w, cur_h, r);
		if (show)
			draw_screens(dset, r, swap);
		draw_ui(r, &cur_stats);
		if (show) {
			set_fence[dset] = rsxkit_fence();
			set_used[dset] = 1;
		}
		if (show)
			last_dset = dset;
	}
	tb7 = tb_clock();
	rsxkit_frame_end();
	put(M_BEGIN, tb_us(tb1 - tb0));
	put(M_INPUT, tb_us(tb2 - tb1));
	put(M_SCR, tb_us(tb5 - tb4));
	put(M_CONV, tb_us(tb6 - tb5));
	put(M_DRAW, tb_us(tb7 - tb6));
	put(M_END, tb_us(tb_clock() - tb7));
	frame_no++;
	vid_frames_seen = frame_no;
	save_flush(0);
	if (++stat_n >= STATS_FRAMES)
		log_stats();
}

/* Prueba automática: la ROM de carga tiene que estar viva en las dos CPUs, sin errores. */
static uint32_t autotest_frames = AUTOTEST_FRAMES;

/* Con una ROM externa (rom.nds) el autotest no comprueba la ROM de carga: solo corre los cuadros que diga frames.txt (una
 * cifra) para tomar mediciones en RPCS3; el RESULTADO sigue la autoprueba del audio y del SPE. */
static void autotest_check(void)
{
	if (rom_buf != (uint8_t *)carga_bin) {
		salsads_log("autoprueba: ROM externa (%s), %u cuadros de la DS: sin las comprobaciones de la ROM de carga", rom_name,
			    (unsigned)nds->frames);
		return;
	}
	const uint32_t magic = nds_peek32(nds, NDS_CPU9, 0x023A0000u);
	const uint32_t i9 = nds_peek32(nds, NDS_CPU9, 0x023A0004u), b9 = nds_peek32(nds, NDS_CPU9, 0x023A0008u);
	const uint32_t i7 = nds_peek32(nds, NDS_CPU9, 0x023A000Cu), b7 = nds_peek32(nds, NDS_CPU9, 0x023A0010u);

	if (magic != CARGA_MAGIC)
		fail("el ARM9 de la ROM de carga no dejó su marca");
	if (i9 < 10)
		fail("el ARM9 no completó iteraciones de la ROM de carga");
	if (i7 < 5)
		fail("el ARM7 no completó iteraciones de la ROM de carga");
	if (b9 || b7)
		fail("alguna iteración de la ROM de carga dio un resultado distinto del esperado");
	if (nds->frames < AUTOTEST_FRAMES - 2)
		fail("la DS no corrió todos los cuadros");
	/* la pantalla de arriba no es lisa (el texto y la barra de la ROM de carga) */
	if (last_dset >= 0) {
		const uint16_t *a = tex_px(last_dset, 0);
		int distinct = 0;

		for (int y = 0; y < TEX_H && distinct < 3; y++)
			for (int x = 0; x < TEX_W && distinct < 3; x++)
				if (a[y * (TEX_PITCH / 2) + x] != a[0])
					distinct++;
		if (distinct < 3)
			fail("la pantalla de arriba de la ROM de carga está lisa");
	} else {
		fail("no se dibujó ningún cuadro");
	}
	salsads_log("autoprueba: cuadros de la DS %u, iteraciones ARM9 %u (errores %u), ARM7 %u (errores %u)", (unsigned)nds->frames,
		    (unsigned)i9, (unsigned)b9, (unsigned)i7, (unsigned)b7);
}

/* ---- estadísticas por juego ------------------------------------------------------------------------------------------- */

/* Todo lo que se acumula por juego vuelve a cero al abrir uno (el núcleo también: nds_init). `prev_silent` es del audio, que
 * sigue abierto entre juegos, y no se toca. */
static void stats_reset(void)
{
	memset(buf, 0, sizeof buf);
	memset(nbuf, 0, sizeof nbuf);
	stat_n = 0;
	last_begin_us = 0;
	memset(&cur_stats, 0, sizeof cur_stats);
	acc_gx_ticks = acc_gx_cycles = acc_gx_polys = acc_gx_verts = acc_gx_swaps = acc_frames = 0;
	acc_slices = acc_jumps = acc_events = acc_i9 = acc_i7 = acc_emu_ticks = acc_a9_ticks = acc_a7_ticks = acc_prof_n = 0;
	acc_i9p = acc_i7p = 0;
	memset(acc_evn, 0, sizeof acc_evn);
	memset(acc_evt, 0, sizeof acc_evt);
	memset(acc_evn_all, 0, sizeof acc_evn_all);
	memset(&acc_r3d, 0, sizeof acc_r3d);
	acc_r3d_frames = acc_r3d_early = 0;
	r3d_post_us = acc_r3d_pre_us = acc_r3d_lat_us[0] = acc_r3d_lat_us[1] = 0;
	r3d_fail = 0;
	memset(&geom_acc, 0, sizeof geom_acc);
	memset(&snd_acc, 0, sizeof snd_acc);
	memset(prof_buf, 0, sizeof prof_buf);
	prof_n = 0;
	prev_i9 = prev_i7 = 0;
#ifdef NUCLEO_JIT
	memset(&prev_js, 0, sizeof prev_js);
#endif
}

/* ---- memoria: informe al cerrar cada juego ------------------------------------------------------------------------------ */

/* Qué hay reservado por subsistema (KB) y cuánto cambió frente al cierre anterior y frente al segundo cierre (el primero
 * todavía reserva lo que el juego pide «para siempre» a mitad de la partida). Lo que cambia de un cierre al siguiente es una
 * fuga o fragmentación: el heap de newlib en uso, la memoria de usuario del sistema que queda libre y el tope del heap. */
struct mem_close_in {
	size_t rom_pages; /* bytes de páginas del sistema que el juego tenía (ROM entera o caché de fragmentos), antes de soltarlos */
	size_t rom_len;
};

struct mem_snap {
	int used, free_, blocks, arena;
	long top;
	uint32_t user_free, user_total;
};

#define MEM_TOL_BYTES 4096
static unsigned mem_closes;
static struct mem_snap mem_prev, mem_base;

static void mem_take(struct mem_snap *m)
{
	static struct {
		u32 total;
		u32 available;
	} um __attribute__((aligned(16)));
	const struct mallinfo mi = mallinfo();

	lv2syscall1(352, (u64)(uintptr_t)&um);
	m->used = mi.uordblks;
	m->free_ = mi.fordblks;
	m->blocks = mi.ordblks;
	m->arena = mi.arena;
	m->top = (long)(uintptr_t)sbrk(0);
	if ((s32)p1 == 0) {
		m->user_free = um.available;
		m->user_total = um.total;
	} else {
		m->user_free = m->user_total = 0;
	}
}

static void mem_report(const char *when)
{
	struct mem_snap m;

	mem_take(&m);
	salsads_log("memoria %s: heap %d KB (%d en uso, %d libres en %d bloques, tope %p), usuario %u de %u KB libres, páginas del "
		    "sistema en uso por la aplicación %u KB", when, m.arena >> 10, m.used >> 10, m.free_ >> 10, m.blocks, (void *)m.top,
		    m.user_free >> 10, m.user_total >> 10, (unsigned)(big_bytes() >> 10));
}

static void mem_close_report(const struct mem_close_in *in)
{
	struct mem_snap m;
	const int spes = (wk[0] ? 2 : 0) + (wk_snd ? 1 : 0) + (wk3d[0] ? 1 : 0) + (wk3d[1] ? 1 : 0) + (wk_geom ? 1 : 0);
#ifdef NUCLEO_JIT
	const unsigned jit_kb = jit_on ? REC_CACHE_BYTES >> 10 : 0u;
#else
	const unsigned jit_kb = 0;
#endif

	mem_take(&m);
	mem_closes++;
	salsads_log("memoria, reservado por subsistema (KB): estructura de la máquina (fija) %u, texturas del RSX (fija) 1024, "
		    "interfaz del RSX (fija) 2048, SPE (%d, persistentes), caché de código del recompilador (en el texto, fija) %u, "
		    "ROM del juego cerrado %u (páginas del sistema que tenía, soltadas al cerrar: %u), páginas del sistema que siguen "
		    "en uso %u",
		    (unsigned)(sizeof *nds >> 10), spes, jit_kb, (unsigned)(in->rom_len >> 10), (unsigned)(in->rom_pages >> 10),
		    (unsigned)(big_bytes() >> 10));
	if (mem_closes > 1)
		salsads_log("memoria, cambio frente al cierre anterior (juego %u): heap en uso %+d KB, libres %+d KB en %+d bloques, "
			    "arena %+d KB, tope %+ld KB, usuario libre %+d KB",
			    mem_closes, (m.used - mem_prev.used) >> 10, (m.free_ - mem_prev.free_) >> 10, m.blocks - mem_prev.blocks,
			    (m.arena - mem_prev.arena) >> 10, (m.top - mem_prev.top) >> 10,
			    ((int)m.user_free - (int)mem_prev.user_free) >> 10);
	if (mem_closes == 2)
		mem_base = m;
	if (mem_closes > 2) {
		const int d_used = m.used - mem_base.used, d_user = (int)m.user_free - (int)mem_base.user_free;
		const int leak = d_used > MEM_TOL_BYTES || d_user < -MEM_TOL_BYTES;

		salsads_log("memoria, acumulado desde el segundo cierre (%u juegos): heap en uso %+d bytes, tope %+ld KB, usuario libre "
			    "%+d bytes: %s (tolerancia %d bytes)", mem_closes - 2, d_used, (m.top - mem_base.top) >> 10, d_user,
			    leak ? "FUGA" : "sin fuga", MEM_TOL_BYTES);
		if (leak && autotest)
			fail("la memoria crece entre aperturas y cierres de juegos");
	}
	mem_prev = m;
}

/* ---- abrir y cerrar un juego ------------------------------------------------------------------------------------------- */

/* Prepara la máquina para `path` (NULL = ROM de carga). Todo lo grande de tamaño fijo se reserva una vez al arrancar la
 * aplicación (la estructura `nds`, las texturas del RSX, los SPE, la caché del recompilador); aquí solo se reinicia, y lo que
 * depende de la ROM se pide y se suelta con el juego. 0 = bien. */
static int game_open(const char *path)
{
	stats_reset();
	memset(&sub, 0, sizeof sub);
	memset(&prev, 0, sizeof prev);
	sub_count = 0;
	frame_no = 0;
	save_last_frame = 0;
	combo_prev = 0;
	exit_t0 = 0;
	exit_game = 0;
	exit_progress = 0.0f;
	last_dset = -1;
	memset(set_used, 0, sizeof set_used);
	if (rom_open(path) != 0)
		return -1;
	nds_init(nds); /* deja la máquina en blanco; los ganchos (SPE, sonido) y el recompilador se conservan */
	if (geom_on)
		gx_set_executor(&nds->gx, &geom_ops);
	snd_load_cfg();
	sds_input_init(&input);
	memset(&keys, 0, sizeof keys);
	if ((rom_file ? nds_load_rom_paged(nds, rom_file_rd, rom_file, rom_len, rom_page_shift(), (size_t)ROM_CACHE_MB << 20)
		      : nds_load_rom(nds, rom_buf, rom_len)) != 0 || nds_boot_direct(nds) != 0) {
		salsads_log("FALLO la ROM no es válida (cabecera o binarios)");
		nds_rom_free(nds);
		nds->cart.rom = NULL;
		rom_close();
		return -1;
	}
	save_load();
	salsads_log("ROM: %s; entrada ARM9 0x%08x, ARM7 0x%08x; cuántos bytes ocupa el núcleo: %u", rom_name,
		    (unsigned)rd32le(rom_buf + 0x24), (unsigned)rd32le(rom_buf + 0x34), (unsigned)sizeof *nds);
#ifdef NUCLEO_JIT
	if (jit_on && (nds->jit9 || nds->jit7)) {
		jit_clocks();
		salsads_rec_marker(JIT_MARKER, NUCLEO_VERSION, 1);
	}
#endif
	return 0;
}

/* Cierra el juego: espera lo que los SPE y el RSX tengan en vuelo, escribe el guardado y suelta lo que dependía de la ROM. Los
 * SPE quedan en espera de trabajos, listos para el juego siguiente. */
static void game_close(void)
{
	struct mem_close_in in = { big_bytes(), rom_len };

	salsads_log("cierre: guardado");
	save_flush(1);
	salsads_log("cierre: esperando sonido, geometría y 3D");
	if (snd_on && nds->snd.prev >= 0)
		snd_wait(NULL, nds->snd.prev);
	if (geom_on) {
		gx_sync(&nds->gx, 0);
		geom_collect((int)geom_last_seq);
	}
	if (r3d_on)
		r3d_spe_wait(NULL);
	salsads_log("cierre: esperando la composición 2D y el RSX");
	if (spu_on) {
		vid_collect(&sub);
		vid_collect(&prev);
	}
	memset(&sub, 0, sizeof sub);
	memset(&prev, 0, sizeof prev);
	for (int i = 0; i < 2; i++)
		if (set_used[i]) {
			rsxkit_fence_wait(set_fence[i]); /* el RSX ya no lee las texturas */
			set_used[i] = 0;
		}
#ifdef NUCLEO_JIT
	if (jit_on) {
		salsads_log("cierre: vaciando la caché del recompilador");
		if (nds->jit9)
			arec_flush(nds->jit9); /* el código traducido era de este juego */
		if (nds->jit7)
			arec_flush(nds->jit7);
		salsads_rec_marker(JIT_MARKER, NUCLEO_VERSION, 0);
	}
#endif
	salsads_log("cierre: soltando la ROM");
	nds_rom_free(nds);
	nds->cart.rom = NULL;
	rom_close();
	mem_close_report(&in);
}

/* ---- selector de ROM ---------------------------------------------------------------------------------------------------- */

#define LAST_ROM_PATH SALSADS_DATA_DIR "/ultima-rom.txt"
#define MAX_ROMS 256
#define NAME_MAX_LEN 120

struct rom_ent {
	char name[NAME_MAX_LEN];
	unsigned long size;
};
static struct rom_ent rom_list[MAX_ROMS];
static int rom_count;
static char sel_msg[160];
static uint64_t sel_msg_until;

static int ends_nds(const char *n)
{
	const size_t l = strlen(n);

	return l > 4 && n[l - 4] == '.' && (n[l - 3] | 0x20) == 'n' && (n[l - 2] | 0x20) == 'd' && (n[l - 1] | 0x20) == 's';
}

static int name_cmp(const void *a, const void *b)
{
	const char *x = ((const struct rom_ent *)a)->name, *y = ((const struct rom_ent *)b)->name;

	for (; *x && *y; x++, y++) {
		const int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;

		if (cx != cy)
			return cx - cy;
	}
	return (unsigned char)*x - (unsigned char)*y;
}

/* Lista los *.nds de SALSADS_DATA_DIR ordenados por nombre (newlib: opendir; nada de libsysfs, que cuelga la consola). */
static void rom_scan(void)
{
	DIR *d = opendir(SALSADS_DATA_DIR);
	struct dirent *de;

	rom_count = 0;
	if (!d) {
		salsads_log("selector: no pude abrir " SALSADS_DATA_DIR);
		return;
	}
	while ((de = readdir(d)) != NULL && rom_count < MAX_ROMS) {
		char full[NAME_MAX_LEN + 40];
		struct stat st;

		if (de->d_name[0] == '.' || !ends_nds(de->d_name) || strlen(de->d_name) >= NAME_MAX_LEN)
			continue;
		snprintf(full, sizeof full, SALSADS_DATA_DIR "/%s", de->d_name);
		if (stat(full, &st) != 0 || S_ISDIR(st.st_mode))
			continue;
		snprintf(rom_list[rom_count].name, NAME_MAX_LEN, "%s", de->d_name);
		rom_list[rom_count].size = (unsigned long)st.st_size;
		rom_count++;
	}
	closedir(d);
	qsort(rom_list, (size_t)rom_count, sizeof rom_list[0], name_cmp);
	salsads_log("selector: %d ROM(s) en " SALSADS_DATA_DIR, rom_count);
}

static void last_rom_load(char *out, size_t n)
{
	FILE *f = fopen(LAST_ROM_PATH, "r");

	out[0] = 0;
	if (!f)
		return;
	if (fgets(out, (int)n, f)) {
		size_t l = strlen(out);

		while (l && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' '))
			out[--l] = 0;
	}
	fclose(f);
}

static void last_rom_save(const char *name)
{
	FILE *f = fopen(LAST_ROM_PATH, "w");

	if (f) {
		fprintf(f, "%s\n", name);
		fclose(f);
	}
}

#define SEL_ROW_H 30.0f
#define SEL_TOP 150.0f
#define SEL_ROWS 12

static void sel_draw(int cur, int top, int n, int embedded)
{
	char line[200];

	rsxkit_ui_begin(UI_W, UI_H);
	rsxkit_ui_text(60, 28, "SalsaDS", 3.0f, 0xFFFFFFFF, 0);
	snprintf(line, sizeof line, "%s", NUCLEO_VERSION);
	rsxkit_ui_text(60 + 8 * 3 * 8 + 20, 44, line, 1.4f, 0x909090FF, 0);
	rsxkit_ui_text(60, 100, embedded ? "No hay archivos .nds en " SALSADS_DATA_DIR "/" : "Elige un juego", 1.6f, 0xC0FFC0FF, 0);
	for (int r = 0; r < SEL_ROWS && top + r < n; r++) {
		const int i = top + r;
		const float y = SEL_TOP + (float)r * SEL_ROW_H;
		char shown[80];

		if (embedded) {
			snprintf(shown, sizeof shown, "ROM de prueba integrada");
			line[0] = 0;
		} else {
			const size_t l = strlen(rom_list[i].name);

			if (l > 62)
				snprintf(shown, sizeof shown, "%.59s...", rom_list[i].name);
			else
				snprintf(shown, sizeof shown, "%s", rom_list[i].name);
			snprintf(line, sizeof line, "%lu.%01lu MB", rom_list[i].size >> 20, (rom_list[i].size & 0xFFFFFul) * 10 >> 20);
		}
		if (i == cur)
			rsxkit_ui_rect(50, y - 4, UI_W - 100, SEL_ROW_H - 2, 0x305080FF);
		rsxkit_ui_text(64, y, shown, 1.8f, i == cur ? 0xFFFFFFFF : 0xB8B8B8FF, 0);
		if (line[0])
			rsxkit_ui_text(UI_W - 64 - 8 * 1.8f * (float)strlen(line), y, line, 1.8f, i == cur ? 0xFFFFFFFF : 0x808080FF, 0);
	}
	if (top > 0)
		rsxkit_ui_text(UI_W - 120, SEL_TOP - 28, "mas arriba", 1.2f, 0x909090FF, 0);
	if (top + SEL_ROWS < n)
		rsxkit_ui_text(UI_W - 120, SEL_TOP + SEL_ROWS * SEL_ROW_H, "mas abajo", 1.2f, 0x909090FF, 0);
	if (sel_msg[0] && now_us() < sel_msg_until)
		rsxkit_ui_text(60, UI_H - 130, sel_msg, 1.5f, 0xFF8080FF, 0);
	rsxkit_ui_text(60, UI_H - 90, "Arriba / Abajo: elegir      X: abrir      O: salir de SalsaDS", 1.5f, 0xFFFFFFFF, 0);
	rsxkit_ui_text(60, UI_H - 62, "Dentro del juego: mantener SELECT + START durante 1 s para volver aqui", 1.3f, 0xC0C0C0FF, 0);
	snprintf(line, sizeof line, "%d juego(s)", embedded ? 0 : n);
	rsxkit_ui_text(60, UI_H - 36, line, 1.2f, 0x808080FF, 0);
	rsxkit_ui_end();
}

/* Muestra la lista y devuelve 1 con `out` = nombre elegido (cadena vacía = ROM de prueba integrada), o 0 si hay que salir. */
static int select_rom(char *out, size_t outsz)
{
	char last[NAME_MAX_LEN];
	int cur = 0, top = 0, embedded, rep_dir = 0;
	uint32_t held_prev, btn;
	uint64_t next_rep = 0;

	rom_scan();
	embedded = rom_count == 0;
	last_rom_load(last, sizeof last);
	for (int i = 0; i < rom_count; i++)
		if (!strcmp(rom_list[i].name, last))
			cur = i;
	const int n = embedded ? 1 : rom_count;

	pad_read();
	held_prev = ds3.buttons; /* lo que ya estaba apretado (la salida del juego, la B del juego anterior) no cuenta */
	if (cur >= SEL_ROWS)
		top = cur - SEL_ROWS / 2;
	while (!quit) {
		const uint64_t t = now_us();
		int dir = 0, now_dir;

		sysUtilCheckCallback();
		rsxkit_frame_begin(CLEAR_RGB);
		pad_read();
		btn = ds3.buttons;
		if ((btn & (1u << SDS_DS3_UP)) || ds3.ly < 64)
			now_dir = -1;
		else if ((btn & (1u << SDS_DS3_DOWN)) || ds3.ly > 192)
			now_dir = 1;
		else if (btn & ((1u << SDS_DS3_L1) | (1u << SDS_DS3_LEFT)))
			now_dir = -SEL_ROWS;
		else if (btn & ((1u << SDS_DS3_R1) | (1u << SDS_DS3_RIGHT)))
			now_dir = SEL_ROWS;
		else
			now_dir = 0;
		if (now_dir != rep_dir) {
			rep_dir = now_dir;
			dir = now_dir;
			next_rep = t + 400000; /* primera repetición a los 0,4 s, luego cada 70 ms */
		} else if (now_dir && t >= next_rep) {
			dir = now_dir;
			next_rep = t + 70000;
		}
		if (dir && !embedded) {
			/* un paso da la vuelta al llegar al borde; un salto de página se detiene en el borde */
			if (dir == -1 && cur == 0)
				cur = n - 1;
			else if (dir == 1 && cur == n - 1)
				cur = 0;
			else
				cur += dir;
			if (cur < 0)
				cur = 0;
			if (cur >= n)
				cur = n - 1;
		}
		if (cur < top)
			top = cur;
		if (cur >= top + SEL_ROWS)
			top = cur - SEL_ROWS + 1;
		sel_draw(cur, top, n, embedded);
		rsxkit_frame_end();
		if ((btn & ~held_prev) & (1u << SDS_DS3_CROSS)) {
			if (embedded) {
				out[0] = 0;
			} else {
				snprintf(out, outsz, "%s", rom_list[cur].name);
				last_rom_save(out);
			}
			return 1;
		}
		if ((btn & ~held_prev) & (1u << SDS_DS3_CIRCLE))
			return 0;
		held_prev &= btn; /* un botón soltado y apretado de nuevo vuelve a contar */
	}
	return 0;
}

/* ---- el juego en marcha ----------------------------------------------------------------------------------------------- */

/* Corre cuadros hasta que se pida salir de la aplicación (quit), volver al selector (exit_game) o, con la prueba automática, se
 * cumplan sus cuadros. */
static void game_run(void)
{
	while (!quit && !exit_game) {
#ifdef NUCLEO_JIT
		if (jit_guard_ptr && (jit_guard_ptr[0] != jit_guard_val[0] || jit_guard_ptr[1] != jit_guard_val[1])) {
			salsads_log("jit: las rutinas de la caché de PS3MAPI cambiaron (0x%08x 0x%08x en vez de 0x%08x 0x%08x) en el "
				    "cuadro %u: otra reserva tomó su dirección; se apaga el JIT y sigue el intérprete",
				    (unsigned)jit_guard_ptr[0], (unsigned)jit_guard_ptr[1], (unsigned)jit_guard_val[0],
				    (unsigned)jit_guard_val[1], (unsigned)frame_no);
			nds_jit_detach(nds);
			jit_on = 0;
			jit_guard_ptr = NULL;
			salsads_rec_marker(JIT_MARKER, NUCLEO_VERSION, 0);
		}
#endif
		render_frame();
		if (autotest && frame_no >= autotest_frames) {
			autotest_check();
			break;
		}
	}
	if (exit_game)
		salsads_log("juego: SELECT + START mantenidos, vuelta al selector (%u cuadros de la DS)", (unsigned)nds->frames);
}

int main(void)
{
	char banner[160];
	unsigned auto_cycles = 1;
	const rsxkit_screen *si;

	snprintf(banner, sizeof banner, "nucleo %s (%s, %s)", NUCLEO_VERSION, NUCLEO_TITLE, NUCLEO_APPID);
	salsads_log_init_banner("nucleo", banner);
	autotest = salsads_autotest();
	if (autotest) {
		FILE *ff = fopen(SALSADS_DATA_DIR "/frames.txt", "r");
		unsigned nfr = 0;

		if (ff) {
			if (fscanf(ff, "%u", &nfr) == 1 && nfr > 0 && nfr < 100000)
				autotest_frames = nfr;
			fclose(ff);
		}
		/* ciclos.txt: cuántas veces abre y cierra el juego la prueba automática (comprueba que la memoria no crece) */
		ff = fopen(SALSADS_DATA_DIR "/ciclos.txt", "r");
		if (ff) {
			unsigned nc = 0;

			if (fscanf(ff, "%u", &nc) == 1 && nc > 0 && nc < 1000)
				auto_cycles = nc;
			fclose(ff);
		}
	}
	sysUtilRegisterCallback(0, sysutil_cb, NULL);
	load_config();
	sds_input_init(&input);
	memset(&keys, 0, sizeof keys);
	ds3 = neutral_pad();
	ioPadInit(7);
	nds_rom_set_allocator(big_alloc, big_free);

	/* La máquina: una sola estructura para toda la vida de la aplicación; cada juego la reinicia con nds_init */
	nds = memalign(128, sizeof *nds);
	if (!nds) {
		salsads_log("RESULTADO nucleo: FALLO sin memoria para el núcleo");
		salsads_log_shutdown();
		return 1;
	}
	memset(nds, 0, sizeof *nds);
	nds->log = core_log;
	{
		struct stat gst;

		nds->gx_lento = stat(GX_LENTO_FLAG, &gst) == 0; /* (nds_init lo conserva) */
		salsads_log("gx: FIFO de geometría por %s (%s)", nds->gx_lento ? "la ruta anterior (gx-lento)" : "las rutas cortas",
			    GX_LENTO_FLAG);
	}
	nds_init(nds);
	tb_hz = (uint64_t)sysGetTimebaseFrequency();
	mem_report("al arrancar");

	if (rsxkit_init(NULL) != 0) {
		salsads_log("RESULTADO nucleo: FALLO rsxkit_init");
		salsads_log_shutdown();
		return 1;
	}
	rsxkit_set_wait_hook(wait_hook, NULL, 100000, 100000);
	if (rsxkit_ui_init() != 0) {
		salsads_log("RESULTADO nucleo: FALLO rsxkit_ui_init");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	tex_mem = memalign(1u << 20, 1u << 20);
	if (!tex_mem || rsxkit_map_main(tex_mem, 1u << 20, &tex_off) != 0) {
		salsads_log("RESULTADO nucleo: FALLO memoria de texturas");
		rsxkit_shutdown();
		salsads_log_shutdown();
		return 1;
	}
	memset(tex_mem, 0, 1u << 20);
	/* Los dos SPE del 2D (un trabajador por pantalla); si no arrancan, el PPE dibuja con el compositor del host */
	/* SPE: 2D A, 2D B, sonido, render 3D × 2 y geometría */
	if (spukit_init(6) == 0) {
		for (int e = 0; e < 2 && (e == 0 || spu_on); e++) {
			wk[e] = spukit_worker_start(spu_gpu2d_bin, e ? "gpu2d-b" : "gpu2d-a", 0);
			spu_on = wk[e] != NULL;
		}
		if (!spu_on)
			salsads_log("los SPE del 2D no arrancaron (spukit %d): el PPE dibuja con el compositor del host", spukit_last_error());
	} else {
		salsads_log("spukit_init falló: el PPE dibuja con el compositor del host");
	}
	if (spu_on) {
		nds->vid.ops.submit = vid_submit;
		nds->vid.ops.wait = vid_wait;
		autotest_verify = autotest;
		salsads_log("composición 2D: un SPE por pantalla (decrementador %u Hz)", (unsigned)spukit_tick_hz());
	}
	/* El SPE del sonido: mezcla los 16 canales; si no arranca, el núcleo mezcla en el PPE (sin ritmo de más, pero suena) */
	if (spu_on) {
		wk_snd = spukit_worker_start(spu_snd_bin, "snd", 0);
		if (!wk_snd)
			salsads_log("el SPE del sonido no arrancó (spukit %d): el PPE mezcla", spukit_last_error());
	}
	/* Render 3D: dos SPE con bandas alternas; si no arrancan o la autoprueba falla, el PPE rasteriza (lento) */
	nds->vid.r3d_band = 16;
	nds->vid.r3d_units = 1;
	if (spu_on) {
		for (int u = 0; u < 2; u++)
			wk3d[u] = spukit_worker_start(spu_raster3d_bin, u ? "r3d-1" : "r3d-0", 0);
		if (wk3d[0] && wk3d[1]) {
			if (r3d_spe_selftest() == 0) {
				r3d_on = 1;
				nds->vid.r3d_units = 2;
				nds->vid.ops.submit3d = r3d_spe_submit;
				nds->vid.ops.wait3d = r3d_spe_wait;
				{
					struct stat rst;

					nds->vid.r3d_alt = stat(R3D_ALT_FLAG, &rst) == 0;
					salsads_log("render 3D: dos SPE, bandas de 16 líneas con reparto %s (autoprueba contra el PPE: igual)",
						    nds->vid.r3d_alt ? "alterno (existe " R3D_ALT_FLAG ")" : "por costo");
				}
			} else {
				failures++;
				salsads_log("render 3D: la autoprueba de los SPE falló: rasteriza el PPE");
			}
		} else {
			salsads_log("render 3D: los SPE no arrancaron (spukit %d): rasteriza el PPE", spukit_last_error());
		}
	}
	/* Motor de geometría en el sexto SPE (salvo con GEOM_PPE_FLAG); si no arranca o la autoprueba falla, el PPE ejecuta
	 * las órdenes (modo directo) */
	struct stat gst;
	if (spu_on && stat(GEOM_PPE_FLAG, &gst) == 0)
		salsads_log("geometría: en el PPE (existe %s)", GEOM_PPE_FLAG);
	else if (spu_on) {
		wk_geom = spukit_worker_start(spu_geom_bin, "geom", 0);
		if (wk_geom && geom_selftest() == 0) {
			gx_set_executor(&nds->gx, &geom_ops);
			geom_on = 1;
			salsads_log("geometría: órdenes ejecutadas en un SPE (autoprueba contra el PPE: igual)");
		} else if (wk_geom) {
			failures++;
			salsads_log("geometría: la autoprueba del SPE falló: ejecuta el PPE");
		} else {
			salsads_log("geometría: el SPE no arrancó (spukit %d): ejecuta el PPE", spukit_last_error());
		}
	}
	{
		const int rc = audio_out_init();

		salsads_log("audio: salida de la PS3 %s", rc == 0 ? "abierta (48 kHz, 2 canales)" : "NO se abrió: sin sonido");
		if (rc != 0)
			salsads_log("audio: audio_out_init devolvió %d", rc);
	}
	nds->snd.ops.sink = snd_sink;
	if (wk_snd) {
		if (snd_selftest() == 0) {
			nds->snd.ops.submit = snd_submit;
			nds->snd.ops.wait = snd_wait;
			snd_on = 1;
			salsads_log("sonido: mezclador en un SPE (autoprueba contra el PPE: igual)");
		} else {
			failures++;
			salsads_log("sonido: la autoprueba del SPE falló: mezcla el PPE");
		}
	} else {
		snd_on = 0;
	}
	/* El JIT se prepara al final del arranque (fase52): las páginas de PS3MAPI no quedan apartadas en el mapa de
	 * memoria del proceso, y una reserva posterior (RSX, SPE, audio) podía recibir la misma dirección y reemplazar la
	 * página con las rutinas comunes del recompilador (en la PS3: 0x60000000 en 0xFF y el juego en negro). */
#ifdef NUCLEO_JIT
	{
		struct stat jst;

		if (stat(NOJIT_FLAG, &jst) == 0) {
			salsads_log("jit: %s existe: el ARM9 y el ARM7 con el intérprete (el respaldo)", NOJIT_FLAG);
		} else if (autotest) {
			salsads_log("jit: prueba automática (RPCS3): su texto es de solo lectura, no se intenta; el ARM9 y el ARM7 con el intérprete (el respaldo)");
		} else {
			char why[160], mwhy[160];
			uint64_t us = 0;
			void *mapi_code = NULL;
			enum rec_probe_status ps;

			if (stat(JIT_MAPI_FLAG, &jst) != 0) {
				snprintf(mwhy, sizeof mwhy, "experimental: crear %s para probarla", JIT_MAPI_FLAG);
			} else {
				mapi_code = salsads_rec_cache_mapi(REC_MAPI_BYTES, JIT_MAPI_MARKER, NUCLEO_VERSION, 2000, mwhy,
								   sizeof mwhy, &us);
				if (mapi_code)
					atexit(salsads_rec_cache_mapi_free);
			}
			if (mapi_code)
				salsads_log("jit: caché por PS3MAPI: %s", mwhy);
			else
				salsads_log("jit: caché por PS3MAPI no (%s): se usa la del segmento de texto", mwhy);
			ps = mapi_code ? REC_PROBE_OK : salsads_rec_probe(JIT_MARKER, NUCLEO_VERSION, 2000, why, sizeof why, &us);

			if (ps == REC_PROBE_OK) {
				struct nds_jit_cfg jc;

				memset(&jc, 0, sizeof jc);
				jc.code = mapi_code ? mapi_code : salsads_rec_cache();
				jc.code_bytes = mapi_code ? REC_MAPI_BYTES : REC_CACHE_BYTES;
				/* la zona de PS3MAPI está lejos del texto (0x70000000): ningún bl llega a C, así que se emite de una
				 * vez por mtctr/bctrl en lugar de intentar el bl y repetir cada bloque */
				jc.far_calls = mapi_code != NULL;
				jc.icache_flush = salsads_rec_flush;
				/* formas anteriores de las mejoras del recompilador (A/B) y medición de tiempos */
				{
					char ol[256];
					size_t o = 0;

					ol[0] = 0;
					for (unsigned k = 0; k < sizeof jit_opt_files / sizeof jit_opt_files[0]; k++)
						if (stat(jit_opt_files[k].file, &jst) == 0) {
							jc.opts |= jit_opt_files[k].opt;
							o += (size_t)snprintf(ol + o, sizeof ol - o, "%s%s", o ? ", " : "", jit_opt_files[k].what);
						}
					if (nds->gx_lento)
						o += (size_t)snprintf(ol + o, sizeof ol - o, "%sFIFO de geometría por la ruta lenta anterior (gx-lento)",
								      o ? ", " : "");
					jit_times = stat(JIT_TIMES_FLAG, &jst) == 0;
					salsads_log("jit: opciones del recompilador: %s; tiempos por parte (%s): %s", o ? ol : "todas las mejoras",
						    JIT_TIMES_FLAG, jit_times ? "sí" : "no");
				}
				/* el ARM7 (ARMv4T) con su propio recompilador: la cola de la caché (REC_ARM7_BYTES) y tablas propias */
				if (stat(ARM7_NOJIT_FLAG, &jst) == 0) {
					salsads_log("jit: %s existe: el ARM7 con el intérprete", ARM7_NOJIT_FLAG);
				} else {
					jc.arm7 = 1;
					jc.code7_bytes = REC_ARM7_BYTES;
				}
				if (nds_jit_attach(nds, &jc) == 0) {
					const uint32_t w_after_attach = ((const volatile uint32_t *)jc.code)[0];

					FILE *qf = fopen(JIT_QUANTUM_FILE, "r");
					unsigned q = JIT_QUANTUM;

					if (qf) {
						unsigned v = 0;

						if (fscanf(qf, "%u", &v) == 1 && v >= 16 && v <= 20000)
							q = v;
						fclose(qf);
					}
					nds->quantum = q;
					salsads_log("jit: tramo del planificador de %u ciclos del ARM9 (%s)", q,
						    q == JIT_QUANTUM ? "el de omisión" : JIT_QUANTUM_FILE);
					jit_on = 1;
					arec_stats(nds->jit9); /* (las estadísticas empiezan en cero) */
					jit_clocks();
					salsads_rec_marker(JIT_MARKER, NUCLEO_VERSION, 1);
					if (mapi_code) {
						sys_ppu_thread_t wt;

						{
							char lay[320];
							void *h = malloc(64);

							arec_debug_layout(nds->jit9, lay, sizeof lay);
							salsads_log("jit: memoria: %s; malloc(64) %p, sbrk %p", lay, h, sbrk(0));
							{
								const volatile uint32_t *q = (const volatile uint32_t *)mapi_code;

								salsads_log("jit: primera palabra: al escribir 0x%08x, tras icbi 0x%08x, tras las tablas 0x%08x, "
									    "al volver de attach 0x%08x, ahora 0x%08x",
									    (unsigned)arec_dbg_first[0], (unsigned)arec_dbg_first[1],
									    (unsigned)arec_dbg_first[2], (unsigned)w_after_attach, (unsigned)q[0]);
							}
							free(h);
						}
						jit_guard_ptr = (const volatile uint32_t *)mapi_code;
						jit_guard_val[0] = jit_guard_ptr[0];
						jit_guard_val[1] = jit_guard_ptr[1];

						sysThreadCreate(&wt, jit_watch_thread, NULL, 1500, 16 * 1024, 0, "jit-vigia");
					}
					salsads_log("jit: el ARM9 con el recompilador (caché de código de %u KB %s, prueba de %u µs); el ARM7 %s",
						    (unsigned)((jc.code_bytes - (nds->jit7 ? jc.code7_bytes : 0)) >> 10), mapi_code ? "de PS3MAPI" : "en el texto",
						    (unsigned)us,
						    nds->jit7 ? "con el recompilador (caché propia de 1024 KB, tablas propias)"
							      : (jc.arm7 ? "con el intérprete (el recompilador del ARM7 no se pudo crear: sin memoria)" : "con el intérprete"));
				} else {
					salsads_log("jit: el recompilador no se pudo crear (sin memoria): el ARM9 y el ARM7 con el intérprete");
				}
			} else if (ps == REC_PROBE_SKIPPED) {
				salsads_log("jit: el ARM9 y el ARM7 con el intérprete: %s", why);
			} else {
				salsads_log("jit: la prueba de la caché %s: %s; el ARM9 y el ARM7 con el intérprete (el respaldo)",
					    ps == REC_PROBE_HUNG ? "no volvió" : "falló", why);
			}
		}
	}
#endif
	si = rsxkit_screen_info();
	cur_w = (int)si->width;
	cur_h = (int)si->height;
	salsads_log("salida %dx%d%s", cur_w, cur_h, autotest ? " (prueba automática: RPCS3 no mide)" : "");


	/* Un juego tras otro: selector, abrir, correr, cerrar. Con la prueba automática no hay selector: rom.nds si existe (o la ROM
	 * de carga), `ciclos.txt` veces. */
	{
		unsigned done = 0;

		while (!quit) {
			char name[NAME_MAX_LEN], path[NAME_MAX_LEN + 40];
			struct stat pst;
			int opened;

			if (autotest) {
				opened = game_open(stat(ROM_PATH, &pst) == 0 ? ROM_PATH : NULL);
				if (opened != 0)
					opened = game_open(NULL); /* rom.nds no sirve: la ROM de carga */
			} else {
				if (!select_rom(name, sizeof name))
					break;
				if (name[0]) {
					snprintf(path, sizeof path, SALSADS_DATA_DIR "/%s", name);
					opened = game_open(path);
					if (opened != 0) {
						snprintf(sel_msg, sizeof sel_msg, "No se pudo abrir %s", name);
						sel_msg_until = now_us() + 4000000;
					}
				} else {
					opened = game_open(NULL);
				}
			}
			if (opened != 0) {
				if (autotest) {
					fail("no se pudo abrir ninguna ROM");
					break;
				}
				continue;
			}
			game_run();
			game_close();
			mem_report("tras cerrar el juego");
			if (autotest && ++done >= auto_cycles)
				break;
		}
	}
	/* Fin de la aplicación: ahora sí se detienen los SPE y el audio */
	salsads_log("salida: deteniendo los SPE y el audio");
	if (wk_snd)
		spukit_worker_stop(wk_snd);
	if (wk_geom)
		spukit_worker_stop(wk_geom);
	salsads_log("salida: audio");
	audio_out_shutdown();
	salsads_log("salida: SPE del 3D y del 2D");
	for (int u = 0; u < 2; u++)
		if (wk3d[u])
			spukit_worker_stop(wk3d[u]);
	if (spu_on)
		for (int e = 0; e < 2; e++)
			if (wk[e])
				spukit_worker_stop(wk[e]);
	mem_report("al salir");
	salsads_log("RESULTADO nucleo: %s", failures ? "FALLO" : "OK");
	if (!rsxkit_lost()) {
		rsxkit_finish();
		rsxkit_shutdown();
	}
	ioPadEnd();
	salsads_log_shutdown();
	return failures ? 1 : 0;
}
