# Review of Saturn Hardware Articles

This review compares the technical notes supplied on 2026-10-03 with the
current code and validation evidence. The articles are useful as references
and checklists: some examples describe work already implemented, while others
propose backends or hardware accuracy beyond the current scope. The original
Portuguese review is preserved in
[`pt-BR/ANALISE-ANEXOS-HARDWARE.md`](pt-BR/ANALISE-ANEXOS-HARDWARE.md).

## Implemented areas

| Area | Current project state |
| --- | --- |
| SH-2 | Decoder, CFG discovery, SIR-L0 lowering, interpreter, differential comparison, runtime resolution of indirect targets, and explicit delay slots. Win64 JIT and static C++ AOT backends are also present. |
| CD and ISO 9660 | `CdRuntime` reads 2048/2352/2448-byte images, handles commands/HIRQ/FIFO, and looks up ISO files by path. HLE does not simulate the drive servo, SH-1 microprocessor, or physical drive timing. |
| SCU DMA | Three channels with registers, address steps, observed activation factors, FIFO-data waits, and transfer history. Copying may be synchronous in HLE; completion and interrupt delivery are observed at scheduler boundaries. This is not a general bus-cycle model. |
| SMPC | Byte-only registers, basic reset, `INTBACK`, status/peripheral handshake, and two active-low digital pads. RTC and SMEM still return zero. |
| VDP1/VDP2 | MMIO memory/registers, VDP1 command traversal, software framebuffer, and BACK/NBG0/NBG1 composition, including PNT/cell maps and selected windows and effects. A Vulkan compute path is now present for VDP1; this does not imply complete GPU video emulation. |
| SCSP | HLE for the observed mailbox, PCM Start/Stop subset, stereo mixer, and audio frames. The 68K is not executed, and envelopes, DSP, sequences, and the full SCSP are not emulated. |
| Diagnostics | `--trace`, `--trace-game`, block/MMIO context, focused map traces, and video/audio snapshots. The project favors targeted evidence over logging every step indiscriminately. |

## VDP1 command interpretation

The Sega manual clarifies how VDP1 sprite shapes are interpreted:

- Shape `0` is a fixed sprite. It uses XA/YA, CMDSIZE, and direction bits;
  other coordinates are ignored.
- Shape `1` with zoom point `0` uses vertices A and C as opposite area
  corners. Their order can also flip the image.
- Other zoom points and shape `2` need dedicated implementation. The current
  shape `2` path covers its bounding rectangle; it does not reproduce the
  hardware's quadrilateral transform.

In `CMDSIZE`, X occupies bits 13–8 and each unit represents 8 pixels; Y
occupies bits 7–0 and directly encodes 1–255 pixels. Therefore `0x0210` means
16×16 pixels. Although the specification's HTML table appears to append
“/8” to both dimensions, the manual prose restricts division to width and the
overview confirms a maximum vertical size of 255. See the [VDP1 manual,
rectangular sprite command](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_51.htm)
and [Sega sprite limits](https://www.infochunk.com/saturn/segahtml_en/hard/intr/hon/p03_30.htm).

This corrected a Rockman X4 case with 74 active shape-`0` sprites. The old
renderer stretched textures using B/C/D and had swapped the X/Y CMDSIZE
fields, interpreting `0x0210` as 128×2 instead of 16×16. The renderer and
command report now use the hardware fields: shape `0` uses XA/YA, and shape
`1` with zoom point `0` uses A/C. The old pixel capture is not suitable for
evaluating composition. Corrected replay and pixel-level VDP1/VDP2 comparison
are documented in the
[title-map report](ROCKMAN-X4-TITLE-MAP-FIX.md#per-pixel-vdp1vdp2-priority-composition-2026-10-04).
Primary references: [VDP1 manual §7.4 — Fixed sprite](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_40.htm)
and [§7.5 — Rectangle sprite](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p07_50.htm).

Directly translating Saturn chips into shaders remains a possible future
backend direction. The current renderer reads runtime memory arrays and
composes pixels on the host, so dirty pages and staging buffers do not speed
up the current path. A GPU backend should mark pages at MMIO/DMA writes and
submit deltas while preserving visibility order.

## Corrections and explicit limits

- **BRAM:** One article gives `0x20000000` as the base. Sega's map places the
  window at `0x00180000..0x0018FFFF`, which is
  `0x20180000..0x2018FFFF` through the project's cache-bypass alias. The
  window spans 64 KiB of addresses for 32 KiB of physical storage. MAME also
  ignores even bytes and packs odd bytes. This runtime does not yet persist
  backup RAM to a file. References: [Sega SCU map](https://antime.kapsi.fi/sega/files/ST-097-R5-072694.pdf)
  and [MAME implementation](https://github.com/mamedev/mame/blob/master/src/mame/sega/saturn.cpp).
- **Endianness:** Guest memory and registers already use explicit big-endian
  operations. The recompiler must not read Saturn structs through host pointer
  casts or scatter `bswap` operations; it should use runtime memory helpers.
- **BIOS/TRAPA:** SIR represents `TRAPA` as an architectural event, and
  `BootRomServices` implements boot services needed by the corpus. This is
  neither a complete BIOS nor a reason to intercept every vector.
- **Ghidra:** `DecompInterface` can export pseudocode for manual naming and
  analysis. Its C output is decompiler reconstruction, not original source or
  compilable recompiler output. Function lists should not automatically feed
  the dispatcher: symbols depend on reachable code and validated function
  boundaries. In the local corpus, `0.BIN` loads at `0x06009000` and enters at
  `0x06010000`; do not apply the generic `0x06004000` example without checking
  the image map. An external symbol table can still enrich traces and reports.
- **Timing:** The scheduler reproduces events at configurable deterministic
  block intervals. It is not equivalent to 263 scanline slices with validated
  cycle counts. Launcher presentation pacing is separate from SH-2 semantics.
  Keep `sleep` and thread barriers out of the reference interpreter.
- **Two SH-2 CPUs and audio:** An HLE service models the observed secondary
  producer mailbox, but a second SH-2 does not execute concurrent code. The
  observed PCM does not confirm CD-DA playback or complete SCSP/68K emulation.
  These are future compatibility goals, not prerequisites for fixing current
  composition issues.
- **DMA:** An instantaneous generic `memcpy` is not sufficient as universal
  semantics. Source/destination, address steps, FIFO availability, activation
  factor, data visibility before completion, and interrupt delivery matter;
  the runtime keeps these events observable in the scheduler.

The [Ghidra decompiler export script](https://github.com/NationalSecurityAgency/ghidra/blob/master/Ghidra/Features/Decompiler/src/main/java/ghidra/app/decompiler/DecompInterface.java)
is a real interface for requesting C pseudocode and can be used as an external
research tool. A robust script should protect initialization and its main loop
with `try/finally` so `dispose()` always runs, and it should declare UTF-8 for
output. Even a successful decompilation may rely on program types, globals,
and helper functions; it is not a standalone C translation unit. Start by
inspecting functions and building a reviewed name/address map for traces.
Headers extracted from the first pseudocode line and dispatchers built from
every detected function do not replace the CFG, runtime resolver, or SIR
contracts.

## Recommended next steps

1. Inspect sprite sources and colors in saved captures; record the limits of
   shape `0`, shape `1`, and shape `2` interpretation.
2. Continue frame-by-frame VDP2 validation: active layers, read addresses,
   priority/CRAM, and independent comparison of composed pixels.
3. Add tested dirty tracking for cross-page accesses only when introducing a
   GPU backend, and publish VRAM/CRAM at the correct synchronization points.
4. Treat persistent BRAM, a second real SH-2, broader BIOS support, CD-DA, and
   full SCSP behavior as separate tasks, each driven by corpus behavior and
   differential tests.
