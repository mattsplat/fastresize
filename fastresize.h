// fastresize.h - a small, reusable, single-purpose image decode/resize/
// composite/encode library. No general processing pipeline, no lazy
// evaluation graph, no operation-result caching of its own - just the
// operations this project's compositing task actually needs, implemented
// directly on top of stb_image / stb_image_resize2 / stb_image_write.
//
// Every Image also carries an `opaque` rect - a conservative bound on where
// its non-transparent pixels are - because these assets are mostly
// transparent margin, and compositeOver uses it to skip that margin. See
// pillow-simd-comparison.md for where that idea and the integer compositing
// loop came from, and what is deliberately left on the table.
//
// Why this exists: every libvips-backed candidate in this benchmark (php,
// node/sharp, cpp-vips, rust-vips) measured slower than every candidate
// using a plain decode->resize->composite->encode pipeline (kotlin/Java2D,
// go/x-image, this project's own stb-based `cpp` candidate) - see
// RESULTS.md. The likely reason: libvips is built for a different problem
// (large-scale/streaming pipelines with many chained operations) than this
// one (a modest number of moderate images, composited once, output fully
// materialized as a PNG regardless) - its lazy-graph machinery is overhead
// here, not a win. github.com/tranhuucanh/fast_resize independently makes
// the same architectural bet against libvips (SIMD stb_image_resize2, no
// general pipeline) and claims 1.7-2.9x over it; this header extracts the
// exact algorithm already proven correct (against the golden payload check)
// in services/cpp/main.cpp into a form other C/C++ candidates can reuse
// without copy-pasting it.
//
// Self-contained: this header defines STB_IMAGE_IMPLEMENTATION and friends
// itself, so a consumer just includes it - no macro dance required. Include
// it from exactly one translation unit per binary (the usual stb rule).

#pragma once

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

// Optional faster PNG encoder - see encodePng() at the bottom of this file.
// Not header-only, so the consumer must also compile/link fpng.cpp.
#ifdef FASTRESIZE_FPNG
#include "fpng.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastresize {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

// Always RGBA8, straight (non-premultiplied) alpha - matches every other
// candidate in this project.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<unsigned char> pixels;  // width * height * 4 bytes

  // Sub-rect that is guaranteed to contain every pixel with alpha != 0.
  // These assets are overwhelmingly a small drawing on a large transparent
  // canvas (measured on this project's own fixtures: the backgrounds are
  // 100% filled, but the foregrounds and risers are only 10-37% - i.e. up
  // to 90% of their pixels are transparent margin), so knowing where the
  // drawing actually is lets compositeOver skip the margin entirely.
  //
  // Invariant: CONSERVATIVE SUPERSET. It must contain every non-transparent
  // pixel, but is allowed to be larger - so "the whole image" is always a
  // legal value, and code that can't cheaply keep it tight just says that.
  // Never rely on pixels outside it being transparent for anything but a
  // skip-this-work decision.
  Rect opaque;

  Image() = default;
  Image(int w, int h)
      : width(w),
        height(h),
        pixels(static_cast<size_t>(w) * h * 4, 0),
        opaque{0, 0, w, h} {}
};

// The alpha byte's position inside a 4-byte RGBA pixel word, as a mask -
// derived rather than hardcoded so it holds on either endianness. Folds to
// a constant.
inline std::uint32_t alphaWordMask() {
  const unsigned char px[4] = {0, 0, 0, 0xFF};
  std::uint32_t m;
  std::memcpy(&m, px, 4);
  return m;
}

// Tight bounding box of pixels with alpha != 0, or an empty rect if the
// image is fully transparent.
//
// This project sees both extremes - full-bleed backgrounds and drawings
// floating in mostly-empty canvases - so a row is handled two ways. If both
// end pixels are non-transparent the row spans the full width and is
// settled in two loads, no scan. Otherwise the row is swept in 16-pixel
// blocks, OR-ing whole pixel words (contiguous, so it vectorises, unlike a
// strided walk of the alpha byte alone) and stopping at the first block
// that holds anything - so finding content is proportional to how far in it
// starts, and only a genuinely empty row costs a full pass.
inline Rect opaqueBounds(const Image& img) {
  if (img.width <= 0 || img.height <= 0) return Rect{};
  const std::uint32_t aMask = alphaWordMask();
  const int w = img.width;
  int minX = w, minY = img.height, maxX = -1, maxY = -1;

  for (int y = 0; y < img.height; ++y) {
    const unsigned char* row = &img.pixels[static_cast<size_t>(y) * w * 4];
    int first, last;

    if (row[3] != 0 && row[static_cast<size_t>(w - 1) * 4 + 3] != 0) {
      first = 0;
      last = w - 1;  // full-bleed row
    } else {
      first = -1;
      for (int x = 0; x < w; x += 16) {
        const int n = std::min(16, w - x);
        std::uint32_t acc = 0;
        for (int i = 0; i < n; ++i) {
          std::uint32_t px;
          std::memcpy(&px, row + static_cast<size_t>(x + i) * 4, 4);
          acc |= px;
        }
        if (acc & aMask) {
          first = x;
          while (row[static_cast<size_t>(first) * 4 + 3] == 0) ++first;
          break;
        }
      }
      if (first < 0) continue;  // fully transparent row
      last = w - 1;
      while (row[static_cast<size_t>(last) * 4 + 3] == 0) --last;
    }

    if (y < minY) minY = y;
    maxY = y;
    if (first < minX) minX = first;
    if (last > maxX) maxX = last;
  }

  if (maxX < 0) return Rect{};
  return Rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
}

// Maps a rect from one image size to another, rounding OUTWARD and adding a
// pixel of slack on every side - deliberately loose, to keep Image::opaque's
// superset invariant true after a resample whose filter footprint can drag
// a non-transparent pixel a fraction outside the naive scaled box.
inline Rect scaleRect(const Rect& r, int srcW, int srcH, int dstW, int dstH) {
  if (r.empty() || srcW <= 0 || srcH <= 0) return Rect{};
  const double sx = static_cast<double>(dstW) / srcW;
  const double sy = static_cast<double>(dstH) / srcH;
  int x0 = std::max(0, static_cast<int>(std::floor(r.x * sx)) - 1);
  int y0 = std::max(0, static_cast<int>(std::floor(r.y * sy)) - 1);
  int x1 = std::min(dstW, static_cast<int>(std::ceil((r.x + r.w) * sx)) + 1);
  int y1 = std::min(dstH, static_cast<int>(std::ceil((r.y + r.h) * sy)) + 1);
  if (x0 >= x1 || y0 >= y1) return Rect{};
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

// Decodes any stb-supported format (PNG, JPEG, BMP, ...) from an in-memory
// buffer into RGBA8.
inline Image decode(const unsigned char* data, size_t len) {
  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &channels, 4);
  if (!pixels) {
    throw std::runtime_error(std::string("fastresize: decode failed: ") + stbi_failure_reason());
  }
  Image img;
  img.width = w;
  img.height = h;
  img.pixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
  stbi_image_free(pixels);
  img.opaque = opaqueBounds(img);
  return img;
}

inline Image decode(const std::string& bytes) {
  return decode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

// Reads just width/height from an in-memory buffer's header, without
// decoding any pixel data - much cheaper than decode() when only dimensions
// are needed (e.g. to compute a scale factor before deciding whether a full
// decode+resize is even necessary at all).
struct Dimensions {
  int width = 0;
  int height = 0;
};

inline Dimensions probeDimensions(const unsigned char* data, size_t len) {
  Dimensions dims;
  int channels = 0;
  if (!stbi_info_from_memory(data, static_cast<int>(len), &dims.width, &dims.height, &channels)) {
    throw std::runtime_error(std::string("fastresize: probe failed: ") + stbi_failure_reason());
  }
  return dims;
}

inline Dimensions probeDimensions(const std::string& bytes) {
  return probeDimensions(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

// Resizes to an exact target size - callers compute their own target
// dimensions (same convention every candidate in this project already
// follows: a global scale factor derived from the reference image, applied
// to each image's own native size). Linear filtering, stb_image_resize2's
// straightforward SIMD-accelerated default - see NearestNeighbor note in
// resizeNearest below if a caller wants the cheaper kernel instead.
inline Image resize(const Image& src, int targetWidth, int targetHeight) {
  Image out(std::max(1, targetWidth), std::max(1, targetHeight));
  stbir_resize_uint8_linear(src.pixels.data(), src.width, src.height, 0, out.pixels.data(),
                             out.width, out.height, 0, STBIR_RGBA);
  out.opaque = scaleRect(src.opaque, src.width, src.height, out.width, out.height);
  return out;
}

// Same as resize(), but with nearest-neighbor sampling (STBIR_FILTER_POINT_
// SAMPLE) - the cheapest possible kernel, no interpolation. Matches the
// choice already made for the go and cpp candidates after measuring it
// meaningfully faster with acceptable visual quality for this project's
// assets. Uses the "medium" stbir_resize() API (not the uint8_linear/srgb
// convenience wrappers) since only it exposes an explicit filter choice.
inline Image resizeNearest(const Image& src, int targetWidth, int targetHeight) {
  Image out(std::max(1, targetWidth), std::max(1, targetHeight));
  stbir_resize(src.pixels.data(), src.width, src.height, 0, out.pixels.data(), out.width,
               out.height, 0, STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP,
               STBIR_FILTER_POINT_SAMPLE);
  out.opaque = scaleRect(src.opaque, src.width, src.height, out.width, out.height);
  return out;
}

// Copies out a sub-rectangle (clamped to the source). Offered so a caller
// that wants the *resize* to skip transparent margins too can crop first -
// resize() and resizeNearest() deliberately don't do that themselves,
// because resampling a sub-rect in isolation is not bit-identical to
// resampling the whole image and then cropping (different filter footprint
// at the crop edge, different sampling-grid alignment). Cropping here is
// exact; the caller then owns adding (rect.x, rect.y) back when it
// composites.
inline Image crop(const Image& src, Rect r) {
  const int x0 = std::max(0, r.x);
  const int y0 = std::max(0, r.y);
  const int x1 = std::min(src.width, r.x + r.w);
  const int y1 = std::min(src.height, r.y + r.h);
  if (x0 >= x1 || y0 >= y1) return Image();

  Image out(x1 - x0, y1 - y0);
  for (int y = y0; y < y1; ++y) {
    const unsigned char* sp = &src.pixels[(static_cast<size_t>(y) * src.width + x0) * 4];
    unsigned char* dp = &out.pixels[static_cast<size_t>(y - y0) * out.width * 4];
    std::copy(sp, sp + static_cast<size_t>(out.width) * 4, dp);
  }
  // Intersect the source's opaque rect with the crop, in output coordinates.
  const int ox0 = std::max(x0, src.opaque.x);
  const int oy0 = std::max(y0, src.opaque.y);
  const int ox1 = std::min(x1, src.opaque.x + src.opaque.w);
  const int oy1 = std::min(y1, src.opaque.y + src.opaque.h);
  out.opaque = (ox0 >= ox1 || oy0 >= oy1) ? Rect{}
                                          : Rect{ox0 - x0, oy0 - y0, ox1 - ox0, oy1 - oy0};
  return out;
}

// Standard "over" alpha compositing onto `canvas` at (x, y), straight
// (non-premultiplied) alpha. Out-of-bounds pixels are clipped.
//
// Integer arithmetic throughout - the same blend the old floating-point
// version computed, `(s*a + d*(255-a)) / 255`, but without a per-pixel
// double divide, and with the clip test hoisted out of the loop into an
// overlap rectangle computed once. Borrowed from pillow-simd's
// AlphaComposite.c (see pillow-simd-comparison.md), minus that file's
// hand-written SSE4/AVX2 intrinsics, which would need a NEON twin here.
//
// This loop does NOT auto-vectorise, and that was checked, not assumed:
// clang -O3 -mcpu=native emits zero vector instructions for it. Removing
// the branches below doesn't help, nor does swapping `/ 255` for the
// shift-based div255 - clang won't vectorise the interleaved RGBA byte
// access with a per-pixel alpha broadcast either way, and both variants
// measured SLOWER than this one (28.1 ms shipped vs 32.4 and 34.4 on the
// golden payload's 18 layers). Skipping transparent and opaque pixels beats
// doing arithmetic on them. SIMD here needs real intrinsics or nothing.
//
// Two shortcuts on top: the loop is bounded by src.opaque, so a drawing on
// a large transparent canvas costs only its drawing, and fully transparent
// or fully opaque pixels skip the blend entirely.
inline void compositeOver(Image& canvas, const Image& src, int x, int y) {
  // Clamp the source's opaque rect to the source, then to the canvas. Only
  // this overlap can change any pixel.
  const int sx0 = std::max(0, src.opaque.x);
  const int sy0 = std::max(0, src.opaque.y);
  const int sx1 = std::min(src.width, src.opaque.x + src.opaque.w);
  const int sy1 = std::min(src.height, src.opaque.y + src.opaque.h);
  if (sx0 >= sx1 || sy0 >= sy1) return;

  const int x0 = std::max(0, x + sx0);
  const int y0 = std::max(0, y + sy0);
  const int x1 = std::min(canvas.width, x + sx1);
  const int y1 = std::min(canvas.height, y + sy1);
  if (x0 >= x1 || y0 >= y1) return;

  for (int cy = y0; cy < y1; ++cy) {
    const unsigned char* sp =
        &src.pixels[(static_cast<size_t>(cy - y) * src.width + (x0 - x)) * 4];
    unsigned char* dp = &canvas.pixels[(static_cast<size_t>(cy) * canvas.width + x0) * 4];
    for (int cx = x0; cx < x1; ++cx, sp += 4, dp += 4) {
      const unsigned a = sp[3];
      if (a == 0) continue;  // transparent source: canvas unchanged
      if (a == 255) {        // opaque source: straight copy
        dp[0] = sp[0];
        dp[1] = sp[1];
        dp[2] = sp[2];
        dp[3] = 255;
        continue;
      }
      const unsigned na = 255u - a;
      dp[0] = static_cast<unsigned char>((sp[0] * a + dp[0] * na) / 255u);
      dp[1] = static_cast<unsigned char>((sp[1] * a + dp[1] * na) / 255u);
      dp[2] = static_cast<unsigned char>((sp[2] * a + dp[2] * na) / 255u);
      dp[3] = static_cast<unsigned char>((a * 255u + dp[3] * na) / 255u);
    }
  }
}

// Encodes to PNG bytes.
//
// Two encoders, chosen at build time. stb_image_write is the default and
// needs nothing but this header. Define FASTRESIZE_FPNG (and add fpng.cpp
// to the link) to use fpng instead: measured on the golden payload's
// 8557x4000 canvas, 817ms -> 68ms, a 12x cut on what is otherwise ~82% of
// a request - and that was fpng's *scalar* fallback, since it has no NEON
// path; on x86 with SSE4.1+PCLMUL it does better still. Output is
// byte-for-byte a valid PNG and pixel-identical (verified: mean and max
// absolute difference both 0.000 against the stb encoding), the cost being
// a ~8% larger file (2.33MB vs 2.16MB here) because fpng trades ratio for
// speed. If response size matters more than latency, libspng+zlib-ng is
// the other direction: ~2.4x faster than stb and ~45% smaller.
#ifdef FASTRESIZE_FPNG
inline std::string encodePng(const Image& img) {
  static const bool initialized = [] {
    fpng::fpng_init();
    return true;
  }();
  (void)initialized;
  std::vector<unsigned char> out;
  if (!fpng::fpng_encode_image_to_memory(img.pixels.data(), img.width, img.height, 4, out,
                                         fpng::FPNG_ENCODE_SLOWER)) {
    throw std::runtime_error("fastresize: png encode failed (fpng)");
  }
  return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}
#else
inline std::string encodePng(const Image& img) {
  int outLen = 0;
  unsigned char* data =
      stbi_write_png_to_mem(img.pixels.data(), img.width * 4, img.width, img.height, 4, &outLen);
  if (!data) {
    throw std::runtime_error("fastresize: png encode failed");
  }
  std::string result(reinterpret_cast<char*>(data), outLen);
  STBIW_FREE(data);
  return result;
}
#endif

}  // namespace fastresize
