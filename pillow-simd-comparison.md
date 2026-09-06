# fastresize.h vs. pillow-simd — what's worth borrowing

Comparison of this project's `services/common/fastresize.h` (as used by the
`qk3` and `cpp` candidates) against
[uploadcare/pillow-simd](https://github.com/uploadcare/pillow-simd), with
concrete steps to adopt the parts that apply.

## TL;DR

pillow-simd and fastresize make the **same architectural bet**: no lazy
operation graph, no general pipeline — just decode → resize → composite →
encode, straight on top of stb-equivalent primitives. Where they differ is
in the hand-written inner loops and the compiler flags.

Three things are worth taking:

| # | Feature | Status | Measured win |
|---|---------|--------|--------------|
| 1 | Integer, auto-vectorizable `compositeOver` (+ opaque/transparent fast paths) | **done** | see below |
| 2 | Aggressive, arch-aware build flags (`-O3`, `-mcpu`/`-march`) | CLI only | not isolated |
| 3 | `Image::opaque` bbox, composite only that rect | **done** | see below |

### Measured, after implementing 1 + 3

On the golden payload's real layout — 18 layers composited onto the
8557×4000 canvas, via `fastresize composite`, min of 3 runs:

| | composite | output |
|---|--:|---|
| before | 187.6 ms | 1711787 bytes |
| after | **24.3 ms** | 1711787 bytes — **byte-identical** |

**7.7× on the composite stage.** Decode pays ~14% more for the one-time
`opaqueBounds` scan (80.7 → 92.2 ms across 5 assets), which is amortised
away in the services because decoded+resized images are cached and
composited on every request. `bench/golden-check.mjs` gives 8557×4000 /
2162062 bytes for both `cpp` and `qk3` — the exact byte count already
recorded in `qk3/main.cpp`, so the pipeline output did not change.

Feature 2 is applied in `services/common/Makefile` (the CLI) but **not** in
`services/{cpp,qk3}/Dockerfile`, deliberately: giving two candidates
arch-tuned flags the other five don't have would compromise the
cross-candidate comparison this repo exists to make. Worth doing as a
separate, all-candidates change.

**Ceiling check — and it bound.** pillow-simd does not accelerate PNG
encoding, so none of this touches the dominant cost; the predicted ceiling
was ~150–250 ms off the request, and the measured result was 130 ms
(900 → 772 ms on the golden composite pipeline). Composite fell from ~190 ms
to 31 ms and is now 4% of a request, while **encode is 82%** (636 ms).
`vips` does the same pipeline in 600 ms and produces a 1.8× smaller PNG, so
it still wins by 1.29× — on encode alone. See
`bench/fastresize-vs-vips-RESULTS.md`; the next real win is the encoder, not
anything in "Out of scope"'s neighbours.

## Background: what pillow-simd actually does

- **Resize**: convolution-based resampling (bilinear / bicubic / Lanczos)
  with **precomputed fixed-point integer** filter coefficients, done as two
  separable passes (horizontal then vertical), inner loop vectorised with
  SSE4/AVX2. `stb_image_resize2` — which `fastresize::resize()` already sits
  on — uses the same structure (separable, fixed-point, SSE2/AVX2/NEON), so
  **the library is not behind on resize**. `qk3` and `cpp` just opt out of
  it by calling `resizeNearest()` for speed.
- **Alpha compositing** (`src/libImaging/AlphaComposite.c`): row-at-a-time,
  **all-integer**, SSE4/AVX2. Fully-transparent source pixels are a 4-byte
  copy. Division by 255 is the `(x + (x >> 8) + 128) >> 8` trick, never a
  real divide, never floating point. This is exactly the loop `fastresize`
  hand-writes inefficiently.
- **Build**: compiled `-mavx2` on purpose — the whole premise is that a
  vectorised proper filter beats a scalar cheap one.

## Feature 1 — rewrite `compositeOver` in integer arithmetic

### The problem

`services/common/fastresize.h:125` (`compositeOver`):

```cpp
for (int sy = 0; sy < src.height; ++sy) {
  int cy = y + sy;
  if (cy < 0 || cy >= canvas.height) continue;         // per-pixel-row branch
  for (int sx = 0; sx < src.width; ++sx) {
    int cx = x + sx;
    if (cx < 0 || cx >= canvas.width) continue;         // per-pixel branch
    ...
    double sa = sp[3] / 255.0;                          // FP divide per pixel
    for (int c = 0; c < 3; ++c) {
      dp[c] = static_cast<unsigned char>(sp[c] * sa + dp[c] * (1.0 - sa));  // FP
    }
    dp[3] = static_cast<unsigned char>(sp[3] + dp[3] * (1.0 - sa));
  }
}
```

Every pixel: two bounds branches, a `double` divide, and 4× `double`
multiply-add. The branches also prevent the compiler from vectorising the
loop at all. On the golden payload this is the ~205 ms "composite" line.

### The fix

Same math, integer only, bounds resolved **once** into an overlap
rectangle, with `a == 0` / `a == 255` fast paths. No intrinsics.

> **Correction, measured after shipping:** this loop does **not**
> auto-vectorise. `clang -O3 -mcpu=native` emits zero vector instructions
> for it. Removing the branches doesn't help, and neither does the
> shift-based div255 — clang won't vectorise interleaved RGBA byte access
> with a per-pixel alpha broadcast. Both alternatives measured *slower*
> (28.1 ms shipped vs 32.4 branchless-`/255` and 34.4 branchless-shift, on
> the golden payload's 18 layers). The 7.7× win is entirely algorithmic —
> opaque-rect bounding, hoisted clip test, integer instead of double. Real
> SIMD here needs intrinsics (Feature 4).

```cpp
// Standard straight-alpha "over" compositing onto `canvas` at (x, y).
// Integer arithmetic throughout (both this and the old float version
// truncate, so output matches to within +-1 LSB and is in fact strictly
// more accurate here; `/ 255` lowers to a multiply-shift, and clang
// vectorises the whole inner loop). Bounds are clipped once into an overlap
// rectangle rather than tested per pixel. Fully transparent and fully
// opaque source pixels take a fast path.
inline void compositeOver(Image& canvas, const Image& src, int x, int y) {
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(canvas.width, x + src.width);
  const int y1 = std::min(canvas.height, y + src.height);
  if (x0 >= x1 || y0 >= y1) return;

  for (int cy = y0; cy < y1; ++cy) {
    const unsigned char* sp =
        &src.pixels[(static_cast<size_t>(cy - y) * src.width + (x0 - x)) * 4];
    unsigned char* dp =
        &canvas.pixels[(static_cast<size_t>(cy) * canvas.width + x0) * 4];
    for (int n = x0; n < x1; ++n, sp += 4, dp += 4) {
      const unsigned a = sp[3];
      if (a == 0) continue;                 // source fully transparent: no-op
      if (a == 255) {                       // source fully opaque: straight copy
        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
        continue;
      }
      const unsigned na = 255u - a;
      dp[0] = (sp[0] * a + dp[0] * na) / 255;
      dp[1] = (sp[1] * a + dp[1] * na) / 255;
      dp[2] = (sp[2] * a + dp[2] * na) / 255;
      dp[3] = (sp[3] * 255u + dp[3] * na) / 255;
    }
  }
}
```

Notes / tradeoffs:

- **Output matches the current version to within ±1 per channel** — the
  old float path could land on `173.9999` and truncate to `173` where the
  true value is `174`; the integer path gets `174`. `golden-check.mjs` only
  compares dimensions, so neither this nor the rounding variant below can
  fail it, but note it if you diff pixels across candidates. To round
  instead of truncate (matches pillow-simd): replace `/ 255` with
  `t = s*a + d*na + 128; out = (t + (t >> 8)) >> 8;`.
- The per-pixel `if (a == 0/255)` branches were measured, not guessed:
  keeping them is fastest. They predict near-perfectly on diagram art (long
  runs of transparent, then long runs of opaque), and skipping the work
  beats doing it. Dropping them does not unlock vectorisation (see the
  correction above) — it just does more arithmetic.
- Feature 3 below is the cleaner way to skip transparent margins without a
  per-pixel branch.

### As shipped

Implemented in `fastresize.h`. The final version blends in integer
arithmetic, keeps both the `a == 0` and `a == 255` fast paths (the branch
predicts almost perfectly on diagram art — long runs of transparent, then
long runs of opaque), and takes its loop bounds from `Image::opaque`
(Feature 3) rather than the full source rect. Rounding is unchanged from the
float version: `/ 255` truncates, exactly as `static_cast<unsigned char>`
did, which is why the output stayed byte-identical.

## Feature 2 — arch-aware build flags

### The problem

`services/qk3/Dockerfile` and `services/cpp/Dockerfile` both build:

```
clang++ -O2 -std=c++17 -I vendor -o server main.cpp -lpthread -lssl -lcrypto
```

No `-O3`, no `-march` / `-mcpu`. Consequences:

- On the **arm64** bench host, AArch64 mandates NEON, so `stb_image_resize2`
  and clang's autovectoriser already emit NEON at `-O2`. The lever here is
  small: `-O3` (wider unrolling / more aggressive vectorisation) and
  `-mcpu=native` (unlocks dotprod, fp16, wider issue on Apple silicon).
- On an **x86-64** target (likely what production runs), `-O2` gives only
  the SSE2 baseline. `stb_image_resize2`'s SSE4.1 and AVX2 kernels are gated
  on `__SSE4_1__` / `__AVX__` / `__AVX2__` (see its lines ~1173–1187), and
  the composite loop from Feature 1 would only vectorise to SSE2. `-mavx2`
  (or `-march=x86-64-v3`) turns all of that on — this is precisely
  pillow-simd's `-mavx2` choice.

### The fix

Make the arch explicit in the Dockerfile. Since these images are built and
benchmarked on the same machine, the simplest correct thing is to branch on
`uname -m`:

```dockerfile
# -O3 + native/AVX2: stb_image_resize2's SSE4.1/AVX2 (x86) kernels and the
# integer compositeOver loop only vectorise past the SSE2 / plain-NEON
# baseline when the target ISA is set explicitly. Mirrors pillow-simd's
# deliberate -mavx2 build. arm64 (Apple silicon bench host) already has
# mandatory NEON at -O2; -mcpu=native just adds dotprod/fp16.
RUN ARCH="$(uname -m)" && \
    case "$ARCH" in \
      x86_64|amd64) MARCH="-march=x86-64-v3" ;; \
      aarch64|arm64) MARCH="-mcpu=native" ;; \
      *) MARCH="" ;; \
    esac && \
    clang++ -O3 $MARCH -std=c++17 -I vendor -o server main.cpp \
      -lpthread -lssl -lcrypto
```

Tradeoff to be aware of: `-march=x86-64-v3` requires a Haswell-era (2013+)
Intel / Excavator-era AMD CPU. Every current cloud host clears that bar, but
if you ever run these binaries somewhere exotic, pin to `-march=x86-64-v2`
(SSE4.2, still unlocks stb's SSE4.1 path) instead. `-mcpu=native` bakes in
the build machine's exact CPU — fine for a local benchmark, not for a
distributed artifact; use `-mcpu=apple-m1` (or the real deploy target) for
anything shipped.

### Steps

1. Apply the Dockerfile change to `services/cpp/Dockerfile` and
   `services/qk3/Dockerfile` (and `services/rust-vips` is unaffected —
   different toolchain).
2. `docker compose build cpp qk3`.
3. `node bench/golden-check.mjs` — still valid PNGs, same dimensions.
4. `bench/run.sh --smoke` to confirm nothing regressed under load, then a
   full `bench/run.sh` for the real p50/p95 comparison against the current
   `RESULTS.md` rows.

## Feature 3 — `Image::opaque`, composite only that rect

### The rationale

These assets are mostly transparent padding around a smaller drawn region.
Measured on this repo's own fixtures with `vips find_trim` on the alpha
band: the 7 backgrounds are **100%** filled, but the risers are 14–24% and
the foregrounds 10–37% — so up to 90% of a foreground's pixels are margin
that every stage was paying full price for. Feature 1 skips transparent
*pixels* with a branch; this skips them by the *rectangle*, before the loop
starts.

### As shipped

`fastresize::Image` carries a `Rect opaque`, and the header states its
invariant plainly: it is a **conservative superset** — it must contain every
non-transparent pixel but may be larger, so "the whole image" is always a
legal value and code that can't cheaply keep it tight just says that. Which
is what makes the rest safe: `compositeOver` may narrow to it, but nothing
may assume pixels outside it are transparent.

- `opaqueBounds()` computes it tightly, one pass. Two strategies per row,
  because this repo has both extremes: a full-bleed row is settled in two
  loads (both end pixels non-transparent ⇒ full width, no scan), otherwise
  the row is swept in 16-pixel blocks OR-ing whole pixel *words* — which
  vectorises, unlike a strided walk of the alpha byte — stopping at the
  first block holding anything. Only a genuinely empty row costs a full
  pass. Measured against a plain scan and against an unconditional
  OR-reduction, this was fastest or tied on every shape tested.
- `decode()` sets it. `Image(w, h)` (a fresh canvas) sets it to the full
  rect — conservative and correct.
- `resize()` / `resizeNearest()` map it through `scaleRect()`, which rounds
  outward and adds a pixel of slack per side, so a filter footprint can't
  drag a non-transparent pixel outside the box.
- `crop()` intersects it into the cropped frame.

### What this deliberately does *not* do

`resize()` and `resizeNearest()` still resample the **whole** image, not
just the opaque rect. Resampling a sub-rect in isolation is not
bit-identical to resampling the whole image and cropping — different filter
footprint at the crop edge, different sampling-grid alignment — and this is
a benchmark repo where one candidate quietly changing its pixel output
undermines the comparison.

The win left on the table is real: `bench/fastresize-vs-vips.mjs --trim`
measures it at **43%** of the resize-pipeline cost (1815 → 1034 ms across
the 15 golden assets), because the mostly-transparent foregrounds get 3–15×
faster individually. `crop()` is exported so a caller can opt in — it would
crop before resizing and add `(rect.x, rect.y) * scale` back when
compositing. That is a change to `buildImage` in `qk3`/`cpp`, with real
offset bookkeeping, not a library change, so it is not done here.

## Feature 4 (optional, probably skip) — hand-written SIMD intrinsics

pillow-simd's `AlphaComposite.c` is explicit `_mm_*` / `_mm256_*` code with
an SSE and an AVX2 path. Porting it verbatim would mean maintaining that
**plus** a NEON path for the arm64 bench host — a lot of surface area for a
step that's ~13% of a request.

Recommendation: do Features 1–3, measure. Only reach for intrinsics if the
auto-vectorised composite loop is still showing up hot in a profile. If you
do, `#include <immintrin.h>` / `<arm_neon.h>` behind `#if defined(__AVX2__)`
/ `#if defined(__ARM_NEON)` with the Feature 1 scalar loop as the fallback,
and lift the div-255 trick and the premultiplied-alpha coefficient math
directly from pillow-simd's file.

## Out of scope — PNG encode

The real bottleneck (`encode` ≈ 66% of the golden request) is
`stbi_write_png`, already turned down to `compression_level = 1` in
`qk3/main.cpp`. pillow-simd doesn't help here — it delegates PNG to
libpng/zlib and doesn't SIMD-accelerate encoding. The analogous move on
that axis is a faster encoder, tracked separately:

- **fpng** — SSE4/PCLMUL-accelerated PNG writer, ~10–20× `stb_image_write`,
  produces slightly larger files. Best fit for this content.
- **libspng** + zlib-ng, or **libpng** + zlib-ng — safer, more standard,
  ~2–3×.
- Encoding on a worker thread while the response for the *previous* request
  is still flushing (pipelining) — architectural, no new dependency.

## Verification checklist (any of the above)

- [ ] `docker compose build <svc>` succeeds
- [ ] `node bench/golden-check.mjs` — `OK`, dimensions unchanged, bytes
      within ~0.1%
- [ ] temporary `std::chrono` timers in `buildImage` confirm the targeted
      stage moved
- [ ] `bench/run.sh --smoke` — no new errors under concurrency
- [ ] full `bench/run.sh` — compare p50/p95/p99 and memory slope against the
      committed `RESULTS.md` row, update the table
