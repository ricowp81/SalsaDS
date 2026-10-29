/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Banco de señales sin ROM: seno de Fs/16, silencio y 16 voces; salida float LE.
 * Compilar junto con snd_mix.c y -lm. Uso: audio-hd-quality DIRECTORIO. */
#include "audio/snd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef SND_MODE_HD
#define SND_MODE_HD 2u
#endif
static unsigned char memory[65536];
static void fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	(void)ctx; memcpy(dst, memory + (addr & 65535u), SND_LINE);
}
static void store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	(void)ctx; memcpy(memory + (addr & 65535u), src, n);
}
static void setup(snd_state *s, unsigned mode, unsigned period, float amplitude, unsigned voices)
{
	snd_reset(s); s->mode = mode; s->soundcnt = 0x807f;
	for (unsigned i=0; i<32; i++) {
		int16_t x = (int16_t)lrint(32767*amplitude*sin(6.283185307179586*i/16));
		memory[2*i] = (uint8_t)x; memory[2*i+1] = (uint8_t)(x>>8);
	}
	for (unsigned i=0; i<voices; i++) {
		snd_chan *c = &s->ch[i];
		c->cnt = (1u<<29) | (1u<<27) | (voices==1 ? 127u : 8u);
		c->on = 1; c->pos = -3; c->len = 16; c->tmr = (uint16_t)(65536-period); c->tcnt = c->tmr;
	}
}
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	uint16_t endian=1; if (*(uint8_t *)&endian != 1) return 2;
	static snd_state s;
	static float out[2*SND_OUT_MAX];
	const snd_bus bus = {NULL, fetch, store};
	const unsigned rates[] = {8000,16000,22000};
	for (unsigned r=0; r<3; r++) for (unsigned level=0; level<3; level++) for (unsigned mode=0; mode<3; mode++) {
		unsigned period=(unsigned)lrint((double)SND_RATE_NATIVE*512/rates[r]);
		float amplitude=level==0 ? 0.5f : level==1 ? 0.01f : 0;
		setup(&s,mode,period,amplitude,1);
		char path[1024]; snprintf(path,sizeof path,"%s/%u-%u-%u.f32",argv[1],rates[r],level,mode);
		FILE *f=fopen(path,"wb"); if (!f) return 3;
		for (unsigned frame=0; frame<240; frame++) {
			uint32_t n=0; snd_run(&s,NULL,0,547,&bus,NULL,out,SND_OUT_MAX,&n);
			if (fwrite(out,2*sizeof(float),n,f)!=n) return 4;
		}
		fclose(f);
		printf("Fs nominal %u, efectiva %.9f, nivel %.4f, modo %u\n",rates[r],(double)SND_RATE_NATIVE*512/period,amplitude,mode);
	}
	for (unsigned mode=0; mode<3; mode++) {
		setup(&s,mode,1047,0.5f,16);
		clock_t start=clock();
		for (unsigned frame=0; frame<6000; frame++) {
			uint32_t n; snd_run(&s,NULL,0,547,&bus,NULL,out,SND_OUT_MAX,&n);
		}
		printf("HOST 16 voces modo %u: %.6f ms/cuadro\n",mode,1000.0*(clock()-start)/CLOCKS_PER_SEC/6000);
	}
	return 0;
}
