# SaturnBox

SaturnBox is an experimental Sega Saturn static recompiler and runtime. It starts from the versioned C++ and Rust modules already developed in `saturn-recomp`: the SH-2 decoder and CFG builder, SIR, interpreter, Win64 JIT, C++ AOT emitter, Saturn HLE devices, and disc reader. SaturnBox is a separate project that keeps this implementation and applies the ideas captured in the Anybox article notes.

The current validated game path targets Rockman X4 (`T-1221G V1.002`). It reaches the title and selected first-stage loading paths; it is not a complete Saturn emulator or a full native port of the game. The HLE runtime models CD Block, SCU DMA, VDP1/VDP2, SMPC, and part of SCSP. The second SH-2 and the SCSP 68K are not fully emulated.

## What SaturnBox reuses and adds

- **Differential validation:** the SH-2 reference executor and SIR interpreter provide a comparison path for native code.
- **Code discovery:** the decoder, CFG recovery, direct-call evidence, dynamic target resolution, and overlay signatures are retained.
- **Separate runtime layers:** core memory, SIR, SH-2, Saturn devices, and frontends remain separate C++ libraries.
- **Incremental AOT compilation:** `saturn-aot --output-dir` emits stable C++ translation units by 64 KiB guest address bank. It rewrites only generated files whose contents changed, so the host compiler can reuse unchanged objects.

AOT discovery and SIR lowering still process the input images on every run. Persistent caching of those earlier stages remains future work. See [the roadmap](docs/ROADMAP.md) and [SIR/backend notes](docs/SIR.md).

## Source layout

```text
libs/core/       addresses and sparse MemoryMap
libs/sir/        backend-independent SIR and interpreter
libs/sh2/        SH-2 decoder, CFG, lifter, and differential executor
libs/saturn/     Saturn memory map, HLE devices, scheduler, and video/audio
tools/saturn-sh2/   inspection CLI and runtime launcher
tools/saturn-aot/   CFG-to-SIR-to-C++ static emitter
rust/saturn-disc/ ISO-9660 and BIN/CUE/MDF disc reader
 tests/          CPU, SIR, memory, JIT, and device tests
docs/           architecture, ISA/runtime contracts, and Rockman X4 reports
```

The dependency direction is `core → sir → sh2 → saturn`; tools consume these libraries.

## Build

Requirements: CMake 3.20+ and a C++20 compiler. Vulkan is enabled by default. To build the CPU renderer without downloading Vulkan dependencies:

```powershell
cmake -S . -B build -DSATURNBOX_VULKAN=OFF
cmake --build build --config Release --target saturn-aot saturn-sh2
```

The Vulkan compute path accelerates VDP1 sprite and polygon batches when a
compatible loader and GPU are available. VDP2 backgrounds still compose on the
CPU; the runtime includes an initial RBG0 bitmap rotation path. See
[Vulkan VDP1/VDP2 rendering](docs/VULKAN-VDP1-VDP2.md) for supported effects
and remaining background modes.

To build the existing test target, configure with `-DSATURNBOX_BUILD_TESTS=ON` (the default), then run CTest:

```powershell
cmake -S . -B build -DSATURNBOX_VULKAN=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Incremental static compilation

Extract the executable segments from a Saturn disc with `saturn-disc`, or provide equivalent SH-2 binaries. The AOT tool accepts a base address for every segment and one or more entry points. For example:

```powershell
saturn-aot `
  --segment 0x06002000 build/rockman/ip.bin `
  --segment 0x06009000 build/rockman/0.BIN `
  --segment 0x00220000 build/rockman/X4LOW.X4 `
  --entry 0x06002E40 --entry 0x06010000 `
  --entry 0x00220700 --entry 0x00220800 `
  --entry-list build/rockman/observed-entries.txt `
  --output-dir build/rockman-static

cmake -S . -B build-aot `
  -DSATURNBOX_VULKAN=OFF `
  -DSATURNBOX_STATIC_MODULE_DIR=build/rockman-static
cmake --build build-aot --config Release --target saturn-sh2
```

`--output-dir` creates one `.cpp` shard per occupied 64 KiB guest address bank, a registry translation unit, a CMake source list, and a manifest. Re-running AOT with unchanged inputs preserves each generated file's timestamp. Stale shards are removed only when the previous SaturnBox manifest recorded them. The existing `--output <module.cpp>` single-translation-unit mode remains available through `SATURNBOX_STATIC_MODULE_SOURCE`.

During a replay, `--aot-entry-output` records observed SH-2 targets. Pass that file back with `--entry-list` to seed another AOT discovery pass. The runtime tries compiled static blocks first, then the JIT, with the SIR interpreter available as a reference. The Rockman X4 static route and its limits are described in [the static recompilation report](docs/ROCKMAN-X4-STATIC-RECOMPILATION.md).

## Runtime commands

The CLI treats a raw SH-2 binary as big-endian 16-bit instruction words. Common commands include:

```powershell
saturn-sh2 decode game.bin --base 0x06004000
saturn-sh2 cfg game.bin --base 0x06004000 --entry 0x06004000 --output game.cfg.dot
saturn-sh2 analyze game.bin --base 0x06004000 --entry 0x06004000
saturn-sh2 run game.bin --base 0x06004000 --entry 0x06004000 --steps 10000

saturn-sh2 boot build/rockman/aip.bin `
  --aip-base 0x06002E40 `
  --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'C:/Isos/Rockman X4.mdf' --disc-sector-size 2448 `
  --steps 1000000
```

`boot` stops when the expected program entry is reached unless `--continue-after-execdmp` is specified. `--vblank-blocks N` schedules deterministic video/interrupt events after every N SIR blocks. `--frame-dir` saves PPM frames; `--frame-capture-after-blocks N` saves one selected frame with diagnostics. `--audio-output` writes PCM WAV output. On Windows, `launch` opens a native window and can use XInput gamepads.

The Windows presentation window defaults to 1280×720 and keeps the Saturn
frame's aspect ratio, adding black bars when needed. Use
`--window-size 1920x1080` to choose another output size, `--integer-scale` to
prefer whole-pixel enlargement when it fits, or `--smooth-scale` for HALFTONE
filtering of video. These options change host presentation only: VDP1/VDP2 and
frame captures stay at the game's native resolution. A wider window does not
expand the game's camera or reveal off-screen stage content.

Use `--interpreter` to repeat a boot scenario through the SIR interpreter for comparison. Diagnostic tracing can be expensive and should not be used to measure interactive performance.

## Saturn disc reader

The Rust crate `rust/saturn-disc` reads ISO-9660 from `.iso` files and the first data track of `.cue`, `.bin`, and `.mdf` images. It supports raw Mode 1/2352, Mode 2/2352, and Mode 1/2448 sectors, lists files and extents, and extracts the IP, AIP, or a named file:

```powershell
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- list GAME.CUE --extents
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- inspect GAME.CUE
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract GAME.CUE 0.BIN --output 0.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-ip GAME.CUE --output IP.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-aip GAME.CUE --output AIP.BIN
```

`FIRST READ ADDRESS` is the load address, not necessarily the executable entry point. The AIP determines when and where execution transfers.

## Documentation

- [Architecture and module boundaries](docs/ARCHITECTURE.md)
- [Vulkan VDP1/VDP2 rendering](docs/VULKAN-VDP1-VDP2.md)
- [Roadmap and current replay evidence](docs/ROADMAP.md)
- [SIR semantics and native backends](docs/SIR.md)
- [SH-2 lifter contract](docs/SH2-LIFTER.md)
- [SMPC behavior](docs/SMPC.md)
- [Disc image format and boot metadata](docs/DISC-IMAGES.md)
- [Rockman X4 validation corpus](docs/ROCKMAN-X4.md)
- [Rockman X4 static recompilation](docs/ROCKMAN-X4-STATIC-RECOMPILATION.md)
- [Generated native SH-2 module, 2026-10-31](docs/pt-BR/ROCKMAN-X4-AOT-20261031.md)
- [Rockman X4 player-route checkpoint](docs/ROCKMAN-X4-PLAYER-TEST-CHECKPOINT.md)
- [Title-map and video rendering investigation](docs/ROCKMAN-X4-TITLE-MAP-FIX.md)
- [FMV and post-FMV checkpoint](docs/ROCKMAN-X4-FMV-CHECKPOINT.md)
- [NBG0 map reads and producers](docs/ROCKMAN-X4-NBG0-MAP-READ.md), [write trace](docs/ROCKMAN-X4-NBG0-MAP-PRODUCER.md)
- [Hardware article review](docs/HARDWARE-ARTICLE-REVIEW.md)

The detailed Portuguese Rockman X4 investigation logs are retained under
[`docs/pt-BR/`](docs/pt-BR/); the main documentation set is in English.

The repository does not include game images or other copyrighted game data. Use files you are authorized to access.
