/*
 * SalsaDS — marco de pruebas para las ROMs homebrew de la DS (ARM9, y ARM7
 * con sds7.c: sin pantalla, su bloque de resultados va aparte).
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

#define SDS_ARM9_RESULTS_ADDR 0x02380000u
#define SDS_IPC_OK 0xAu  /* el ARM7 avisa al ARM9 por IPCSYNC (bits 8-11 de su lado) de cómo terminó */
#define SDS_IPC_FAIL 0xBu
#define SDS_ARM7_RESULTS_ADDR 0x02390000u /* el ARM7 deja el suyo aquí; el ARM9 lo añade al veredicto */
#ifdef ARM7
#define SDS_RESULTS_ADDR SDS_ARM7_RESULTS_ADDR
#else
#define SDS_RESULTS_ADDR SDS_ARM9_RESULTS_ADDR
#endif
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
	uint32_t diverge;          /* 0x2C comprobaciones informativas que no coincidieron (no cuentan como fallo) */
	uint32_t mark;             /* 0x30 marcador de avance para depurar (sds_mark) */
	uint32_t reserved[3];      /* 0x34 */
};

/* Registro de fallos, a continuación del bloque (SDS_RESULTS_ADDR + 0x40): los primeros SDS_FAILLOG_MAX fallos
 * con el grupo, el identificador, lo obtenido y lo esperado. Sirve para ver de un vistazo todo lo que falla. */
#define SDS_FAILLOG_OFFSET 0x40u
#define SDS_FAILLOG_MAX 16u
struct sds_fail {
	uint32_t group; /* índice del grupo (desde 1) */
	uint32_t id;
	uint32_t got;
	uint32_t want;
};

/* Divergencias: el comportamiento esperado sale de GBATEK o del manual de ARM pero un emulador de referencia
 * (melonDS 1.1) hace otra cosa. Se anotan aparte (SDS_DIVERGE_OFFSET, hasta SDS_DIVERGE_MAX) y no cambian el
 * veredicto: sirven para ver, con cualquier emulador, en qué casos de borde se separa de lo documentado. */
#define SDS_DIVERGE_OFFSET 0x140u
#define SDS_DIVERGE_MAX 32u

/* Deja un número en el bloque de resultados: dice hasta dónde llegó la ROM si se cuelga. */
void sds_mark(uint32_t n);

void sds_init(const char *title);
/* Empieza un grupo de pruebas (cierra el anterior y escribe su línea). */
void sds_group(const char *name);
/* Compara; `id` identifica la comprobación en el reporte (SDS_CHECK usa la línea). */
void sds_check(uint32_t got, uint32_t want, uint32_t id);
/* Cierra el último grupo, escribe el veredicto y no vuelve. */
void sds_finish(void) __attribute__((noreturn));
/* Pone un texto en una línea de la pantalla (para ROMs que lo necesiten). */
void sds_print_row(int row, const char *text);

/* Para las ROMs de gráficos: ni usan la pantalla para el texto (la imagen es el resultado) ni cambian los motores.
 * `sds_init_quiet` solo limpia y publica el bloque de resultados; `sds_finish_quiet` publica el veredicto (1 = OK) y
 * no vuelve. Las expectativas de píxeles (`sds_expect_pixel`) quedan en SDS_EXPECT_ADDR para que quien mira la
 * imagen (tools/gfxcheck.py) compruebe los píxeles conocidos. */
void sds_init_quiet(void);
void sds_finish_quiet(int ok) __attribute__((noreturn));
#define SDS_EXPECT_ADDR 0x02380800u
#define SDS_EXPECT_MAX 1000u
#define SDS_EXPECT_MAGIC 0x45505847u /* "GXPE" */
/* screen 0 = motor A, 1 = motor B; color BGR555 que debe tener el píxel (x, y) de ese motor. */
void sds_expect_pixel(int screen, int x, int y, uint16_t color);

/* Registro ampliado para correr en una DS real (log en la SD, ver sds.c). Anota TODAS las llamadas a SDS_DIVERGE (también
 * las que coinciden con la documentación) con el valor del hardware, el documentado y el de melonDS 1.1. Para las
 * comprobaciones sin valor de melonDS conocido se usa SDS_MELON_NA. No toca el bloque de resultados en RAM. */
#define SDS_MELON_NA 0xFFFFFFFEu
void sds_diverge_at(uint32_t got, uint32_t doc, uint32_t melon, uint32_t id, const char *file);
#define SDS_DIVERGE_M(got, doc, melon) sds_diverge_at((uint32_t)(got), (uint32_t)(doc), (uint32_t)(melon), __LINE__, __FILE__)

/* El ARM7 deja su registro ampliado aquí (grupos y sitios de divergencia) para que el ARM9 lo añada al log. */
#define SDS_X7_ADDR 0x023B0000u
#define SDS_X7_MAGIC 0x37584453u /* "SDX7" */
#define SDS_X7_GROUPS 24u
#define SDS_X7_SITES 64u
struct sds_x7 {
	uint32_t magic, ngroups, nsites, nsites_total;
	struct {
		char name[12];
		uint32_t checks, failed;
	} g[SDS_X7_GROUPS];
	struct {
		uint32_t id, got, doc, melon, group;
		char file[20];
	} s[SDS_X7_SITES];
};

/* Una nota de texto para el log de la SD (p. ej. "no aplica en flashcart"); en el ARM7 no hace nada. */
void sds_note(const char *text);

#define SDS_CHECK(got, want) sds_check((uint32_t)(got), (uint32_t)(want), __LINE__)
/* Informativa: no falla, solo se registra si difiere. */
void sds_diverge(uint32_t got, uint32_t want, uint32_t id);
#define SDS_DIVERGE(got, want) sds_diverge_at((uint32_t)(got), (uint32_t)(want), SDS_MELON_NA, __LINE__, __FILE__)
#define SDS_DIVERGE_ID(got, want, id) sds_diverge_at((uint32_t)(got), (uint32_t)(want), SDS_MELON_NA, (id), __FILE__)
#define SDS_CHECK_ID(got, want, id) sds_check((uint32_t)(got), (uint32_t)(want), (id))

#endif
