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
	return (float)(0.5 * sin(6.283185307179586 * (kind == 3 ? 73.416*t : kind ? 200*t + 1100*t*t : 997*t)));
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

static void simulation2(int kind, unsigned speed)
{
	audio_stretch2 s = {0};
	float fifo[2*16384], out[2*STRETCH_HOP], previous[2] = {0};
	unsigned have = 4096, index = have, crossings = 0, ne = 0, nb = 0;
	double edge = 0, body = 0, maxstep = 0;
	for (unsigned i = 0; i < have; i++)
		fifo[2*i] = fifo[2*i+1] = signal_at(i, kind);
	for (unsigned h = 0; h < 1000; h++) {
		unsigned rate = speed ? speed : h < 200 || h >= 700 ? 100 : h < 500 ? 50 : 0;
		unsigned add = h%5 ? 0 : 2400*rate/100;
		CHECK(have + add <= 16384);
		for (unsigned i = 0; i < add; i++)
			fifo[2*(have+i)] = fifo[2*(have+i)+1] = signal_at(index++, kind);
		have += add;
		unsigned used = audio_stretch2_run(&s, fifo, have, out);
		CHECK(used <= have);
		if (speed == 100) CHECK(memcmp(out, fifo, sizeof out) == 0);
		memmove(fifo, fifo + 2*used, 2*(have-used)*sizeof(float));
		have -= used;
		for (unsigned i = 0; i < STRETCH_HOP; i++) {
			float x = out[2*i];
			CHECK(isfinite(x) && x == out[2*i+1] && fabsf(x) <= 0.501f);
			if (h) {
				double d = x - 2*previous[0] + previous[1];
				if (i < 8) { edge += d*d; ne++; }
				else { body += d*d; nb++; }
				if (fabs(x-previous[0]) > maxstep) maxstep = fabs(x-previous[0]);
			}
			if (previous[0] <= 0 && x > 0) crossings++;
			previous[1] = previous[0]; previous[0] = x;
		}
	}
	double hf = (edge/ne)/(body/nb);
	printf("audio v2 señal %d producción %u: faltas %llu, uniones %llu, HF %.4f, salto %.5f, Hz %.2f\n",
								kind, speed, (unsigned long long)s.missing, (unsigned long long)s.joins, hf, maxstep, crossings/10.0);
	CHECK(s.missing == 0 && hf < 2);
	if (!kind && speed) { CHECK(fabs(crossings/10.0 - 997) < 3); CHECK(maxstep < 0.08); }
	if (kind == 3 && speed) { CHECK(fabs(crossings/10.0 - 73.416) < 0.5); CHECK(maxstep < 0.008); }
	if (speed == 100) CHECK(!s.inserted && !s.active && !s.valid);
	if (!speed) CHECK(!s.valid); /* recuperación: retorna a la copia nominal */
}

static void stereo2(void)
{
	audio_stretch2 s = {0};
	float fifo[2*8192], out[2*STRETCH_HOP];
	unsigned have = 0, index = 0;
	for (unsigned h = 0; h < 400; h++) {
		unsigned add = h ? 240 : 4096;
		for (unsigned i = 0; i < add; i++) {
			double phase = 6.283185307179586 * 997 * index++ / 48000;
			fifo[2*(have+i)] = 0.5f*(float)sin(phase);
			fifo[2*(have+i)+1] = 0.25f*(float)cos(phase);
		}
		have += add;
		unsigned used = audio_stretch2_run(&s, fifo, have, out);
		CHECK(used <= have);
		memmove(fifo, fifo+2*used, 2*(have-used)*sizeof(float)); have -= used;
		for (unsigned i = 0; i < STRETCH_HOP; i++) {
			float e = out[2*i]*out[2*i] + 4*out[2*i+1]*out[2*i+1];
			CHECK(e > 0.24f && e < 0.251f);
		}
	}
	CHECK(!s.missing);
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
	CHECK(audio_rate_adjust(3072, 3072) == 0.0f);
	CHECK(fabsf(audio_rate_adjust(3168, 3072) - 0.00046875f) < 1e-9f);
	CHECK(audio_rate_adjust(4096, 3072) == 0.0025f);
	CHECK(audio_rate_adjust(0, 3072) == -0.0025f);
	/* Ráfagas de 799/800 pares por cuadro contra bloques de 480: a velocidad
	 * completa no hay uniones aunque la disponibilidad oscile. */
	{
		audio_stretch2 nominal = {0};
		float fifo[2*8192], out_nominal[2*STRETCH_HOP];
		unsigned have = 4096, produced = have;

		for (unsigned i = 0; i < have; i++)
			fifo[2*i] = fifo[2*i+1] = signal_at(i, 0);
		for (unsigned h = 0; h < 600; h++) {
			if ((h % 5) < 3) {
				unsigned add = (h & 1) ? 799 : 800;
				for (unsigned i = 0; i < add; i++)
					fifo[2*(have+i)] = fifo[2*(have+i)+1] = signal_at(produced++, 0);
				have += add;
			}
			unsigned used = audio_stretch2_run(&nominal, fifo, have, out_nominal);
			CHECK(used == STRETCH_HOP);
			CHECK(memcmp(out_nominal, fifo, sizeof out_nominal) == 0);
			memmove(fifo, fifo + 2*used, 2*(have-used)*sizeof(float));
			have -= used;
		}
		CHECK(nominal.joins == 0 && nominal.inserted == 0 && nominal.active == 0);
	}
	stereo2();
	for (int kind = 0; kind < 4; kind++)
		for (unsigned speed = 0; speed <= 100; speed += 25)
			simulation2(kind, speed);
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
	audio_stretch2 s2 = {0};
	CHECK(audio_stretch2_run(&s2, in, 0, out) == 0);
	CHECK(audio_stretch2_run(&s2, in, 10, out) == 10);
	CHECK(s2.missing == 950 && s2.credit == 0);
}
