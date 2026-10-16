/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — caché de fragment programs generados (rsxkit/fpgen.h).
 *
 * Cada descripción distinta se genera una vez: el microcódigo va a un
 * lugar fijo de un pool en la memoria local del RSX (RSXKIT_FP_SLOT bytes
 * por programa, RSXKIT_FP_CACHE programas) y queda un rsxFragmentProgram
 * mínimo con lo que rsxLoadFragmentProgramLocation necesita (registros,
 * control, TEXCOORD que lee). La búsqueda es por hash de la descripción
 * canónica en una tabla abierta.
 *
 * Si el pool se llena (más programas distintos que RSXKIT_FP_CACHE desde el
 * arranque) se espera al RSX y se vacía todo: cuesta un cuadro, se cuenta
 * en las estadísticas y no debería pasar con un juego (decenas de
 * programas). Los programas no se modifican una vez escritos: el RSX
 * puede tener el microcódigo en su caché de instrucciones y el lugar de un
 * programa solo se reutiliza tras ese vaciado.
 */
#include <string.h>

#include "internal.h"

#define RSXKIT_FP_CACHE 128
#define RSXKIT_FP_SLOT  ((RSXKIT_FP_MAX_WORDS * 4 + 63) & ~63u)
#define RSXKIT_FP_TABLE (4 * RSXKIT_FP_CACHE)  /* potencia de 2 */

struct fp_entry {
	rsxkit_fp_desc desc;
	rsxFragmentProgram hdr;
	u32 offset;
};

static struct {
	u8 *pool;
	u32 pool_offset;
	struct fp_entry e[RSXKIT_FP_CACHE];
	u16 table[RSXKIT_FP_TABLE];  /* índice + 1; 0 = libre */
	u32 n;
	u32 generation;
	rsxkit_fp_stats stats;
} fc;

static void clear_cache(void)
{
	memset(fc.table, 0, sizeof fc.table);
	fc.n = 0;
	fc.generation++;
	fc.stats.programs = 0;
}

void rsxkit_fp_flush(void)
{
	if (fc.pool)
		clear_cache();
}

uint32_t rsxkit_fp_generation(void)
{
	return fc.generation;
}

static int cache_init(void)
{
	if (fc.pool)
		return RSXKIT_OK;
	fc.pool = rsxMemalign(64, RSXKIT_FP_CACHE * RSXKIT_FP_SLOT);
	if (!fc.pool)
		return RSXKIT_ENOMEM;
	rsxAddressToOffset(fc.pool, &fc.pool_offset);
	clear_cache();
	return RSXKIT_OK;
}

void rsxkit_fp_shutdown(void)
{
	/* La memoria del RSX queda reservada hasta el fin del proceso (como
	 * los programas precompilados). */
	memset(&fc, 0, sizeof fc);
}

void rsxkit_fp_get_stats(rsxkit_fp_stats *out)
{
	*out = fc.stats;
}

void rsxkit_fp_count_load(void)
{
	fc.stats.loads++;
}

const struct rsxkit_fp_cached *rsxkit_fp_lookup(const rsxkit_fp_desc *desc)
{
	if (cache_init() != RSXKIT_OK)
		return NULL;
	rsxkit_fp_desc d = *desc;
	if (rsxkit_fp_desc_canon(&d) != 0)
		return NULL;
	u32 h = rsxkit_fp_desc_hash(&d), i = h & (RSXKIT_FP_TABLE - 1);
	for (;;) {
		u16 t = fc.table[i];
		if (!t)
			break;
		if (!memcmp(&fc.e[t - 1].desc, &d, sizeof d))
			return (const struct rsxkit_fp_cached *)&fc.e[t - 1];
		i = (i + 1) & (RSXKIT_FP_TABLE - 1);
	}
	if (fc.n == RSXKIT_FP_CACHE) {
		rsxkit_finish();
		clear_cache();
		fc.stats.evictions++;
		i = h & (RSXKIT_FP_TABLE - 1);
	}
	/* Generar y copiar a su lugar, con las mitades de cada palabra
	 * intercambiadas como las lee el RSX. */
	u32 words[RSXKIT_FP_MAX_WORDS];
	rsxkit_fp_info info;
	int n = rsxkit_fpgen_emit(&d, words, RSXKIT_FP_MAX_WORDS, &info);
	if (n <= 0)
		return NULL;
	struct fp_entry *e = &fc.e[fc.n];
	u32 *dst = (u32 *)(fc.pool + fc.n * RSXKIT_FP_SLOT);
	for (int k = 0; k < n; k++)
		dst[k] = rsxkit_fp_word_swap(words[k]);
	e->desc = d;
	memset(&e->hdr, 0, sizeof e->hdr);
	e->hdr.num_regs = (u16)info.num_regs;
	e->hdr.num_insn = (u16)info.ninsn;
	e->hdr.fp_control = info.fp_control;
	e->hdr.texcoords = info.texcoords;
	e->hdr.texcoord2D = info.texcoord2d;
	e->hdr.texcoord3D = info.texcoord3d;
	e->offset = fc.pool_offset + fc.n * RSXKIT_FP_SLOT;
	fc.table[i] = (u16)(fc.n + 1);
	fc.n++;
	fc.stats.programs = fc.n;
	fc.stats.generated++;
	return (const struct rsxkit_fp_cached *)e;
}
