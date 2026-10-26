/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX3D_H
#define SALSADS_RSX3D_H
#include "gpu3d/r3d.h"
typedef struct rsx3d_stats {
    uint32_t polys, draws, textures, uploaded, cache_hits;
    uint32_t draws_before, state_changes;
    uint64_t identity_us, vertex_us, state_us, lookup_us, decode_us, upload_us;
    uint64_t raster_ns, export_ns;
    uint64_t prepare_us, copy_us, total_us, wait_us, reuse_us;
} rsx3d_stats;
struct spukit_worker;
void rsx3d_decode_worker(struct spukit_worker *worker);
/* Llamar después de encolar el raster de respaldo: tiene prioridad sobre RGBA8. */
void rsx3d_decode_kick(void);
void rsx3d_decode_drain(void);
int rsx3d_decode_test(void);
int rsx3d_decode_raster_test(const r3d_job *job);
int rsx3d_init(void);
int rsx3d_selftest(void);
int rsx3d_reference_test(void);
/* Opcional, válido solo para la siguiente llamada; 41 páginas físicas de 16 KiB.
 * Los sellos deben cambiar al escribir y no reutilizarse tras reiniciar el núcleo. */
void rsx3d_identity_versions(uint64_t vram_ea, const uint64_t *versions);
/* 1: exportado a out_ea; 0: cuadro no soportado, salida intacta; -1: error. */
/* Ranuras 0/1: retirar antes de reutilizar. No retiene listas ni VRAM del juego.
 * wait publica out_ea; debe preceder captura, composición y reciclado del set. */
int rsx3d_submit(const r3d_job *job, unsigned slot, rsx3d_stats *stats);
int rsx3d_wait(unsigned slot, rsx3d_stats *stats);
int rsx3d_render(const r3d_job *job, rsx3d_stats *stats);
const char *rsx3d_reason(void);
void rsx3d_shutdown(void);
#endif
