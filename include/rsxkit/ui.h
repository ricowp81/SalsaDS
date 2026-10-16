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
 * escala 1 mide RSXKIT_FONT_W × RSXKIT_FONT_H (8 × 13) unidades por
 * carácter; el atlas tiene los glifos al doble (Scale2x, alfa de 8 bits),
 * así que a escala 2 en una pantalla de 720 líneas con espacio lógico de
 * 720 va un téxel por píxel.
 *
 * Memoria: rsxkit_ui_init reserva 2 MB de memoria principal mapeada al RSX
 * (el atlas y dos búferes de vértices que se turnan con una barrera). Los
 * quads que no caben en un cuadro (más de RSXKIT_UI_MAX_QUADS) se descartan
 * y se cuentan.
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
#define RSXKIT_UI_IMAGES      1    /* ranuras de imagen (carátulas) */
#define RSXKIT_UI_IMAGE_MAX_W 320  /* tamaño máximo de una imagen subida */
#define RSXKIT_UI_IMAGE_MAX_H 448
#define RSXKIT_UI_MAX_GROUPS 1024  /* llamadas al RSX por cuadro; las de más van juntas */

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
/* Envía el lote al RSX. */
void rsxkit_ui_end(void);

/* Rectángulo con un color por esquina (arriba izq., arriba der., abajo
 * izq., abajo der.). */
void rsxkit_ui_gradient(float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br);
static inline void rsxkit_ui_rect(float x, float y, float w, float h, uint32_t rgba)
{
	rsxkit_ui_gradient(x, y, w, h, rgba, rgba, rgba, rgba);
}

/* Texto UTF-8 con la esquina superior izquierda en (x, y). Si tiene más de
 * max_cells caracteres (0 = sin límite), dibuja max_cells − 1 y «…».
 * Devuelve el ancho dibujado. */
float rsxkit_ui_text(float x, float y, const char *utf8, float scale, uint32_t rgba, uint32_t max_cells);

/* La onda en el instante t (segundos), a lo ancho del espacio lógico. */
void rsxkit_ui_wave(const rsxkit_wave *w, float t);

/* Imágenes (carátulas): rsxkit_ui_image_set sube una imagen RGBA8888 (bytes
 * R, G, B, A; w × h de hasta RSXKIT_UI_IMAGE_MAX_W × _H) a la ranura y
 * espera a que el RSX termine los cuadros que la usan, así que se llama
 * entre cuadros y rara vez. rsxkit_ui_image la dibuja estirada al
 * rectángulo (x, y, w, h); `rgba` la tiñe y le pone alfa (0xFFFFFFFF = tal
 * cual, 0xFFFFFF80 = a media opacidad). Devuelve 0 o un código de rsxkit. */
int rsxkit_ui_image_set(int slot, const uint8_t *rgba, uint32_t w, uint32_t h);
void rsxkit_ui_image(int slot, float x, float y, float w, float h, uint32_t rgba);

/* Quads descartados desde el arranque (no cupieron en un cuadro). */
uint32_t rsxkit_ui_dropped(void);

/* ---- Sin RSX: pruebas y vistas previas en la PC --------------------------- */

/* Atlas de la fuente: RGBA8888 (bytes R, G, B, A), blanco con el alfa de
 * los glifos al doble de tamaño y un bloque blanco opaco para los
 * rectángulos. Las coordenadas de textura de los vértices van en téxeles
 * del atlas. */
#define RSXKIT_UI_ATLAS_W 288
#define RSXKIT_UI_ATLAS_H 392
void rsxkit_ui_atlas(uint8_t *rgba, uint32_t pitch);
/* Sin RSX: el tamaño de la imagen de una ranura (lo que rsxkit_ui_image_set
 * avisa al subirla); sus coordenadas de textura van en téxeles de esa imagen. */
void rsxkit_ui_image_size(int slot, uint32_t w, uint32_t h);
/* De qué ranura es el grupo `g` de rsxkit_ui_groups: 0 es el atlas de la
 * fuente (rectángulos, texto, ondas) y n > 0 la ranura n − 1. */
uint32_t rsxkit_ui_group_image(uint32_t g);
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

#ifdef __cplusplus
}
#endif

#endif
