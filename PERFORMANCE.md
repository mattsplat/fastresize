# Why fastresize is faster than libvips (for this workload)

[BENCHMARKS.md](./BENCHMARKS.md) has the numbers. This is the *why*: the one
architectural bet, and the handful of concrete techniques that follow from
it. None of it is exotic — the point is that for a narrow workload you can
skip most of what a general image library spends time on.

## The bet

libvips is a streaming pipeline engine. An operation like `resize` doesn't
compute pixels; it appends a node to a lazy graph, and nothing runs until
you ask to write the result. Then libvips walks the graph region by region,
in parallel, keeping only small tiles of each intermediate in memory. For
its intended workload — long operation chains over images too big to hold
in RAM, on a server doing many of them at once — that design is excellent
and hard to beat.

This library is for the opposite shape:

- a **modest number of images** (one to a few dozen),
- of **modest size** (megapixels, not gigapixels — they fit in RAM many
  times over),
- **composited once** and **written out as one PNG**, every time, no matter
  how many layers went in.

For that shape, the lazy graph is pure overhead. You pay to build it, to
partition it into regions, to synchronise threads across it — and then you
materialize the whole thing at full resolution anyway, because the output
is a single flat PNG. So fastresize doesn't build one. Every operation is
an ordinary function that takes an `Image` (a `std::vector<unsigned char>`
of RGBA8) and returns a new one, computed immediately, start to finish.

The cost of that choice is real and we don't hide it: for a *short* chain
(decode two layers, blend, save) libvips' fusion — never materializing the
full-resolution intermediate — wins, and [BENCHMARKS.md](./BENCHMARKS.md)
shows it winning composite by ~2×. The bet only pays once the chain is long
enough that everything gets materialized at the end regardless, which is
the workload this was extracted from.

## What that buys, concretely

### 1. No per-operation bookkeeping

`resize(img, w, h)` is a call to one SIMD stb routine and a `vector`
allocation. There is no graph node, no region logic, no thread pool spun up
for a single call. libvips' fixed per-call overhead is why its lead shrinks
as images grow and why fastresize is **6.9× faster on a small image but
2.4× on a large one** (BENCHMARKS.md) — the overhead is roughly constant, so
it dominates more when the actual pixel work is small.

### 2. A resize kernel that does exactly as much work as asked

Resizing goes straight through [`stb_image_resize2`](https://github.com/nothings/stb),
which is SIMD-accelerated (SSE2/AVX2, NEON). Two knobs matter:

- **`resizeNearest()`** uses `STBIR_FILTER_POINT_SAMPLE` — no interpolation
  at all. `vips resize` defaults to lanczos3, a 6-tap separable convolution.
  For downscaling line art and UI assets the quality difference is
  invisible and the cost difference is not: the nearest rows in BENCHMARKS.md
  are 2.4–6.9× faster; part of that gap is fastresize simply not doing the
  convolution vips chose to do.
- **`resize()`** (linear) is there when you want interpolation, still a
  fraction of lanczos3's work.

The caller computes its own target dimensions, so there is no "figure out
the scale factor" pass either.

### 3. Integer alpha compositing, with the arithmetic skipped where it can't matter

`compositeOver()` is the standard "over" blend, `(s·a + d·(255−a)) / 255`,
done in integers — no per-pixel floating-point divide. It's adapted from
[pillow-simd](https://github.com/uploadcare/pillow-simd)'s `AlphaComposite.c`,
minus that file's hand-written SSE/AVX intrinsics.

Two branches carry most of the speedup:

- `a == 0` (transparent source pixel) → skip, canvas untouched.
- `a == 255` (opaque source pixel) → straight copy, no blend math.

Only genuinely translucent pixels run the full expression. For the assets
this targets — solid fills and hard edges, little anti-aliasing — the large
majority of pixels take one of the two fast paths. (This loop deliberately
is *not* auto-vectorised; that was measured. clang can't vectorise the
interleaved-RGBA-byte access with a per-pixel alpha broadcast, and both a
branchless variant and a shift-based `div255` measured *slower* than this
on a real 18-layer composite. SIMD here would need real intrinsics.)

### 4. The opaque rect — never touch transparent margin

Every `Image` carries `Rect opaque`: a **conservative superset** of where
its non-transparent pixels are. `decode()` computes it tightly (a
vectorised alpha scan, `opaqueBounds()`); `resize*()` scale it forward;
`crop()` intersects it.

`compositeOver()` clips its loop to `src.opaque ∩ canvas` before doing any
work. The assets this library was built for are 63–90% transparent margin —
a small drawing floating on a large canvas — so this shortcut alone is
worth **7.7×** on the real composite workload it was measured against
(187.6 → 24.3 ms). libvips has no equivalent: it blends the declared
bounding box.

`crop(img, img.opaque)` extends the same idea to resize and caching — trim
the asset to its drawing first, and every later stage is proportionally
cheaper. `fr_opaque_rect()` / `fr_crop()` expose it over the C ABI.

### 5. Build for the machine

`-O3` plus `-march=x86-64-v3` / `-mcpu=native`. stb's resize kernels and
the composite loop only vectorise past the SSE2 / baseline-NEON floor when
the target ISA is explicit — the default `-O2` on a generic target leaves
most of the SIMD on the table. This is a Makefile flag, not code, but it's
a measurable part of the gap.

### 6. Optional: a PNG encoder built for speed

Encoding is often the single most expensive stage — for a large output
canvas it can dominate the request. Build with `FPNG=1` and `encodePng()` uses
[fpng](https://github.com/richgel999/fpng) instead of `stb_image_write`:
**~12× faster** (817 → 68 ms on an 8557×4000 canvas), pixel-identical
output, ~8% larger file. `encodePngRgb()` goes further for consumers whose
downstream decoder has a slow RGBA path — it flattens onto a background and
writes 3 channels, so the alpha plane never gets encoded or re-parsed
downstream.

## What we deliberately don't do

- **No operation graph / lazy evaluation.** Covered above — it's the whole
  bet.
- **No tiling or out-of-core.** Images are assumed to fit in RAM with room
  to spare. If yours don't, use libvips.
- **No format zoo.** Decodes anything stb supports; only ever writes PNG.
- **No colour management, no ICC, no premultiplied-alpha mode.** RGBA8,
  straight alpha, sRGB-assumed.
- **No hand-written SIMD in this repo.** The SIMD that matters lives in stb
  and fpng. The composite loop is scalar on purpose (§3).

If you need any of those, this is the wrong library and that's by design —
the speed is a direct consequence of the narrow scope.
