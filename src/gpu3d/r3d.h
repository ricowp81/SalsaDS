/*
 * SalsaDS — motor de render 3D de la DS (Fase 3.2): rasteriza un `gx_frame` (la lista de polígonos en pantalla que
 * deja el motor de geometría de la 3.1) en una imagen de 256×192 que el motor 2D A usa como su capa BG0.
 *
 * Código propio, escrito desde GBATEK y la comparación de comportamiento con melonDS ejecutándolo (docs/fase3.md,
 * sección 3.2). Ni se leyó ni se copió código de melonDS, NooDS ni DeSmuME.
 *
 * Tres piezas, el mismo resultado bit a bit:
 *   r3d_px.h    las reglas de la DS (bordes, interpolación con corrección por w, texturas, modos de polígono, prueba de
 *               profundidad, translúcidos, sombras, niebla, bordes): funciones `static inline` en enteros, compartidas
 *   r3d_ref.c   la referencia: el cuadro entero en memoria, polígono por polígono, cada texel leído de la VRAM
 *   r3d_core.c  el rasterizador por bandas de líneas que corre en los SPE (y, con la E/S de memcpy, en el host y el PPE):
 *               lista ordenada en el local store, polígonos y vértices por DMA, caché de texturas por bloques
 *               decodificados con LRU, una banda en el local store mientras se dibuja
 *
 * Todo es aritmética entera (con 64 bits donde hace falta): el SPE, el PPE y el host dan los mismos píxeles.
 *
 * Salida: 256 × 192 palabras de 32 bits en el orden de bytes de quien las lee (R3D_PX_*): color de 6 bits por canal y
 * alfa de 5 bits; alfa 0 = transparente (la capa 3D deja ver lo de abajo).
 */
#ifndef SALSADS_GPU3D_R3D_H
#define SALSADS_GPU3D_R3D_H

#include <stdint.h>

#include "gpu3d/gx.h"

#define R3D_W 256
#define R3D_H 192

/* Píxel de salida */
#define R3D_PX(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define R3D_PX_R(p) ((p) & 63u)
#define R3D_PX_G(p) (((p) >> 8) & 63u)
#define R3D_PX_B(p) (((p) >> 16) & 63u)
#define R3D_PX_A(p) (((p) >> 24) & 31u)

/* Memoria de texturas: 4 ranuras de 128 KB (bancos A-D en modo textura) y 6 de paletas de 16 KB (E: 0-3, F/G: 0, 1,
 * 4, 5). Cada ranura es un desplazamiento dentro de la instantánea de la VRAM del núcleo (los 9 bancos seguidos, 656
 * KB, más 128 bytes en cero) o R3D_UNMAPPED (se lee cero, como la referencia 2D). */
#define R3D_UNMAPPED 0xFFFFFFFFu
#define R3D_VRAM_ZERO_OFF 0xA4000u /* G2D_VRAM_SIZE: los ceros que siguen a la instantánea */
#define R3D_TEX_SLOTS 4
#define R3D_PAL_SLOTS 6

/* Banderas del trabajo */
#define R3D_F_NOEDGE 1u   /* no hacer edge marking aunque DISP3DCNT lo pida (diagnóstico) */
#define R3D_F_NOFOG 2u    /* ídem, niebla */
#define R3D_F_BLANK 4u    /* no hay cuadro de geometría: sale transparente */

/* Un trabajo: un cuadro, repartido en bandas de `band` líneas entre `nunits` unidades (SPE); esta es la `unit`. Las
 * bandas se reparten alternas: la unidad u dibuja las bandas u, u + nunits, u + 2·nunits... (≤ 112 bytes: va dentro
 * de un trabajo de spukit) */
typedef struct r3d_job {
	uint64_t frame_ea; /* gx_frame (alineado a 128) */
	uint64_t vram_ea;  /* instantánea de la VRAM (alineada a 128) */
	uint64_t out_ea;   /* R3D_W × R3D_H palabras de 32 bits (alineado a 128) */
	uint32_t texslot[R3D_TEX_SLOTS];
	uint32_t palslot[R3D_PAL_SLOTS];
	uint32_t nunits, unit;
	uint32_t band;     /* líneas por banda: 8, 16 (normal), 32 o 64; divisor de 192 */
	uint32_t flags;    /* R3D_F_* */
	uint32_t pad[2];
} __attribute__((aligned(16))) r3d_job;

/* Contadores de un trabajo (una unidad) */
typedef struct r3d_stats {
	uint32_t polys;       /* polígonos del cuadro (los que llegan del motor de geometría) */
	uint32_t band_polys;  /* pares banda·polígono dibujados */
	uint32_t rows;        /* líneas de polígono recorridas */
	uint32_t pixels;      /* píxeles que pasaron la cobertura */
	uint32_t written;     /* píxeles escritos (color o sombra) */
	uint32_t tex_px;      /* texeles pedidos */
	uint32_t tex_hits;    /* de ellos, en el bloque del texel anterior o en la caché */
	uint32_t tex_misses;  /* bloques decodificados (fallos de la caché) */
	uint32_t tex_evict;   /* bloques desalojados */
	uint32_t tex_bytes;   /* bytes de VRAM traídos por DMA para texturas y paletas */
	uint32_t line_miss;   /* líneas de 128 bytes de VRAM traídas */
	uint32_t bands;       /* bandas dibujadas */
	uint32_t dma_bytes;   /* polígonos, vértices y salida */
	uint32_t pad;
	/* tiempos (ticks del decrementador del SPE, 79,8 MHz; en el host, ceros) */
	uint32_t t_sort, t_list, t_fetch, t_raster, t_tex, t_post, t_out, t_total;
	/* desglose de t_raster (Fase 3.4): preparar polígonos, calcular aristas de cada línea, dibujar píxeles (incluye texturas) */
	uint32_t t_setup, t_rows, t_pix;
} r3d_stats;

/* ---- Referencia (r3d_ref.c) ------------------------------------------------------------------------- */
/* Rasteriza `f` con la VRAM de la instantánea `vram` (R3D_VRAM_ZERO_OFF bytes + 128 en cero) en `out`. `f` = NULL:
 * transparente. */
void r3d_render_ref(const gx_frame *f, const uint8_t *vram, const uint32_t texslot[R3D_TEX_SLOTS],
		    const uint32_t palslot[R3D_PAL_SLOTS], uint32_t flags, uint32_t *out);

/* ---- Rasterizador por bandas (r3d_core.c) -------------------------------------------------------------- */
/* El contexto (estado y búferes de la unidad) es grande: en el SPE vive en el local store (r3d_core_ctx_size). */
typedef struct r3d_ctx r3d_ctx;
uint32_t r3d_core_ctx_size(void);
/* Dibuja las bandas de la unidad del trabajo. 0 = bien; < 0, trabajo inválido. */
int r3d_core_render(r3d_ctx *c, const r3d_job *j, r3d_stats *st);

/* Sonda de aritmética para la autoprueba (el mismo resultado en el SPE, el PPE y el host): `which` 0..8. */
uint32_t r3d_math_probe(uint32_t seed, uint32_t which);
#define R3D_NPROBES 9

/* Ranuras de textura y paleta a partir de VRAMCNT (bancos A-G, 7 bytes) y del desplazamiento de cada banco en la
 * instantánea (bank_off[0..6]). */
void r3d_slots_from_vramcnt(const uint8_t vramcnt[7], const uint32_t bank_off[7], uint32_t texslot[R3D_TEX_SLOTS],
			    uint32_t palslot[R3D_PAL_SLOTS]);

/* FNV-1a de 64 bits sobre los valores (no los bytes) de una imagen: igual en LE y en BE */
uint64_t r3d_hash_image(const uint32_t *img);

#endif
