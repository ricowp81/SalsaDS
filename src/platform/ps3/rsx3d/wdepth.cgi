/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Profundidad W por fragmento, la misma expresión en todos los programas que
 * la escriben (shade, toon, sombra, máscara, rango, niebla): dos polígonos con
 * la misma W entera deben dar el mismo valor de 24 bits, porque la DS resuelve
 * el empate frente/espalda con «menor o igual».
 *   s.x = 1  (normal): 1/pos.w redondeado al entero (en un plano llega como
 *            w−ε) por la escala 2^wshift entera (q.w), ambas sin ε de
 *            interpolación; después se divide por 16777215.
 *   s.x = 0  (3d-rsx-w-truncada): floor(1/pos.w)·q.y, la forma anterior,
 *            con q.y interpolada; sin paridad en los empates.
 * Sin ramas: el selector entra como 0/1 exactos (step) y multiplica. */
#define RSX3D_WDEPTH(pos, q, s) \
    (min(floor(1.0 / (pos).w + 0.5 * step(0.5, (s).x)) * \
         (floor((q).w + 0.5) / 16777215.0 * step(0.5, (s).x) + (q).y * (1.0 - step(0.5, (s).x))), 1.0))
