/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — 2D en el RSX para menús y superposiciones: rectángulos con
 * degradado, texto con la fuente de rsxkit/text.h hecho textura (escala
 * libre y suave) y ondas animadas para el fondo. Todo se junta en un lote de
 * triángulos con mezcla alfa que sale en rsxkit_ui_end: unas pocas órdenes
 * al RSX por cuadro y casi nada de CPU.
 *
 * Uso, una vez por cuadro (dibuja sobre la pantalla del cuadro en curso):
 *
 *     rsxkit_frame_begin(0x000000);
 *     rsxkit_ui_begin(1280.0f, 720.0f);        espacio lógico (se estira a la pantalla)
 *     rsxkit_ui_gradient(0, 0, 1280, 720, arriba, arriba, abajo, abajo);
 *     rsxkit_ui_wave(&ola, segundos);
 *     rsxkit_ui_text(100, 300, "Hola", 3.0f, 0xFFFFFFFF, 0);
 *     rsxkit_ui_end();
 *     rsxkit_frame_end();
 *
 * Colores 0xRRGGBBAA. Coordenadas lógicas con la y hacia abajo. El texto a
 * escala 1 mide RSXKIT_FONT_H (13) unidades de alto; con la fuente propia
 * (la 8x13) cada carácter mide RSXKIT_FONT_W (8) de ancho. El atlas tiene
 * los glifos al doble (Scale2x, alfa de 8 bits), así que a escala 2 en una
 * pantalla de 720 líneas con espacio lógico de 720 va un téxel por píxel.
 *
 * Fuente externa (rsxkit_ui_font): con una fuente proporcional (la del
 * sistema, rsxkit/sysfont.h, u otra que dé glifos con la interfaz de abajo)
 * el texto se arma con sus glifos, que se rasterizan la primera vez que
 * hacen falta y se guardan en una caché del atlas con reemplazo del menos
 * usado. El ancho de un texto ya no es celdas × 8: medirlo con
 * rsxkit_ui_text_width (con la 8x13 da exactamente celdas × 8 × escala). Lo
 * que la fuente externa no tenga sale con la 8x13.
 *
 * Memoria: rsxkit_ui_init reserva 6 MB de memoria principal mapeada al RSX
 * (el atlas de 1,9 MB con la caché de glifos, dos búferes de vértices que se
 * turnan con una barrera y la imagen). Los quads que no caben en un cuadro
 * (más de RSXKIT_UI_MAX_QUADS) se descartan y se cuentan.
 *
 * La geometría (la que arma rsxkit_ui_text, _gradient y _wave) no usa el
 * RSX: esta cabecera compila en cualquier host. Lo que envía al RSX es
 * rsxkit_ui_init/_begin/_end.
 */
#ifndef RSXKIT_UI_H
#define RSXKIT_UI_H

#include <stdint.h>

#include "rsxkit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSXKIT_UI_MAX_QUADS  4096
#define RSXKIT_UI_IMAGES      4    /* ranuras de imagen (carátulas, íconos del cartucho, miniaturas, botones) */
#define RSXKIT_UI_IMAGE_MAX_W 320  /* tamaño máximo de una imagen subida */
#define RSXKIT_UI_IMAGE_MAX_H 448
#define RSXKIT_UI_MAX_GROUPS 1024  /* llamadas al RSX por cuadro; las de más van juntas */
#define RSXKIT_UI_MAX_HOOKS     4  /* ganchos de rsxkit_ui_hook por cuadro */
#define RSXKIT_UI_HOOK_GROUP  255  /* rsxkit_ui_group_image de un gancho */

/* Una onda: una cresta hecha de dos senos, con una línea brillante encima y
 * una lámina debajo que se desvanece. */
typedef struct rsxkit_wave {
	float y;          /* altura media de la cresta (lógica) */
	float depth;      /* alto de la lámina bajo la cresta */
	float amp[2];     /* amplitud de cada seno */
	float freq[2];    /* ciclos a lo ancho del espacio lógico */
	float speed[2];   /* ciclos por segundo (el signo da la dirección) */
	float phase;      /* desfase, en vueltas */
	float line;       /* grosor de la línea de la cresta (0: sin línea) */
	uint32_t crest;   /* color de la línea */
	uint32_t sheet;   /* color de la lámina junto a la cresta */
} rsxkit_wave;

/* Reserva y mapea la memoria y arma el atlas de la fuente. Después de
 * rsxkit_init; una vez. Devuelve 0 o un código negativo de rsxkit. */
int rsxkit_ui_init(void);

/* Empieza el lote en la pantalla del cuadro en curso (después de
 * rsxkit_frame_begin), con un espacio lógico de lw × lh. */
void rsxkit_ui_begin(float lw, float lh);
/* Igual, sobre un destino de render (rsxkit_target_create) en lugar de la
 * pantalla: la imagen queda para usarla como textura (después de
 * rsxkit_ui_end, rsxkit_texture_sync). El destino no se borra: rsxkit_clear
 * antes de dibujar si hace falta. Sin efecto si el destino no se pudo ligar
 * (rsxkit_ui_end no dibuja nada). */
void rsxkit_ui_begin_target(rsxkit_target *t, float lw, float lh);
/* Envía el lote al RSX. */
void rsxkit_ui_end(void);

/* Rectángulo con un color por esquina (arriba izq., arriba der., abajo
 * izq., abajo der.). */
void rsxkit_ui_gradient(float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
static inline void rsxkit_ui_rect(float x, float y, float w, float h, uint32_t rgba)
{
	rsxkit_ui_gradient(x, y, w, h, rgba, rgba, rgba, rgba);
}

/* Texto UTF-8 con la esquina superior izquierda en (x, y). Con max_cells
 * (0 = sin límite) el texto se corta con «…» si no cabe en max_cells celdas
 * de la 8x13 (max_cells × 8 × scale unidades: con la 8x13, max_cells − 1
 * caracteres y «…»). Devuelve el ancho dibujado. */
float rsxkit_ui_text(float x, float y, const char *utf8, float scale, uint32_t rgba, uint32_t max_cells);
/* Igual, con el límite como ancho (unidades lógicas; 0 o menos = sin
 * límite): si el texto mide más, va lo que quepa y «…» dentro de max_w. */
float rsxkit_ui_text_fit(float x, float y, const char *utf8, float scale, uint32_t rgba, float max_w);
/* Ancho de un texto a esa escala, con la fuente en uso (lo que dibujaría
 * rsxkit_ui_text sin límite). */
float rsxkit_ui_text_width(const char *utf8, float scale);
/* El ancho que dibujaría rsxkit_ui_text_fit con ese límite (para alinear a
 * la derecha un texto que puede salir cortado). */
float rsxkit_ui_text_fit_width(const char *utf8, float scale, float max_w);
/* Corte de línea: bytes del primer renglón de `utf8` que caben en max_w a
 * esa escala. Corta en un espacio o, si no hay uno que sirva (o en texto
 * japonés, chino o coreano), entre dos caracteres; sin dejar al principio
 * del renglón siguiente la puntuación de cierre del CJK. Siempre avanza al
 * menos un carácter. En *next (si no es NULL) deja dónde empieza el
 * renglón siguiente (sin los espacios del corte); al final, el '\0'. */
size_t rsxkit_ui_text_wrap(const char *utf8, float scale, float max_w, const char **next);

/* La onda en el instante t (segundos), a lo ancho del espacio lógico. */
void rsxkit_ui_wave(const rsxkit_wave *w, float t);

/* Imágenes (carátulas): rsxkit_ui_image_set sube una imagen RGBA8888 (bytes
 * R, G, B, A; w × h de hasta RSXKIT_UI_IMAGE_MAX_W × _H) a la ranura y
 * espera a que el RSX termine los cuadros que la usan, así que se llama
 * entre cuadros y rara vez. rsxkit_ui_image la dibuja estirada al
 * rectángulo (x, y, w, h); `rgba` la tiñe y le pone alfa (0xFFFFFFFF = tal
 * cual, 0xFFFFFF80 = a media opacidad). Devuelve 0 o un código de rsxkit. */
int rsxkit_ui_image_set(int slot, const uint8_t *rgba, uint32_t w, uint32_t h);
/* Reescribe el rectángulo (x, y, w, h) de una imagen ya subida con
 * rsxkit_ui_image_set (por ejemplo, una casilla de un atlas de miniaturas):
 * `rgba` son w × h píxeles RGBA8888 seguidos. Espera a que el RSX termine los
 * cuadros en vuelo, como image_set, así que no conviene más de una por cuadro.
 * Devuelve 0 o RSXKIT_EARG (ranura sin imagen o rectángulo fuera de ella). */
int rsxkit_ui_image_update(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *rgba);
void rsxkit_ui_image(int slot, float x, float y, float w, float h, uint32_t rgba);
/* Como rsxkit_ui_image, pero solo el rectángulo (sx, sy, sw, sh) de la
 * imagen, en píxeles: varias figuras en una misma ranura (un atlas). */
void rsxkit_ui_image_part(int slot, float x, float y, float w, float h, float sx, float sy, float sw, float sh,
			  uint32_t rgba);

/* Un dibujo propio (3D con rsxkit_draw_triangles, por ejemplo) en medio del
 * lote 2D: lo anterior queda debajo y lo posterior encima. rsxkit_ui_end
 * llama a fn(user) en ese punto, con la pantalla del cuadro ligada y el
 * espacio lógico de rsxkit_ui_begin; fn puede cambiar los vértices con
 * rsxkit_vertices (rsxkit_ui_end vuelve a poner los suyos) y no debe llamar
 * a otra función de rsxkit_ui. Hasta RSXKIT_UI_MAX_HOOKS por cuadro (de más,
 * se descartan y se cuentan en rsxkit_ui_dropped). */
typedef void (*rsxkit_ui_hook_fn)(void *user);
void rsxkit_ui_hook(rsxkit_ui_hook_fn fn, void *user);

/* Quads descartados desde el arranque (no cupieron en un cuadro). */
uint32_t rsxkit_ui_dropped(void);

/* ---- Sin RSX: pruebas y vistas previas en la PC --------------------------- */

/* Atlas de la fuente: RGBA8888 (bytes R, G, B, A), blanco con el alfa de
 * los glifos al doble de tamaño y un bloque blanco opaco para los
 * rectángulos. Las coordenadas de textura de los vértices van en téxeles
 * del atlas. Arriba a la izquierda (RSXKIT_UI_ATLAS_FIXED_W × _H), la 8x13:
 * 16 celdas de 18 × 28 por fila, todos los glifos más el bloque blanco (22
 * filas = 352 celdas). El resto son las RSXKIT_UI_GLYPH_SLOTS casillas de
 * 28 × 28 de la caché de glifos de la fuente externa (a la derecha de la
 * 8x13 y debajo), vacías al armarlo. rsxkit_ui_atlas lo arma y lo deja como
 * destino de la caché: los glifos nuevos se escriben ahí mientras se arma
 * el lote. */
#define RSXKIT_UI_ATLAS_W 512
#define RSXKIT_UI_ATLAS_H 952
#define RSXKIT_UI_ATLAS_FIXED_W 288
#define RSXKIT_UI_ATLAS_FIXED_H 616
#define RSXKIT_UI_GLYPH_SLOT 28
#define RSXKIT_UI_GLYPH_SLOTS 392
void rsxkit_ui_atlas(uint8_t *rgba, uint32_t pitch);
/* Sin RSX: el tamaño de la imagen de una ranura (lo que rsxkit_ui_image_set
 * avisa al subirla); sus coordenadas de textura van en téxeles de esa imagen. */
void rsxkit_ui_image_size(int slot, uint32_t w, uint32_t h);
/* De qué ranura es el grupo `g` de rsxkit_ui_groups: 0 es el atlas de la
 * fuente (rectángulos, texto, ondas) y n > 0 la ranura n − 1. */
uint32_t rsxkit_ui_group_image(uint32_t g);
/* El gancho del grupo `g` (si su rsxkit_ui_group_image es RSXKIT_UI_HOOK_GROUP;
 * si no, NULL). */
void rsxkit_ui_group_hook(uint32_t g, rsxkit_ui_hook_fn *fn, void **user);
/* Lo que dibujen rsxkit_ui_gradient, _text y _wave va a `v` (hasta `cap`
 * vértices, triángulos sueltos) en un espacio lógico de lw × lh, en vez de
 * al lote del RSX. Con v NULL no se acepta nada. */
void rsxkit_ui_batch(rsxkit_vertex *v, uint32_t cap, float lw, float lh);
/* Vértices en el lote. */
uint32_t rsxkit_ui_count(void);
/* Grupos del lote: cada llamada de dibujo (rectángulo, texto, láminas o
 * líneas de una onda) es un grupo, y rsxkit_ui_end manda uno por llamada
 * al RSX: lo que se superpone queda en llamadas distintas, que se mezclan
 * en orden también en RPCS3 (dentro de una sola, RPCS3 no ve lo anterior).
 * Devuelve cuántos y deja en *first el primer vértice de cada uno. */
uint32_t rsxkit_ui_groups(const uint32_t **first);

/* ---- Fuente externa y caché de glifos (sin RSX) ---------------------------- */

/* Lado de la caja de un glifo externo, en téxeles del atlas: el alto de un
 * renglón (13 unidades al doble, como la 8x13 en el atlas). */
#define RSXKIT_GLYPH_BOX 26

typedef struct rsxkit_glyph_info {
	float advance;  /* avance de la pluma, en téxeles del atlas (2 por unidad) */
	int left;       /* columna, desde la pluma, donde empieza la imagen (puede ser < 0) */
	int width;      /* columnas de la imagen (0..RSXKIT_GLYPH_BOX; 0: sin tinta, un espacio) */
} rsxkit_glyph_info;

/* Quien da los glifos. render rasteriza `cp` en `alpha` (cobertura de 8
 * bits, RSXKIT_GLYPH_BOX × RSXKIT_GLYPH_BOX, `pitch` bytes por fila, a cero
 * al llamar): la caja es el renglón (fila 0 arriba, el alto entero de 13
 * unidades) y la columna 0 es la primera de la imagen, `left` téxeles a la
 * derecha de la pluma. Devuelve 0, o < 0 si no tiene el glifo (se dibuja
 * con la 8x13). Lo llama rsxkit_ui_text y compañía, en el hilo que arma el
 * lote. */
typedef struct rsxkit_glyph_source {
	const char *name;  /* para el log */
	void *user;
	int (*render)(void *user, uint32_t cp, uint8_t *alpha, uint32_t pitch, rsxkit_glyph_info *g);
} rsxkit_glyph_source;

/* La fuente del texto: `src` (que debe vivir mientras se use) o, con NULL,
 * la 8x13. Al cambiarla se vacía la caché. */
void rsxkit_ui_font(const rsxkit_glyph_source *src);
const rsxkit_glyph_source *rsxkit_ui_font_source(void);

/* Estado de la caché, para el log. */
typedef struct rsxkit_ui_font_stats {
	uint32_t hits;       /* glifos dibujados que ya estaban en el atlas */
	uint32_t renders;    /* rasterizados (primera vez o tras salir del atlas) */
	uint32_t evictions;  /* casillas reemplazadas (la menos usada) */
	uint32_t fallback;   /* glifos que la fuente no tiene, dibujados con la 8x13 */
	uint32_t deferred;   /* glifos que no se dibujaron en un cuadro (sin casilla libre o sin presupuesto) */
	uint32_t resets;     /* veces que se vació la tabla de glifos conocidos */
	uint32_t known;      /* glifos medidos (en la tabla) */
	uint32_t resident;   /* casillas ocupadas */
	uint32_t slots;      /* casillas en total */
} rsxkit_ui_font_stats;
void rsxkit_ui_font_stats_get(rsxkit_ui_font_stats *s);
/* ¿Cambió el atlas desde la última llamada? (ui.c: para la barrera antes
 * del dibujo). */
int rsxkit_ui_atlas_dirty(void);

#ifdef __cplusplus
}
#endif

#endif
