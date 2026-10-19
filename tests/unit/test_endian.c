/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Memoria little-endian de la máquina emulada: debe dar lo mismo en un host
 * little-endian y en uno big-endian. */
#include <string.h>

#include "check.h"
#include "util/endian.h"

void test_endian(void)
{
	/* Bytes como los guardaría la DS: 0x78 en la dirección más baja. */
	static const uint8_t mem[8] = { 0x78, 0x56, 0x34, 0x12, 0xef, 0xbe, 0xad, 0xde };

	CHECK_EQ(rd32le(mem), 0x12345678);
	CHECK_EQ(rd32le(mem + 4), 0xdeadbeef);
	CHECK_EQ(rd16le(mem), 0x5678);
	CHECK_EQ(rd16le(mem + 2), 0x1234);
	CHECK_EQ(rd16le(mem + 5), 0xadbe); /* no alineado */

	uint8_t out[8];
	memset(out, 0, sizeof out);
	wr32le(out, 0xdeadbeef);
	wr16le(out + 4, 0xcafe);
	CHECK_EQ(out[0], 0xef);
	CHECK_EQ(out[1], 0xbe);
	CHECK_EQ(out[2], 0xad);
	CHECK_EQ(out[3], 0xde);
	CHECK_EQ(out[4], 0xfe);
	CHECK_EQ(out[5], 0xca);
	CHECK_EQ(rd32le(out), 0xdeadbeef);
}
