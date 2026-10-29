/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — el 2D de rsxkit/ui.h en el RSX: memoria mapeada con el atlas y
 * dos búferes de vértices, y el lote dibujado sobre la pantalla del cuadro
 * con el 3D de draw3d.c (textura × color del vértice, mezcla alfa). La
 * geometría la arma ui_geom.c, que también escribe en el atlas los glifos
 * nuevos de la fuente externa mientras arma el lote: el RSX los ve porque
 * rsxkit_vertices invalida la caché de texturas en cada lote, después de la
 * barrera de memoria de rsxkit_ui_end.
 */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "rsxkit/ui.h"

#define UI_BYTES     (6u << 20)
#define ATLAS_PITCH  (RSXKIT_UI_ATLAS_W * 4)
#define ATLAS_BYTES  ((ATLAS_PITCH * RSXKIT_UI_ATLAS_H + 0xFFFFu) & ~0xFFFFu)
#define VB_CAP       (RSXKIT_UI_MAX_QUADS * 6)
#define VB_BYTES     (VB_CAP * (uint32_t)sizeof(rsxkit_vertex))
/* Una ranura de imagen: pitch fijo de 320 píxeles (múltiplo de 64 bytes). */
#define IMG_PITCH    (RSXKIT_UI_IMAGE_MAX_W * 4)
#define IMG_BYTES    ((IMG_PITCH * RSXKIT_UI_IMAGE_MAX_H + 0xFFFFu) & ~0xFFFFu)
#define IMG_BASE     (ATLAS_BYTES + 2 * VB_BYTES)

_Static_assert(IMG_BASE + RSXKIT_UI_IMAGES * IMG_BYTES <= UI_BYTES, "la memoria del 2D no alcanza");
_Static_assert(IMG_PITCH % 64 == 0, "pitch de imagen múltiplo de 64");
_Static_assert(ATLAS_PITCH % 64 == 0, "pitch de textura múltiplo de 64");

static struct {
	int ready, active, half;
	uint8_t *mem;
	uint32_t off;
	rsxkit_vertex *vb[2];
	uint32_t vb_off[2];
	uint32_t fence[2];
	rsxkit_draw draw;
	rsxkit_draw img[RSXKIT_UI_IMAGES];  /* como draw, con la textura de cada ranura */
	int img_ok[RSXKIT_UI_IMAGES];
} ui;

int rsxkit_ui_init(void)
{
	if (ui.ready)
		return RSXKIT_OK;
	ui.mem = memalign(1u << 20, UI_BYTES);
	if (!ui.mem)
		return RSXKIT_ENOMEM;
	int rc = rsxkit_map_main(ui.mem, UI_BYTES, &ui.off);
	if (rc != RSXKIT_OK) {
		free(ui.mem);
		ui.mem = NULL;
		return rc;
	}
	rsxkit_ui_atlas(ui.mem, ATLAS_PITCH);
	for (int i = 0; i < 2; i++) {
		ui.vb[i] = (rsxkit_vertex *)(ui.mem + ATLAS_BYTES + i * VB_BYTES);
		ui.vb_off[i] = ui.off + ATLAS_BYTES + i * VB_BYTES;
	}

	rsxkit_draw *d = &ui.draw;
	memset(d, 0, sizeof *d);
	d->blend = RSXKIT_BLEND_ALPHA;
	d->cull = RSXKIT_CULL_NONE;
	d->ntex = 1;
	rsxkit_texture *t = &d->tex[0];
	t->offset = ui.off;
	t->pitch = ATLAS_PITCH;
	t->width = RSXKIT_UI_ATLAS_W;
	t->height = RSXKIT_UI_ATLAS_H;
	t->location = RSXKIT_MEM_MAIN;
	t->format = RSXKIT_RGBA8888;
	t->wrap_s = t->wrap_t = RSXKIT_WRAP_CLAMP;
	t->filter = RSXKIT_FILTER_LINEAR;
	t->xf[0] = 1.0f / RSXKIT_UI_ATLAS_W;
	t->xf[1] = 1.0f / RSXKIT_UI_ATLAS_H;
	/* color = T0 · C */
	for (int c = 0; c < 4; c++)
		d->k[RSXKIT_K_T0][1][c] = 1.0f;
	/* Las ranuras de imagen: el mismo estado, con otra textura (sin tamaño
	 * hasta que se sube una). */
	for (int i = 0; i < RSXKIT_UI_IMAGES; i++) {
		ui.img[i] = ui.draw;
		rsxkit_texture *it = &ui.img[i].tex[0];
		it->offset = ui.off + IMG_BASE + (uint32_t)i * IMG_BYTES;
		it->pitch = IMG_PITCH;
	}
	ui.ready = 1;
	return RSXKIT_OK;
}

int rsxkit_ui_image_set(int slot, const uint8_t *rgba, uint32_t w, uint32_t h)
{
	if (!ui.ready)
		return RSXKIT_EARG;
	if (slot < 0 || slot >= RSXKIT_UI_IMAGES || !rgba || !w || !h || w > RSXKIT_UI_IMAGE_MAX_W ||
	    h > RSXKIT_UI_IMAGE_MAX_H)
		return RSXKIT_EARG;
	/* El RSX puede estar leyendo la imagen anterior en un cuadro en vuelo. */
	for (int i = 0; i < 2; i++)
		if (ui.fence[i])
			rsxkit_fence_wait(ui.fence[i]);
	uint8_t *dst = ui.mem + IMG_BASE + (size_t)slot * IMG_BYTES;
	for (uint32_t y = 0; y < h; y++)
		memcpy(dst + (size_t)y * IMG_PITCH, rgba + (size_t)y * w * 4, (size_t)w * 4);
	__sync_synchronize();
	rsxkit_texture *t = &ui.img[slot].tex[0];
	t->width = (uint16_t)w;
	t->height = (uint16_t)h;
	t->xf[0] = 1.0f / (float)w;
	t->xf[1] = 1.0f / (float)h;
	ui.img_ok[slot] = 1;
	rsxkit_ui_image_size(slot, w, h);
	return RSXKIT_OK;
}

int rsxkit_ui_image_update(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *rgba)
{
	if (!ui.ready || slot < 0 || slot >= RSXKIT_UI_IMAGES || !ui.img_ok[slot] || !rgba || !w || !h)
		return RSXKIT_EARG;
	const rsxkit_texture *t = &ui.img[slot].tex[0];
	if (x > t->width || w > t->width - x || y > t->height || h > t->height - y)
		return RSXKIT_EARG;
	for (int i = 0; i < 2; i++)
		if (ui.fence[i])
			rsxkit_fence_wait(ui.fence[i]);
	uint8_t *dst = ui.mem + IMG_BASE + (size_t)slot * IMG_BYTES + (size_t)y * IMG_PITCH + (size_t)x * 4;
	for (uint32_t r = 0; r < h; r++)
		memcpy(dst + (size_t)r * IMG_PITCH, rgba + (size_t)r * w * 4, (size_t)w * 4);
	__sync_synchronize();
	return RSXKIT_OK;
}

static void ui_begin(rsxkit_target *t, float lw, float lh)
{
	if (!ui.ready)
		return;
	ui.half ^= 1;
	/* El RSX ya no lee este búfer (lo usó hace dos cuadros). */
	if (ui.fence[ui.half])
		rsxkit_fence_wait(ui.fence[ui.half]);
	if (t) {
		if (rsxkit_bind(t, NULL, lw, lh) != RSXKIT_OK)
			return;
	} else {
		rsxkit_bind_frame(lw, lh);
	}
	ui.draw.scissor[0] = ui.draw.scissor[1] = 0.0f;
	ui.draw.scissor[2] = lw;
	ui.draw.scissor[3] = lh;
	/* Las ranuras de imagen se copiaron de ui.draw al iniciar: el recorte también. */
	for (int i = 0; i < RSXKIT_UI_IMAGES; i++)
		memcpy(ui.img[i].scissor, ui.draw.scissor, sizeof ui.draw.scissor);
	rsxkit_ui_batch(ui.vb[ui.half], VB_CAP, lw, lh);
	ui.active = 1;
}

void rsxkit_ui_begin(float lw, float lh)
{
	ui_begin(NULL, lw, lh);
}

void rsxkit_ui_begin_target(rsxkit_target *t, float lw, float lh)
{
	ui_begin(t, lw, lh);
}

void rsxkit_ui_end(void)
{
	if (!ui.active)
		return;
	ui.active = 0;
	uint32_t n = rsxkit_ui_count();
	if (n) {
		/* Una llamada por grupo (rsxkit_ui_groups): con el estado igual,
		 * cada una son unas pocas palabras en el búfer de comandos. */
		const uint32_t *first;
		uint32_t groups = rsxkit_ui_groups(&first);
		if (rsxkit_ui_atlas_dirty())
			__sync_synchronize();  /* los glifos nuevos, antes de que el RSX lea */
		rsxkit_vertices(ui.vb_off[ui.half], RSXKIT_MEM_MAIN);
		for (uint32_t g = 0; g < groups; g++) {
			uint32_t end = g + 1 < groups ? first[g + 1] : n;
			uint32_t img = rsxkit_ui_group_image(g);
			if (img == RSXKIT_UI_HOOK_GROUP) {
				rsxkit_ui_hook_fn fn;
				void *user;
				rsxkit_ui_group_hook(g, &fn, &user);
				if (fn) {
					fn(user);
					rsxkit_vertices(ui.vb_off[ui.half], RSXKIT_MEM_MAIN);
				}
				continue;
			}
			rsxkit_draw *d = img && ui.img_ok[img - 1] ? &ui.img[img - 1] : &ui.draw;
			if (end > first[g] && (!img || ui.img_ok[img - 1]))
				rsxkit_draw_triangles(d, first[g], end - first[g]);
		}
	}
	ui.fence[ui.half] = rsxkit_fence();
	rsxkit_ui_batch(NULL, 0, 0.0f, 0.0f);
}
