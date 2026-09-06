// fastresize - a small CLI over services/common/fastresize.h, so the exact
// decode/resize/composite/encode primitives the `qk3` and `cpp` candidates
// use can be run and timed from the command line the same way `vips` is:
//
//   fastresize resize     in.png out.png 0.5
//   fastresize resize     in.png out.png --width 800
//   fastresize resize     in.png out.png 0.5 --nearest
//   fastresize thumbnail  in.png out.png 500
//   fastresize composite  out.png --size 1200x800 a.png b.png@120,40
//   fastresize header     in.png
//
// Add -v / --verbose to any command to print per-stage timings (decode,
// resize, encode, ...) to stderr - the point of the tool is head-to-head
// measurement against `vips`.
//
// Scope note: fastresize.h only *encodes* PNG (it decodes anything stb
// supports), so output is always PNG regardless of the <out> extension.
// That matches this benchmark's scope (PNG compositing only); it is the one
// real difference from `vips`, which writes many formats.

#include "fastresize.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool gVerbose = false;

// stb_image_write's PNG deflate effort (0-9). stb's default is 8; qk3 sets
// it to 1 at runtime (measured ~28% faster encode, <0.1% larger output on
// diagram content). Left at the library default here; --png-level overrides.
int gPngLevel = -1;

void timed(const char* stage, double ms) {
  if (gVerbose) std::fprintf(stderr, "  %-9s %8.2f ms\n", stage, ms);
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeFile(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) throw std::runtime_error("write failed: " + path);
}

fastresize::Image decodeFile(const std::string& path) {
  auto t0 = Clock::now();
  std::string raw = readFile(path);
  fastresize::Image img = fastresize::decode(raw);
  timed("decode", msSince(t0));
  return img;
}

void encodeFile(const fastresize::Image& img, const std::string& path) {
  if (gPngLevel >= 0) stbi_write_png_compression_level = gPngLevel;
  auto t0 = Clock::now();
  std::string png = fastresize::encodePng(img);
  timed("encode", msSince(t0));
  writeFile(path, png);
  if (gVerbose) std::fprintf(stderr, "  %-9s %8zu bytes -> %s\n", "output", png.size(), path.c_str());
}

// Applies the resize, honouring the same "skip when it's a no-op" shortcut
// scaleImage() uses in qk3/cpp.
fastresize::Image doResize(const fastresize::Image& src, int w, int h, bool nearest) {
  if (w == src.width && h == src.height) return src;
  auto t0 = Clock::now();
  fastresize::Image out =
      nearest ? fastresize::resizeNearest(src, w, h) : fastresize::resize(src, w, h);
  timed(nearest ? "resize/nn" : "resize/lin", msSince(t0));
  return out;
}

// ---- argument helpers -------------------------------------------------------

bool takeFlag(std::vector<std::string>& args, const std::string& name) {
  for (auto it = args.begin(); it != args.end(); ++it) {
    if (*it == name) {
      args.erase(it);
      return true;
    }
  }
  return false;
}

// Returns "" if not present. Errors if present without a value.
std::string takeOpt(std::vector<std::string>& args, const std::string& name) {
  for (auto it = args.begin(); it != args.end(); ++it) {
    if (*it == name) {
      if (std::next(it) == args.end()) throw std::runtime_error(name + " needs a value");
      std::string v = *std::next(it);
      args.erase(it, it + 2);
      return v;
    }
  }
  return "";
}

int usage() {
  std::fprintf(stderr,
      "fastresize - CLI over services/common/fastresize.h\n\n"
      "  fastresize resize    <in> <out> <scale>        [--nearest] [-v]\n"
      "  fastresize resize    <in> <out> --width <px>   [--height <px>] [--nearest] [-v]\n"
      "  fastresize thumbnail <in> <out> <max-dim>      [--nearest] [-v]\n"
      "  fastresize composite <out> --size <W>x<H> <layer[@x,y]>...  [-v]\n"
      "  fastresize header    <in>\n\n"
      "Global: -v/--verbose (stage timings), --png-level <0-9> (stb default 8; qk3 uses 1)\n"
      "Output is always PNG (fastresize.h encodes PNG only).\n");
  return 2;
}

// ---- subcommands ----------------------------------------------------------

int cmdResize(std::vector<std::string> args, bool thumbnail) {
  bool nearest = takeFlag(args, "--nearest") || takeFlag(args, "--point");
  std::string widthOpt = takeOpt(args, "--width");
  std::string heightOpt = takeOpt(args, "--height");
  if (args.size() < 2) return usage();

  const std::string in = args[0];
  const std::string out = args[1];
  fastresize::Image src = decodeFile(in);

  int targetW, targetH;
  if (thumbnail) {
    if (args.size() < 3) return usage();
    double maxDim = std::stod(args[2]);
    double s = std::min(maxDim / src.width, maxDim / src.height);
    targetW = std::max(1, static_cast<int>(src.width * s));
    targetH = std::max(1, static_cast<int>(src.height * s));
  } else if (!widthOpt.empty() || !heightOpt.empty()) {
    if (!widthOpt.empty() && !heightOpt.empty()) {
      targetW = std::stoi(widthOpt);
      targetH = std::stoi(heightOpt);
    } else if (!widthOpt.empty()) {
      double s = std::stod(widthOpt) / src.width;
      targetW = std::stoi(widthOpt);
      targetH = std::max(1, static_cast<int>(src.height * s));
    } else {
      double s = std::stod(heightOpt) / src.height;
      targetH = std::stoi(heightOpt);
      targetW = std::max(1, static_cast<int>(src.width * s));
    }
  } else {
    if (args.size() < 3) return usage();
    double scale = std::stod(args[2]);
    targetW = std::max(1, static_cast<int>(src.width * scale));
    targetH = std::max(1, static_cast<int>(src.height * scale));
  }

  if (gVerbose)
    std::fprintf(stderr, "  %-9s %dx%d -> %dx%d\n", "target", src.width, src.height, targetW, targetH);
  fastresize::Image dst = doResize(src, targetW, targetH, nearest);
  encodeFile(dst, out);
  return 0;
}

// Parses "path" or "path@x,y".
struct Layer {
  std::string path;
  int x = 0, y = 0;
};

Layer parseLayer(const std::string& spec) {
  Layer l;
  auto at = spec.rfind('@');
  if (at == std::string::npos) {
    l.path = spec;
    return l;
  }
  l.path = spec.substr(0, at);
  std::string off = spec.substr(at + 1);
  auto comma = off.find(',');
  if (comma == std::string::npos) throw std::runtime_error("bad layer offset (want @x,y): " + spec);
  l.x = std::stoi(off.substr(0, comma));
  l.y = std::stoi(off.substr(comma + 1));
  return l;
}

int cmdComposite(std::vector<std::string> args) {
  std::string sizeOpt = takeOpt(args, "--size");
  if (args.empty() || sizeOpt.empty()) return usage();

  auto xpos = sizeOpt.find('x');
  if (xpos == std::string::npos) throw std::runtime_error("--size wants <W>x<H>");
  int W = std::stoi(sizeOpt.substr(0, xpos));
  int H = std::stoi(sizeOpt.substr(xpos + 1));

  const std::string out = args[0];
  fastresize::Image canvas(W, H);

  double compositeMs = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    Layer l = parseLayer(args[i]);
    fastresize::Image img = decodeFile(l.path);
    auto t0 = Clock::now();
    fastresize::compositeOver(canvas, img, l.x, l.y);
    compositeMs += msSince(t0);
  }
  timed("composite", compositeMs);
  encodeFile(canvas, out);
  return 0;
}

int cmdHeader(std::vector<std::string> args) {
  if (args.empty()) return usage();
  std::string raw = readFile(args[0]);
  fastresize::Dimensions d = fastresize::probeDimensions(raw);
  // Mirrors `vipsheader`'s default one-line form.
  std::printf("%s: %dx%d uchar, 4 bands (forced RGBA)\n", args[0].c_str(), d.width, d.height);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage();

  try {
    gVerbose = takeFlag(args, "-v") || takeFlag(args, "--verbose");
    std::string lvl = takeOpt(args, "--png-level");
    if (!lvl.empty()) gPngLevel = std::stoi(lvl);

    std::string cmd = args.front();
    args.erase(args.begin());

    if (cmd == "resize") return cmdResize(std::move(args), /*thumbnail=*/false);
    if (cmd == "thumbnail") return cmdResize(std::move(args), /*thumbnail=*/true);
    if (cmd == "composite") return cmdComposite(std::move(args));
    if (cmd == "header" || cmd == "info") return cmdHeader(std::move(args));
    if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage();
    std::fprintf(stderr, "fastresize: unknown command '%s'\n", cmd.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fastresize: %s\n", e.what());
    return 1;
  }
  return usage();
}
