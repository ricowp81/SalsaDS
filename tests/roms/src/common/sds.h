/*
 * SalsaDS — marco de pruebas para las ROMs homebrew de la DS (ARM9).
 *
 * Cada ROM de prueba reporta OK/FALLO por dos vías que un emulador puede
 * leer sin ayuda humana:
 *
 *  1. Un bloque de resultados en RAM principal, en la dirección fija
 *     SDS_RESULTS_ADDR (struct sds_results, little-endian). Cuando
 *     `state` vale 1 la ROM terminó; `verdict` es 1 = OK o 2 = FALLO.
 *     Se lee con el stub de GDB del emulador, con un volcado de memoria o,
 *     en SalsaDS, directamente.
 *  2. La pantalla de arriba (modo de visualización directa de VRAM: el
 *     banco A en modo LCDC y los motores 2D sin usar): una línea por grupo
 *     de pruebas y una franja verde («TODO OK») o roja («FALLO»).
 *
 * Código propio de SalsaDS; solo usa registros de hardware, no libnds.
 */
#ifndef SDS_H
#define SDS_H

#include <stdint.h>

#define SDS_RESULTS_ADDR 0x02380000u
#define SDS_MAGIC 0x52534453u /* "SDSR" en little-endian */
#define SDS_VERDICT_OK 1u
#define SDS_VERDICT_FAIL 2u

struct sds_results {
	uint32_t magic;            /* 0x00 SDS_MAGIC */
	uint32_t version;          /* 0x04 1 */
	uint32_t state;            /* 0x08 0 = corriendo, 1 = terminada */
	uint32_t total;            /* 0x0C comprobaciones hechas */
	uint32_t failed;           /* 0x10 comprobaciones fallidas */
	uint32_t groups;           /* 0x14 grupos terminados */
	uint32_t first_fail_group; /* 0x18 índice (desde 1) del primer grupo con fallo, 0 si no hay */
	uint32_t first_fail_id;    /* 0x1C identificador del primer fallo (línea de fuente) */
	uint32_t first_fail_got;   /* 0x20 */
	uint32_t first_fail_want;  /* 0x24 */
	uint32_t verdict;          /* 0x28 SDS_VERDICT_* (solo con state == 1) */
	uint32_t reserved[5];      /* 0x2C */
};

void sds_init(const char *title);
/* Empieza un grupo de pruebas (cierra el anterior y escribe su línea). */
void sds_group(const char *name);
/* Compara; `id` identifica la comprobación en el reporte (SDS_CHECK usa la línea). */
void sds_check(uint32_t got, uint32_t want, uint32_t id);
/* Cierra el último grupo, escribe el veredicto y no vuelve. */
void sds_finish(void) __attribute__((noreturn));
/* Pone un texto en una línea de la pantalla (para ROMs que lo necesiten). */
void sds_print_row(int row, const char *text);

#define SDS_CHECK(got, want) sds_check((uint32_t)(got), (uint32_t)(want), __LINE__)

#endif
