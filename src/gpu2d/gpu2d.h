/*
 * SalsaDS — composición 2D de la DS (Fase 0.5).
 *
 * Qué entra y qué sale de la composición de una pantalla (un «motor» de la
 * DS: el A o el B). El mismo código portable corre en la Mac, en el PPE y en
 * un SPE:
 *
 *   gpu2d_ref.c      referencia píxel por píxel, lo más directa posible
 *   gpu2d_compose.c  compositor por líneas: es el que corre en el SPE (con
 *                    kernels SIMD) y, con kernels escalares, en el host
 *   gpu2d_scene.c    escenas sintéticas deterministas para probar y medir
 *
 * Modelo (lo mínimo para medir el costo; el resto de la DS queda para la
 * Fase 2): 4 fondos en modo texto (4 y 8 bits por píxel, los 4 tamaños,
 * volteos, desplazamiento), 128 OBJ normales (1D y 2D, las 12 formas y
 * tamaños, volteos, 4 y 8 bits), prioridades, color de fondo, pantalla en
 * blanco forzado y cambios de registros y de paleta a mitad de cuadro.
 * No están: ventanas, mezclas alfa, mosaico, brillo maestro, fondos
 * afines o de mapa de bits, paletas extendidas, OBJ afines, el modo de
 * objeto semitransparente/ventana/bitmap y la capa 3D. El OAM y la VRAM
 * se toman como una foto al empezar el cuadro: escribirlos a mitad de
 * cuadro hará falta (como un registro de cambios de VRAM) y no se midió.
 *
 * La memoria de la DS (VRAM, paletas, OAM) está en little-endian, tal como
 * la guardará el núcleo; la imagen de salida son medias palabras RGBA5551
 * en el orden de bytes del que la lee (el PPE/SPE: big-endian; el host de
 * pruebas: el suyo), listas para rsxkit_fb_upload.
 */
#ifndef SALSADS_GPU2D_GPU2D_H
#define SALSADS_GPU2D_GPU2D_H

#include <stdint.h>

#define GPU2D_W 256
#define GPU2D_H 192
#define GPU2D_NBG 4
#define GPU2D_NOBJ 128

/* DISPCNT (los bits que se usan). */
#define GPU2D_DISPCNT_OBJ_1D     (1u << 4)   /* mapeo de teselas de OBJ en 1D */
#define GPU2D_DISPCNT_BLANK      (1u << 7)   /* pantalla en blanco forzada */
#define GPU2D_DISPCNT_BG(n)      (1u << (8 + (n)))
#define GPU2D_DISPCNT_OBJ        (1u << 12)
#define GPU2D_DISPCNT_OBJ_SHIFT(d) (((d) >> 20) & 3u)  /* granularidad 1D: 32 << n bytes */
#define GPU2D_DISPCNT_CHAR_OFF(d)  ((((d) >> 24) & 7u) * 0x10000u)
#define GPU2D_DISPCNT_SCR_OFF(d)   ((((d) >> 27) & 7u) * 0x10000u)

/* BGxCNT. */
#define GPU2D_BGCNT_PRIO(c)   ((c) & 3u)
#define GPU2D_BGCNT_CHAR(c)   ((((c) >> 2) & 15u) * 0x4000u)
#define GPU2D_BGCNT_256(c)    (((c) >> 7) & 1u)
#define GPU2D_BGCNT_SCR(c)    ((((c) >> 8) & 31u) * 0x800u)
#define GPU2D_BGCNT_SIZE(c)   (((c) >> 14) & 3u)  /* 0 256², 1 512×256, 2 256×512, 3 512² */

/* Registros que cambian por línea. */
enum {
	GPU2D_REG_DISPCNT = 0,
	GPU2D_REG_BGCNT = 1,    /* 1..4 */
	GPU2D_REG_HOFS = 5,     /* 5..8 */
	GPU2D_REG_VOFS = 9,     /* 9..12 */
	GPU2D_REG_PAL = 16,     /* escritura de paleta: valor = índice << 16 | color */
};

/* Un cambio: antes de dibujar la línea `line`, `reg` pasa a `value`. La
 * lista va ordenada por línea; la sobreescritura de un registro en el mismo
 * cuadro se resuelve en el orden de la lista. 8 bytes. */
typedef struct gpu2d_change {
	uint16_t line;
	uint16_t reg;
	uint32_t value;
} gpu2d_change;

/* Descriptor de un motor para un cuadro. Las direcciones son efectivas
 * (EA) en memoria principal; en el host, punteros convertidos. Las
 * regiones de VRAM son la vista ya resuelta de los bancos (los fondos de
 * A: 512 KB; OBJ de A: 256 KB; B: 128 KB y 128 KB); las lecturas se
 * envuelven con `size - 1`. Las paletas son 256 colores de fondo seguidos
 * de 256 de OBJ (1 KB, BGR555 little-endian). */
typedef struct gpu2d_engine {
	uint64_t bg_vram_ea, obj_vram_ea, pal_ea, oam_ea, chg_ea, out_ea;
	uint32_t bg_size, obj_size;      /* potencias de 2 */
	uint32_t nchg;
	uint32_t dispcnt;                /* registros al empezar el cuadro */
	uint16_t bgcnt[GPU2D_NBG];
	uint16_t hofs[GPU2D_NBG];
	uint16_t vofs[GPU2D_NBG];
	uint32_t pad[1];
} gpu2d_engine;

/* Un cuadro: las dos pantallas. 128 bytes de alineación para el DMA. */
typedef struct gpu2d_frame {
	gpu2d_engine eng[2];
} __attribute__((aligned(128))) gpu2d_frame;

/* BGR555 de la DS → RGBA5551 (alfa 1). */
static inline uint16_t gpu2d_to_5551(uint16_t c)
{
	return (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | 1u);
}

#define GPU2D_BLANK_PIXEL 0xFFFFu

/* ---- Referencia (gpu2d_ref.c) ------------------------------------------ */
/* Dibuja la pantalla en `out` (GPU2D_W × GPU2D_H medias palabras). Ignora
 * `e->out_ea`. */
void gpu2d_ref_engine(const gpu2d_engine *e, uint16_t *out);

/* ---- Compositor por líneas (gpu2d_compose.c) ---------------------------- */
/* Espacio de trabajo del compositor (el local store del SPE o un búfer del
 * host). Su tamaño y alineación los fija gpu2d_compose.h. */
struct gpu2d_ctx;

/* ---- Escenas sintéticas (gpu2d_scene.c) --------------------------------- */
enum {
	GPU2D_SCENE_TIPICA,   /* 4 fondos de 4 bits, algo de raster, 40 OBJ */
	GPU2D_SCENE_PESADA,   /* raster en cada línea en todos los fondos, 128 OBJ grandes */
	GPU2D_SCENE_EXTREMA,  /* peor caso de DMA: cada línea cambia de fila de teselas, 128 OBJ de 64×64 de 8 bits */
	GPU2D_SCENE_ALEATORIA, /* configuración aleatoria por `seed`: para verificar */
	GPU2D_SCENE_COUNT
};

/* Memoria de una pantalla para una escena. Todo alineado a 128 bytes. */
typedef struct gpu2d_scene_mem {
	uint8_t *bg_vram, *obj_vram, *pal, *oam;
	gpu2d_change *chg;
	uint32_t bg_size, obj_size, chg_cap;
} gpu2d_scene_mem;

/* Tamaños de las regiones de A (engine = 0) y de B (1). */
uint32_t gpu2d_scene_bg_size(int engine);
uint32_t gpu2d_scene_obj_size(int engine);
#define GPU2D_SCENE_CHG_CAP 8192

/* Llena la memoria y el descriptor (sin `out_ea`) de un motor con la escena
 * `scene`; `seed` la varía. Determinista en cualquier host. */
void gpu2d_scene_build(gpu2d_engine *e, const gpu2d_scene_mem *m, int engine, int scene, uint32_t seed);

/* FNV-1a de 64 bits sobre los valores de una imagen (no sobre sus bytes:
 * da lo mismo en LE y en BE). */
uint64_t gpu2d_hash_image(const uint16_t *img);

#endif
