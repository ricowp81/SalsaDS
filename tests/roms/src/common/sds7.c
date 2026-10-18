/*
 * SalsaDS — marco de pruebas para el ARM7 (ver sds.h): la misma interfaz que
 * sds.c pero sin pantalla. Los resultados van a SDS_ARM7_RESULTS_ADDR; el
 * ARM9 de la ROM los lee y los suma a su veredicto.
 */
#include "sds.h"

static struct sds_results *const R = (struct sds_results *)SDS_RESULTS_ADDR;
static struct sds_fail *const FAILLOG = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_FAILLOG_OFFSET);
static const char *group_name;
static uint32_t group_checks, group_failed;
static struct sds_x7 *const X = (struct sds_x7 *)SDS_X7_ADDR;

static void copy_name(char *dst, const char *src, unsigned n)
{
	unsigned i = 0;

	for (; src && src[i] && i < n - 1; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = 0;
}

static void close_group(void)
{
	if (!group_name)
		return;
	if (X->ngroups < SDS_X7_GROUPS) {
		copy_name(X->g[X->ngroups].name, group_name, 12);
		X->g[X->ngroups].checks = group_checks;
		X->g[X->ngroups].failed = group_failed;
	}
	X->ngroups++;
	R->groups++;
	group_name = 0;
}

void sds_init(const char *title)
{
	volatile uint32_t *w = (volatile uint32_t *)R;

	(void)title;
	for (unsigned i = 0; i < (SDS_DIVERGE_OFFSET + SDS_DIVERGE_MAX * sizeof(struct sds_fail)) / 4; i++)
		w[i] = 0;
	R->magic = SDS_MAGIC;
	R->version = 1;
	for (unsigned i = 0; i < sizeof *X / 4; i++)
		((volatile uint32_t *)X)[i] = 0;
	X->magic = SDS_X7_MAGIC;
}

void sds_group(const char *name)
{
	close_group();
	group_name = name;
	group_checks = group_failed = 0;
}

void sds_check(uint32_t got, uint32_t want, uint32_t id)
{
	R->total++;
	group_checks++;
	if (got != want) {
		group_failed++;
		if (R->failed < SDS_FAILLOG_MAX) {
			FAILLOG[R->failed].group = R->groups + 1;
			FAILLOG[R->failed].id = id;
			FAILLOG[R->failed].got = got;
			FAILLOG[R->failed].want = want;
		}
		R->failed++;
		if (!R->first_fail_group) {
			R->first_fail_group = R->groups + 1;
			R->first_fail_id = id;
			R->first_fail_got = got;
			R->first_fail_want = want;
		}
	}
}

void sds_diverge(uint32_t got, uint32_t want, uint32_t id)
{
	struct sds_fail *log = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_DIVERGE_OFFSET);

	if (got == want)
		return;
	if (R->diverge < SDS_DIVERGE_MAX) {
		log[R->diverge].group = R->groups + 1;
		log[R->diverge].id = id;
		log[R->diverge].got = got;
		log[R->diverge].want = want;
	}
	R->diverge++;
}

void sds_diverge_at(uint32_t got, uint32_t doc, uint32_t melon, uint32_t id, const char *file)
{
	const char *b = file;

	for (const char *p = file; *p; p++)
		if (*p == '/')
			b = p + 1;
	if (X->nsites < SDS_X7_SITES) {
		unsigned n = X->nsites++;

		X->s[n].id = id;
		X->s[n].got = got;
		X->s[n].doc = doc;
		X->s[n].melon = melon;
		X->s[n].group = R->groups + 1;
		copy_name(X->s[n].file, b, 20);
	}
	X->nsites_total++;
	sds_diverge(got, doc, id);
}

void sds_note(const char *text)
{
	(void)text;
}

void sds_mark(uint32_t n)
{
	R->mark = n;
}

void sds_print_row(int row, const char *text)
{
	(void)row;
	(void)text;
}

void sds_finish(void)
{
	close_group();
	R->verdict = R->failed ? SDS_VERDICT_FAIL : SDS_VERDICT_OK;
	R->state = 1;
	/* Aviso al ARM9 por IPCSYNC (E/S: no pasa por la caché de datos, a diferencia del bloque de resultados):
	 * 0xA = todo bien, 0xB = hubo fallos. El detalle está en el bloque, que se lee con el emulador. */
	*(volatile uint16_t *)0x04000180 = (uint16_t)((R->failed ? SDS_IPC_FAIL : SDS_IPC_OK) << 8);
	for (;;)
		;
}
