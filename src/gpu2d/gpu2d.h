/*
 * SalsaDS — motores 2D de la DS (A y B): estructuras compartidas (Fase 2.1).
 *
 * Qué entra y qué sale de la composición de un cuadro de una pantalla (un «motor»). El mismo código portable
 * corre en la Mac, en el PPE y en un SPE:
 *
 *   gpu2d_ref.c      referencia píxel por píxel, lo más directa posible (la «verdad» de las pruebas)
 *   gpu2d_compose.c  compositor por líneas con DMA, caché de teselas y kernels SIMD: es el que corre en el SPE
 *                    (y, con kernels escalares, en el host)
 *   gpu2d_scene.c    escenas sintéticas y aleatorias deterministas para probar y medir
 *
 * Qué hace cada motor (todo en little-endian, como la memoria de la DS):
 *   - 4 fondos: de texto (4 y 8 bits, 4 tamaños, volteos, desplazamiento, paletas extendidas), afines (mapa de
 *     8 bits), extendidos (mapa de 16 bits, mapa de bits de 256 colores y de color directo) y el de mapa de bits
 *     grande del modo 6 (motor A). Rotación y escala con los puntos de referencia por línea, envoltura o
 *     transparencia fuera del área, mosaico.
 *   - 128 OBJ: normales, afines (y de doble tamaño), semitransparentes, de ventana de OBJ y de mapa de bits;
 *     mapeo 1D y 2D, 4 y 8 bits, paletas extendidas, mosaico.
 *   - Ventanas 0 y 1 y ventana de OBJ; prioridades; mezcla alfa y brillo (BLDCNT/BLDALPHA/BLDY); brillo maestro.
 *   - Modos de pantalla de DISPCNT: 0 (blanca), 1 (gráficos), 2 (VRAM, motor A) y 3 (memoria principal, motor A).
 *   - La capa 3D del motor A (DISPCNT bit 3, BG0): la imagen del motor de render (Fase 3.2, `img3d_ea`), con BG0HOFS y
 *     la mezcla de los píxeles 3D translúcidos con el objetivo 2. No hay captura de pantalla.
 *
 * Un cuadro llega como una INSTANTÁNEA tomada al empezar el cuadro (línea 0) más un REGISTRO DE CAMBIOS por línea
 * de lo que el juego escribió mientras se dibujaba (registros de video, paleta, OAM): los efectos de raster
 * dependen de eso. La VRAM, en cambio, solo se toma al empezar el cuadro: lo que el juego escriba en ella durante
 * las líneas visibles no se ve hasta el cuadro siguiente (no se vigila: los juegos actualizan la VRAM en el
 * VBlank).
 *
 * La VRAM de la instantánea es el arreglo de los 9 bancos del núcleo (A..I en ese orden, 656 KB) seguido de
 * 128 bytes en cero; las vistas de fondos y de OBJ (lo que mapea VRAMCNT) llegan como tablas de páginas de 16 KB
 * con el desplazamiento de cada página dentro de ese arreglo (o G2D_UNMAPPED: se lee cero). Cada vista se repite
 * (envuelve) en su tamaño, que es una potencia de 2.
 *
 * La imagen de salida son medias palabras RGBA5551 (bit 0 = 1) en el orden de bytes del que las lee (el PPE y el
 * SPE: big-endian; el host de pruebas: el suyo), listas para una textura del RSX de pitch 512.
 */
#ifndef SALSADS_GPU2D_GPU2D_H
#define SALSADS_GPU2D_GPU2D_H

#include <stdint.h>

#define GPU2D_W 256
#define GPU2D_H 192
#define GPU2D_NBG 4
#define GPU2D_NOBJ 128

#define G2D_VRAM_SIZE 0xA4000u   /* 656 KB: bancos A..I */
#define G2D_VRAM_ZERO 128u       /* bytes en cero tras la VRAM de la instantánea: lo que se lee de una página sin mapear */
#define G2D_UNMAPPED 0xFFFFFFFFu
#define G2D_BG_PAGES_A 32        /* vistas de 16 KB por página: A 512 KB / 256 KB, B 128 KB / 128 KB */
#define G2D_OBJ_PAGES_A 16
#define G2D_BG_PAGES_B 8
#define G2D_OBJ_PAGES_B 8

/* Registros de E/S del motor: medias palabras desde su base (0x04000000 el A, 0x04001000 el B), índice =
 * desplazamiento / 2. Cubren los desplazamientos 0x00..0x6F. */
#define G2D_NREGS 56
enum {
	G2D_R_DISPCNT = 0,    /* 32 bits: 0 y 1 */
	G2D_R_BGCNT = 4,      /* 4..7 */
	G2D_R_BGHOFS = 8,     /* BG0 8, BG1 10, BG2 12, BG3 14 */
	G2D_R_BGVOFS = 9,     /* 9, 11, 13, 15 */
	G2D_R_BG2PA = 16,     /* PA PB PC PD, X (2), Y (2) */
	G2D_R_BG3PA = 24,
	G2D_R_WIN0H = 32,
	G2D_R_WIN1H = 33,
	G2D_R_WIN0V = 34,
	G2D_R_WIN1V = 35,
	G2D_R_WININ = 36,
	G2D_R_WINOUT = 37,
	G2D_R_MOSAIC = 38,
	G2D_R_BLDCNT = 40,
	G2D_R_BLDALPHA = 41,
	G2D_R_BLDY = 42,
	G2D_R_DISPCAP = 50,   /* 32 bits (motor A; no se usa) */
	G2D_R_MASTER_BRIGHT = 54,
};

/* DISPCNT. */
#define G2D_DC_BGMODE(d)     ((d) & 7u)
#define G2D_DC_3D            (1u << 3)    /* BG0 es la capa 3D (motor A) */
#define G2D_DC_OBJ_1D        (1u << 4)    /* mapeo de teselas de OBJ en 1D */
#define G2D_DC_OBJBMP_256    (1u << 5)    /* OBJ de mapa de bits 2D: 256 de ancho (si no, 128) */
#define G2D_DC_OBJBMP_1D     (1u << 6)    /* OBJ de mapa de bits 1D */
#define G2D_DC_BLANK         (1u << 7)    /* pantalla en blanco forzada */
#define G2D_DC_BG(n)         (1u << (8 + (n)))
#define G2D_DC_OBJ           (1u << 12)
#define G2D_DC_WIN0          (1u << 13)
#define G2D_DC_WIN1          (1u << 14)
#define G2D_DC_OBJWIN        (1u << 15)
#define G2D_DC_DISPMODE(d)   (((d) >> 16) & 3u)
#define G2D_DC_VRAMBANK(d)   (((d) >> 18) & 3u)
#define G2D_DC_OBJ_SHIFT(d)  (((d) >> 20) & 3u)   /* frontera de OBJ 1D: 32 << n bytes */
#define G2D_DC_OBJBMP_BND    (1u << 22)           /* frontera de OBJ bitmap 1D: 256 (si no, 128) */
#define G2D_DC_CHAR_OFF(d)   ((((d) >> 24) & 7u) * 0x10000u)
#define G2D_DC_SCR_OFF(d)    ((((d) >> 27) & 7u) * 0x10000u)
#define G2D_DC_BGEXTPAL      (1u << 30)
#define G2D_DC_OBJEXTPAL     (1u << 31)

/* BGxCNT. */
#define G2D_BGCNT_PRIO(c)    ((c) & 3u)
#define G2D_BGCNT_CHAR(c)    ((((c) >> 2) & 15u) * 0x4000u)
#define G2D_BGCNT_MOSAIC     (1u << 6)
#define G2D_BGCNT_256(c)     (((c) >> 7) & 1u)
#define G2D_BGCNT_SCR(c)     ((((c) >> 8) & 31u) * 0x800u)
#define G2D_BGCNT_SCR_BMP(c) ((((c) >> 8) & 31u) * 0x4000u)
#define G2D_BGCNT_WRAP       (1u << 13)           /* BG2/BG3 afines: envolver (si no, transparente fuera) */
#define G2D_BGCNT_SLOT2      (1u << 13)           /* BG0/BG1 de texto con paleta extendida: ranura 2 / 3 */
#define G2D_BGCNT_SIZE(c)    (((c) >> 14) & 3u)

/* Cambios que el juego hizo mientras se dibujaba el cuadro. `line` es la PRIMERA línea (0..191) en la que se ve
 * el cambio (los de antes de dibujar la línea 0 ya están en la instantánea). La lista va ordenada por línea (y,
 * dentro de una línea, por orden de escritura). */
typedef struct gpu2d_change {
	uint16_t line;
	uint16_t reg;     /* < G2D_NREGS: media palabra de E/S; o G2D_CHG_PAL / G2D_CHG_OAM */
	uint32_t value;
} gpu2d_change;

#define G2D_CHG_PAL 0x40u   /* value = índice (0..511: 256 de fondo y 256 de OBJ) << 16 | color BGR555 */
#define G2D_CHG_OAM 0x41u   /* value = media palabra de la OAM (0..511) << 16 | dato */

/* Bandera del descriptor. */
#define G2D_F_ENGINE_B 1u

/* Descriptor de un motor para un cuadro. Las direcciones son efectivas (EA) en memoria principal; en el host,
 * punteros convertidos. 16 bytes de alineación (DMA) y múltiplo de 16 de tamaño. */
typedef struct gpu2d_engine {
	uint64_t vram_ea;    /* instantánea de la VRAM: G2D_VRAM_SIZE bytes + G2D_VRAM_ZERO de ceros */
	uint64_t pal_ea;     /* 1 KB: 256 colores de fondo y 256 de OBJ (BGR555) */
	uint64_t oam_ea;     /* 1 KB */
	uint64_t chg_ea;     /* nchg × gpu2d_change */
	uint64_t out_ea;     /* GPU2D_W × GPU2D_H medias palabras */
	uint64_t mmem_ea;    /* modo 3: GPU2D_H líneas de 512 bytes (BGR555); 0 si no hay */
	uint64_t img3d_ea;   /* motor A: la capa 3D (Fase 3.2, src/gpu3d/r3d.h: 256 × 192 palabras de 32 bits); 0 = vacía */
	uint32_t nchg;
	uint32_t flags;      /* G2D_F_* */
	uint32_t bgpages;    /* tamaño de la vista de fondos en páginas de 16 KB (potencia de 2) */
	uint32_t objpages;   /* ídem, de OBJ */
	uint16_t regs[G2D_NREGS];  /* registros al empezar el cuadro (los cambios del cuadro van aparte) */
	uint32_t bgpg[G2D_BG_PAGES_A];    /* desplazamiento de cada página de la vista de fondos, o G2D_UNMAPPED */
	uint32_t objpg[G2D_OBJ_PAGES_A];  /* ídem, de OBJ */
	uint32_t extbg[4];   /* paletas extendidas de fondo: desplazamiento de cada ranura de 8 KB, o G2D_UNMAPPED */
	uint32_t extobj;     /* paleta extendida de OBJ (8 KB) */
	uint32_t vdisp[4];   /* modo 2: banco de VRAM A..D como LCDC (desplazamiento) o G2D_UNMAPPED */
	uint32_t pad[1];
} __attribute__((aligned(16))) gpu2d_engine;

/* Un cuadro: las dos pantallas. 128 bytes de alineación para el DMA. */
typedef struct gpu2d_frame {
	gpu2d_engine eng[2];
} __attribute__((aligned(128))) gpu2d_frame;

static inline uint32_t g2d_dispcnt(const gpu2d_engine *e)
{
	return (uint32_t)e->regs[G2D_R_DISPCNT] | ((uint32_t)e->regs[G2D_R_DISPCNT + 1] << 16);
}

/* BGR555 de la DS → RGBA5551 (alfa 1). */
static inline uint16_t gpu2d_to_5551(uint16_t c)
{
	return (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | 1u);
}

/* ---- Referencia (gpu2d_ref.c) ------------------------------------------------------------------------ */
/* Dibuja la pantalla en `out` (GPU2D_W × GPU2D_H medias palabras RGBA5551). Ignora `e->out_ea`. */
void gpu2d_ref_engine(const gpu2d_engine *e, uint16_t *out);

/* Lecturas de la referencia a páginas de VRAM sin banco desde la última vez que se puso en cero (diagnóstico). */
extern unsigned gpu2d_ref_unmapped_reads;

/* FNV-1a de 64 bits sobre los valores de una imagen (no sobre sus bytes: da lo mismo en LE y en BE). */
uint64_t gpu2d_hash_image(const uint16_t *img);

#endif
