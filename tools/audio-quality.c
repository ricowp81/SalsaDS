/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Banco reproducible: acordes con armónicos y percusión, salida float estéreo.
 * gcc -O2 -Isrc tools/audio-quality.c src/audio/stretch.c -lm -o build/audio-quality
 * build/audio-quality DIRECTORIO (existente); audio-quality.py lee los .f32. */
#include "audio/stretch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static float music(unsigned i, unsigned c)
{
	double t = i/48000.0, x = 0;
	const double notes[] = {130.8128, 164.8138, 195.9977, 261.6256};
	for (unsigned k=0; k<4; k++)
		for (unsigned h=1; h<=4; h++)
			x += 0.065/h * sin(6.283185307179586*notes[k]*h*t + c*0.3*k);
	unsigned age = i%24000;
	uint32_t r = i*747796405u+2891336453u;
	r = ((r >> ((r >> 28)+4)) ^ r)*277803737u;
	float noise = (int32_t)((r >> 22)^r)/2147483648.0f;
	x += .25*exp(-(double)age/180.0)*noise + .22*exp(-(double)age/600.0)*sin(6.283185307179586*75*t);
	return (float)x;
}
int main(int argc, char **argv)
{
	uint16_t endian = 1;
	if (argc != 2 || *(const unsigned char *)&endian != 1) return 2;
	static float fifo[2*8192], out[2*STRETCH_HOP];
	for (unsigned speed=0; speed<3; speed++) {
		unsigned percent = speed==0 ? 80 : speed==1 ? 67 : 100;
		for (unsigned version=1; version<=2; version++) {
			audio_stretch a={0}; audio_stretch2 b={0};
			char path[1024]; snprintf(path,sizeof path,"%s/v%u-%u.f32",argv[1],version,percent);
			FILE *f=fopen(path,"wb"); if (!f) return 2;
			snprintf(path,sizeof path,"%s/v%u-%u.seams",argv[1],version,percent);
			FILE *seams=fopen(path,"w"); if (!seams) { fclose(f); return 2; }
			unsigned have=4096, index=have, consumed=0, diff=0, join_count=0;
			clock_t cost=0;
			for(unsigned i=0;i<have;i++) for(unsigned c=0;c<2;c++) fifo[2*i+c]=music(i,c);
			for(unsigned hop=0;hop<2000;hop++) {
				unsigned desired=4096+(unsigned)((uint64_t)hop*480*(speed==1?2:percent)/(speed==1?3:100)/800)*800;
				unsigned add=desired-index;
				if(have+add>8192) return 3;
				for(unsigned i=0;i<add;i++) for(unsigned c=0;c<2;c++) fifo[2*(have+i)+c]=music(index+i,c);
				have+=add;index+=add;
				unsigned blending=a.blending; uint64_t joins=b.joins;
				clock_t t=clock();
				unsigned used=version==1 ? audio_stretch_run(&a,fifo,have<STRETCH_GRAIN?have:STRETCH_GRAIN,out) : audio_stretch2_run(&b,fifo,have,out);
				cost+=clock()-t;
				if (version==1 ? (!used || blending) : b.joins!=joins) {
					join_count++;
					fprintf(seams,"%u\n%u\n",hop*480,hop*480+(version==1?240:480));
				}
				if(used>have) return 4;
				for(unsigned j=0;j<2*STRETCH_HOP;j++) if(!isfinite(out[j])) return 7;
				if(percent==100 && memcmp(out,fifo,sizeof out)) diff++;
				memmove(fifo,fifo+2*used,2*(have-used)*sizeof(float));have-=used;consumed+=used;
				if(fwrite(out,sizeof out,1,f)!=1) return 5;
			}
			fclose(f); fclose(seams);
			unsigned long long missing=version==1?a.missing:b.missing;
			printf("v%u producción %u%%: faltas %llu, diferencias nominales %u, entrada %u, uniones %llu, host %.3f us/10ms\n",version,percent,missing,diff,consumed,(unsigned long long)join_count,1e6*cost/CLOCKS_PER_SEC/2000);
			if(missing || diff) return 6;
		}
	}
	return 0;
}
