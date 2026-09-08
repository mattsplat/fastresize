#include "fastresize_capi.h"
#include "fastresize.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

struct FRImage {
  fastresize::Image img;
};

namespace {
// thread_local, not a single global: two requests can each be mid-pipeline
// on their own blocking thread at once (see MAX_CONCURRENT_REQUESTS in
// main.rs), and a shared buffer would let one thread's error clobber
// another's before it's read.
thread_local std::string g_last_error;

void setError(const std::exception& e) { g_last_error = e.what(); }

unsigned char clampByte(int v) { return static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v)); }
}  // namespace

extern "C" {

FRImage* fr_decode(const uint8_t* data, size_t len) {
  try {
    return new FRImage{fastresize::decode(data, len)};
  } catch (const std::exception& e) {
    setError(e);
    return nullptr;
  }
}

int fr_probe_dimensions(const uint8_t* data, size_t len, int* out_width, int* out_height) {
  try {
    fastresize::Dimensions d = fastresize::probeDimensions(data, len);
    *out_width = d.width;
    *out_height = d.height;
    return 0;
  } catch (const std::exception& e) {
    setError(e);
    return -1;
  }
}

FRImage* fr_resize_nearest(const FRImage* src, int target_width, int target_height) {
  try {
    return new FRImage{fastresize::resizeNearest(src->img, target_width, target_height)};
  } catch (const std::exception& e) {
    setError(e);
    return nullptr;
  }
}

FRImage* fr_new_canvas(int width, int height) {
  if (width <= 0 || height <= 0) {
    g_last_error = "fr_new_canvas: non-positive dimensions";
    return nullptr;
  }
  try {
    return new FRImage{fastresize::Image(width, height)};
  } catch (const std::exception& e) {
    setError(e);
    return nullptr;
  }
}

FRImage* fr_new_canvas_rgba(int width, int height, int r, int g, int b, int a) {
  if (width <= 0 || height <= 0) {
    g_last_error = "fr_new_canvas_rgba: non-positive dimensions";
    return nullptr;
  }
  try {
    auto* handle = new FRImage{fastresize::Image(width, height)};
    fastresize::fill(handle->img, clampByte(r), clampByte(g), clampByte(b), clampByte(a));
    return handle;
  } catch (const std::exception& e) {
    setError(e);
    return nullptr;
  }
}

void fr_fill(FRImage* img, int r, int g, int b, int a) {
  fastresize::fill(img->img, clampByte(r), clampByte(g), clampByte(b), clampByte(a));
}

void fr_composite_over(FRImage* canvas, const FRImage* src, int x, int y) {
  fastresize::compositeOver(canvas->img, src->img, x, y);
}

int fr_width(const FRImage* img) { return img->img.width; }
int fr_height(const FRImage* img) { return img->img.height; }

int fr_opaque_rect(const FRImage* img, int* x, int* y, int* w, int* h) {
  if (!img) {
    g_last_error = "fr_opaque_rect: null image";
    return -1;
  }
  const fastresize::Rect& r = img->img.opaque;
  *x = r.x;
  *y = r.y;
  *w = r.w;
  *h = r.h;
  return 0;
}

FRImage* fr_crop(const FRImage* src, int x, int y, int w, int h) {
  try {
    return new FRImage{fastresize::crop(src->img, fastresize::Rect{x, y, w, h})};
  } catch (const std::exception& e) {
    setError(e);
    return nullptr;
  }
}

int fr_encode_png(const FRImage* img, uint8_t** out_data, size_t* out_len) {
  try {
    std::string png = fastresize::encodePng(img->img);
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(png.size()));
    if (!buf) {
      g_last_error = "fr_encode_png: out of memory";
      return -1;
    }
    std::memcpy(buf, png.data(), png.size());
    *out_data = buf;
    *out_len = png.size();
    return 0;
  } catch (const std::exception& e) {
    setError(e);
    return -1;
  }
}

int fr_encode_png_rgb(const FRImage* img, int r, int g, int b, uint8_t** out_data,
                      size_t* out_len) {
  try {
    std::string png = fastresize::encodePngRgb(img->img, clampByte(r), clampByte(g), clampByte(b));
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(png.size()));
    if (!buf) {
      g_last_error = "fr_encode_png_rgb: out of memory";
      return -1;
    }
    std::memcpy(buf, png.data(), png.size());
    *out_data = buf;
    *out_len = png.size();
    return 0;
  } catch (const std::exception& e) {
    setError(e);
    return -1;
  }
}

void fr_free_buffer(uint8_t* data) { std::free(data); }
void fr_image_free(FRImage* img) { delete img; }

const char* fr_last_error(void) { return g_last_error.c_str(); }

}  // extern "C"
