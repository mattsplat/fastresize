# Benchmarks: fastresize vs libvips

Real numbers, not estimates — measured on this machine, right now, via each
tool's own CLI (`fastresize` here, `vips`/`vipsthumbnail` for libvips), so
both pay the same process-startup, decode, and encode costs. Reproduce with
[hyperfine](https://github.com/sharkdp/hyperfine) (`--warmup 3`, 10+ runs);
commands are given per row.

> **Stale — pending a re-run.** These numbers were taken when `make` still
> defaulted to the `stb_image_write` PNG encoder. `make` now builds with
> **fpng** (`make FPNG=0` for the old behaviour), which encodes ~12× faster
> (817 ms → 68 ms on an 8557×4000 canvas; see `encodePng()` in
> `fastresize.h`). This barely moves the **resize** rows below — their
> outputs are small, so encode is a minor share — but the **composite** row,
> whose output is a full-size canvas, is dominated by encode and is expected
> to narrow sharply or reverse once re-measured. The webserver benchmark in
> [detail-image-bench](https://github.com/mattsplat/detail-image-bench)
> already runs the fpng build and its fastresize candidate (`qk3`) matches
> libvips there.

**Machine**: Apple M4 Pro (arm64), macOS 26.6.2. **fastresize**: this repo,
built via `make FPNG=0` (`-O3 -mcpu=native`, `stb_image_write` encoder).
**vips**: 8.18.4 (Homebrew). Test images are real production PNG assets from
[detail-image-bench](https://github.com/mattsplat/detail-image-bench)'s
fixtures — three sizes: large (6000×7680, 740 KB), medium (3600×6240,
216 KB), small (1200×4320, 17 KB).

## Resize (decode → resize → encode)

| Image | Operation | fastresize | vips | Result |
|---|---|---|---|---|
| large | resize 0.25×, nearest | 155.5 ms ± 3.1 | 372.0 ms ± 5.3 | **fastresize 2.4×** |
| large | resize 0.25×, each tool's default kernel (linear vs. lanczos3) | 234.9 ms ± 3.3 | 328.7 ms ± 7.2 | **fastresize 1.4×** |
| medium | resize 0.25×, nearest | 77.7 ms ± 1.8 | 232.6 ms ± 7.1 | **fastresize 3.0×** |
| small | resize 0.25×, nearest | 18.1 ms ± 0.8 | 125.8 ms ± 3.7 | **fastresize 6.9×** |
| large | thumbnail to fit 1000px, nearest | 100.2 ms ± 1.7 | 437.6 ms ± 2.9 (`vipsthumbnail`) | **fastresize 4.4×** |

```sh
fastresize resize   large.png out.png 0.25 --nearest
vips       resize   large.png out.png 0.25 --kernel nearest

fastresize thumbnail large.png out.png 1000 --nearest
vipsthumbnail         large.png -s 1000 --path out.png
```

fastresize's advantage *grows* as the image shrinks (2.4× on the large
image, 6.9× on the small one) — consistent with libvips carrying more fixed
per-call overhead (building its lazy operation graph) that a single stb
call doesn't pay, on top of doing genuinely more work for the same kernel
(this run's linear-vs-lanczos3 row understates fastresize's edge for that
reason — lanczos3 is real extra convolution work vips is choosing to do by
default that a plain resize doesn't).

## Composite (decode ×2 → alpha blend → encode) — vips won this one on the stb build

| Operation | fastresize (`FPNG=0`) | vips | Result |
|---|---|---|---|
| composite 2 layers onto a 3600×6240 canvas | 668.8 ms ± 7.0 | 316.9 ms ± 2.1 | **vips 2.1×** |
| same, vips capped to 1 thread (`--vips-concurrency=1`) | 668.8 ms ± 7.0 | 312.7 ms ± 2.1 | **vips 2.1×** (not a threading artifact) |

```sh
fastresize composite out.png --size 3600x6240 medium.png small.png@100,100
vips       composite2 medium.png small.png out.png over --x 100 --y 100
```

**The encoder was most of it.** Writing a full-size output canvas with
`stb_image_write` is the dominant stage of this operation — in
detail-image-bench's per-stage timers it was the majority of the request,
more than decode, resize and composite combined. The default build now uses
fpng (817 ms → 68 ms on an 8557×4000 canvas). That alone should bring this
row close to — or under — vips; it needs a re-run to say by how much.

**What's left after that** is libvips' pipeline fusion: a single
`composite2`-then-save decodes, blends, and encodes in one streamed
evaluation with no full-resolution intermediate, where `fastresize.h`
materializes each stage as a `std::vector<unsigned char>`. For a short
two-image chain that fusion is a real edge, and single-threading vips
doesn't remove it — it's the pipelining, not core count. It stops mattering
once the chain is long enough that everything gets materialized once at the
end regardless (see below).

This doesn't contradict the case made in `fastresize.h`'s header comment,
which is about a *longer* chain (10-18 layers) that still gets fully
materialized into one final PNG regardless of how many operations built up
to it. Reproducing that shape faithfully needs one real evaluation graph
built in-process (what a webserver using either library actually does),
not N separate CLI processes round-tripping through PNG files between every
layer — that would penalize vips with N-1 extra encode/decode passes a real
caller never pays, and isn't a fair test of anything. That longer-chain,
in-process comparison is exactly what
[detail-image-bench](https://github.com/mattsplat/detail-image-bench) is:
a full webserver benchmark (not a CLI microbenchmark) where every
libvips-backed candidate measured slower than the plain-pipeline
candidates using this library, across four independent implementations
(PHP, Node/sharp, and two C++/Rust libvips bindings).

## Takeaway

Resize and thumbnail — the two operations most callers actually spend time
on — favor fastresize, by a growing margin as images get smaller, and the
fpng default doesn't change that (small outputs, encode is a minor share).

Composite, on the stb encoder, favored vips at the CLI/single-op level:
partly the slow encoder (now fixed by default), partly pipeline fusion — a
real architectural edge for a *short* chain that stops applying once enough
operations chain together that everything gets materialized once at the end
anyway, which is the shape of workload this library was extracted from. In
that in-process, many-layer webserver benchmark
([detail-image-bench](https://github.com/mattsplat/detail-image-bench)), the
fastresize candidate matches libvips on median latency and throughput and
trails only on the tail.
