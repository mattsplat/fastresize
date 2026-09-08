// fastresize_test.cpp - a dependency-free test binary over fastresize.h.
// Every fixture is synthesised in memory (no committed images), and pixel
// checks round-trip through the header's own decode(), so the whole suite
// is just this one translation unit plus the stb headers the library
// already needs.
//
//   make test           # stb encoder
//   make FPNG=1 test     # fpng encoder
//
// Exit code is the number of failed checks (0 = all passed).

#include "fastresize.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
const char* g_case = "";

void startCase(const char* name) {
  g_case = name;
  std::printf("- %s\n", name);
}

void fail(const char* expr, const char* file, int line, const std::string& detail) {
  ++g_failures;
  std::printf("  FAIL [%s] %s:%d  %s%s%s\n", g_case, file, line, expr,
              detail.empty() ? "" : "  ", detail.c_str());
}

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) fail(#cond, __FILE__, __LINE__, "");            \
  } while (0)

#define CHECK_EQ(got, want)                                                        \
  do {                                                                             \
    auto g_ = (got);                                                               \
    auto w_ = (want);                                                              \
    if (!(g_ == w_))                                                               \
      fail(#got " == " #want, __FILE__, __LINE__,                                  \
           "got " + std::to_string(g_) + ", want " + std::to_string(w_));          \
  } while (0)

// ---- fixtures -------------------------------------------------------------

fastresize::Image solid(int w, int h, unsigned char r, unsigned char g, unsigned char b,
                        unsigned char a) {
  fastresize::Image img(w, h);
  fastresize::fill(img, r, g, b, a);
  return img;
}

// A transparent w x h canvas with one opaque colour rect composited in.
fastresize::Image withRect(int w, int h, int rx, int ry, int rw, int rh,
                           std::array<unsigned char, 3> rgb) {
  fastresize::Image canvas(w, h);
  fastresize::Image rect = solid(rw, rh, rgb[0], rgb[1], rgb[2], 255);
  fastresize::compositeOver(canvas, rect, rx, ry);
  return canvas;
}

std::array<unsigned char, 4> pixel(const fastresize::Image& img, int x, int y) {
  const unsigned char* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
  return {p[0], p[1], p[2], p[3]};
}

bool hasPngSignature(const std::string& s) {
  return s.size() > 8 && static_cast<unsigned char>(s[0]) == 0x89 && s[1] == 'P' && s[2] == 'N' &&
         s[3] == 'G';
}

// PNG IHDR colour-type byte: 2 = RGB, 6 = RGBA. Offset 25 = 8 sig + 4 len +
// 4 "IHDR" + 4 width + 4 height + 1 bit-depth.
int pngColorType(const std::string& s) { return static_cast<unsigned char>(s[25]); }

// ---- tests ---------------------------------------------------------------

void testOpaqueBounds() {
  startCase("opaqueBounds");

  fastresize::Rect empty = fastresize::opaqueBounds(fastresize::Image(20, 20));
  CHECK(empty.empty());

  fastresize::Image r = withRect(64, 48, 10, 12, 20, 16, {255, 0, 0});
  fastresize::Rect b = fastresize::opaqueBounds(r);
  CHECK_EQ(b.x, 10);
  CHECK_EQ(b.y, 12);
  CHECK_EQ(b.w, 20);
  CHECK_EQ(b.h, 16);

  fastresize::Rect full = fastresize::opaqueBounds(solid(30, 10, 1, 2, 3, 255));
  CHECK_EQ(full.x, 0);
  CHECK_EQ(full.y, 0);
  CHECK_EQ(full.w, 30);
  CHECK_EQ(full.h, 10);
}

void testFill() {
  startCase("fill");

  fastresize::Image img(8, 4);
  fastresize::fill(img, 12, 34, 56, 255);
  CHECK((pixel(img, 0, 0) == std::array<unsigned char, 4>{12, 34, 56, 255}));
  CHECK((pixel(img, 7, 3) == std::array<unsigned char, 4>{12, 34, 56, 255}));
  CHECK_EQ(img.opaque.w, 8);
  CHECK_EQ(img.opaque.h, 4);

  fastresize::fill(img, 0, 0, 0, 0);
  CHECK(img.opaque.empty());
}

void testCrop() {
  startCase("crop");

  fastresize::Image src = fastresize::decode(fastresize::encodePng(withRect(64, 48, 8, 6, 24, 18, {0, 200, 0})));

  fastresize::Image c = fastresize::crop(src, fastresize::Rect{8, 6, 24, 18});
  CHECK_EQ(c.width, 24);
  CHECK_EQ(c.height, 18);
  // The whole cropped region is the opaque rect, remapped to (0, 0).
  CHECK_EQ(c.opaque.x, 0);
  CHECK_EQ(c.opaque.y, 0);
  CHECK_EQ(c.opaque.w, 24);
  CHECK_EQ(c.opaque.h, 18);

  fastresize::Image clamped = fastresize::crop(src, fastresize::Rect{-5, -5, 1000, 1000});
  CHECK_EQ(clamped.width, 64);
  CHECK_EQ(clamped.height, 48);
}

void testResize() {
  startCase("resize");

  fastresize::Image src = withRect(40, 40, 5, 5, 30, 30, {200, 100, 50});

  fastresize::Image lin = fastresize::resize(src, 17, 9);
  CHECK_EQ(lin.width, 17);
  CHECK_EQ(lin.height, 9);

  fastresize::Image nn = fastresize::resizeNearest(src, 80, 80);
  CHECK_EQ(nn.width, 80);
  CHECK_EQ(nn.height, 80);
  // 2x point-sample upscale: source (10,10) lands at (20,20) unchanged.
  CHECK((pixel(nn, 20, 20) == std::array<unsigned char, 4>{200, 100, 50, 255}));
  // opaque rect stays a superset of the scaled content.
  CHECK(nn.opaque.x <= 10 && nn.opaque.y <= 10);
  CHECK(nn.opaque.x + nn.opaque.w >= 70 && nn.opaque.y + nn.opaque.h >= 70);
}

void testCompositeOver() {
  startCase("compositeOver");

  // opaque source overwrites; transparent source leaves the canvas alone.
  fastresize::Image canvas = solid(10, 10, 0, 0, 240, 255);
  fastresize::Image opaque = solid(4, 4, 250, 250, 250, 255);
  fastresize::compositeOver(canvas, opaque, 1, 1);
  CHECK((pixel(canvas, 2, 2) == std::array<unsigned char, 4>{250, 250, 250, 255}));
  CHECK((pixel(canvas, 8, 8) == std::array<unsigned char, 4>{0, 0, 240, 255}));

  fastresize::Image clear(10, 10);  // all transparent
  fastresize::compositeOver(canvas, clear, 0, 0);
  CHECK((pixel(canvas, 8, 8) == std::array<unsigned char, 4>{0, 0, 240, 255}));

  // half-alpha blend, integer (s*a + d*(255-a)) / 255 with a=128:
  //   R: (240*128 + 0*127)/255   = 120
  //   B: (0*128   + 240*127)/255 = 119
  fastresize::Image half = solid(2, 2, 240, 0, 0, 128);
  half.opaque = fastresize::Rect{0, 0, 2, 2};
  fastresize::compositeOver(canvas, half, 5, 5);
  auto px = pixel(canvas, 5, 5);
  CHECK_EQ(int(px[0]), 120);
  CHECK_EQ(int(px[2]), 119);
  CHECK_EQ(int(px[3]), 255);

  // out-of-bounds placement is clipped, not a crash / OOB write.
  fastresize::compositeOver(canvas, opaque, -100, -100);
  fastresize::compositeOver(canvas, opaque, 1000, 1000);
}

void testEncodePng() {
  startCase("encodePng");

  fastresize::Image src = withRect(48, 32, 6, 4, 20, 14, {17, 34, 51});
  std::string png = fastresize::encodePng(src);
  CHECK(hasPngSignature(png));
  CHECK_EQ(pngColorType(png), 6);

  fastresize::Image back = fastresize::decode(png);
  CHECK_EQ(back.width, 48);
  CHECK_EQ(back.height, 32);
  CHECK((pixel(back, 10, 8) == std::array<unsigned char, 4>{17, 34, 51, 255}));
  CHECK_EQ(back.pixels[3 * 4 + 3], 0);  // (3,0) is transparent margin

  CHECK((fastresize::probeDimensions(png).width == 48));
  CHECK((fastresize::probeDimensions(png).height == 32));
}

void testFlattenRgb() {
  startCase("flattenRgb");

  // Hand-built RGBA input: one opaque, one half-alpha, one fully transparent.
  fastresize::Image img(3, 1);
  img.pixels = {90,  91, 92, 255,   // p0 opaque
                240, 0,  0,  128,   // p1 half-alpha
                7,   7,  7,  0};    // p2 transparent (junk RGB)
  img.opaque = fastresize::Rect{0, 0, 3, 1};

  std::vector<unsigned char> rgb = fastresize::flattenRgb(img, 10, 20, 30);
  CHECK_EQ(rgb.size(), static_cast<size_t>(3 * 3));

  // p0: fully opaque -> source colour verbatim
  CHECK_EQ(int(rgb[0]), 90);
  CHECK_EQ(int(rgb[1]), 91);
  CHECK_EQ(int(rgb[2]), 92);
  // p1: straight-alpha "over", (s*a + bg*(255-a)) / 255 with a = 128
  //   R (240*128 + 10*127)/255 = 125,  G (0 + 20*127)/255 = 9,  B (0 + 30*127)/255 = 14
  CHECK_EQ(int(rgb[3]), 125);
  CHECK_EQ(int(rgb[4]), 9);
  CHECK_EQ(int(rgb[5]), 14);
  // p2: fully transparent -> background verbatim, junk RGB discarded
  CHECK_EQ(int(rgb[6]), 10);
  CHECK_EQ(int(rgb[7]), 20);
  CHECK_EQ(int(rgb[8]), 30);
}

void testEncodePngRgb() {
  startCase("encodePngRgb");

  fastresize::Image canvas(40, 24);  // transparent
  fastresize::Image square = solid(20, 12, 255, 0, 0, 255);
  fastresize::compositeOver(canvas, square, 5, 5);

  std::string png = fastresize::encodePngRgb(canvas, 10, 20, 30);
  CHECK(hasPngSignature(png));
  CHECK_EQ(pngColorType(png), 2);

  fastresize::Image back = fastresize::decode(png);  // decode re-expands to RGBA
  CHECK_EQ(back.width, 40);
  CHECK_EQ(back.height, 24);
  CHECK((pixel(back, 0, 0) == std::array<unsigned char, 4>{10, 20, 30, 255}));   // was transparent
  CHECK((pixel(back, 10, 8) == std::array<unsigned char, 4>{255, 0, 0, 255}));   // was opaque

  // white background flattens a fully transparent image to solid white
  fastresize::Image t(8, 8);
  CHECK((pixel(fastresize::decode(fastresize::encodePngRgb(t, 255, 255, 255)), 0, 0) ==
         std::array<unsigned char, 4>{255, 255, 255, 255}));
}

void testDecodeRejectsGarbage() {
  startCase("decode rejects garbage");
  bool threw = false;
  try {
    fastresize::decode(std::string("definitely not an image"));
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
}

}  // namespace

int main() {
  std::printf("fastresize_test (%s encoder)\n",
#ifdef FASTRESIZE_FPNG
              "fpng"
#else
              "stb"
#endif
  );

  testOpaqueBounds();
  testFill();
  testCrop();
  testResize();
  testCompositeOver();
  testEncodePng();
  testFlattenRgb();
  testEncodePngRgb();
  testDecodeRejectsGarbage();

  if (g_failures == 0) {
    std::printf("\nOK\n");
    return 0;
  }
  std::printf("\n%d check(s) failed\n", g_failures);
  return g_failures;
}
