/*
 * SalsaDS — entrada: DualShock 3 → botones de la DS y puntero del táctil.
 * Ver dspad.h.
 */
#include "frontend/dspad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f

static const char *const ds3_names[SDS_DS3_NBUTTONS] = {
	"CRUZ", "CIRCULO", "CUADRADO", "TRIANGULO", "L1", "R1", "L2", "R2",
	"L3", "R3", "START", "SELECT", "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA",
};

static const char *const input_names[SDS_IN_COUNT] = {
	"A", "B", "X", "Y", "L", "R", "START", "SELECT", "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA",
	"TOQUE", "RECENTRAR", "DISPOSICION", "TRINQUETE",
};

static const char *const axis_names[3] = { "X", "Y", "Z" };

const char *sds_ds3_button_name(unsigned button)
{
	return button < SDS_DS3_NBUTTONS ? ds3_names[button] : "NADA";
}

void sds_padcfg_default(struct sds_padcfg *c)
{
	static const uint8_t def[SDS_IN_COUNT] = {
		[SDS_IN_A] = SDS_DS3_CIRCLE, [SDS_IN_B] = SDS_DS3_CROSS,
		[SDS_IN_X] = SDS_DS3_TRIANGLE, [SDS_IN_Y] = SDS_DS3_SQUARE,
		[SDS_IN_L] = SDS_DS3_L1, [SDS_IN_R] = SDS_DS3_R1,
		[SDS_IN_START] = SDS_DS3_START, [SDS_IN_SELECT] = SDS_DS3_SELECT,
		[SDS_IN_UP] = SDS_DS3_UP, [SDS_IN_DOWN] = SDS_DS3_DOWN,
		[SDS_IN_LEFT] = SDS_DS3_LEFT, [SDS_IN_RIGHT] = SDS_DS3_RIGHT,
		[SDS_IN_TOUCH] = SDS_DS3_R2, [SDS_IN_RECENTER] = SDS_DS3_L3,
		[SDS_IN_LAYOUT] = SDS_DS3_L2, [SDS_IN_CLUTCH] = SDS_DS3_NONE,
	};
	memcpy(c->source, def, sizeof def);
	memset(c->source2, SDS_DS3_NONE, sizeof c->source2);
	c->pointer = SDS_POINTER_STICK;
	c->deadzone = 12;
	c->invert_x = 0;
	c->invert_y = 1;  /* medido en la PS3: con la inclinación el eje vertical sale invertido */
	c->axis_lateral = SDS_AXIS_X;
	c->axis_gravity = SDS_AXIS_Y;
	c->speed = 200.0f;
	c->curve = 1.0f;
	c->filter_ms = 120.0f;
	c->tilt_deadzone = 1.0f;
	c->sens_x = 4.0f;
	c->sens_y = 4.0f;
	c->gyro_gain = 0.0f;
	c->gyro_sens = 1.0f;
	c->gyro_dz = 2.0f;
}

/* ---- Texto ------------------------------------------------------------- */

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

static char upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int same_word(const char *a, size_t alen, const char *b)
{
	if (alen != strlen(b))
		return 0;
	for (size_t i = 0; i < alen; i++)
		if (upper(a[i]) != b[i])
			return 0;
	return 1;
}

static int find_name(const char *w, size_t len, const char *const *names, int n)
{
	for (int i = 0; i < n; i++)
		if (same_word(w, len, names[i]))
			return i;
	return -1;
}

/* Siguiente palabra de [p, end): devuelve su inicio y largo, y avanza p. */
static const char *next_word(const char **p, const char *end, size_t *len)
{
	const char *s = *p;
	while (s < end && is_space(*s))
		s++;
	const char *w = s;
	while (s < end && !is_space(*s))
		s++;
	*len = (size_t)(s - w);
	*p = s;
	return w;
}

static int parse_float(const char *w, size_t len, float *out)
{
	char tmp[32];
	if (len == 0 || len >= sizeof tmp)
		return -1;
	memcpy(tmp, w, len);
	tmp[len] = '\0';
	char *e;
	const double v = strtod(tmp, &e);
	if (e == tmp || *e != '\0')
		return -1;
	*out = (float)v;
	return 0;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void set_err(char *err, size_t n, int line, const char *msg)
{
	if (err && n)
		snprintf(err, n, "línea %d: %s", line, msg);
}

int sds_padcfg_parse(struct sds_padcfg *c, const char *text, size_t len, char *err, size_t err_size)
{
	int bad = 0, line_no = 0;
	if (err && err_size)
		err[0] = '\0';
	const char *p = text, *end = text + len;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		const char *line_end = eol ? eol : end;
		line_no++;
		const char *q = p;
		p = eol ? eol + 1 : end;
		const char *hash = memchr(q, '#', (size_t)(line_end - q));
		const char *stop = hash ? hash : line_end;
		const char *eq = memchr(q, '=', (size_t)(stop - q));
		/* línea vacía o de comentario */
		{
			const char *s = q;
			while (s < stop && is_space(*s))
				s++;
			if (s == stop)
				continue;
		}
		if (!eq) {
			if (!bad++)
				set_err(err, err_size, line_no, "falta «=»");
			continue;
		}
		size_t klen;
		const char *kq = q;
		const char *key = next_word(&kq, eq, &klen);
		const char *vq = eq + 1;
		size_t vlen;
		const char *val = next_word(&vq, stop, &vlen);
		int ok = 0;
		const int in = find_name(key, klen, input_names, SDS_IN_COUNT);
		if (in >= 0) {
			uint8_t a = SDS_DS3_NONE, b = SDS_DS3_NONE;
			int ia = find_name(val, vlen, ds3_names, SDS_DS3_NBUTTONS);
			if (ia >= 0 || same_word(val, vlen, "NADA")) {
				if (ia >= 0)
					a = (uint8_t)ia;
				size_t l2;
				const char *w2 = next_word(&vq, stop, &l2);
				if (l2) {
					const int ib = find_name(w2, l2, ds3_names, SDS_DS3_NBUTTONS);
					if (ib >= 0 || same_word(w2, l2, "NADA")) {
						if (ib >= 0)
							b = (uint8_t)ib;
						ok = 1;
					}
				} else {
					ok = 1;
				}
				if (ok) {
					c->source[in] = a;
					c->source2[in] = b;
				}
			}
		} else if (same_word(key, klen, "PUNTERO")) {
			if (same_word(val, vlen, "STICK")) {
				c->pointer = SDS_POINTER_STICK;
				ok = 1;
			} else if (same_word(val, vlen, "INCLINACION")) {
				c->pointer = SDS_POINTER_TILT;
				ok = 1;
			} else if (same_word(val, vlen, "GIRO")) {
				c->pointer = SDS_POINTER_GYRO;
				ok = 1;
			}
		} else if (same_word(key, klen, "EJE_LATERAL") || same_word(key, klen, "EJE_GRAVEDAD")) {
			const int ax = find_name(val, vlen, axis_names, 3);
			if (ax >= 0) {
				if (upper(key[4]) == 'L')
					c->axis_lateral = (uint8_t)ax;
				else
					c->axis_gravity = (uint8_t)ax;
				ok = 1;
			}
		} else {
			float f;
			if (parse_float(val, vlen, &f) == 0) {
				ok = 1;
				if (same_word(key, klen, "ZONA_MUERTA"))
					c->deadzone = (uint8_t)clampf(f, 0.0f, 90.0f);
				else if (same_word(key, klen, "VELOCIDAD"))
					c->speed = clampf(f, 10.0f, 2000.0f);
				else if (same_word(key, klen, "CURVA"))
					c->curve = clampf(f, 0.5f, 4.0f);
				else if (same_word(key, klen, "FILTRO_MS"))
					c->filter_ms = clampf(f, 0.0f, 2000.0f);
				else if (same_word(key, klen, "ZONA_MUERTA_INCLINACION"))
					c->tilt_deadzone = clampf(f, 0.0f, 20.0f);
				else if (same_word(key, klen, "SENSIBILIDAD_X"))
					c->sens_x = clampf(f, 0.1f, 50.0f);
				else if (same_word(key, klen, "SENSIBILIDAD_Y"))
					c->sens_y = clampf(f, 0.1f, 50.0f);
				else if (same_word(key, klen, "INVERTIR_X"))
					c->invert_x = f != 0.0f;
				else if (same_word(key, klen, "INVERTIR_Y"))
					c->invert_y = f != 0.0f;
				else if (same_word(key, klen, "GIRO_SENSIBILIDAD"))
					c->gyro_sens = clampf(f, 0.01f, 100.0f);
				else if (same_word(key, klen, "GIRO_ZONA_MUERTA"))
					c->gyro_dz = clampf(f, 0.0f, 50.0f);
				else if (same_word(key, klen, "GIROSCOPIO"))
					c->gyro_gain = clampf(f, -10.0f, 10.0f);
				else
					ok = 0;
			}
		}
		if (!ok && !bad++)
			set_err(err, err_size, line_no, "no entendida");
	}
	return bad;
}

/* Un float con `dec` decimales sin usar printf de flotantes. */
static int put_float(char *buf, size_t size, float v, int dec)
{
	int neg = v < 0;
	if (neg)
		v = -v;
	unsigned scale = 1;
	for (int i = 0; i < dec; i++)
		scale *= 10;
	const unsigned long n = (unsigned long)(v * (float)scale + 0.5f);
	if (dec == 0)
		return snprintf(buf, size, "%s%lu", neg ? "-" : "", n);
	return snprintf(buf, size, "%s%lu.%0*lu", neg ? "-" : "", n / scale, dec, n % scale);
}

int sds_padcfg_format(const struct sds_padcfg *c, char *buf, size_t size)
{
	size_t used = 0;
	char f[24];
#define PUT(...)                                                                \
	do {                                                                    \
		int n_ = snprintf(buf + used, size - used, __VA_ARGS__);        \
		if (n_ < 0 || (size_t)n_ >= size - used)                        \
			return -1;                                              \
		used += (size_t)n_;                                             \
	} while (0)
#define PUTF(key, v, dec, comment)                                              \
	do {                                                                    \
		put_float(f, sizeof f, (v), (dec));                             \
		PUT("%s = %s    # %s\n", key, f, comment);                      \
	} while (0)
	if (!size)
		return -1;
	buf[0] = '\0';
	PUT("# SalsaDS — controles. Nombres del DS3: CRUZ CIRCULO CUADRADO TRIANGULO L1 R1 L2 R2 L3 R3\n");
	PUT("# START SELECT ARRIBA ABAJO IZQUIERDA DERECHA NADA. Hasta dos botones por entrada.\n");
	PUT("# Botón de toque: TOQUE = R2 (cambiar a R3: «TOQUE = R3»).\n");
	for (int k = 0; k < SDS_IN_COUNT; k++) {
		PUT("%s = %s", input_names[k], c->source[k] == SDS_DS3_NONE ? "NADA" : sds_ds3_button_name(c->source[k]));
		if (c->source[k] < SDS_DS3_NBUTTONS && c->source2[k] < SDS_DS3_NBUTTONS)
			PUT(" %s", sds_ds3_button_name(c->source2[k]));
		if (k == SDS_IN_TOUCH)
			PUT("    # la DS «toca» mientras se mantiene apretado");
		else if (k == SDS_IN_RECENTER)
			PUT("    # la postura actual pasa a ser el centro (y el puntero vuelve al centro)");
		else if (k == SDS_IN_LAYOUT)
			PUT("    # cambia la distribución de las pantallas");
		else if (k == SDS_IN_CLUTCH)
			PUT("    # modo GIRO: el puntero solo se mueve mientras se mantiene (NADA = siempre)");
		PUT("\n");
	}
	PUT("PUNTERO = %s    # STICK (stick derecho), INCLINACION (Sixaxis, absoluto) o GIRO (rotar mueve, relativo)\n",
	    c->pointer == SDS_POINTER_GYRO ? "GIRO" : c->pointer == SDS_POINTER_TILT ? "INCLINACION" : "STICK");
	PUT("zona_muerta = %u    # stick derecho: %% del recorrido que se ignora\n", c->deadzone);
	PUTF("velocidad", c->speed, 0, "stick derecho: píxeles por segundo a fondo");
	PUTF("curva", c->curve, 2, "1 = proporcional; mayor = más fino cerca del centro");
	PUTF("filtro_ms", c->filter_ms, 0, "inclinación: constante de tiempo del filtro paso bajo (0 = sin filtro)");
	PUTF("zona_muerta_inclinacion", c->tilt_deadzone, 2, "inclinación: grados alrededor del centro que se ignoran");
	PUTF("sensibilidad_x", c->sens_x, 2, "inclinación: píxeles por grado, a los lados");
	PUTF("sensibilidad_y", c->sens_y, 2, "inclinación: píxeles por grado, cabeceo");
	PUT("invertir_x = %u\ninvertir_y = %u    # 1 si el puntero se mueve al revés\n", c->invert_x, c->invert_y);
	PUT("eje_lateral = %s    # eje del Sixaxis que mide la inclinación a los lados (X, Y o Z)\n",
	    axis_names[c->axis_lateral]);
	PUT("eje_gravedad = %s    # eje que lee la gravedad con el control plano\n", axis_names[c->axis_gravity]);
	PUTF("giroscopio", c->gyro_gain, 2, "modo inclinación: px por (unidad cruda · s) de guiñada; 0 = no se usa");
	PUTF("giro_sensibilidad", c->gyro_sens, 2, "modo GIRO: px por (unidad cruda · s) de guiñada");
	PUTF("giro_zona_muerta", c->gyro_dz, 1, "modo GIRO: unidades crudas alrededor del sesgo que se ignoran");
#undef PUT
#undef PUTF
	return (int)used;
}

int sds_padcfg_summary(const struct sds_padcfg *c, char *buf, size_t size)
{
	return snprintf(buf, size, "toque=%s%s%s recentrar=%s disposicion=%s puntero=%s",
			sds_ds3_button_name(c->source[SDS_IN_TOUCH]),
			c->source2[SDS_IN_TOUCH] < SDS_DS3_NBUTTONS ? "+" : "",
			c->source2[SDS_IN_TOUCH] < SDS_DS3_NBUTTONS ? sds_ds3_button_name(c->source2[SDS_IN_TOUCH]) : "",
			sds_ds3_button_name(c->source[SDS_IN_RECENTER]), sds_ds3_button_name(c->source[SDS_IN_LAYOUT]),
			c->pointer == SDS_POINTER_GYRO ? "giro" : c->pointer == SDS_POINTER_TILT ? "inclinacion" : "stick");
}

/* ---- Botones ----------------------------------------------------------- */

void sds_input_init(struct sds_input *in)
{
	memset(in, 0, sizeof *in);
	in->x = SDS_TOUCH_W / 2.0f;
	in->y = SDS_TOUCH_H / 2.0f;
}

static int pressed(const struct sds_padcfg *c, uint32_t buttons, int input)
{
	const uint8_t a = c->source[input], b = c->source2[input];
	return (a < SDS_DS3_NBUTTONS && (buttons & (1u << a))) || (b < SDS_DS3_NBUTTONS && (buttons & (1u << b)));
}

/* Stick izquierdo como cruceta: una dirección se enciende al pasar de
 * STICK_ON desde el centro y se apaga al volver por debajo de STICK_OFF, para
 * que no parpadee cerca del umbral. */
#define STICK_ON  80
#define STICK_OFF 48

static uint16_t stick_axis(uint16_t keys, int v, int neg, int pos)
{
	const uint16_t n = (uint16_t)(1u << neg), p = (uint16_t)(1u << pos);
	const int d = v - 128;
	if (d <= -STICK_ON)
		return (uint16_t)((keys & ~p) | n);
	if (d >= STICK_ON)
		return (uint16_t)((keys & ~n) | p);
	if (d > -STICK_OFF)
		keys &= (uint16_t)~n;
	if (d < STICK_OFF)
		keys &= (uint16_t)~p;
	return keys;
}

void sds_pad_map(const struct sds_padcfg *c, struct sds_input *in, const struct sds_ds3 *pad, struct sds_keys *out)
{
	static const uint8_t key_of[SDS_IN_TOUCH] = {
		[SDS_IN_A] = SDS_KEY_A, [SDS_IN_B] = SDS_KEY_B, [SDS_IN_X] = SDS_KEY_X, [SDS_IN_Y] = SDS_KEY_Y,
		[SDS_IN_L] = SDS_KEY_L, [SDS_IN_R] = SDS_KEY_R, [SDS_IN_START] = SDS_KEY_START,
		[SDS_IN_SELECT] = SDS_KEY_SELECT, [SDS_IN_UP] = SDS_KEY_UP, [SDS_IN_DOWN] = SDS_KEY_DOWN,
		[SDS_IN_LEFT] = SDS_KEY_LEFT, [SDS_IN_RIGHT] = SDS_KEY_RIGHT,
	};
	uint32_t now = 0;
	uint16_t keys = 0;
	for (int k = 0; k < SDS_IN_COUNT; k++) {
		if (!pressed(c, pad->buttons, k))
			continue;
		now |= 1u << k;
		if (k < SDS_IN_TOUCH)
			keys |= (uint16_t)(1u << key_of[k]);
	}
	in->stick_keys = stick_axis(in->stick_keys, pad->lx, SDS_KEY_LEFT, SDS_KEY_RIGHT);
	in->stick_keys = stick_axis(in->stick_keys, pad->ly, SDS_KEY_UP, SDS_KEY_DOWN);
	keys |= in->stick_keys;
	/* La DS no tiene cruceta opuesta a la vez: arriba+abajo e izquierda+derecha
	 * no se dan juntos en el hardware; se anulan. */
	if ((keys & (1u << SDS_KEY_UP)) && (keys & (1u << SDS_KEY_DOWN)))
		keys &= (uint16_t) ~((1u << SDS_KEY_UP) | (1u << SDS_KEY_DOWN));
	if ((keys & (1u << SDS_KEY_LEFT)) && (keys & (1u << SDS_KEY_RIGHT)))
		keys &= (uint16_t) ~((1u << SDS_KEY_LEFT) | (1u << SDS_KEY_RIGHT));
	out->keys = keys;
	out->touch_held = (now >> SDS_IN_TOUCH) & 1u;
	out->clutch_held = c->source[SDS_IN_CLUTCH] == SDS_DS3_NONE && c->source2[SDS_IN_CLUTCH] == SDS_DS3_NONE
				   ? 1u : (uint8_t)((now >> SDS_IN_CLUTCH) & 1u);
	out->recenter = (uint8_t)(((now & ~in->prev_inputs) >> SDS_IN_RECENTER) & 1u);
	out->layout = (uint8_t)(((now & ~in->prev_inputs) >> SDS_IN_LAYOUT) & 1u);
	in->prev_inputs = now;
}

/* ---- Puntero ----------------------------------------------------------- */

/* Inclinación a los lados y cabeceo, en grados, de las lecturas del
 * acelerómetro (centro nominal 512). Los ángulos no dependen de la escala
 * de los sensores: son la dirección de la gravedad. */
static void tilt_angles(const struct sds_padcfg *c, const struct sds_ds3 *pad, float *roll, float *pitch)
{
	float a[3];
	for (int i = 0; i < 3; i++)
		a[i] = (float)pad->accel[i] - 512.0f;
	const int lat = c->axis_lateral, grav = c->axis_gravity;
	const int fwd = (lat == grav) ? (lat + 1) % 3 : 3 - lat - grav;
	const float gl = a[lat], gg = a[grav], gf = a[fwd];
	*roll = atan2f(gl, sqrtf(gg * gg + gf * gf)) * (180.0f / PI_F);
	*pitch = atan2f(gf, fabsf(gg)) * (180.0f / PI_F);
}

static float dead(float v, float dz)
{
	if (v > dz)
		return v - dz;
	if (v < -dz)
		return v + dz;
	return 0.0f;
}

/* Cuánto de `d` se puede avanzar desde `pos` sin pasar de [0, hi]. */
static float step_limited(float pos, float d, float hi)
{
	if (d > 0.0f) {
		const float room = hi - pos;
		return room <= 0.0f ? 0.0f : (d < room ? d : room);
	}
	const float room = -pos;
	return room >= 0.0f ? 0.0f : (d > room ? d : room);
}

void sds_pointer_update(const struct sds_padcfg *c, struct sds_input *in, const struct sds_ds3 *pad, float dt,
			const struct sds_keys *keys)
{
	const float maxx = SDS_TOUCH_W - 1.0f, maxy = SDS_TOUCH_H - 1.0f;

	/* Stick derecho: velocidad proporcional a la inclinación, con zona muerta
	 * radial. */
	float sx = ((float)pad->rx - 128.0f) / 127.0f, sy = ((float)pad->ry - 128.0f) / 127.0f;
	const float mag = sqrtf(sx * sx + sy * sy);
	const float dz = (float)c->deadzone / 100.0f;
	float dx = 0.0f, dy = 0.0f;
	if (mag > dz && mag > 0.0f) {
		float m = (mag > 1.0f ? 1.0f : mag);
		m = (m - dz) / (1.0f - dz);
		if (c->curve != 1.0f)
			m = powf(m, c->curve);
		dx = sx / mag * m * c->speed * dt;
		dy = sy / mag * m * c->speed * dt;
	}

	if (c->pointer == SDS_POINTER_TILT && pad->sensors_ok) {
		float roll, pitch;
		tilt_angles(c, pad, &roll, &pitch);
		in->raw_roll = roll;
		in->raw_pitch = pitch;
		if (!in->tilt_init) {
			in->f_roll = in->c_roll = roll;
			in->f_pitch = in->c_pitch = pitch;
			in->tilt_init = 1;
		}
		const float alpha = c->filter_ms <= 0.0f ? 1.0f : 1.0f - expf(-dt * 1000.0f / c->filter_ms);
		in->f_roll += alpha * (roll - in->f_roll);
		in->f_pitch += alpha * (pitch - in->f_pitch);
		if (keys->recenter) {
			in->c_roll = in->f_roll;
			in->c_pitch = in->f_pitch;
			in->off_x = in->off_y = 0.0f;
			in->gyro_x = 0.0f;
		}
		float tx = dead(in->f_roll - in->c_roll, c->tilt_deadzone) * c->sens_x;
		float ty = dead(in->f_pitch - in->c_pitch, c->tilt_deadzone) * c->sens_y;
		if (c->invert_x)
			tx = -tx;
		if (c->invert_y)
			ty = -ty;
		/* Giroscopio (opcional): el giro sobre el eje vertical, integrado y
		 * con fuga para que la deriva se olvide. */
		if (c->gyro_gain != 0.0f) {
			in->gyro_x += dead((float)pad->gyro - 512.0f, 2.0f) * c->gyro_gain * dt;
			in->gyro_x *= expf(-dt / 2.0f);
		}
		/* El desplazamiento del stick se suma a la inclinación sin pasarse del
		 * borde: contra el borde no se acumula (para no tener que «devolver»
		 * lo de más), y una inclinación que se pasa del borde no se descuenta
		 * del desplazamiento. */
		const float base_x = SDS_TOUCH_W / 2.0f + tx + in->gyro_x, base_y = SDS_TOUCH_H / 2.0f + ty;
		in->off_x += step_limited(base_x + in->off_x, dx, maxx);
		in->off_y += step_limited(base_y + in->off_y, dy, maxy);
		in->x = clampf(base_x + in->off_x, 0.0f, maxx);
		in->y = clampf(base_y + in->off_y, 0.0f, maxy);
	} else if (c->pointer == SDS_POINTER_GYRO && pad->sensors_ok) {
		/* Relativo, como un ratón: guiñada integrada (horizontal) y cambio del
		 * cabeceo entre cuadros (vertical). */
		float roll, pitch;
		tilt_angles(c, pad, &roll, &pitch);
		in->raw_roll = roll;
		in->raw_pitch = pitch;
		if (!in->tilt_init) {
			in->f_roll = roll;
			in->f_pitch = in->prev_f_pitch = pitch;
			in->tilt_init = 1;
			memcpy(in->prev_accel, pad->accel, sizeof in->prev_accel);
		}
		const float alpha = c->filter_ms <= 0.0f ? 1.0f : 1.0f - expf(-dt * 1000.0f / c->filter_ms);
		in->f_pitch += alpha * (pitch - in->f_pitch);
		const float g = (float)pad->gyro;
		float gx = 0.0f, gy = 0.0f;
		if (in->gyro_cal_n < SDS_GYRO_CAL_FRAMES) {
			/* Calibración inicial: el sesgo es el promedio con el control quieto. */
			in->gyro_cal_sum += g;
			in->gyro_cal_n++;
			in->gyro_bias = in->gyro_cal_sum / (float)in->gyro_cal_n;
		} else {
			/* Quieto: giroscopio dentro de ±4 del sesgo y acelerómetro sin cambios. */
			int still = fabsf(g - in->gyro_bias) <= 4.0f;
			for (int i = 0; i < 3; i++)
				if (abs((int)pad->accel[i] - (int)in->prev_accel[i]) > 3)
					still = 0;
			in->still_frames = still ? in->still_frames + 1 : 0;
			if (in->still_frames >= 10)
				in->gyro_bias += (1.0f - expf(-dt / 1.0f)) * (g - in->gyro_bias);
			const float w = dead(g - in->gyro_bias, c->gyro_dz);
			gx = w * c->gyro_sens * dt;
			gy = dead(in->f_pitch - in->prev_f_pitch, 0.02f) * c->sens_y;
			if (c->invert_x)
				gx = -gx;
			if (c->invert_y)
				gy = -gy;
			in->gyro_dx_net += gx;
			if (!keys->clutch_held)
				gx = gy = 0.0f;  /* trinquete: «con el ratón levantado» */
		}
		memcpy(in->prev_accel, pad->accel, sizeof in->prev_accel);
		in->prev_f_pitch = in->f_pitch;
		if (keys->recenter) {
			in->x = SDS_TOUCH_W / 2.0f;
			in->y = SDS_TOUCH_H / 2.0f;
			in->gyro_dx_net = 0.0f;
		}
		in->x = clampf(in->x + gx + dx, 0.0f, maxx);
		in->y = clampf(in->y + gy + dy, 0.0f, maxy);
	} else {
		if (keys->recenter) {
			in->x = SDS_TOUCH_W / 2.0f;
			in->y = SDS_TOUCH_H / 2.0f;
		}
		in->x = clampf(in->x + dx, 0.0f, maxx);
		in->y = clampf(in->y + dy, 0.0f, maxy);
	}
	in->touching = keys->touch_held;
}

void sds_touch_state(const struct sds_input *in, int *x, int *y, int *down)
{
	*x = (int)(in->x + 0.5f);
	*y = (int)(in->y + 0.5f);
	if (*x > SDS_TOUCH_W - 1)
		*x = SDS_TOUCH_W - 1;
	if (*y > SDS_TOUCH_H - 1)
		*y = SDS_TOUCH_H - 1;
	*down = in->touching;
}
