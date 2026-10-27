# SaturnBox Roadmap

This document tracks the next technical steps toward turning Rockman X4's
SH-2 execution into a native PC game.

## Reused foundation and the Anybox direction

SaturnBox is a separate tree started from versioned files in `saturn-recomp`.
The CPU, SIR, runtime, tools, and validation code were kept as the foundation;
local dumps, frames, and traces were not imported.

The four techniques prioritized in the Anybox article are applied as follows:

- **Differential validation:** already available between the reference SH-2
  executor and SIR interpreter; keep it as the acceptance criterion for
  lifter changes.
- **Function discovery:** recursive CFG discovery and dynamic target
  resolution are implemented; future records should retain evidence origin
  and confidence.
- **Separate runtime:** core, SIR, SH-2, and device code are distinct
  libraries, though session assembly is still concentrated in the launcher.
  Extracting a reusable session is a later step.
- **Incremental compilation:** `saturn-aot --output-dir` emits translation
  units by 64 KiB guest-address bank and rewrites each file only when content
  changes. This makes host compilation incremental; decoder, CFG, and lowering
  still process the full input on every run.

## Current state

- The AIP/IP and `0.BIN` are loaded into a sparse `MemoryMap`.
- Boot initializes the master stack and VBR, installs minimal boot-ROM
  services, and resolves SIR blocks on demand.
- The Rockman X4 flow reaches `0x06010000`.
- `CMP/STR` is implemented in the decoder, SIR lowering, and differential
  executor.
- Without `--continue-after-execdmp`, `SYS_EXECDMP` ends the harness with
  `status: sleeping`. With continuation enabled, the harness uses the HLE CD,
  SMPC, DMA, VDP1/VDP2, and SCSP runtimes described below; this does not mean
  that every game mode runs completely.
- Boot services and MMIO carry block context, caller, step, and registers. The
  twelve materialized services have distinct traceable targets; interrupt,
  clock, semaphore, and `SYS_SET/CHGSCUIM` dispatches update observable
  RAM/MMIO state.
- The address map recognizes SMPC, and its runtime handles `INTBACK`, reset,
  and digital pads on two ports. The CLI configures buttons with
  `--pad1/--pad2`.
- CD Block HLE accepts a raw image through `--disc`, implements HIRQ/commands,
  FIFO, ISO-9660 path reads, and DMA of resources into SH-2 memory. SCU DMA
  handles direct and indirect modes (12-byte descriptors with an end code),
  waits for CD FIFO data, and recognizes VBlank-IN/OUT, HBlank-IN, Timer 0,
  VDP1 draw end, and manual start. VDP1/VDP2 provide VRAM, framebuffer, CRAM,
  command-table traversal, and BACK/NBG0–NBG3 composition into `VideoFrame`,
  including PNT, 8×8/16×16 cells, and 1×1/2×1/2×2 VDP2 planes.
- `INTBACK` remains armed after its request and is re-executed at VBlank-IN to
  synchronize pads with the scheduler.
- With image `T-1221G V1.002`, the current path only sends `SNDOFF` (`0x07`)
  and `SNDON` (`0x06`) to SMPC before `SYS_EXECDMP`; it does not request
  `INTBACK` yet.
- The real trace reaches `SYS_EXECDMP` from `0x0601CED4`, with
  `R0=0xFFFFFFE9`. Continuing reveals a multiplayer CD loop with accesses to
  `0x25xxxxxx` aliases, but still no `PTMR=1`. The deterministic SCU/VBlank
  scheduler can now be enabled from the CLI, and each event advances video
  presentation. A first `VideoFrame` can be observed even when the VBlank
  handler is masked.
- The scheduler also delivers SCU-DMA channel completion factors (D0/D1/D2 at
  vectors `0x4B`/`0x4A`/`0x49`). In the real corpus this passes through the
  indirect dispatcher at `0x060303CC` and makes the 832-byte upload to VDP1
  VRAM observable.
- Runtime work from steps 1–6 is integrated: BIOS interrupt callbacks return
  through `RTS` with saved state; CD/SCU DMA feed resources; VBlank,
  HBlank/VBlank-out, and subcode are scheduled; VDP1 runs PTMR=1/2 with
  paletted sprites, clipping, and local coordinates; VDP2 applies W0/W1
  windows; `INTBACK` is resampled at VBlank; and SCSP produces stereo PCM
  frames per VBlank. The CLI exposes `--frame-dir` (PPM) and `--audio-output`
  (WAV) for an external frontend. `saturn-sh2 launch` also opens a 320×224
  Win32 window and plays PCM through `waveOut`, paced at 60 Hz and closed with
  the window.

## First local SaturnBox replay (2026-10-27)

Using a Rockman X4 image, SaturnBox extracted the AIP/IP, `0.BIN`, and
`X4LOW.X4`, generated shards from four initial entries, and built the launcher.
A one-million-block replay recorded 218 observed addresses; adding them as
roots increased the AOT module from 2,443 to 3,466 blocks.

In the one-million-block AOT/JIT replay, 875,062 blocks ran statically and
124,938 through the JIT. `--interpreter` reached the same last block
(`0x0603CDC0`), register state, and handoff. Neither JIT nor AOT fell back to
the interpreter. Both replays stopped at the step limit before producing a
frame. Another 47 blocks were lowered on demand, so static code discovery is
still incomplete.

An extended route with VBlank every 14,000 blocks captured a 320×240 title
screen at about 19.5 million blocks. In the 20-million-block replay, AOT ran
6,445,626 blocks and JIT ran 13,554,386, with no interpreter fallback; 3,513
blocks were lowered on demand. The saved frame near the title contains visible
pixels, but the output was black again at the 20-million-block limit. The
static path does not cover this entire route yet.

## Next technical sequence

The goal remains a statically recompiled port. Reference-executor and runtime
optimizations can be reused by generated code; expanding a generic emulator is
not the product goal. Immediate priorities:

- Persist/cache CFG and SIR to avoid repeating analysis and lowering. The key
  must include byte and overlay hashes, entry points, decoder/lifter/SIR/emitter
  revisions, runtime ABI, and host compiler options. Current incremental
  shards only save C++ compilation when generated content is unchanged.
- Extract memory, devices, scheduler, CPUs, and executors from launcher setup
  into a reusable session, so new frontends can be added without duplicating
  SH-2 or peripheral semantics.
- The per-block static C++ emitter is connected to the runtime;
  `--output-dir` separates output by 64 KiB guest-address bank, and
  `--aot-module-output` exports SIR discovered by a replay. The opening/title
  module has 7,771 functions and covers 20 million blocks without JIT or
  interpreter. A first-stage capture has 11,232 functions and covers 30 million
  blocks through stream `FAD 18061`, also without JIT or fallback. The extended
  route has 13,691 functions and covers 510 million static blocks through a
  stream beginning at the `ST04_0.BIN` extent, without JIT or fallback; its 65
  shared records match hybrid execution. The disc has 174 `ST*` files (114
  `.BIN` files in 80 `STxx` prefixes, plus 60 `.ARC` files), but only `ST00`
  and `ST04` have been tied to routes. Include the other stages and resources,
  exercise AOT variants during gameplay, and cover the other processors before
  calling the result a complete native port. The emitter and
  `saturn-aot --variant` now combine complete images with overlays at the same
  address; execution selects by SIR signature when the resolver supplies the
  active version. Boot now invalidates local resolution when a write changes
  the code page and revalidates the target on the next block entry. A
  160M-step X replay recorded 13 first-time block re-resolutions at reused
  addresses, all before 104.94M and with no known prior signature; no active
  code swap was confirmed during `right` input from 122M to 145M. Validation
  of a real overlay swap during gameplay remains pending.
- Sustain real-time video/audio. Optional state snapshots, dense register/RAM
  queries, Win32 polling every 1,024 blocks, and a PCM queue with a 50 ms
  prebuffer are implemented. The window title shows actual FPS; the Win32
  frontend requests a 1 ms timer during a session, waits before presenting a
  frame, and replenishes the PCM reserve when fewer than three buffers remain.
  The summary reports underruns, waits, audio errors, and presentation/wait
  interval percentiles to measure remaining stutter.
- The VDP1 Vulkan compute backend executes batches of sprites/quads with four
  vertices and bilinear UVs, zoom points, mesh, Gouraud, and RGB blending. One
  invocation per pixel applies commands in order. The current transfer path
  stages snapshots through host-coherent memory into device-local storage and
  restricts dispatch to the union of visible primitive bounds. Fences keep the
  framebuffer coherent for CPU reads/writes and presentation. Asynchronous
  submission can overlap SH-2/VDP2 composition with GPU work. Degenerate
  lines/quads fall back to the CPU reference after earlier commands complete.
  VDP2 layer composition and the current RBG0 affine bitmap path remain on the
  CPU; RBG0 tile maps and coefficient-table modes remain future work. Vulkan
  presentation and multiple frames/batches in flight also remain.
- The window accepts keyboard and two XInput gamepads, with deadzone,
  hot-plug, release on focus loss, and help on F1. CLI-configured ports retain
  priority. Expand input validation and stage progression.
- Expand visual and audio validation across stages. An opening replay without
  Start played all of `OP.CPK`; 89.933 seconds of PCM retained sequence and
  alignment with the source, with 99.9985% of samples matching within one
  PCM16 unit. Small deviations remain at buffer transitions, as does keeping
  the audio queue free of underruns.

SH-2 overclocking is a later experimental option. The scheduler currently uses
blocks as its deterministic unit; increasing blocks per VBlank is not precise
overclocking and does not fix execution that runs below the required PC speed.

1. Validate real indirect descriptors in replay and add modeled activation
   factors that are still missing (Timer 1 and SCSP request); observe real
   destinations beyond the VDP1 upload. Channel-specific limits/alignment, bus
   restrictions, and timing during DMA remain open. In current X-route
   captures, all 256 observed descriptors use `mode=0x7`; none exercise bit
   24, so replay does not validate a real indirect list. For Timer 0,
   `T0C=0` coincides with VBlank-OUT; it is not a second event at line-counter
   wrap. The list format and end marker follow the
   [Sega SCU manual, indirect DMA section](https://docs.exodusemulator.com/Archives/SSDDV1R1E/a/a04/hon/p024.htm)
   and the [final Sega SCU DMA table specification](https://antime.kapsi.fi/sega/files/ST-210-110194.pdf).
2. Improve VDP1 coverage and missing effects using evidence from real command
   lists; zoom points and transformed quads already have geometric support in
   the reference runtime.
3. Complete VDP2 composition with per-character/pixel color-calculation flags
   and extended calculation, then validate shadows in replay. Both priority
   candidates, ratio/add calculations, and normal/MSB shadows are covered.
4. Expand the real SCSP/68K producer (registers, sound RAM, envelopes, and a
   second-processor scheduler); the mailbox and minimal PCM mixer are modeled.
5. Expand native SIR backends for Win64 x64. The on-demand JIT and static C++
   emitter use 8/16/32-bit memory/MMIO, carry/borrow/overflow, `DIV1`, indirect
   jumps, and interrupts. Static CD replays to the title ran 20 million
   blocks; another covered 30 million through the first-stage stream. These
   paths matched the interpreter and used static code. The 510-million-block
   route also used static code only and matched the hybrid capture. Other
   stages, overlay versions at the same address, full dual-SH-2 execution (the
   slave's FRT-polling slice already runs X's texture producer), the SCSP 68K,
   and runtime/helper optimization remain.
6. Expand the launcher to cross-platform window/audio backends and add PC
   packaging; a minimal Win32 backend is already available in `launch`.

The current slice runs the requested order (SCU/CD DMA, real descriptor,
VDP2 cell/tile, and `INTBACK` at VBlank). With `--vblank-blocks 400000`,
Rockman publishes its first `VideoFrame` after handoff. The route crosses the
dispatcher at `0x060303CC`, and a mailbox producer releases polling at
`0x06010722` (`0x0605E796`/`0x0605E798`). The routine also programs VDP1
TVMR/FBCR, so this point synchronizes the framebuffer with a shared mailbox.
Real SCSP (RAM/registers `0x05A/0x05B`) is not cycle-accurate, but the runtime
already produces a deterministic stereo PCM stream at each VBlank.

The Win32 launcher turns the harness into a basic interactive window/audio
session. A cross-platform backend and faster SIR/JIT execution are still needed
to sustain 60 Hz on slower hardware. Until then, `--frame-dir` and
`--audio-output` provide a verifiable bridge for external frontends.

## Scenario-based validation method

Following the
[SaturnRecomp compatibility matrix](https://github.com/sonsegajp/SaturnRecomp/blob/main/docs/COMPATIBILITY.md),
record each route as an observable scenario: opening/title, stage transition,
and gameplay with scheduled input. For each scenario, retain a frame or visual
sequence, input events, sectors/resources read, and audio when available;
separate the last point actually reached from work not yet exercised. Boot
success alone does not count as stage coverage. In this replay, the attached
image is the game's normal opening; the side capture with the incorrect sprite
was later, during the stage.
