# Architecture

## Goal

The target is a **static recompilation** of each game for PC: translate SH-2
code before execution and link it to a native runtime for video, audio, and
system services. The current JIT and interpreter are code-discovery and
validation paths; they do not yet produce a complete static game executable.

SaturnBox is an in-progress Saturn recompiler. It uses the SIR interpreter as a
correctness reference and an HLE runtime for Saturn devices while preparing
native PC code generation:

```text
SH-2 image → decoder → CFG/functions → SIR-L0 → SIR passes → backend/runtime
```

M0 ends at CFG construction. M1 materializes and interprets SIR-L0 blocks to
validate CPU semantics. Early M2 adds `MemoryMap` support for the non-contiguous
IP/AIP and first executable regions, an observable boot harness, and
`BootRomServices`, which installs the minimum pointers expected by the AIP and
models its synchronization word. Each service has its own identity and emits a
trace with the caller block and an SH-2 state snapshot. `SYS_SET/GETUINT`,
`SYS_SET/GETSINT`, semaphore, and `SYS_SET/CHGSCUIM` dispatches update the
relevant tables, SCU mask, and System Work Area. The map accepts both the
physical `0x05xxxxxx` window and the cache-bypass `0x25xxxxxx` alias.

The runtime includes image-backed CD Block HLE (commands, HIRQ, FIFO, ISO-9660
path lookup, and DMA into SH-2 memory), all three SCU DMA channels
(`D?R/W/C/AD/EN/MD`), and VDP1/VDP2 memory and registers. Video traverses the
VDP1 command table, composites BACK/NBG0–NBG3 and the supported RBG0 bitmap
path by priority, decodes PNT and 8×8 or 16×16 cells, and arranges consecutive
pages in VDP2 1×1, 2×1, or 2×2 planes. The output retains per-pixel priorities
so VDP2 color calculation and shadow can be applied before RGB is committed to
`VideoFrame`.
`ScspRuntime` models `0x05A/0x05B` RAM and registers, handles the observed
mailbox, and plays a subset of PCM Start/Stop commands. It does not execute the
68K or emulate the complete SCSP. SMPC implements a byte-only interface, basic
reset commands, and `INTBACK` for two digital pads, sampled at VBlank.

On Windows x64, `Win64JitBlockExecutor` compiles 32-bit operations on demand,
including carry/borrow, overflow, and `DIV1`, for SIR blocks, then returns each
result to `ProgramExecutor`. 8/16/32-bit accesses use `MachineState` helpers
with big-endian RAM and MMIO. State updates follow instruction order in blocks
that access memory. Branches, calls, returns, and handlers can be chained with
native jumps. After each block, a `ProgramExecutor` continuation resolves
control flow, publishes context, and runs interrupt checks and hooks. MMIO
accesses preserve the same order and context. Blocks that cannot be compiled
fall back to the interpreter without repeating hooks that already ran.

A 20-million-block Rockman X4 CD replay reached the title with no interpreter
fallbacks, 19,999,999 chained transfers, and video, audio, and traces matching
the interpreter. See [SIR](SIR.md) for the validation details. Other game
phases and dual-SH-2 execution have not been validated. The SIR interpreter
remains the reference; CD, SCU, VDP1, VDP2, SMPC, and SCSP are HLE models, and
the window/video/audio frontend uses native host facilities.

The M1 executor chains blocks with an explicit step limit. An indirect jump
sends its computed address to a runtime resolver. If the target is inside the
image but outside the initial CFG, the runtime performs discovery and lowering
on demand. Peripheral regions go through an interceptable MMIO port rather
than RAM storage.

Before each block, `ProgramExecutor` publishes an `ExecutionContext` to the
interceptor chain: current address, previous block, step index, and a register
snapshot when requested. Without tracing, it publishes address/caller/step
without copying the register map at every block; BIOS calls still retain their
snapshots. This context accompanies each `MmioAccess` and `BootServiceCall`. It
is diagnostic information, not part of SIR semantics.

The executor also exposes an `InterruptHook`. `ScuEventScheduler` uses it to
publish VBlank-in to the SCU, honor masks and priorities, and resolve vector
`0x40`. On entry, PC/SR are pushed and `BL`/`IMASK` are applied; `RTE` restores
the interrupted block. Automatic generation is deterministic by block count
and disabled when its period is zero. Each VBlank also triggers SCU DMA
channels configured for VBlank-IN, resamples an armed `INTBACK` in
`SmpcRuntime`, and advances HLE scan-out: `VideoRuntime` composites
BACK/NBG0/NBG1, overlays the rendered VDP1 result, and publishes a pending
`VideoFrame`. When `PTMR=1` starts a command list, drawing completion remains
pending until the next block boundary; the scheduler then publishes the SCU
sprite-draw-end event (bit 13, vector `0x4D`).

## Layers

### Native-port graphics runtime

Distorted sprites and polygons retain all four VDP1 vertices. The reference
renderer uses inverse bilinear interpolation of the quad to find per-pixel UVs,
with inclusive edges and clipping before pixel traversal. It also supports zoom
points, polylines, mesh, Gouraud shading on RGB sprites/polygons, half-bright,
half-transparency, and RGB shadow. This is a geometric approximation; it does
not certify all VDP1 rasterization rules or every game effect.

The optional Vulkan VDP1 compute backend consumes the same ordered primitive
data as the CPU renderer. One invocation owns each destination pixel, so
transparency and blending retain command order without splitting a distorted
quad into two affine-UV triangles. It supports sprites, transformed quads and
polygons, texture modes, mesh, Gouraud, and RGB blend/shadow operations. Batches
use device-local storage with host-coherent staging and dispatch only across
visible primitive bounds. CPU VDP2 composition can overlap an outstanding GPU
batch; a fence is waited before a CPU framebuffer read, bank change, or
presentation. VDP2 layer composition and RBG0 rotation still use the CPU path.
See [Vulkan VDP1/VDP2 status](VULKAN-VDP1-VDP2.md) and
[devMiyax's compute-shader description](https://www.yabasanshiro.com/blog/2026-05-09_vdp1_emulation_with_compute_shader).

In the launcher without diagnostics, VDP1 commands still draw in their
original order, but VDP2/VDP1 host composition is deferred until
`presentFrame()`. Commands, VBlanks, interrupts, and audio progress are not
suppressed. This avoids composing a final image for every manual PTMR between
presentations. Reference/diagnostic modes keep immediate composition for their
observers.

Backing RAM uses on-demand 64 KiB pages. Reads and writes still check MMIO
before RAM; missing pages have the same meaning as zero-filled pages.
`memory()` materializes the sparse representation only when requested and
must be queried again after writes. Internal observers use `readRaw8()` to
avoid re-entering interceptors. State comparisons ignore entirely zero pages.

```text
core
 └─ sir       backend-independent representation
     └─ sh2   SH-2 decoder and static analysis
         └─ saturn declarative address-map classification
             └─ tools
```

`core` contains `Address` and `MemoryMap`. The map maintains non-overlapping
segments, provides safe big-endian reads, and does not expose host memory to
the SH-2. File loading happens only at the tool boundary; frontends receive
only this addressed image.

## Decoder and CFG

The decoder is a pure transformation from `(pc, 16-bit word)` to
`DecodedInstruction`. Encoding rules live in a mask table rather than being
spread across the decoder and CFG. An instruction exposes typed operands,
control flow, delay-slot information, and a precomputed target for direct
transfers.

`BlockBuilder` uses two phases:

1. Recursively discover reachable code and leaders.
2. Assemble blocks after all leaders are known.

This lets a later branch split an already-decoded sequence at an instruction
inside it. Direct calls create edges to both the callee and return site; the
target becomes a high-confidence `FunctionCandidate`. Complete function
recovery is a later phase, once there is more evidence and constant
propagation.

## Delay slots

The decoder only marks a transfer. `BlockBuilder` creates a
`DelayedTerminator { transfer, slot }` and preserves the relationship without
executing the instruction. `BRA`, `BSR`, `JMP`, `JSR`, `RTS`, and `RTE` always
execute their slot. SH-2 `BT/S` and `BF/S` also execute it on both paths, so
they are classified as `DelaySlot::Always`.

The analyzer treats a missing, unsupported, or control-transfer slot as
unsafe. These cases appear explicitly in the report instead of being accepted
as a reliable CFG.

PC-relative targets always use `PC + 4 + sign_extend(disp) * 2`. The decoder
stores the result so CFG and lifter code do not duplicate the rule.

## Milestones

- **M1:** SH-2 dispatch and lowering to SIR-L0, interpreter, and differential
  tests.
- **M2:** `MemoryMap`, IP/AIP and executable boot, constant propagation, more
  precise addresses, traces, minimal boot/MMIO services, and resolution of the
  game's remaining indirect jumps.
- **M3:** SIR lowering to a native backend and a minimal runtime.
- **Later:** dual-SH-2 execution and gradual increases in HLE device accuracy.
