/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Autoprueba aislada del mismo mezclador de audio en el PPE y un SPE. */
#include <ppu-types.h>
#include <stdint.h>
#include <string.h>
#include <sys/process.h>

#include <spukit/spukit.h>

#include "audio/snd.h"
#include "platform/ps3/log.h"
#include "spu/snd/job.h"
#include "spu_snd_bin.h"
#include "../../roms/src/snd/adpcm.h"

#define PROGRAM "spe-audio"
#define ST_MEM 0x10000u
#ifndef AUDIO_TEST_VERSION
#define AUDIO_TEST_VERSION "desconocida"
#endif

SYS_PROCESS_PARAM(1001, 0x10000);

static uint8_t mem_spe[ST_MEM] __attribute__((aligned(128)));
static uint8_t mem_ppe[ST_MEM] __attribute__((aligned(128)));
static snd_ent log_buf[SND_LOG_MAX] __attribute__((aligned(128)));
static int16_t nat_spe[2*SND_TICKS_MAX] __attribute__((aligned(128)));
static int16_t nat_ppe[2*SND_TICKS_MAX];
static float out_spe[2*SND_OUT_MAX] __attribute__((aligned(128)));
static float out_ppe[2*SND_OUT_MAX];
static snd_state state_spe __attribute__((aligned(128)));
static snd_state state_ppe __attribute__((aligned(128)));
static uint32_t nlog;

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	(void)ctx;
	if ((addr >> 23) == 4) memcpy(dst, mem_ppe + (addr & (ST_MEM-1)), SND_LINE);
	else memset(dst, 0, SND_LINE);
}

static void store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	(void)ctx;
	if ((addr >> 23) == 4) memcpy(mem_ppe + (addr & (ST_MEM-1)), src, n);
}

static void w(uint32_t off, uint32_t v)
{
	log_buf[nlog++] = (snd_ent){ .off=(uint16_t)off, .val=(uint16_t)v, .mask=0xffff };
}

static void w32(uint32_t off, uint32_t v) { w(off, v); w(off+2, v >> 16); }

static int closef(float a, float b, float eps)
{
	const float d = a-b;
	return d <= eps && d >= -eps;
}

static int chan_equal(const snd_chan *a, const snd_chan *b)
{
	return a->cnt==b->cnt && a->sad==b->sad && a->len==b->len && a->tmr==b->tmr && a->pnt==b->pnt &&
	       a->keyon==b->keyon && a->loop_valid==b->loop_valid && a->on==b->on && a->hdr==b->hdr && a->pos==b->pos && a->tcnt==b->tcnt && a->out==b->out &&
	       a->adv==b->adv && a->adi==b->adi && a->alv==b->alv && a->ali==b->ali && a->lfsr==b->lfsr &&
	       a->tag==b->tag && memcmp(a->line,b->line,SND_LINE)==0;
}

static int float_state_equal(const snd_state *a, const snd_state *b)
{
	if (a->resample_phase != b->resample_phase || a->hd_phase != b->hd_phase || a->hd_active != b->hd_active) return 0;
	for (int s=0;s<2;s++) for (int i=0;i<SND_HIST;i++) if (!closef(a->hist[s][i],b->hist[s][i],0.0005f)) return 0;
	for (int c=0;c<SND_NCH;c++) {
		if (a->hd_period[c]!=b->hd_period[c] || a->hd_fade[c]!=b->hd_fade[c] || a->hd_on[c]!=b->hd_on[c] ||
		    !closef(a->hd_inv[c],b->hd_inv[c],0.00001f)) return 0;
		for (int i=0;i<4;i++) if (!closef(a->hd_hist[c][i],b->hd_hist[c][i],0.0005f)) return 0;
		for (int s=0;s<2;s++) if (!closef(a->hd_last[c][s],b->hd_last[c][s],0.0005f) ||
					       !closef(a->hd_anchor[c][s],b->hd_anchor[c][s],0.0005f)) return 0;
	}
	return 1;
}

static void diagnose(int pass)
{
	for (int c=0;c<SND_NCH;c++) {
		const snd_chan *s=&state_spe.ch[c], *p=&state_ppe.ch[c];
		if (!chan_equal(s,p))
			salsads_log("estado trabajo %d voz %d: pos %d/%d tcnt %u/%u salida %d/%d ADPCM %d,%d,%d,%d/%d,%d,%d,%d ruido %u/%u tag %08x/%08x",
				pass,c,s->pos,p->pos,s->tcnt,p->tcnt,s->out,p->out,s->adv,s->adi,s->alv,s->ali,
				p->adv,p->adi,p->alv,p->ali,s->lfsr,p->lfsr,s->tag,p->tag);
	}
	if (state_spe.resample_phase!=state_ppe.resample_phase || state_spe.hd_phase!=state_ppe.hd_phase ||
	    state_spe.hd_active!=state_ppe.hd_active)
		salsads_log("estado trabajo %d fases: remuestreo %08x/%08x HD %u/%u activa %u/%u",pass,
			state_spe.resample_phase,state_ppe.resample_phase,state_spe.hd_phase,state_ppe.hd_phase,
			state_spe.hd_active,state_ppe.hd_active);
	for (int c=0;c<SND_NCH;c++) {
		uint32_t hist=0,lr=0;
		for (int i=0;i<4;i++) hist+=!closef(state_spe.hd_hist[c][i],state_ppe.hd_hist[c][i],0.0005f);
		for (int side=0;side<2;side++)
			lr+=!closef(state_spe.hd_last[c][side],state_ppe.hd_last[c][side],0.0005f) ||
			    !closef(state_spe.hd_anchor[c][side],state_ppe.hd_anchor[c][side],0.0005f);
		if (hist || lr || state_spe.hd_period[c]!=state_ppe.hd_period[c] || state_spe.hd_fade[c]!=state_ppe.hd_fade[c] ||
		    state_spe.hd_on[c]!=state_ppe.hd_on[c] || !closef(state_spe.hd_inv[c],state_ppe.hd_inv[c],0.00001f))
			salsads_log("estado trabajo %d voz %d HD: historia %u salida/ancla %u periodo %u/%u envolvente %u/%u encendida %u/%u inversa %.9g/%.9g",
				pass,c,hist,lr,state_spe.hd_period[c],state_ppe.hd_period[c],state_spe.hd_fade[c],state_ppe.hd_fade[c],
				state_spe.hd_on[c],state_ppe.hd_on[c],state_spe.hd_inv[c],state_ppe.hd_inv[c]);
		if (lr)
			salsads_log("estado trabajo %d voz %d HD valores: salida %.9g,%.9g/%.9g,%.9g ancla %.9g,%.9g/%.9g,%.9g",pass,c,
				state_spe.hd_last[c][0],state_spe.hd_last[c][1],state_ppe.hd_last[c][0],state_ppe.hd_last[c][1],
				state_spe.hd_anchor[c][0],state_spe.hd_anchor[c][1],state_ppe.hd_anchor[c][0],state_ppe.hd_anchor[c][1]);
	}
}

int main(void)
{
	spukit_worker *worker;
	uint32_t bad_nat=0,bad_state=0,bad_mem=0,bad_out=0;
	const uint32_t D16=0x02000100u,D8=0x02000500u,DAD=0x02000700u,CAP=0x02002000u;
	snd_bus bus={NULL,fetch,store};

	salsads_log_init_banner(PROGRAM, PROGRAM " " AUDIO_TEST_VERSION);
	for (uint32_t i=0;i<256;i++) { wr16(mem_spe+0x100+2*i,(uint16_t)(((int)i-128)*200)); mem_spe[0x500+i]=(uint8_t)(i*3); }
	memcpy(mem_spe+0x700,adpcm_data,sizeof adpcm_data); memcpy(mem_ppe,mem_spe,sizeof mem_ppe);
	snd_reset(&state_spe); snd_reset(&state_ppe);
	if (spukit_init(1) != 0 || !(worker=spukit_worker_start(spu_snd_bin,"snd",0))) {
		salsads_log("RESULTADO " PROGRAM ": FALLO al iniciar spukit"); salsads_log_shutdown(); return 1;
	}
	for (int pass=0;pass<8;pass++) {
		job_snd job={0}; uint32_t nt=300u+77u*(uint32_t)(pass&1), nppe=0;
		nlog=0;
		if (!(pass&3)) {
			snd_reset(&state_spe); snd_reset(&state_ppe);
			w(0x100,0x807f); w32(0x04,D16&0x07ffffffu); w(0x08,0x10000u-700u); w32(0x0c,128);
			w32(0x00,0x20000000u|(1u<<27)|127u|(60u<<16)|0x80000000u);
			w32(0x14,D8); w(0x18,0xfe00); w32(0x1c,64); w32(0x10,(1u<<27)|100u|(1u<<8)|(20u<<16)|0x80000000u);
			w32(0x24,DAD); w(0x28,0xfe00); w(0x2a,1); w32(0x2c,3);
			w32(0x20,(1u<<27)|(2u<<29)|127u|(80u<<16)|0x80000000u);
			w32(0x80,(3u<<29)|127u|(64u<<16)|(3u<<24)|0x80000000u); w(0x88,0xff00);
			w32(0xe0,(3u<<29)|127u|(64u<<16)|0x80000000u); w(0xe8,0xfc00);
			w32(0x110,CAP&0x07ffffffu); w(0x114,64); w(0x108,0x84);
		}
		job.state_ea=(uintptr_t)&state_spe; job.log_ea=(uintptr_t)log_buf; job.out_ea=(uintptr_t)out_spe;
		job.nat_ea=(uintptr_t)nat_spe; job.nlog=nlog; job.nticks=nt; job.out_max=SND_OUT_MAX;
		job.mode=(uint32_t)(pass&3) | (pass>=4?SND_MODE_CORE_OLD:0u); job.ratio_adj=0.003f*(float)(pass&3); job.rgn_ea[0]=(uintptr_t)mem_spe; job.rgn_mask[0]=ST_MEM-1;
		uint32_t seq=spukit_post(worker,JOB_SND,&job,sizeof job); const spukit_result *result=spukit_wait(worker,seq);
		job_snd_result jr; memcpy(&jr,result->data,sizeof jr);
		state_ppe.mode=job.mode; state_ppe.ratio_adj=job.ratio_adj;
		(void)snd_run(&state_ppe,log_buf,nlog,nt,&bus,nat_ppe,out_ppe,SND_OUT_MAX,&nppe);
		for (uint32_t i=0;i<2*nt;i++) bad_nat += nat_spe[i]!=nat_ppe[i];
		if (jr.out_n!=nppe) bad_out++; else for (uint32_t i=0;i<2*nppe;i++) bad_out += !closef(out_spe[i],out_ppe[i],0.0005f);
		if (jr.applied!=nlog || state_spe.busy!=state_ppe.busy || state_spe.tick!=state_ppe.tick ||
		    state_spe.peak!=state_ppe.peak || state_spe.soundcnt!=state_ppe.soundcnt || state_spe.bias!=state_ppe.bias ||
		    state_spe.log_applied!=state_ppe.log_applied || state_spe.nout!=state_ppe.nout ||
		    state_spe.ticks_run!=state_ppe.ticks_run || !float_state_equal(&state_spe,&state_ppe)) { bad_state++; diagnose(pass); }
		for (int c=0;c<SND_NCH;c++) bad_state += !chan_equal(&state_spe.ch[c],&state_ppe.ch[c]);
		for (int c=0;c<2;c++) bad_state += memcmp(&state_spe.cap[c],&state_ppe.cap[c],sizeof(snd_cap))!=0;
		bad_mem += memcmp(mem_spe,mem_ppe,sizeof mem_ppe)!=0;
	}
	spukit_worker_stop(worker);
	salsads_log("autoprueba audio aislada (8 trabajos, dos núcleos): muestras %u, estado %u, memoria %u, salida %u",bad_nat,bad_state,bad_mem,bad_out);
	salsads_log("RESULTADO " PROGRAM ": %s",bad_nat||bad_state||bad_mem||bad_out?"FALLO":"OK");
	salsads_log_shutdown(); return bad_nat||bad_state||bad_mem||bad_out?1:0;
}
