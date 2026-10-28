/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnóstico del host: SND_TRACE=prefijo, SND_TRACE_FROM/TO en ticks nativos.
 * PCM16 LE intercalado: .raw.pcm (16 voces), .voices.pcm (16 pares L/R).
 * El núcleo de producción no compila ni llama esta instrumentación. */
#ifdef SND_DIAGNOSTIC
static FILE *snd_trace_raw, *snd_trace_mix, *snd_trace_state;
static uint64_t snd_trace_from, snd_trace_to = UINT64_MAX;

static void snd_trace_close(void)
{
	if (snd_trace_raw) fclose(snd_trace_raw);
	if (snd_trace_mix) fclose(snd_trace_mix);
	if (snd_trace_state) fclose(snd_trace_state);
}

void snd_trace_tick(const snd_state *st, uint64_t tick, const int16_t *raw, const int16_t *mix)
{
	static int initialized;
	static uint32_t last[16][7];
	static int32_t lastpos[16];
	uint8_t bytes[64];
	if (!initialized) {
		const char *prefix = getenv("SND_TRACE"), *v;
		char path[1024];
		initialized = 1;
		if (!prefix) return;
		snprintf(path, sizeof path, "%s.raw.pcm", prefix); snd_trace_raw = fopen(path, "wb");
		snprintf(path, sizeof path, "%s.voices.pcm", prefix); snd_trace_mix = fopen(path, "wb");
		snprintf(path, sizeof path, "%s.state.tsv", prefix); snd_trace_state = fopen(path, "w");
		if (!snd_trace_raw || !snd_trace_mix || !snd_trace_state) {
			fprintf(stderr, "no se pudo abrir SND_TRACE\n"); exit(1);
		}
		v = getenv("SND_TRACE_FROM"); if (v) snd_trace_from = strtoull(v, NULL, 0);
		v = getenv("SND_TRACE_TO"); if (v) snd_trace_to = strtoull(v, NULL, 0);
		fprintf(snd_trace_state, "tick\tch\ton\tfmt\tcnt\ttmr\tsad\tpnt\tlen\tpos\ttcnt\tadv\tadi\tbias\tsoundcnt\tcap0\tcap1\tcap0pos\tcap1pos\n");
		atexit(snd_trace_close);
	}
	if (!snd_trace_raw || tick < snd_trace_from || tick >= snd_trace_to) return;
	for (int i = 0; i < 16; i++) wr16le(bytes + 2*i, (uint16_t)raw[i]);
	if (fwrite(bytes, 32, 1, snd_trace_raw) != 1) exit(1);
	for (int i = 0; i < 32; i++) wr16le(bytes + 2*i, (uint16_t)mix[i]);
	if (fwrite(bytes, 64, 1, snd_trace_mix) != 1) exit(1);
	for (int i = 0; i < 16; i++) {
		const snd_chan *c = &st->ch[i];
		const uint32_t regs[7] = {c->cnt, c->tmr, c->sad, c->pnt, c->len, c->on, st->soundcnt};
		if (memcmp(last[i], regs, sizeof regs) || c->pos < lastpos[i] || !(tick & 511u))
			fprintf(snd_trace_state, "%llu\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%d\t%u\t%d\t%d\t%u\t%u\t%u\t%u\t%u\t%u\n",
				(unsigned long long)tick, i, c->on, c->cnt >> 29 & 3u, c->cnt, c->tmr, c->sad,
				c->pnt, c->len, c->pos, c->tcnt, c->adv, c->adi, st->bias, st->soundcnt,
				st->cap[0].cnt, st->cap[1].cnt, st->cap[0].pos, st->cap[1].pos);
		memcpy(last[i], regs, sizeof regs); lastpos[i] = c->pos;
	}
}
#endif
