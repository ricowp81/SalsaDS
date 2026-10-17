/*
 * SalsaDS — marco de pruebas para el ARM7 (ver sds.h): la misma interfaz que
 * sds.c pero sin pantalla. Los resultados van a SDS_ARM7_RESULTS_ADDR; el
 * ARM9 de la ROM los lee y los suma a su veredicto.
 */
#include "sds.h"

static struct sds_results *const R = (struct sds_results *)SDS_RESULTS_ADDR;
static struct sds_fail *const FAILLOG = (struct sds_fail *)(SDS_RESULTS_ADDR + SDS_FAILLOG_OFFSET);
static const char *group_name;

static void close_group(void)
{
	if (!group_name)
		return;
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
}

void sds_group(const char *name)
{
	close_group();
	group_name = name;
}

void sds_check(uint32_t got, uint32_t want, uint32_t id)
{
	R->total++;
	if (got != want) {
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
