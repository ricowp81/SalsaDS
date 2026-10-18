/*
 * SalsaDS — entrada: DualShock 3 → botones de la DS, y el puntero del táctil.
 *
 * No depende de PSL1GHT (el programa convierte padData a struct sds_ds3): corre
 * igual en las pruebas del host.
 *
 * Botones, por omisión:
 *   A ○ · B ✕ · X △ · Y □ · L L1 · R R1 · Start · Select · cruceta
 *   el stick izquierdo hace de cruceta (con histéresis)
 *   L2 cambia la distribución de las pantallas
 *   R2 es el botón de toque (la DS «toca» mientras está apretado)
 *   L3 recentra la inclinación (la postura actual pasa a ser el centro)
 *
 * Táctil: un puntero sobre la pantalla táctil (256×192). Se mueve con el
 * stick derecho (velocidad proporcional a la inclinación del stick, con zona
 * muerta) o con la inclinación del control (Sixaxis: inclinar a los lados mueve
 * en horizontal y cabecear en vertical, relativo a la postura de recentrado);
 * en el modo de inclinación el stick derecho sigue moviendo el puntero.
 * El puntero solo toca mientras se mantiene apretado el botón de toque:
 * arrastrar es moverlo con el botón apretado y soltarlo es levantar el lápiz.
 *
 * Todo se cambia con un archivo de texto (SALSADS_DATA_DIR/controles.cfg), en
 * el espíritu del de Salsa64:
 *
 *   # comentario
 *   TOQUE = R3            botón de toque (hasta dos botones del DS3)
 *   RECENTRAR = L3
 *   DISPOSICION = L2
 *   A = CIRCULO           cualquier botón de la DS: A B X Y L R START SELECT
 *   PUNTERO = INCLINACION   o STICK                ARRIBA ABAJO IZQUIERDA DERECHA
 *   zona_muerta = 12      stick derecho: % del recorrido que se ignora
 *   velocidad = 200       stick derecho: píxeles por segundo a fondo
 *   curva = 1.0           1 = proporcional; > 1 = más fino cerca del centro
 *   filtro_ms = 120       inclinación: constante de tiempo del filtro paso bajo
 *   zona_muerta_inclinacion = 1.0   grados alrededor del centro que se ignoran
 *   sensibilidad_x = 4.0  píxeles por grado, a los lados
 *   sensibilidad_y = 4.0  píxeles por grado, cabeceo
 *   invertir_x = 0   invertir_y = 0
 *   eje_lateral = X  eje_gravedad = Y   ejes del Sixaxis (X, Y o Z)
 *   giroscopio = 0.0  modo inclinación: px por (unidad cruda · s) de guiñada (0 = no se usa)
 *   PUNTERO = GIRO    rotar el control mueve el puntero, relativo (como un ratón):
 *     horizontal = guiñada del giroscopio integrada (sesgo calibrado al arrancar y
 *     recalibrado con el control quieto), vertical = cambio del cabeceo entre cuadros
 *   giro_sensibilidad = 1.0   px por (unidad cruda · s) de guiñada
 *   giro_zona_muerta = 2      unidades crudas alrededor del sesgo que se ignoran
 *   TRINQUETE = R1    (modo GIRO) el puntero solo se mueve mientras se mantiene; NADA = siempre
 *
 * Nombres del DS3: CRUZ CIRCULO CUADRADO TRIANGULO L1 R1 L2 R2 L3 R3 START
 * SELECT ARRIBA ABAJO IZQUIERDA DERECHA NADA. Mayúsculas o minúsculas da igual.
 */
#ifndef SALSADS_FRONTEND_DSPAD_H
#define SALSADS_FRONTEND_DSPAD_H

#include <stddef.h>
#include <stdint.h>

#define SDS_GYRO_CAL_FRAMES 45  /* ~0,75 s quieto al arrancar para medir el sesgo */
#define SDS_TOUCH_W 256
#define SDS_TOUCH_H 192

enum sds_ds3_button {
	SDS_DS3_CROSS, SDS_DS3_CIRCLE, SDS_DS3_SQUARE, SDS_DS3_TRIANGLE,
	SDS_DS3_L1, SDS_DS3_R1, SDS_DS3_L2, SDS_DS3_R2,
	SDS_DS3_L3, SDS_DS3_R3, SDS_DS3_START, SDS_DS3_SELECT,
	SDS_DS3_UP, SDS_DS3_DOWN, SDS_DS3_LEFT, SDS_DS3_RIGHT,
	SDS_DS3_NBUTTONS,
	SDS_DS3_NONE = 0xFF,
};

/* Estado del DS3: botones en bits (1 << enum sds_ds3_button), sticks de 0 a
 * 255 con centro en 128 (X a la derecha, Y hacia abajo) y sensores de 0 a
 * 1023 (centro nominal 512). sensors_ok = 0 si el control no los da. */
struct sds_ds3 {
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
	uint16_t accel[3];  /* X, Y, Z */
	uint16_t gyro;
	uint8_t sensors_ok;
};

/* Botones de la DS (orden del registro KEYINPUT; X e Y en EXTKEYIN). */
enum sds_key {
	SDS_KEY_A, SDS_KEY_B, SDS_KEY_SELECT, SDS_KEY_START,
	SDS_KEY_RIGHT, SDS_KEY_LEFT, SDS_KEY_UP, SDS_KEY_DOWN,
	SDS_KEY_R, SDS_KEY_L, SDS_KEY_X, SDS_KEY_Y,
	SDS_KEY_COUNT,
};

/* Entradas configurables: los 12 botones de la DS y tres acciones. */
enum sds_in_id {
	SDS_IN_A, SDS_IN_B, SDS_IN_X, SDS_IN_Y, SDS_IN_L, SDS_IN_R,
	SDS_IN_START, SDS_IN_SELECT, SDS_IN_UP, SDS_IN_DOWN, SDS_IN_LEFT, SDS_IN_RIGHT,
	SDS_IN_TOUCH, SDS_IN_RECENTER, SDS_IN_LAYOUT, SDS_IN_CLUTCH,
	SDS_IN_COUNT,
};

enum sds_pointer_mode { SDS_POINTER_STICK, SDS_POINTER_TILT, SDS_POINTER_GYRO };
enum sds_axis { SDS_AXIS_X, SDS_AXIS_Y, SDS_AXIS_Z };

struct sds_padcfg {
	uint8_t source[SDS_IN_COUNT];   /* botón del DS3 o SDS_DS3_NONE */
	uint8_t source2[SDS_IN_COUNT];  /* otro botón que hace lo mismo */
	uint8_t pointer;                /* enum sds_pointer_mode */
	uint8_t deadzone;               /* stick derecho, % del recorrido */
	uint8_t invert_x, invert_y;
	uint8_t axis_lateral, axis_gravity;  /* enum sds_axis */
	float speed;                    /* px/s a fondo */
	float curve;
	float filter_ms;
	float tilt_deadzone;            /* grados */
	float sens_x, sens_y;           /* px por grado */
	float gyro_gain;                /* modo inclinación: px por (unidad cruda · s) del giroscopio (0 = apagado) */
	float gyro_sens;                /* modo GIRO: px por (unidad cruda · s) de guiñada */
	float gyro_dz;                  /* modo GIRO: unidades crudas que se ignoran alrededor del sesgo */
};

void sds_padcfg_default(struct sds_padcfg *cfg);
/* Aplica las líneas de `text` sobre `cfg`. Devuelve cuántas líneas no se
 * entendieron; la primera queda descrita en `err` (puede ser NULL). */
int sds_padcfg_parse(struct sds_padcfg *cfg, const char *text, size_t len, char *err, size_t err_size);
/* El archivo con los valores actuales y comentarios; devuelve los bytes
 * escritos (sin el NUL) o -1 si no cupo. */
int sds_padcfg_format(const struct sds_padcfg *cfg, char *buf, size_t size);
/* Una línea de texto con lo esencial (botón de toque, modo del puntero…). */
int sds_padcfg_summary(const struct sds_padcfg *cfg, char *buf, size_t size);
const char *sds_ds3_button_name(unsigned button);

/* Botones de la DS (bits 1 << enum sds_key; 1 = apretado) y acciones. */
struct sds_keys {
	uint16_t keys;
	uint8_t touch_held;     /* el botón de toque está apretado */
	uint8_t clutch_held;    /* trinquete apretado (1 siempre si no hay botón asignado) */
	uint8_t recenter, layout;  /* flancos: 1 solo en el cuadro en que se aprieta */
};

/* Estado interno del puntero y de los flancos. */
struct sds_input {
	uint32_t prev_inputs;   /* entradas apretadas en el cuadro anterior (bits enum sds_in_id) */
	uint16_t stick_keys;    /* cruceta del stick izquierdo (bits sds_key), con histéresis */
	/* puntero (coordenadas de la pantalla táctil) */
	float x, y;
	float off_x, off_y;     /* desplazamiento del stick en el modo de inclinación */
	int touching;
	/* inclinación */
	float f_roll, f_pitch;  /* ángulos filtrados, grados */
	float c_roll, c_pitch;  /* centro */
	float gyro_x;           /* giro integrado con fuga */
	int tilt_init;
	/* últimos ángulos crudos, para el log */
	float raw_roll, raw_pitch;
	/* modo GIRO */
	float gyro_bias;        /* sesgo del giroscopio (unidades crudas) */
	float gyro_cal_sum;
	int gyro_cal_n;         /* muestras de la calibración inicial (hasta SDS_GYRO_CAL_FRAMES) */
	int still_frames;       /* cuadros seguidos con el control quieto */
	float prev_f_pitch;
	uint16_t prev_accel[3];
	float gyro_dx_net;      /* desplazamiento horizontal neto causado por el giroscopio, px (diagnóstico) */
};

void sds_input_init(struct sds_input *in);
/* DS3 → botones de la DS y acciones; actualiza los flancos de `in`. */
void sds_pad_map(const struct sds_padcfg *cfg, struct sds_input *in, const struct sds_ds3 *pad,
		 struct sds_keys *out);
/* Mueve el puntero `dt` segundos. Con `recenter` (flanco) la postura actual
 * del control pasa a ser el centro y el puntero vuelve al centro de la
 * pantalla. `touch` es el estado del botón de toque. */
void sds_pointer_update(const struct sds_padcfg *cfg, struct sds_input *in, const struct sds_ds3 *pad, float dt,
			const struct sds_keys *keys);
/* Posición entera del toque y si hay toque (coordenadas 0..255, 0..191). */
void sds_touch_state(const struct sds_input *in, int *x, int *y, int *down);

#endif
