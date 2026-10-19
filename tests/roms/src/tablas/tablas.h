/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — ROM tablas: tamaños y dirección de los resultados (compartidos por las dos CPUs). */
#ifndef TABLAS_H
#define TABLAS_H

#define TAB_SINE_N 64u
#define TAB_PITCH_N 768u
#define TAB_VOL_N 724u
#define TAB_OOB_N 9u /* a continuación: sine(64, 100, -1), pitch(768, 1000, -1), volume(724, 1000, -1) */
#define TABLAS_ADDR 0x02300000u /* sine, pitch, volume: una palabra por índice */

#endif
