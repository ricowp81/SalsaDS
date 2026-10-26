/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "audio/stretch.h"
#include <math.h>
#include <string.h>
#include <time.h>

static float signal_at(unsigned i, int kind)
{
	double t = i / 48000.0;
	if (kind == 2) {
		uint32_t v = i * 747796405u + 2891336453u;
		v = ((v >> ((v >> 28) + 4)) ^ v) * 277803737u;
		return ((int32_t)((v >> 22) ^ v) / 2147483648.0f) * 0.5f;
	}
	return (float)(0.5 * sin(6.283185307179586 * (kind ? 200*t + 1100*t*t : 997*t)));
}

static void simulation(int kind, int speed)
{
	audio_stretch s = {0};
	float fifo[8192], out[2*STRETCH_HOP], prev[2] = {0};
	unsigned have = 2048, index = have, crossed = 0, emitted = 0;
	double edge = 0, body = 0, maxstep = 0;
	unsigned ne = 0, nb = 0;
	clock_t start = clock();
	for (unsigned i = 0; i < have; i++)
		fifo[2*i] = fifo[2*i+1] = signal_at(i, kind);
	for (unsigned hop = 0; hop < 600; hop++) {
		/* Producción por ráfagas de un cuadro; 100%, 50%, 25% y
		 * transición 100→50→0→100 (sin datos nuevos se repite la historia). */
		unsigned rate = speed == 0 ? (hop < 100 || hop >= 400 ? 100 : hop < 300 ? 50 : 0) : (unsigned)speed;
		unsigned add = hop % 5 == 0 ? 2400 * rate / 100 : 0;
		if (have + add > 4096) add = 4096 - have;
		for (unsigned i = 0; i < add; i++)
			fifo[2*(have+i)] = fifo[2*(have+i)+1] = signal_at(index++, kind);
		have += add;
		unsigned n = have < STRETCH_GRAIN ? have : STRETCH_GRAIN;
		unsigned used = audio_stretch_run(&s, fifo, n, out);
		CHECK(used <= have);
		if (speed == 100) CHECK(memcmp(out, fifo, sizeof out) == 0);
		memmove(fifo, fifo + 2*used, 2*(have-used)*sizeof(float));
		have -= used;
		for (unsigned i = 0; i < STRETCH_HOP; i++) {
			float x = out[2*i];
			CHECK(out[2*i+1] == x && isfinite(x) && fabsf(x) <= 0.501f);
			if (emitted > 2) {
				double d = x - 2*prev[0] + prev[1];
				if (i < 8 || (i >= 232 && i < 248)) { edge += d*d; ne++; }
				else { body += d*d; nb++; }
				if (fabs(x-prev[0]) > maxstep) maxstep = fabs(x-prev[0]);
			}
			if (prev[0] <= 0 && x > 0) crossed++;
			prev[1] = prev[0]; prev[0] = x; emitted++;
		}
	}
	double hf = (edge/ne)/(body/nb), hz = crossed / 6.0;
	printf("audio señal %d velocidad %d: añadidas %llu, faltas %llu, HF uniones/interior %.4f, salto %.5f, Hz %.2f, host %.2f us/salto\n",
	       kind, speed, (unsigned long long)s.inserted, (unsigned long long)s.missing, hf, maxstep, hz,
	       1e6*(clock()-start)/CLOCKS_PER_SEC/600);
	CHECK(s.missing == 0);
	CHECK(hf < 2.0);
	if (!kind) { CHECK(fabs(hz - 997) < 3); CHECK(maxstep < 0.08); }
	if (speed == 100) CHECK(s.inserted == 0 && s.active == 0);
	else CHECK(s.inserted > 0);
}

static double harmonics(float amplitude, int soft)
{
	double power = 0, fundamental = 0;
	for (int h = 1; h <= 15; h++) {
		double re = 0, im = 0;
		for (int i = 0; i < 4800; i++) {
			double phase = 6.283185307179586 * i / 48;
			float x = amplitude * (float)sin(phase);
			x = soft ? audio_soft_limit(x) : x > 1 ? 1 : x < -1 ? -1 : x;
			re += x*cos(h*phase); im += x*sin(h*phase);
		}
		if (h == 1) fundamental = re*re+im*im;
		else power += re*re+im*im;
		if (h == 3 || h == 5) printf("audio amplitud %.1f suave %d H%d %.3f%%\n", amplitude, soft, h, 100*sqrt((re*re+im*im)/fundamental));
	}
	return sqrt(power/fundamental);
}

void test_stretch(void)
{
	for (int kind = 0; kind < 3; kind++)
		for (int speed = 0; speed <= 100; speed += 25)
			simulation(kind, speed);
	/* Estéreo en cuadratura: desplazamiento común, incluso con señales
	 * que se cancelarían al sumar los canales para buscar correlación. */
	audio_stretch stereo = {0};
	float pair[2*STRETCH_GRAIN], block[2*STRETCH_HOP];
	unsigned position = 0;
	for (unsigned hop = 0; hop < 100; hop++) {
		for (unsigned i = 0; i < STRETCH_GRAIN; i++) {
			double phase = 6.283185307179586 * 997 * (position+i) / 48000;
			pair[2*i] = 0.5f*(float)sin(phase);
			pair[2*i+1] = 0.25f*(float)cos(phase);
		}
		unsigned used = audio_stretch_run(&stereo, pair, hop > 4 && hop%2 ? 240 : STRETCH_GRAIN, block);
		position += used;
		for (unsigned i = 0; i < STRETCH_HOP; i++) {
			float energy = block[2*i]*block[2*i] + 4*block[2*i+1]*block[2*i+1];
			CHECK(energy > 0.24f && energy < 0.251f);
		}
	}
	float prev = -1;
	for (int i = -20000; i <= 20000; i++) {
		float x = i / 10000.0f, y = audio_soft_limit(x);
		CHECK(y >= prev && fabsf(y) <= 0.98f);
		CHECK(audio_soft_limit(-x) == -y);
		if (fabsf(x) <= 0.707f) CHECK(memcmp(&x, &y, sizeof x) == 0);
		prev = y;
	}
	double old = harmonics(1, 0), soft = harmonics(1, 1);
	printf("audio THD 0 dBFS anterior %.5f%% suave %.5f%%\n", 100*old, 100*soft);
	CHECK(soft < 0.04);
	old = harmonics(1.4f, 0); soft = harmonics(1.4f, 1);
	printf("audio THD sobretiro 1.4 anterior %.5f%% suave %.5f%%\n", 100*old, 100*soft);
	CHECK(soft < old);
	/* Arranque corto, cero entrada y reinicio: sin lectura fuera de límites. */
	audio_stretch s = {0}; float in[20] = {0}, out[2*STRETCH_HOP];
	CHECK(audio_stretch_run(&s, in, 0, out) == 0);
	CHECK(audio_stretch_run(&s, in, 10, out) == 10);
	CHECK(s.missing == 950);
}
