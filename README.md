# fastresize

A small, single-purpose C++ image decode/resize/composite/encode library —
no pipeline, no lazy evaluation graph, no operation-result caching, just
those four operations, implemented directly on top of `stb_image` +
`stb_image_resize2` + `stb_image_write`.

Extracted from [detail-image-bench](https://github.com/mattsplat/detail-image-bench),
a multi-language benchmark where every libvips-backed candidate measured
slower than this pipeline for a fetch-a-few-images/composite-once/encode-
once workload. [PERFORMANCE.md](./PERFORMANCE.md) explains the architectural
bet against libvips and the concrete techniques it enables;
[BENCHMARKS.md](./BENCHMARKS.md) has real numbers against `vips` (including
the one operation where vips still wins). Used from C++ directly, and from
Rust, Node, and PHP via the C ABI shim below.

## Supported image types

**Input** (decode, probe): anything [stb_image](https://github.com/nothings/stb/blob/master/stb_image.h)
supports — JPEG (baseline & progressive), PNG (1/2/4/8/16-bit-per-channel),
BMP (non-1bpp, non-RLE), TGA, GIF (first frame only), PSD (composited view
only), PIC, PNM (PPM/PGM, binary only), HDR. Always decoded to RGBA8,
straight (non-premultiplied) alpha, regardless of the source's channel
count or bit depth.

**Output** (encode): **PNG only**, via [fpng](https://github.com/richgel999/fpng)
by default (`make FPNG=0` falls back to `stb_image_write` — smaller file,
~12× slower; see `encodePng()` in `fastresize.h`). 4-channel RGBA by
default; `encodePngRgb()` flattens onto a solid background and writes
3-channel RGB instead. This is the one real functional gap against `vips`,
which reads and writes many formats — fastresize is intentionally scoped to
"decode whatever, always produce PNG."

## `fastresize.h` — the library

Every `Image` also carries `Rect opaque` — a **conservative superset** of
where its non-transparent pixels are (it must contain them all, but may be
larger, so "the whole image" is always legal). `decode()` computes it
tightly via `opaqueBounds()`; `resize*()` map it forward; `crop()`
intersects it. `compositeOver()` uses it to skip transparent margins
entirely, worth **7.7×** on a real composite workload (187.6 → 24.3 ms) once
assets are 10–37% filled. Nothing may assume pixels *outside* the rect are
transparent — it is only ever a licence to skip work.

### Library usage (C++)

```cpp
// Include from exactly one translation unit per binary - this header
// defines STB_IMAGE_IMPLEMENTATION and friends itself.
#include "fastresize.h"

int main() {
  std::string bytes = /* read a file, fetch over HTTP, whatever */;

  fastresize::Image bg = fastresize::decode(bytes);
  fastresize::Image fg = fastresize::decode(otherBytes);
  fg = fastresize::resizeNearest(fg, 200, 200);  // cheap kernel, no interpolation

  fastresize::Image canvas(bg.width, bg.height);       // blank, transparent
  fastresize::fill(canvas, 255, 255, 255, 255);        // ...or start opaque white
  fastresize::compositeOver(canvas, bg, 0, 0);
  fastresize::compositeOver(canvas, fg, 40, 40);       // skips fg's transparent margin

  std::string png = fastresize::encodePng(canvas);           // 4-channel RGBA
  std::string rgb = fastresize::encodePngRgb(canvas, 255, 255, 255);  // 3-channel RGB
  // ... write png somewhere
}
```

`crop(img, img.opaque)` trims a mostly-transparent asset down to its drawing
before you resize or cache it; the caller then adds `(opaque.x, opaque.y)`
back when compositing. `fr_opaque_rect()` exposes the same rect over the C
ABI.

`decode`/`probeDimensions` also take a `std::string` overload if you'd
rather not pass a raw pointer+length.

## `fastresize_capi.h` / `fastresize_capi.cpp` — the C ABI

A thin opaque-handle shim over `fastresize.h`'s decode/probe/resize/crop/
fill/composite/encode (`fr_new_canvas_rgba`, `fr_fill`, `fr_opaque_rect`,
`fr_crop`, `fr_encode_png_rgb` alongside the originals), so a non-C++
language can call the exact same implementation via FFI instead of
reimplementing it. No new algorithm lives here. Used from:

- **Rust**: compiled by a `build.rs` (the `cc` crate) and linked in directly.
- **Node**: compiled to `libfastresize_capi.so` and called via
  [`koffi`](https://koffi.dev).
- **PHP**: compiled to `libfastresize_capi.so` and called via `ext-ffi` — see
  [`fastresize-php`](https://github.com/mattsplat/fastresize-php), a Composer
  package wrapping this exact shim, for a complete worked example.

Build the shared library with `make capi` (below); link/load
`libfastresize_capi.so` (or `.dylib` on macOS) from whichever language.

## `fastresize` — the CLI

`fastresize_cli.cpp` is a thin command-line front-end over `fastresize.h`,
so its primitives can be run and timed from a shell the same way `vips`
is — this is what [BENCHMARKS.md](./BENCHMARKS.md) is built on.

### Build

```sh
make            # curls the pinned deps into ./vendor, builds ./fastresize
make FPNG=0     # ...with stb_image_write instead of fpng (see below)
make capi       # builds libfastresize_capi.so/.dylib (the C ABI, see above)
make test       # builds + runs fastresize_test (dependency-free)
make clean
```

`fastresize_test.cpp` is the whole test suite — one translation unit,
synthetic in-memory fixtures, pixel checks that round-trip through the
header's own `decode()`. No framework. CI runs it with both encoders on
Linux and macOS.

`encodePng()` has two backends, picked at build time. **fpng** is the
default (`-DFASTRESIZE_FPNG`, `fpng.cpp` compiled as its own object) —
measured **12×** faster encode (817 ms → 68 ms on an 8557×4000 canvas),
pixel-identical output, ~8% larger file. Encode was the single largest cost
in every workload this library was extracted from (see
[PERFORMANCE.md](./PERFORMANCE.md)), so it is on unless you ask for
`stb_image_write` with `make FPNG=0` (header-only, nothing extra to compile,
smallest file).

`-O3` plus an arch-appropriate `-march`/`-mcpu` — fastresize.h's stb resize
kernels and the `compositeOver` loop only vectorise past the SSE2/baseline-
NEON floor when the target ISA is explicit. No other dependencies.
`./vendor` and the build outputs are gitignored.

### Usage

```sh
fastresize resize    <in> <out> <scale>              [--nearest] [-v]
fastresize resize    <in> <out> --width <px> [--height <px>] [--nearest] [-v]
fastresize thumbnail <in> <out> <max-dim>            [--nearest] [-v]
fastresize composite <out> --size <W>x<H> [--background <r,g,b>] <layer[@x,y]>...   [-v]
fastresize crop      <in> <out> <x> <y> <w> <h>      [-v]
fastresize crop      <in> <out> --opaque             [-v]
fastresize header    <in>
```

Global options: `-v` / `--verbose` prints per-stage timings (decode, resize,
encode, composite) to stderr; `--flatten <r,g,b>` composites the result onto
that background and encodes a 3-channel RGB PNG instead of RGBA;
`--png-level <0-9>` sets the deflate effort — only meaningful in an `FPNG=0`
build (fpng has no such knob), where dropping stb's default 8 to 1 measured
~28% faster encode for <0.1% larger output on diagram-style content.

`--nearest` selects `resizeNearest` (`STBIR_FILTER_POINT_SAMPLE`) instead of
stb's SIMD linear filter — cheaper, no interpolation, a real quality
tradeoff (see [BENCHMARKS.md](./BENCHMARKS.md) for how much it saves).
Output is always PNG regardless of `<out>`'s extension.

### Examples

```sh
# scale by factor, nearest kernel, timed
fastresize resize in.png out.png 0.25 --nearest -v

# fit within a 500px box (proportional)
fastresize thumbnail in.png thumb.png 500

# same job as vips, side by side
fastresize resize in.png a.png 0.25 --nearest --png-level 1
vips       resize in.png b.png 0.25 --kernel nearest

# composite layers onto a fixed canvas at pixel offsets
fastresize composite page.png --size 1200x800 bg.png fg.png@120,40 -v

# dimensions only, no decode (like vipsheader)
fastresize header in.png
```
