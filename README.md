# fastresize

A small, single-purpose C++ image decode/resize/composite/encode library —
no pipeline, no lazy evaluation graph, no operation-result caching, just the
operations most compositing tasks need, implemented directly on top of
`stb_image` + `stb_image_resize2` + `stb_image_write`.

Extracted from [detail-image-bench](https://github.com/mattsplat/detail-image-bench),
a multi-language benchmark where every libvips-backed candidate measured
slower than this pipeline — the case against libvips (and what libvips is
actually built for instead) is in `fastresize.h`'s own header comment. Now
used from C++ directly (`cpp`, `qk3` in that project), and from Rust, Node,
and PHP via the C ABI shim below.

## `fastresize.h`

Single-header decode / resize / composite / encode library, directly on
`stb_image` + `stb_image_resize2` + `stb_image_write`. No pipeline, no lazy
graph — just the operations this benchmark's compositing task needs.
Rationale and the case against libvips are in the file's header comment; a
point-by-point comparison with `uploadcare/pillow-simd` (and what's worth
borrowing) is in [`pillow-simd-comparison.md`](./pillow-simd-comparison.md).

Every `Image` also carries `Rect opaque` — a **conservative superset** of
where its non-transparent pixels are (it must contain them all, but may be
larger, so "the whole image" is always legal). `decode()` computes it
tightly via `opaqueBounds()`; `resize*()` map it forward; `crop()`
intersects it. `compositeOver()` uses it to skip transparent margins
entirely, which is worth **7.7×** on the golden payload's composite stage
(187.6 → 24.3 ms) since these assets are 10–37% filled. Nothing may assume
pixels *outside* the rect are transparent — it is only ever a licence to
skip work.

## `fastresize_capi.h` / `fastresize_capi.cpp` — the C ABI

A thin opaque-handle shim over `fastresize.h`'s decode/probe/resize/
composite/encode, so a non-C++ language can call the exact same
implementation via FFI instead of reimplementing it. No new algorithm lives
here. Used from:

- **Rust**: compiled by a `build.rs` (the `cc` crate) and linked in directly
  — see `rust-vips/build.rs` and `rust-vips/src/ffi.rs` in detail-image-bench.
- **Node**: compiled to `libfastresize_capi.so` and called via
  [`koffi`](https://koffi.dev) — see `node/src/fastresize.ts`.
- **PHP**: compiled to `libfastresize_capi.so` and called via `ext-ffi` — see
  [`fastresize-php`](https://github.com/mattsplat/fastresize-php), a Composer
  package wrapping this exact shim.

Build the shared library with `make capi` (below); link/load
`libfastresize_capi.so` (or `.dylib` on macOS) from whichever language.

## `fastresize` — the CLI

`fastresize_cli.cpp` is a thin command-line front-end over `fastresize.h`,
so the exact primitives the services use can be run and timed from a shell
the same way `vips` is — for head-to-head measurement without an HTTP server
in the loop.

### Build

```sh
make            # curls the pinned stb headers into ./vendor, builds ./fastresize
make FPNG=1     # ...and the fpng encoder (12x faster PNG encode, ~8% larger)
make capi       # builds libfastresize_capi.so/.dylib (the C ABI, see above)
make clean
```

`encodePng()` has two backends, picked at build time: `stb_image_write` by
default (header-only, nothing to link) and **fpng** under
`-DFASTRESIZE_FPNG` (needs `fpng.cpp` on the link line). On the golden
payload's 8557×4000 canvas that is **817 ms → 68 ms**, pixel-identical
output, ~8% larger file. `services/qk3` ships with it; `services/cpp` is
deliberately left on stb as the control.

`-O3` plus an arch-appropriate `-march`/`-mcpu` (see
`pillow-simd-comparison.md`, Feature 2); no other dependencies. `./vendor`
and `./fastresize` are gitignored.

### Usage

```sh
fastresize resize    <in> <out> <scale>              [--nearest] [-v]
fastresize resize    <in> <out> --width <px> [--height <px>] [--nearest] [-v]
fastresize thumbnail <in> <out> <max-dim>            [--nearest] [-v]
fastresize composite <out> --size <W>x<H> <layer[@x,y]>...   [-v]
fastresize header    <in>
```

Global options: `-v` / `--verbose` prints per-stage timings (decode, resize,
encode, composite) to stderr; `--png-level <0-9>` sets stb's deflate effort
(stb defaults to 8, `qk3` runs 1).

`--nearest` selects `resizeNearest` (`STBIR_FILTER_POINT_SAMPLE`, what the
services actually use); the default is stb's SIMD linear filter. Output is
always PNG — `fastresize.h` decodes anything stb supports but only *encodes*
PNG, which is the one real difference from `vips`.

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
