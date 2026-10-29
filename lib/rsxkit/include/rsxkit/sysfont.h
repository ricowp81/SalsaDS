/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — la fuente del sistema de la PS3 (la del XMB, de /dev_flash) como
 * fuente del texto de rsxkit/ui.h. Usa los módulos del firmware libfont,
 * FreeType y libfontFT (cellFont), que carga con sysModuleLoad; las fuentes
 * se leen de la consola al abrirlas, no van en el programa.
 *
 *     if (rsxkit_sysfont_open(RSXKIT_SYSFONT_JAPANESE, 0) == RSXKIT_OK)
 *             rsxkit_ui_font(rsxkit_sysfont_source());
 *     ...
 *     rsxkit_ui_font(NULL);            (de vuelta a la 8x13)
 *     rsxkit_sysfont_close();
 *
 * Los glifos salen a RSXKIT_SYSFONT_PX píxeles por eme en la caja de
 * RSXKIT_GLYPH_BOX de ui.h, con la línea base en la fila
 * RSXKIT_SYSFONT_BASELINE: a escala 2 el texto ocupa el mismo renglón de 13
 * unidades que la 8x13. Lo que la fuente no tiene se dibuja con la 8x13.
 *
 * Memoria: 1 MB de caché de archivos de libfont y lo que pidan la biblioteca
 * y el renderizador (rsxkit_sysfont_info lo cuenta); todo se suelta con
 * rsxkit_sysfont_close. Solo PS3 (PSL1GHT); no es seguro entre hilos: usarla
 * desde el hilo que arma el 2D.
 */
#ifndef RSXKIT_SYSFONT_H
#define RSXKIT_SYSFONT_H

#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSXKIT_SYSFONT_PX       22.0f  /* píxeles por eme en el atlas */
#define RSXKIT_SYSFONT_BASELINE 20     /* fila de la línea base en la caja de 26 */

/* Qué variante del juego de fuentes del sistema: cambia de qué fuente salen
 * los ideogramas (japonés: NewRodin; chino: DFHei5) y el orden de búsqueda. */
typedef enum rsxkit_sysfont_script {
	RSXKIT_SYSFONT_LATIN,     /* Rodin primero; CJK de respaldo */
	RSXKIT_SYSFONT_JAPANESE,  /* NewRodin + YG (coreano) + DFHei5 */
	RSXKIT_SYSFONT_KOREAN,    /* el mismo juego que el japonés (el hangul sale de YG) */
	RSXKIT_SYSFONT_TCHINESE,  /* DFHei5 tradicional primero */
	RSXKIT_SYSFONT_SCHINESE,  /* DFHei5 simplificado primero */
} rsxkit_sysfont_script;

/* Carga los módulos e inicia libfont (la primera vez) y abre el juego de
 * fuentes del sistema para `script` (bold: la negrita, si el firmware la
 * tiene para ese juego; si no, la normal). Llamarla otra vez cambia de
 * juego (vaciar la caché con rsxkit_ui_font). Devuelve RSXKIT_OK o un
 * código negativo; si falla, todo queda cerrado. */
int rsxkit_sysfont_open(rsxkit_sysfont_script script, int bold);
/* La fuente para rsxkit_ui_font, o NULL si no está abierta. */
const rsxkit_glyph_source *rsxkit_sysfont_source(void);
/* ¿La fuente abierta dibuja este carácter? */
int rsxkit_sysfont_has(uint32_t cp);
/* Cierra la fuente, la biblioteca y los módulos. Antes, rsxkit_ui_font(NULL). */
void rsxkit_sysfont_close(void);

typedef struct rsxkit_sysfont_info {
	int open;
	uint32_t type;          /* CELL_FONT_TYPE_* del juego abierto */
	int step;               /* en qué paso falló la última apertura (0: no falló): 1–3 sysModuleLoad
	                         * de FONT, FREETYPE y FONTFT; 4 caché; 5 fontInit; 6 FreeType; 7
	                         * renderizador; 8 fontOpenFontset; 9 escala; 10 fontBindRenderer;
	                         * 11 escala del render; 12 el dibujo de la «H» da error; 13 la «H»
	                         * no deja tinta con ningún modo */
	int32_t error;          /* el código del firmware de ese paso */
	float line_height;      /* fontGetHorizontalLayout, en píxeles */
	float baseline_y;
	int cal_top, cal_bottom;    /* filas de la «H» en la superficie de calibración */
	int id_check;           /* fontGetFontIdCode sirve para saber si falta un glifo */
	uint32_t lib_bytes;     /* reservado ahora por libfont con nuestras funciones */
	uint32_t lib_peak;      /* el máximo */
	uint32_t cache_bytes;   /* caché de archivos */
	uint32_t glyphs;        /* glifos rasterizados */
	uint32_t missing;       /* pedidos que la fuente no tenía */
	int mode;               /* cómo llega la imagen: 1 superficie, 2 copia de fontImageTransInfo, 3 fontGlyphRenderImage */
	char trace[1024];       /* la última apertura paso por paso, con los códigos del firmware */
} rsxkit_sysfont_info;
void rsxkit_sysfont_info_get(rsxkit_sysfont_info *info);

#ifdef __cplusplus
}
#endif

#endif
