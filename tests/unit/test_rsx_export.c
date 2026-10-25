/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_export.h"
#include "gpu3d/rsx_replay.h"
static void replay_roundtrip(void)
{
    static gx_frame f;
    static uint8_t vram[R3D_VRAM_ZERO_OFF];
    r3d_job j;
    memset(&f, 0, sizeof f); memset(&j, 0, sizeof j);
    f.npolys = 1; f.nverts = 3; f.swap_params = 3;
    f.vtx[0].sx = -1; f.vtx[1].s = -32768; f.vtx[2].w = 0x1234567;
    f.poly[0].nverts = 3; f.poly[0].vidx[1] = 1; f.poly[0].vidx[2] = 2;
    f.regs.toon_table[31] = 0x1234; j.palslot[5] = R3D_UNMAPPED;
    vram[123] = 0xab;
    FILE *io = tmpfile();
    CHECK(io != NULL);
    if (!io) return;
    CHECK(r3d_replay_io(io, &f, &j, vram, 0));
    rewind(io);
    CHECK_EQ(fgetc(io), 'R'); CHECK_EQ(fgetc(io), 'S'); CHECK_EQ(fgetc(io), 'X'); CHECK_EQ(fgetc(io), '1');
    rewind(io);
    memset(&f, 0, sizeof f); memset(&j, 0, sizeof j); memset(vram, 0, sizeof vram);
    CHECK(r3d_replay_io(io, &f, &j, vram, 1));
    CHECK_EQ(f.npolys, 1); CHECK_EQ(f.nverts, 3); CHECK_EQ(f.swap_params, 3);
    CHECK(f.vtx[0].sx == -1); CHECK(f.vtx[1].s == -32768); CHECK_EQ(f.vtx[2].w, 0x1234567);
    CHECK_EQ(f.poly[0].vidx[2], 2); CHECK_EQ(f.regs.toon_table[31], 0x1234);
    CHECK_EQ(j.palslot[5], R3D_UNMAPPED); CHECK_EQ(vram[123], 0xab);
    CHECK(!r3d_replay_io(io, &f, &j, vram, 1));
    fclose(io);
}
void test_rsx_export(void)
{
    replay_roundtrip();
    uint8_t b[4];
    /* Los cuatro canales recorren su dominio, con valores vecinos distintos. */
    for (unsigned a = 0; a < 32; a++) for (unsigned r = 0; r < 64; r++)
    for (unsigned g = 0; g < 64; g++) {
        uint32_t p = R3D_PX(r, g, (r + g + a) & 63, a);
        r3d_rsx_pack(b, p);
        CHECK_EQ(b[0], a); CHECK_EQ(b[1], (r + g + a) & 63);
        CHECK_EQ(b[2], g); CHECK_EQ(b[3], r);
        CHECK_EQ(r3d_rsx_unpack(b), p);
    }
}
