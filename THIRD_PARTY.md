# Third-party material in spukit

spukit is MIT (`LICENSE`) and all of its code is original.

| Where | Origin | Licence | What was taken |
|---|---|---|---|
| `include/spukit/lz.h`, `lzjob.h`, `lzstream.h`, `spu/lz.c`, `src/lzstream.c` (block compression on the PPE, the host and an SPE) | The LZ4 block format specification by Yann Collet ([`doc/lz4_Block_format.md`](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)) | Public specification (liblz4 itself is BSD-2-Clause) | Only the format. The compressor and decompressor are written from the specification: there is no LZ4 code. Output was checked both ways against liblz4 1.10.0 |

spukit is built with the [PSL1GHT](https://github.com/ps3dev/PSL1GHT) toolchain (MIT), but contains no code from it.
