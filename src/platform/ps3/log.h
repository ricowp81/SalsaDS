/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — logger para PS3.
 *
 * Cada línea va a la TTY (stdout: RPCS3 la muestra en su consola y en
 * TTY.log), a /dev_hdd0/salsads/<programa>.log (se lee por FTP cuando la red
 * falla; fflush por línea, caro: revisar antes de usarlo en el bucle de
 * emulación) y, si hay red, por UDP hacia la Mac (tools/udplog.py). Los
 * fallos de netSendTo se cuentan y se reportan en salsads_log_shutdown().
 *
 * Destino UDP: broadcast 255.255.255.255:18195 por omisión. Para fijar una
 * IP, crear /dev_hdd0/salsads/log.cfg con "ip" o "ip:puerto".
 *
 * Cada línea lleva "#n", un número de secuencia por programa, para que el
 * receptor cuente pérdidas y duplicados del UDP.
 *
 * Sin asignaciones dinámicas: cada línea se arma en un búfer de pila de
 * SALSADS_LOG_LINE_MAX bytes. Seguro entre hilos: un lwmutex por línea
 * (el frontend escribe desde el hilo de emulación y el de presentación).
 */
#ifndef SALSADS_PLATFORM_PS3_LOG_H
#define SALSADS_PLATFORM_PS3_LOG_H

#include "platform/ps3/paths.h"

#define SALSADS_LOG_PORT     18195
#define SALSADS_LOG_LINE_MAX 1024
#define SALSADS_LOG_CFG      SALSADS_DATA_DIR "/log.cfg"

void salsads_log_init(const char *program);
/* Igual, con `banner` (si no es NULL) como primera línea, también por UDP:
 * la red se prepara antes de escribir nada. */
void salsads_log_init_banner(const char *program, const char *banner);
void salsads_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void salsads_log_shutdown(void);

#endif
