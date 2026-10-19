/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — pruebas de la entrada (Fase 0.6): mapeo DS3 → DS, archivo de
 * configuración y puntero del táctil (stick e inclinación). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "frontend/dspad.h"

#define NEAR(a, b, tol) CHECK(fabsf((float)(a) - (float)(b)) <= (tol))

static struct sds_ds3 neutral(void)
{
	struct sds_ds3 p;
	memset(&p, 0, sizeof p);
	p.lx = p.ly = p.rx = p.ry = 128;
	p.accel[0] = p.accel[1] = p.accel[2] = 512;
	p.gyro = 512;
	p.sensors_ok = 1;
	return p;
}

/* Lecturas del Sixaxis con el control inclinado `roll` grados a los lados y
 * `pitch` grados de cabeceo (X lateral, Y gravedad, Z adelante; 113
 * unidades por g, con la gravedad leyendo negativa en Y como en la consola). */
static void set_tilt(struct sds_ds3 *p, float roll, float pitch)
{
	const float g = 113.0f, rr = roll * 3.14159265f / 180.0f, pp = pitch * 3.14159265f / 180.0f;
	/* gravedad en el marco del control: componente lateral g·sin(roll),
	 * adelante g·cos(roll)·sin(pitch), vertical −g·cos(roll)·cos(pitch) */
	p->accel[0] = (uint16_t)(512.0f + g * sinf(rr) + 0.5f);
	p->accel[2] = (uint16_t)(512.0f + g * cosf(rr) * sinf(pp) + 0.5f);
	p->accel[1] = (uint16_t)(512.0f - g * cosf(rr) * cosf(pp) + 0.5f);
}

static void settle(const struct sds_padcfg *c, struct sds_input *in, const struct sds_ds3 *p, int frames)
{
	struct sds_keys k;
	for (int i = 0; i < frames; i++) {
		sds_pad_map(c, in, p, &k);
		sds_pointer_update(c, in, p, 1.0f / 60.0f, &k);
	}
}

static void test_mapping(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	struct sds_keys k;
	sds_padcfg_default(&c);
	sds_input_init(&in);

	static const struct {
		unsigned button;
		unsigned key;
	} map[] = {
		{ SDS_DS3_CIRCLE, SDS_KEY_A }, { SDS_DS3_CROSS, SDS_KEY_B }, { SDS_DS3_TRIANGLE, SDS_KEY_X },
		{ SDS_DS3_SQUARE, SDS_KEY_Y }, { SDS_DS3_L1, SDS_KEY_L }, { SDS_DS3_R1, SDS_KEY_R },
		{ SDS_DS3_START, SDS_KEY_START }, { SDS_DS3_SELECT, SDS_KEY_SELECT }, { SDS_DS3_UP, SDS_KEY_UP },
		{ SDS_DS3_DOWN, SDS_KEY_DOWN }, { SDS_DS3_LEFT, SDS_KEY_LEFT }, { SDS_DS3_RIGHT, SDS_KEY_RIGHT },
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
		struct sds_ds3 p = neutral();
		p.buttons = 1u << map[i].button;
		sds_pad_map(&c, &in, &p, &k);
		CHECK_EQ(k.keys, 1u << map[i].key);
		CHECK_EQ(k.touch_held, 0);
	}
	/* L2 y L3 no son botones de la DS: son acciones (flancos). */
	struct sds_ds3 p = neutral();
	p.buttons = 1u << SDS_DS3_L2;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.keys, 0);
	CHECK_EQ(k.layout, 1);
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.layout, 0);  /* mantenido: ya no es flanco */
	p.buttons = 0;
	sds_pad_map(&c, &in, &p, &k);
	p.buttons = 1u << SDS_DS3_L2;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.layout, 1);
	/* R2 es el toque por omisión. */
	p.buttons = 1u << SDS_DS3_R2;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.touch_held, 1);
	CHECK_EQ(k.keys, 0);
	/* Arriba y abajo a la vez se anulan. */
	p.buttons = 1u << SDS_DS3_UP | 1u << SDS_DS3_DOWN;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.keys, 0);
}

/* El stick izquierdo hace de cruceta, con histéresis (sube a 80 del centro,
 * baja al volver por debajo de 48). */
static void test_left_stick_dpad(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	struct sds_keys k;
	sds_padcfg_default(&c);
	sds_input_init(&in);
	struct sds_ds3 p = neutral();
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == 0);
	p.lx = 128 - 90;
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == (1u << SDS_KEY_LEFT));
	p.lx = 128 - 60; /* dentro de la histéresis: sigue */
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == (1u << SDS_KEY_LEFT));
	p.lx = 128 - 40; /* por debajo de 48: se suelta */
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == 0);
	p.lx = 128 + 90;
	p.ly = 128 + 90; /* abajo a la derecha */
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == ((1u << SDS_KEY_RIGHT) | (1u << SDS_KEY_DOWN)));
	p.lx = 128 - 90; /* de golpe al otro lado */
	p.ly = 128 - 90;
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == ((1u << SDS_KEY_LEFT) | (1u << SDS_KEY_UP)));
	/* el stick derecho no toca la cruceta */
	p = neutral();
	p.rx = 255;
	sds_input_init(&in);
	sds_pad_map(&c, &in, &p, &k);
	CHECK(k.keys == 0);
}

static void test_config(void)
{
	struct sds_padcfg c, d;
	char err[96];
	sds_padcfg_default(&c);
	const char *txt =
		"# un comentario\n"
		"toque = r3   # minúsculas\n"
		"  RECENTRAR = CRUZ L1\n"
		"A = nada\n"
		"puntero = Inclinacion\n"
		"zona_muerta = 20\n"
		"velocidad = 150.5\n"
		"filtro_ms = 80\n"
		"invertir_y = 1\n"
		"eje_gravedad = Z\n"
		"\n"
		"sensibilidad_x = 2.5\n";
	CHECK_EQ(sds_padcfg_parse(&c, txt, strlen(txt), err, sizeof err), 0);
	CHECK_EQ(c.source[SDS_IN_TOUCH], SDS_DS3_R3);
	CHECK_EQ(c.source2[SDS_IN_TOUCH], SDS_DS3_NONE);
	CHECK_EQ(c.source[SDS_IN_RECENTER], SDS_DS3_CROSS);
	CHECK_EQ(c.source2[SDS_IN_RECENTER], SDS_DS3_L1);
	CHECK_EQ(c.source[SDS_IN_A], SDS_DS3_NONE);
	CHECK_EQ(c.pointer, SDS_POINTER_TILT);
	CHECK_EQ(c.deadzone, 20);
	NEAR(c.speed, 150.5f, 0.01f);
	NEAR(c.filter_ms, 80.0f, 0.01f);
	CHECK_EQ(c.invert_y, 1);
	CHECK_EQ(c.axis_gravity, SDS_AXIS_Z);
	NEAR(c.sens_x, 2.5f, 0.01f);

	/* Lo que falla se cuenta y se describe, y lo demás se aplica. */
	sds_padcfg_default(&d);
	const char *bad = "TOQUE = R3\nBASURA = 4\nB = PLATANO\nsin igual\nPUNTERO = STICK\n";
	CHECK_EQ(sds_padcfg_parse(&d, bad, strlen(bad), err, sizeof err), 3);
	CHECK(strstr(err, "línea 2") != NULL);
	CHECK_EQ(d.source[SDS_IN_TOUCH], SDS_DS3_R3);
	CHECK_EQ(d.source[SDS_IN_B], SDS_DS3_CROSS);  /* sin tocar */

	/* El archivo escrito se lee de vuelta igual. */
	char buf[4096];
	const int n = sds_padcfg_format(&c, buf, sizeof buf);
	CHECK(n > 0);
	sds_padcfg_default(&d);
	CHECK_EQ(sds_padcfg_parse(&d, buf, (size_t)n, err, sizeof err), 0);
	CHECK_EQ(memcmp(d.source, c.source, sizeof c.source), 0);
	CHECK_EQ(memcmp(d.source2, c.source2, sizeof c.source2), 0);
	CHECK_EQ(d.pointer, c.pointer);
	CHECK_EQ(d.deadzone, c.deadzone);
	NEAR(d.speed, c.speed, 1.0f);
	NEAR(d.sens_x, c.sens_x, 0.01f);
	CHECK_EQ(d.axis_gravity, c.axis_gravity);
	/* El archivo por omisión también se lee de vuelta. */
	sds_padcfg_default(&c);
	const int n2 = sds_padcfg_format(&c, buf, sizeof buf);
	CHECK(n2 > 0);
	sds_padcfg_default(&d);
	CHECK_EQ(sds_padcfg_parse(&d, buf, (size_t)n2, err, sizeof err), 0);
	CHECK_EQ(memcmp(d.source, c.source, sizeof c.source), 0);
	/* Un búfer corto no se desborda. */
	CHECK_EQ(sds_padcfg_format(&c, buf, 40), -1);
}

static void test_touch_semantics(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	struct sds_keys k;
	int x, y, down;
	sds_padcfg_default(&c);
	sds_input_init(&in);
	struct sds_ds3 p = neutral();

	/* El puntero se mueve sin tocar. */
	p.rx = 255;
	settle(&c, &in, &p, 10);
	sds_touch_state(&in, &x, &y, &down);
	CHECK(x > 128 + 20);
	CHECK_EQ(down, 0);
	/* Toca solo mientras el botón está apretado. */
	p.buttons = 1u << SDS_DS3_R2;
	settle(&c, &in, &p, 1);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(down, 1);
	/* Arrastrar: sigue tocando mientras se mueve. */
	const int x0 = x;
	settle(&c, &in, &p, 10);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(down, 1);
	CHECK(x > x0);
	/* Soltar: levanta el lápiz y el puntero queda donde estaba. */
	p.buttons = 0;
	p.rx = 128;
	settle(&c, &in, &p, 1);
	const int x1 = x;
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(down, 0);
	CHECK_EQ(x, x1);
	/* Botón de toque mapeado a R3: R2 ya no toca. */
	const char *txt = "TOQUE = R3\n";
	CHECK_EQ(sds_padcfg_parse(&c, txt, strlen(txt), NULL, 0), 0);
	p.buttons = 1u << SDS_DS3_R2;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.touch_held, 0);
	p.buttons = 1u << SDS_DS3_R3;
	sds_pad_map(&c, &in, &p, &k);
	CHECK_EQ(k.touch_held, 1);
}

static void test_stick(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	int x, y, down;
	sds_padcfg_default(&c);
	sds_input_init(&in);
	struct sds_ds3 p = neutral();

	/* Zona muerta: dentro no hay movimiento. */
	p.rx = 128 + 10;  /* 10/127 = 7,9 % < 12 % */
	settle(&c, &in, &p, 60);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(x, 128);
	CHECK_EQ(y, 96);
	/* A fondo: speed píxeles por segundo. */
	p.rx = 255;
	settle(&c, &in, &p, 30);  /* 0,5 s → 100 px */
	NEAR(in.x, 128 + 100, 1.0f);
	/* La mitad del recorrido útil, la mitad de la velocidad. */
	sds_input_init(&in);
	const float dz = 0.12f;
	p.rx = (uint8_t)(128 + (int)((dz + (1.0f - dz) * 0.5f) * 127.0f + 0.5f));
	settle(&c, &in, &p, 60);  /* 1 s */
	NEAR(in.x - 128, 100.0f, 3.0f);
	/* Se queda en el borde. */
	p.rx = 255;
	p.ry = 255;
	settle(&c, &in, &p, 600);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(x, SDS_TOUCH_W - 1);
	CHECK_EQ(y, SDS_TOUCH_H - 1);
	p.rx = 0;
	p.ry = 0;
	settle(&c, &in, &p, 600);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(x, 0);
	CHECK_EQ(y, 0);
	/* L3 vuelve al centro. */
	p.rx = p.ry = 128;
	p.buttons = 1u << SDS_DS3_L3;
	settle(&c, &in, &p, 1);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(x, 128);
	CHECK_EQ(y, 96);
	/* Con curva 2 el medio recorrido va a la cuarta parte. */
	const char *txt = "curva = 2\n";
	sds_padcfg_parse(&c, txt, strlen(txt), NULL, 0);
	sds_input_init(&in);
	p.buttons = 0;
	p.rx = (uint8_t)(128 + (int)((dz + (1.0f - dz) * 0.5f) * 127.0f + 0.5f));
	settle(&c, &in, &p, 60);
	NEAR(in.x - 128, 50.0f * 1.0f, 3.0f);  /* 200 px/s × 0,25 × 1 s */
}

static void test_tilt(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	struct sds_keys k;
	int x, y, down;
	sds_padcfg_default(&c);
	const char *txt = "PUNTERO = INCLINACION\nzona_muerta_inclinacion = 1\nfiltro_ms = 100\ninvertir_y = 0\n";
	CHECK_EQ(sds_padcfg_parse(&c, txt, strlen(txt), NULL, 0), 0);
	sds_input_init(&in);
	struct sds_ds3 p = neutral();

	/* La primera postura es el centro (control plano o como se sostenga). */
	set_tilt(&p, 0, 40);  /* cabeceado 40° desde el principio */
	settle(&c, &in, &p, 5);
	sds_touch_state(&in, &x, &y, &down);
	CHECK_EQ(x, 128);
	CHECK_EQ(y, 96);

	/* 10° a los lados → (10 − 1) × 4 = 36 px, ya asentado el filtro. */
	set_tilt(&p, 10, 40);
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128 + 36, 1.5f);
	NEAR(in.y, 96, 1.0f);
	/* Cabeceo de 5° → (5 − 1) × 4 = 16 px. */
	set_tilt(&p, 10, 45);
	settle(&c, &in, &p, 120);
	NEAR(in.y, 96 + 16, 1.5f);

	/* Dentro de la zona muerta no se mueve. */
	set_tilt(&p, 0.5f, 40);
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128, 0.6f);

	/* Respuesta al escalón: con 100 ms de constante de tiempo, tras un
	 * cuadro (16,7 ms) el filtro alcanza 1 − e^(−0,167) = 15,4 %. */
	sds_input_init(&in);
	set_tilt(&p, 0, 40);
	settle(&c, &in, &p, 3);
	set_tilt(&p, 20, 40);
	settle(&c, &in, &p, 1);
	NEAR(in.f_roll, 0.154f * 20.0f, 0.5f);
	settle(&c, &in, &p, 59);  /* 1 s en total: ya casi todo */
	CHECK(in.f_roll > 19.9f);

	/* Recentrar: la postura actual es el centro y el puntero vuelve a él. */
	set_tilt(&p, 20, 40);
	settle(&c, &in, &p, 120);
	CHECK(in.x > 128 + 50);
	p.buttons = 1u << SDS_DS3_L3;
	sds_pad_map(&c, &in, &p, &k);
	sds_pointer_update(&c, &in, &p, 1.0f / 60.0f, &k);
	p.buttons = 0;
	NEAR(in.x, 128, 0.6f);
	settle(&c, &in, &p, 30);
	NEAR(in.x, 128, 0.6f);
	set_tilt(&p, 25, 40);  /* 5° más allá del nuevo centro */
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128 + 16, 1.5f);

	/* Invertir. */
	const char *inv = "invertir_x = 1\n";
	sds_padcfg_parse(&c, inv, strlen(inv), NULL, 0);
	set_tilt(&p, 25, 40);
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128 - 16, 1.5f);
	sds_padcfg_default(&c);
	sds_padcfg_parse(&c, txt, strlen(txt), NULL, 0);

	/* El stick derecho sigue moviendo el puntero en este modo. */
	sds_input_init(&in);
	set_tilt(&p, 0, 40);
	settle(&c, &in, &p, 3);
	p.rx = 255;
	settle(&c, &in, &p, 30);
	NEAR(in.x, 128 + 100, 1.5f);
	p.rx = 128;
	set_tilt(&p, 3, 40);  /* + (3 − 1) × 4 = 8 px por la inclinación */
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128 + 100 + 8, 1.5f);

	/* Contra el borde, el stick no acumula: soltarlo deja el puntero en el
	 * borde y, al volver la inclinación al centro, solo queda el
	 * desplazamiento que cupo (119 px), no el que se pidió en 10 s. */
	p.rx = 255;
	settle(&c, &in, &p, 600);
	NEAR(in.x, 255, 0.1f);
	p.rx = 128;
	settle(&c, &in, &p, 30);
	NEAR(in.x, 255, 0.1f);
	set_tilt(&p, 0, 40);
	settle(&c, &in, &p, 120);
	NEAR(in.x, 128 + 119, 1.0f);
	p.rx = 0;
	settle(&c, &in, &p, 6);  /* 0,1 s a la izquierda: se mueve de inmediato */
	CHECK(in.x < 128 + 119 - 15.0f);

	/* Sin sensores el modo de inclinación se comporta como el de stick. */
	sds_input_init(&in);
	p = neutral();
	p.sensors_ok = 0;
	p.rx = 255;
	settle(&c, &in, &p, 30);
	NEAR(in.x, 128 + 100, 1.5f);
}

static void test_gyro(void)
{
	struct sds_padcfg c;
	struct sds_input in;
	struct sds_keys k;
	sds_padcfg_default(&c);
	CHECK_EQ(c.invert_y, 1);  /* medido en la PS3 */
	const char *txt = "PUNTERO = GIRO\ninvertir_y = 0\ngiro_sensibilidad = 2\nfiltro_ms = 0\n";
	CHECK_EQ(sds_padcfg_parse(&c, txt, strlen(txt), NULL, 0), 0);
	CHECK_EQ(c.pointer, SDS_POINTER_GYRO);
	sds_input_init(&in);
	struct sds_ds3 p = neutral();
	p.gyro = 520;  /* sesgo de 8 unidades: se calibra y no mueve */
	settle(&c, &in, &p, 120);
	NEAR(in.gyro_bias, 520, 0.5f);
	NEAR(in.x, 128, 0.5f);
	NEAR(in.y, 96, 0.5f);
	/* Giro: 50 unidades sobre el sesgo durante 1 s → (50 − 2) × 2 = 96 px. */
	p.gyro = 570;
	settle(&c, &in, &p, 60);
	NEAR(in.x, 128 + 96, 1.5f);
	/* El cabeceo se mueve por diferencia y el puntero se queda donde llegó. */
	sds_input_init(&in);
	p = neutral();
	settle(&c, &in, &p, 60);
	set_tilt(&p, 0, 5);
	settle(&c, &in, &p, 5);
	set_tilt(&p, 0, 10);
	settle(&c, &in, &p, 60);
	NEAR(in.y, 96 + 40, 3.0f);  /* de 0° (control en reposo sin gravedad) a 10°: 10 × 4 px */
	/* Trinquete: sin el botón apretado no se mueve; al apretarlo sí. */
	const char *cl = "TRINQUETE = R1\n";
	sds_padcfg_parse(&c, cl, strlen(cl), NULL, 0);
	sds_input_init(&in);
	p = neutral();
	settle(&c, &in, &p, 60);
	p.gyro = 570;
	settle(&c, &in, &p, 30);
	NEAR(in.x, 128, 0.5f);
	p.buttons = 1u << SDS_DS3_R1;
	settle(&c, &in, &p, 30);
	CHECK(in.x > 128 + 40);
	/* L3 recentra. */
	p.buttons = 1u << SDS_DS3_L3;
	p.gyro = in.gyro_bias > 0 ? 512 : 512;
	sds_pad_map(&c, &in, &p, &k);
	sds_pointer_update(&c, &in, &p, 1.0f / 60.0f, &k);
	NEAR(in.x, 128, 1.0f);
}

void test_dspad(void)
{
	test_gyro();
	test_mapping();
	test_left_stick_dpad();
	test_config();
	test_touch_semantics();
	test_stick();
	test_tilt();
}
