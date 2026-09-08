# Builds the `fastresize` CLI (fastresize_cli.cpp over fastresize.h) and,
# via `make capi`, the C ABI shared library (fastresize_capi.h/.cpp) that
# lets non-C++ languages call the same implementation over FFI.
#
#   make            # fetch deps + build ./fastresize (fpng encoder)
#   make run ARGS='header some-image.png'
#   make capi       # build libfastresize_capi.so/.dylib
#   make test       # build + run fastresize_test
#   make FPNG=0 ...  # use stb_image_write instead of fpng (smaller PNG, ~12x slower encode)
#   make clean
#
# stb single-headers are vendored the same way the service Dockerfiles do it:
# curl'd at the pinned commit into ./vendor (gitignored), never committed.

STB_COMMIT := f0569113c93ad095470c54bf34a17b36646bbbb5
STB_BASE   := https://raw.githubusercontent.com/nothings/stb/$(STB_COMMIT)
VENDOR     := vendor
HEADERS    := $(VENDOR)/stb_image.h $(VENDOR)/stb_image_write.h $(VENDOR)/stb_image_resize2.h
FPNG_COMMIT := 925796543b9d26b8edfcdcecd94c1dac280f29fc

# PNG encoder, on by default. fpng is ~12x faster than stb_image_write (the
# encode stage was the bottleneck in every candidate this library was
# extracted from - see PERFORMANCE.md), for a ~8% larger file. FPNG=0 falls
# back to stb (header-only, nothing extra to compile or vendor).
FPNG ?= 1

CXX     ?= clang++
# -O3 + arch-native: fastresize.h's stb resize kernels and the compositeOver
# loop only vectorise past the SSE2 / baseline-NEON floor when the target
# ISA is explicit.
ARCH    := $(shell uname -m)
ifneq (,$(filter $(ARCH),x86_64 amd64))
  MARCH := -march=x86-64-v3
else ifneq (,$(filter $(ARCH),aarch64 arm64))
  MARCH := -mcpu=native
else
  MARCH :=
endif
# No -Wall: fastresize.h pulls in the stb single-headers, which are noisy
# under it; the service Dockerfiles don't use it either.
CXXFLAGS ?= -O3 $(MARCH) -std=c++17
LDFLAGS  ?=

# fpng.cpp is compiled as its own object with its own flags: -march=x86-64-v3
# does NOT imply PCLMUL, which fpng's SSE CRC path needs (gcc hard-errors on
# the intrinsic without -mpclmul), and it has no NEON path so SSE is disabled
# on arm. -fno-strict-aliasing is fpng's documented build requirement.
ifneq ($(FPNG),0)
  HEADERS    += $(VENDOR)/fpng.h
  FPNG_FLAGS := -DFASTRESIZE_FPNG
  FPNG_OBJ   := $(VENDOR)/fpng.o
  ifneq (,$(filter $(ARCH),x86_64 amd64))
    FPNG_ARCH := -msse4.1 -mpclmul
  else
    FPNG_ARCH := -DFPNG_NO_SSE=1
  endif
endif

# .dylib on macOS, .so everywhere else - both loadable by koffi/ext-ffi/etc.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SOEXT := dylib
else
  SOEXT := so
endif

fastresize: fastresize_cli.cpp fastresize.h $(HEADERS) $(FPNG_OBJ)
	$(CXX) $(CXXFLAGS) $(FPNG_FLAGS) -I $(VENDOR) -o $@ fastresize_cli.cpp $(FPNG_OBJ) $(LDFLAGS)

libfastresize_capi.$(SOEXT): fastresize_capi.cpp fastresize_capi.h fastresize.h $(HEADERS) $(FPNG_OBJ)
	$(CXX) $(CXXFLAGS) $(FPNG_FLAGS) -fPIC -shared -I $(VENDOR) -o $@ fastresize_capi.cpp $(FPNG_OBJ) $(LDFLAGS)

fastresize_test: fastresize_test.cpp fastresize.h $(HEADERS) $(FPNG_OBJ)
	$(CXX) $(CXXFLAGS) $(FPNG_FLAGS) -I $(VENDOR) -o $@ fastresize_test.cpp $(FPNG_OBJ) $(LDFLAGS)

$(VENDOR)/fpng.o: $(VENDOR)/fpng.cpp $(VENDOR)/fpng.h
	$(CXX) $(CXXFLAGS) $(FPNG_ARCH) -fno-strict-aliasing -fPIC -I $(VENDOR) -c -o $@ $(VENDOR)/fpng.cpp

$(VENDOR)/stb_%.h:
	@mkdir -p $(VENDOR)
	curl -sSL -o $@ $(STB_BASE)/stb_$*.h

$(VENDOR)/fpng.h $(VENDOR)/fpng.cpp:
	@mkdir -p $(VENDOR)
	curl -sSL -o $@ https://raw.githubusercontent.com/richgel999/fpng/$(FPNG_COMMIT)/src/$(@F)

.PHONY: run capi test clean
run: fastresize
	./fastresize $(ARGS)

capi: libfastresize_capi.$(SOEXT)

test: fastresize_test
	./fastresize_test

clean:
	rm -f fastresize fastresize_test libfastresize_capi.so libfastresize_capi.dylib
	rm -rf $(VENDOR)
