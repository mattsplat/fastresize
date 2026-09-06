// fastresize_capi.h - a plain C ABI wrapper around fastresize.h, so a
// non-C++ language can link against the same decode/resize/composite/
// encode implementation instead of reimplementing it or pulling in a
// second image library. No new algorithm lives in fastresize_capi.cpp -
// it's a thin opaque-handle shim over fastresize::Image and friends.
// Existing bindings on top of this: Rust (a hand-written extern "C" block),
// Node (koffi), and PHP (ext-ffi - see
// https://github.com/mattsplat/fastresize-php).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FRImage FRImage;

// Decodes image bytes (PNG/JPEG/...) into RGBA8. NULL on failure - see
// fr_last_error().
FRImage* fr_decode(const uint8_t* data, size_t len);

// Header-only dimension probe, no pixel decode. Returns 0 on success, -1 on
// failure (fr_last_error() has details).
int fr_probe_dimensions(const uint8_t* data, size_t len, int* out_width, int* out_height);

// Nearest-neighbor resize to an exact target size. Does not consume `src`.
// NULL on failure.
FRImage* fr_resize_nearest(const FRImage* src, int target_width, int target_height);

// A new, fully transparent width x height canvas. NULL on failure (e.g.
// non-positive dimensions).
FRImage* fr_new_canvas(int width, int height);

// Alpha-composites `src` onto `canvas` at (x, y) in place, using
// fastresize::compositeOver (opaque-rect-bounded, straight-alpha "over").
void fr_composite_over(FRImage* canvas, const FRImage* src, int x, int y);

int fr_width(const FRImage* img);
int fr_height(const FRImage* img);

// Encodes to PNG. On success returns 0 and sets *out_data/*out_len (caller
// must free the buffer with fr_free_buffer); on failure returns -1 and
// leaves *out_data/*out_len untouched.
int fr_encode_png(const FRImage* img, uint8_t** out_data, size_t* out_len);

void fr_free_buffer(uint8_t* data);
void fr_image_free(FRImage* img);

// Thread-local: reflects the most recent failure on the CALLING thread only,
// valid until that thread's next fr_* call - never call this from a thread
// other than the one that made the failing call. If your caller processes
// each unit of work (e.g. one request) on a single dedicated thread, this
// just works; if it dispatches fr_* calls across a pool, read the error
// immediately after the failing call, on that same thread.
const char* fr_last_error(void);

#ifdef __cplusplus
}
#endif
