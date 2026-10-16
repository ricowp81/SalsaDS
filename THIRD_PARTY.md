# Third-party material in rsxkit

rsxkit is MIT (`LICENSE`). Everything in it is original code, except the
items below, all under permissive licences or in the public domain.

| Where | Origin | Licence | What was taken |
|---|---|---|---|
| `src/video.c` | The `samples/graphics/rsxtest` sample of [PSL1GHT](https://github.com/ps3dev/PSL1GHT) (`f649a08f`) | MIT (notice below) | The RSX and video initialisation, surface and flip sequence, rewritten in C |
| `src/fprog.c`, `src/fpgen.c`, `include/rsxkit/fpgen.h` | The NV40 fragment program instruction format in PSL1GHT's `tools/cgcomp` (`include/nvfx_shader.h`, `fpparser.cpp`, `compilerfp.cpp`, `main.cpp`; `f649a08f`) | MIT (notice below) | Only the instruction format (fields, opcodes, source types, swizzles, precision). The encoder, register allocation and cache are original |
| `src/font_8x13.h` | The X11 «fixed» 8x13 font (`8x13.bdf`, misc-misc, X.Org; ucs-fonts version by Markus Kuhn) | Public domain («Public domain font. Share and enjoy.») | The bitmaps of 217 glyphs, converted with `tools/bdf2c.py` (original, MIT) |
| `src/ui_geom.c` (2× glyph enlargement) | The Scale2x / EPX rule by Andrea Mazzoleni, from its public description (scale2x.it) | Algorithm only, no code | Original implementation of the four neighbour rules |
| `include/rsxkit/ui.h`, `src/ui.c`, `src/ui_geom.c` (image slots of the 2D layer) | Contribution by VIDEOGAMES SCZ (2026-10-13) | MIT, with the contributor's agreement | Code written for this library |

## PSL1GHT licence

```
Copyright (c) 2011 PSL1GHT Development Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```
