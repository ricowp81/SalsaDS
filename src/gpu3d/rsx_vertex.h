/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_VERTEX_H
#define SALSADS_RSX_VERTEX_H
#include "gpu3d/gx_exec.h"
#include <string.h>
#if defined(__ALTIVEC__) && !defined(__SPU__)
#include <altivec.h>
#endif
/* Contrato de 32 bytes del búfer final; no depende de cabeceras de plataforma. */
typedef struct rsx3d_vertex {
    float pos[4], st[2];
    uint8_t rgba[4];
    uint32_t pad;
} rsx3d_vertex;
static inline void rsx3d_vertex_scalar(rsx3d_vertex *b, const gx_vertex *g, uint32_t w)
{
    memset(b, 0, sizeof *b);
    b->pos[0] = g->sx * (float)w; b->pos[1] = g->sy * (float)w;
    b->pos[2] = (g->z / 16777215.0f) * (float)w; b->pos[3] = w;
    b->st[0] = g->s / 16.0f; b->st[1] = g->t / 16.0f;
    b->rgba[0] = g->r; b->rgba[1] = g->g; b->rgba[2] = g->b; b->rgba[3] = 255;
}
static inline void rsx3d_vertex_vector(rsx3d_vertex *b, const gx_vertex *g, uint32_t w)
{
#if defined(__ALTIVEC__) && !defined(__SPU__)
    const vector signed int xy = {g->sx, g->sy, 0, 1};
    const vector unsigned int ww = {w, w, w, w};
    const vector signed int uv = {g->s, g->t, 0, 0};
    const vector float zero = {0, 0, 0, 0};
    vector float pos = vec_madd(vec_ctf(xy, 0), vec_ctf(ww, 0), zero);
    vector float st = vec_ctf(uv, 4);
    memcpy(b->pos, &pos, 16); memcpy((uint8_t *)b + 16, &st, 16);
    /* Mantener la división y sus dos redondeos, sin aproximar el recíproco. */
    b->pos[2] = (g->z / 16777215.0f) * (float)w;
    b->rgba[0] = g->r; b->rgba[1] = g->g; b->rgba[2] = g->b; b->rgba[3] = 255;
#else
    rsx3d_vertex_scalar(b, g, w);
#endif
}
#endif
