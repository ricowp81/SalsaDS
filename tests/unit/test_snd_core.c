/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Vectores independientes del mezclador: aritmética entera a mano y un
 * registro de bits para el ruido. No se generan desde snd_mix.c. */
#include <string.h>
#include "audio/snd.h"
#include "util/endian.h"
#include "check.h"

static uint8_t ram[65536];
static snd_state state;
static snd_ent events[128];
static uint32_t ne;
static int16_t samples[2*SND_TICKS_MAX];

static void fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	(void)ctx; memcpy(dst, ram + (addr & 65535u), SND_LINE);
}
static void store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	(void)ctx; memcpy(ram + (addr & 65535u), src, n);
}
static const snd_bus bus = {0, fetch, store};
static void w(unsigned addr, unsigned value)
{
	events[ne++] = (snd_ent){.tick=0, .off=(uint16_t)addr, .val=(uint16_t)value, .mask=65535};
}
static void w32(unsigned a, uint32_t v) { w(a,v); w(a+2,v>>16); }
static void setup(void)
{
	snd_reset(&state); memset(ram, 0, sizeof ram); ne=0; w(0x100,0x807f);
}
static void channel(unsigned i, unsigned fmt, unsigned pan, unsigned vol, unsigned div,
		    unsigned repeat, unsigned period, unsigned pnt, unsigned len, unsigned addr)
{
	w32(i*16+4, 0x02000000u+addr); w(i*16+8,65536-period); w(i*16+10,pnt); w32(i*16+12,len);
	w32(i*16, 0x80000000u | fmt<<29 | repeat<<27 | pan<<16 | div<<8 | vol);
}
static void run(unsigned n)
{
	const uint32_t used = snd_run(&state, events, ne, n, &bus, samples, 0, 0, 0);
	memmove(events, events+used, (ne-used)*sizeof *events); ne-=used;
}

static void pcm_and_mix(void)
{
	setup();
	for (unsigned i=0;i<32;i++) wr16le(ram+0x100+2*i,(uint16_t)(1000+i*100));
	channel(0,1,0,127,0,1,512,1,3,0x100);
	run(40);
	for (unsigned t=0;t<4;t++) CHECK_EQ(samples[2*t],0);
	for (unsigned t=4;t<40;t++) {
		unsigned pos=t-4; if(pos>=8) pos=2+(pos-8)%6;
		CHECK_EQ(samples[2*t],1000+pos*100);
	}
	setup(); ram[0x100]=0x80; ram[0x101]=0xff; ram[0x102]=0; ram[0x103]=0x7f;
	channel(0,0,127,127,0,2,256,0,1,0x100); run(6);
	CHECK_EQ(samples[2*2+1],-32768); CHECK_EQ(samples[2*3+1],0); CHECK_EQ(samples[2*4+1],32512);
	CHECK_EQ(state.busy,0);
	/* Volumen antes de pan: -16384*37/128/2 = -2368;
	 * izquierda *50/128 = -925, derecha *77/128 = -1425. */
	setup(); for(unsigned i=0;i<32;i++) wr16le(ram+0x100+2*i,0xc000);
	channel(0,1,77,37,1,1,512,0,16,0x100); run(8);
	CHECK_EQ(samples[10],-925); CHECK_EQ(samples[11],-1425);
	/* Sumar antes del volumen maestro evita recortar dos voces prematuramente. */
	setup(); for(unsigned i=0;i<32;i++) wr16le(ram+0x100+2*i,30000);
	w(0x100,0x8040); channel(0,1,0,127,0,1,512,0,16,0x100);
	channel(2,1,0,127,0,1,512,0,16,0x100); run(8); CHECK_EQ(samples[10],30000);
	setup(); w(0x104,600); run(4); CHECK_EQ(samples[4],5632);
	setup(); w(0x100,0x007f); channel(0,1,0,127,0,1,512,0,16,0x100); run(8);
	CHECK_EQ(state.ch[0].on,0); CHECK_EQ(samples[10],0);
}

static void adpcm_loops(void)
{
	static const int16_t expected[24]={1002,1007,1015,1025,1024,1021,1016,1009,
		1009,1011,1015,1021,1021,1019,1015,1009,1009,1011,1015,1021,1021,1019,1015,1009};
	static const uint8_t bytes[4]={0x10,0x32,0x98,0xba};
	/* Paso inicial 16, luego 14,13,...,7; diferencia=floor(paso*(2n+1)/8).
	 * El bucle de PNT=2 guarda predictor 1009 e índice 0 antes del nibble 16. */
	for (unsigned loop=0;loop<=2;loop++) {
		setup(); wr16le(ram+0x100,1000); ram[0x102]=8;
		for(unsigned j=0;j<12;j++) ram[0x104+j]=bytes[j%4];
		channel(0,2,0,127,0,1,512,loop,4-loop,0x100); run(120);
		for(unsigned t=0;t<12;t++) CHECK_EQ(samples[2*t],0);
		for(unsigned t=12;t<120;t++) {
			unsigned p=t-12;
			if(loop==0) { unsigned q=(t-4)%32; CHECK_EQ(samples[2*t],q<8?0:expected[q-8]); }
			else { if(p>=24) p=(loop-1)*8+(p-24)%(32-loop*8); CHECK_EQ(samples[2*t],expected[p]); }
		}
	}
	setup(); wr16le(ram+0x100,0x8000); ram[0x102]=127; ram[0x104]=0xff;
	channel(0,2,0,127,0,2,512,1,3,0x100); run(14);
	CHECK_EQ(samples[24],-32768); CHECK_EQ(state.ch[0].adi,88);
	/* Longitud cero y bucle sin cuerpo: nunca leer indefinidamente ni dividir por cero. */
	setup(); channel(0,1,0,127,0,1,1,0,0,0x100); run(20); CHECK_EQ(state.ch[0].on,0);
	setup(); channel(0,2,0,127,0,1,1,1,0,0x100); run(20); CHECK_EQ(state.ch[0].on,1);
}

static void psg_noise(void)
{
	for(unsigned duty=0;duty<8;duty++) {
		setup(); channel(8,3,0,127,0,0,512,0,0,0); w(0x82,0xe000|(duty<<8)); run(66);
		CHECK_EQ(samples[2],0);
		for(unsigned t=2;t<66;t++) CHECK_EQ(samples[2*t], ((t-2)%8<=duty)?32767:-32767);
	}
	setup(); channel(14,3,0,127,0,0,512,0,0,0); run(130);
	unsigned bits[15]; for(unsigned i=0;i<15;i++) bits[i]=1;
	for(unsigned t=2;t<130;t++) {
		unsigned bit=bits[0]; CHECK_EQ(samples[2*t],bit?-32767:32767);
		for(unsigned i=0;i<14;i++) bits[i]=bits[i+1];
		bits[14]=bit; bits[13]^=bit;
	}
}

static void capture(void)
{
	/* Una nueva recarga no adelanta el desbordamiento que estaba pendiente. */
	setup(); w(0x18,0xfc00); w32(0x110,0x02001000); w(0x114,1024); w(0x108,0x80);
	run(8); CHECK_EQ(state.cap[0].fifo_count,3);
	w(0x18,0xffff); run(1); CHECK_EQ(state.cap[0].fifo_count,4); CHECK_EQ(state.cap[0].pos,0);
	run(1); CHECK_EQ(state.cap[0].pos,1000);
	/* Fuente directa: 0+1 / 2+3, con divisor pero sin volumen ni paneo. */
	setup();
	for(unsigned i=0;i<4;i++) {
		for(unsigned j=0;j<32;j++) wr16le(ram+0x100+i*128+j*2,(uint16_t)(4000+i*2000));
		channel(i,1,64,0,1,1,512,0,16,0x100+i*128);
	}
	w32(0x110,0x02001000); w(0x114,8); w32(0x118,0x02001100); w(0x11c,8); w(0x108,0x8787);
	run(70); CHECK_EQ(rd16le(ram+0x1000+8),5000); CHECK_EQ(rd16le(ram+0x1100+8),9000);
	CHECK_EQ(state.cap[0].on,0); CHECK_EQ(state.cap[1].on,0);
	for(unsigned fmt8=0;fmt8<2;fmt8++) for(unsigned period=256;period<=1024;period*=2) {
		setup(); for(unsigned i=0;i<64;i++) wr16le(ram+0x100+2*i,8192);
		channel(0,1,127,0,1,1,512,0,32,0x100); /* directo sin volumen ni pan: 4096 */
		w(0x18,65536-period); w32(0x110,0x02001000); w(0x114,8); w(0x108,0x86|(fmt8?8:0));
		ram[0x1020]=0x55; run(120);
		unsigned size=fmt8?1:2, count=32/size;
		for(unsigned j=0;j<count;j++) {
			unsigned t=((j+1)*period+511)/512;
			int v=t>=4?4096:0;
			CHECK_EQ(fmt8?ram[0x1000+j]:(int16_t)rd16le(ram+0x1000+2*j),fmt8?v/256:v);
		}
		CHECK_EQ(ram[0x1020],0x55); CHECK_EQ(state.cap[0].on,0);
	}
	/* Realimentación: impulso de 8192, eco a mitad de nivel cada 67 ticks.
	 * La captura se escribe 16 ticks tarde. El canal lee (t-4)%64 y
	 * la escritura (t-17)%64; vuelve a leer el dato del ciclo anterior. */
	for(unsigned chunk=1;chunk<=239;chunk=chunk==1?17:chunk==17?111:239) {
		setup(); wr16le(ram+0x100,8192);
		channel(0,1,0,127,0,2,512,0,32,0x100);
		channel(1,1,0,64,0,1,512,0,32,0x1000);
		w32(0x110,0x02001000); w(0x114,32); w(0x108,0x80);
		for(unsigned t=0;t<240;) {
			unsigned n=240-t<chunk?240-t:chunk; run(n);
			for(unsigned j=0;j<n;j++) {
				unsigned at=t+j; int expected=0;
				if(at>=4 && (at-4)%67==0) expected=8192>>(at-4)/67;
				CHECK_EQ(samples[2*j],expected);
			}
			t+=n;
		}
		if(chunk==239) break;
	}
}

void test_snd_core(void)
{
	pcm_and_mix(); adpcm_loops(); psg_noise(); capture();
}
