# Rockman X4: first static x86-64 recompilation

This report records the first static native-code path for the Sega Saturn
Rockman X4 build `T-1221G V1.002`. It builds on the existing SH-2 decoder,
CFG recovery, SIR lowering, and Saturn HLE runtime. It does not convert the
whole game into a standalone PC executable yet.

## What REA could inspect

The disc image is `H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf`, SHA-256
`c1977179f386a2131cfdaf73417f4de8eb1f7fb984049518c2b2028330b09387`.
REA's Windows Ghidra provider rejected the MDF as an unsupported binary
format, so there is no REA analysis evidence for the disc itself. This is a
tool-format limitation, not a finding about the disc contents.

The local `saturn-disc` reader did parse the image. It reports product
`T-1221G V1.002`, title `ROCKMAN X4`, an IP image of 0x1800 bytes loaded at
`0x06002000`, AIP entry `0x06002E40`, and `0.BIN` (415,320 bytes) loaded at
`0x06009000`. The disc also contains `X4LOW.X4` (565,248 bytes), the overlay
that the title path loads at `0x00220000`. These are observations from the
local disc parser and the existing title-path work, not REA results.

REA inspected the final built launcher
`build/tools/saturn-sh2/Release/saturn-sh2.exe`: Ghidra 12.1.4 identifies it
as a native x86-64 PE. Its SHA-256 is
`30ed3338c018b58ee6ea24fdec1f9026eac1a8e74554918f8e3aeabb3fd73eb3`.
The final overview reported 8 segments, 14,472 procedures, and 1,729 strings
(Evidence `ev_fc3cd5edf54d630d7944b24e118332e0d79d404789e540d240ecb1e2bb70ff39`).

## Static backend added

`saturn-aot` builds a CFG from one or more SH-2 segments and entry points,
lowers reachable blocks to SIR, then emits a C++ translation unit with one
host function per distinct address/signature pair. The normal x64 C++ compiler compiles that source
into the launcher. This gives the generated code the host compiler's x86-64
code generation while keeping SH-2 instruction semantics in the existing SIR
lowering.

At runtime, `StaticNativeBlockExecutor` uses a guest address plus a stable SIR
shape signature to select a generated function. A missing block or mismatched
signature goes to the existing Win64 JIT. When the resolver is asked for an
address again, it snapshots live instruction bytes and replaces the SIR body
if an overlay changed it; the static executor then selects the matching
signature. Writes that replace code without a later block resolution still
need explicit invalidation. Mixed mode dispatches block by block and does not
use the JIT's native tail-chain path.

The emitter and CLI accept several complete image variants in one output
module. Each variant must include all resident segments needed to decode its
roots, plus that version's overlay at its loaded address. `--entry` roots are
shared across variants and filtered to mapped bytes; `--entry-list` roots are
filtered the same way, which lets one runtime trace feed several overlay
versions. Repeated segments may overlap across variants, but not within one
variant. This compiles the versions together; a gameplay replay must still
exercise the resolver switching between those versions to validate the full
overlay path.

The module uses these three segments:

| Segment | SH-2 base | Purpose |
| --- | ---: | --- |
| `ip.bin` | `0x06002000` | Boot IP/AIP code |
| `0.BIN` | `0x06009000` | Main program |
| `X4LOW.X4` | `0x00220000` | Title overlay |

Initial roots: `0x06002E40`, `0x06010000`, `0x00220700`, and `0x00220800`.
Those roots recover 2,443 blocks. A first replay then recorded 1,583 unique
indirect runtime targets with `--aot-entry-output`. Feeding that list back to
`saturn-aot --entry-list` expands static discovery to 1,585 unique roots and
7,865 blocks. All 7,865 lowered and were emitted, with 133,347 SIR
instructions, no skipped blocks, and no lift failures. The entry-list reader
keeps only targets that fall inside the three build-time segments. Other disc
overlays and stage archives are not statically compiled by this module.

For the final build, `--aot-module-output` exports the runtime `Module` after a
replay has resolved its indirect targets. It emitted 7,771 blocks and 131,844
SIR instructions; 56 other discovered blocks were unsupported by the emitter.
The resulting module matches every block used in the recorded 20-million-step
path. The static executor still checks each block signature, so later code
that differs from the captured SIR can use the JIT fallback.

## Reproduce the generated module

With Rust and CMake available, extract the inputs (the exact image path may be
changed to match the local copy):

```powershell
New-Item -ItemType Directory -Force build/rockman | Out-Null
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-ip "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" --output build/rockman/ip.bin
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-aip "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" --output build/rockman/aip.bin
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" 0.BIN --output build/rockman/0.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" X4LOW.X4 --output build/rockman/X4LOW.X4
cmake -S . -B build
cmake --build build --config Release --target saturn-aot
build/tools/saturn-aot/Release/saturn-aot.exe `
  --segment 0x06002000 build/rockman/ip.bin `
  --segment 0x06009000 build/rockman/0.BIN `
  --segment 0x00220000 build/rockman/X4LOW.X4 `
  --entry 0x06002E40 --entry 0x06010000 `
  --entry 0x00220700 --entry 0x00220800 `
  --output build/rockman/rockman_static.cpp
cmake -S . -B build -DSATURNBOX_STATIC_MODULE_SOURCE="${PWD}/build/rockman/rockman_static.cpp"
cmake --build build --config Release --target saturn-sh2
build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --steps 20000000 `
  --pad1 start --pad-after-blocks 13700000 `
  --aot-module-output build/rockman/rockman_observed.cpp
cmake -S . -B build -DSATURNBOX_STATIC_MODULE_SOURCE="${PWD}/build/rockman/rockman_observed.cpp"
cmake --build build --config Release --target saturn-sh2
build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc "H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf" --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --steps 20000000 `
  --pad1 start --pad-after-blocks 13700000
# Compare by adding --interpreter to the same boot command.
```

To compile an overlay that reuses a guest address, add another complete image
after the base image. Repeat the resident segments and substitute the overlay
binary at its loaded base; the variant name is for diagnostics:

```powershell
build/tools/saturn-aot/Release/saturn-aot.exe `
  --segment 0x06002000 build/rockman/ip.bin `
  --segment 0x06009000 build/rockman/0.BIN `
  --segment 0x00220000 build/rockman/X4LOW.X4 `
  --variant first-stage `
  --segment 0x06002000 build/rockman/ip.bin `
  --segment 0x06009000 build/rockman/0.BIN `
  --segment 0x00220000 build/rockman/first-stage-code-overlay.X4 `
  --entry 0x06002E40 --entry 0x06010000 `
  --entry 0x00220700 --entry 0x00220800 `
  --entry-list build/rockman/observed_entries.txt `
  --output build/rockman/rockman_versions.cpp
```

Replace the example code-overlay path/base and entry roots with the extracted
SH-2 overlay and observed addresses for the target route. ARC files are data
archives, not SH-2 code segments, and must not be passed as `--segment`. The
example documents the interface; it does not claim that the first-stage ARC is
currently loaded or executed in the gameplay replay.

The generated C++ and extracted game files live under ignored `build/`.
The checked-in deliverable is the emitter, signature-checked dispatch, and
runtime integration; the build option embeds a generated module in that
launcher configuration.

## Same-build replay comparison

The final 20-million-block replay used the disc with 2448-byte sectors, continued
after `SYS_EXECDMP`, scheduled VBlank every 10,000 blocks, and pressed Start
after block 13,700,000. The AOT and interpreter logs were identical outside
the static native execution counter. Both ended at `step-limit`, block
`0x060303CC`, with the same final CPU registers and handoff state.

| Counter | AOT | SIR interpreter |
| --- | ---: | ---: |
| Static blocks emitted/available | 7,771 | 7,771 |
| Static native executions | 20,000,000 | 0 |
| JIT blocks compiled | 0 | 0 |
| JIT block executions | 0 | 0 |
| Interpreter fallback blocks | 0 | 0 |

Static code handled all 20 million replay block steps. Neither the JIT nor the
interpreter executed a block in the AOT run.

The shared guest/runtime observations were also equal: CD PLAY history,
scheduler delivery counts, VDP1 command counts, VDP2 register values, SCSP
mailbox/audio counters, frame/composition counts, and final SH-2 registers.
The replay emitted 3,626 frames and 2,000 compositions. This validates the
fully static path against the interpreter for this recorded opening/title
path. It is not full-game path coverage, dual-SH-2 emulation, cycle accuracy,
or proof that every Saturn device is complete.

Artifacts from this run are local under `build/artifacts/`:
`aot-observed-20261011.txt` and `interp-observed-20261011.txt`. The generated
C++ source is local at `build/rockman/rockman_observed.cpp`.

## First-stage resource path (2026-10-11)

The static module was extended with a replay that reaches the first stage's CD
stream. Matching the known route required `--vblank-blocks 14000`; using 25000
produced a different title/CD rhythm. The controller press was held from block
13,700,000 through 14,000,000.

The capture replay emitted 11,232 static functions with 192,033 SIR
instructions; 203 discovered blocks were skipped by the emitter. Before this
module was rebuilt into the launcher, the route executed 2,184 JIT blocks for
1,067,137 steps. The rebuilt launcher then completed the same 30-million-step
route with 11,232 static blocks available, 30,000,000 static executions, zero
JIT executions, and zero interpreter fallback. Its non-backend diagnostics
were identical to both the capture replay and the SIR interpreter.

The route reads the stage stream at FAD 18061 for 3,988 sectors. Adding the
CD data-track offset of 150 gives LBA 17911, which is the start of
`ST00_0_Z.BIN` in the ISO extent list. A later 510-million-step route begins
its final recorded stream at FAD 42397 (LBA 42247), the start of
`ST04_0.BIN`. These are start-sector correlations; the recorded reads can
cross adjacent file extents. `saturn-disc list --extents` now exposes LBAs for
repeating this mapping.

The first-stage run transferred 3,373 sectors and deleted 3,371. Its final
VDP1 list had 111 commands: 83 sprites, 8 polygons, and other draw commands. The scheduler
produced 2,142 VBlanks at the diagnostic period of 14,000 guest blocks. The
run ends at `0x00222244` in the loaded `X4LOW.X4` region; this is a
deterministic first-stage loading path, not proof of player-controlled gameplay
or coverage of every stage.

The parsed disc inventory has 174 `ST*` files totaling 261,746,688 bytes:
114 `.BIN` files spanning 80 numbered `STxx` prefixes and 60 `.ARC` files.
The recorded routes start in the `ST00` and `ST04` groups; the remaining
prefixes have not been exercised as phase paths. Only the route above is covered by this module. The
extracted `SDDRVS.MP`
(884 bytes) and `SDDRVS.TSK` (26,610 bytes) identify concrete sound-driver
inputs, but the current `ScspRuntime` still services a mailbox and a small PCM
subset instead of executing the SCSP's 68K instruction stream. The runtime
also has only one executing SH-2 state; the second Saturn SH-2 is not yet
implemented.

Artifacts are local under `build/artifacts/`:
`aot-first-stage-vblank14k-20261011.txt`,
`aot-first-stage-static-replay-20261011.txt`,
`interp-first-stage-vblank14k-20261011.txt`, and
`disc-inventory-20261011.txt`. The emitted module is
`build/rockman/rockman_first_stage_observed.cpp`.

## Extended route through the ST04 resource stream (2026-10-11)

The scripted route from the player checkpoint was captured through 510 million
SH-2 block steps. Its discovered module contains 13,691 emitted functions and
242,687 SIR instructions; 398 other discovered blocks were skipped. Discovery
used 2,249 JIT blocks for 7,149,353 steps and had no interpreter fallback.
The module was compiled into the x64 launcher, which replayed the same
510-million-step route with 13,691 static native blocks, zero JIT executions,
and zero interpreter fallback. The last block was `0x0603085E`. After removing
only backend-specific counters, all 65 shared log records were byte-for-byte
equal to the hybrid replay.

The route later reads at FAD 42397, which maps to LBA 42247 and the start of
`ST04_0.BIN`; the 150-sector FAD/LBA offset is accounted for. The read may
continue across adjacent file extents. The historical frame captured at block
490,000,000 shows X in a phase scene, but its sprite/palette is corrupted and
does not validate movement from controller input. At the route's final block,
VDP1 reports 25 commands and no sprites. This is static native coverage of one
recorded phase path, not a claim that every stage or overlay is covered.

Artifacts: `aot-player-stage-observed-20261011.txt`,
`aot-player-stage-static-replay-uninstrumented-20261011.txt`, and
`disc-extents-20261011.txt` under `build/artifacts/`; the historical image is
`build/artifacts/player-test-iter41-start-stage/captured-frame.png`.

## Overlay invalidation and sprite capture follow-up (2026-10-11)

The dynamic block resolver now fingerprints the live instruction bytes when an
indirect target is resolved. If code at that address changed, it clears the old
SIR body in place, lowers the newly loaded instructions, and reports an overlay
replacement. Keeping the `BasicBlock` identity stable preserves incoming SIR
edges. The static executor rechecks the SIR signature after a body change, and
the JIT cache now does the same before reusing compiled host code. A block
without a matching static variant is recompiled by the JIT.

This is an initial invalidation path, not full multi-version AOT. It sees a
replacement when that address is passed through the runtime resolver. Direct
edges can keep a materialized block without asking the resolver, and the static
module still emits one body per address. Full coverage needs write-aware
invalidation, multiple compiled versions for one address, and routes that load
and enter each overlay.

## Timed overlay validation replay (2026-10-25)

The recovered X route was replayed to 160,000,000 block steps with the same
14,000-block VBlank period, pad sequence, and frame capture at 106,000,000.
The captured PPM is byte-identical to the earlier capture at that checkpoint.
The run completed normally, recorded 194,261 control events with no drops, and
continued executing both SH-2s through the gameplay input window (`right` at
122,000,000; release at 145,000,000). CD PLAY requested FAD 12965
(`ST00_0_X.BIN`) at step 104,191,169 and repeated that same range at
138,295,169 and 153,387,169; it did not request FADs 1975 or 2249.

The resolver recorded 13 block re-lowerings at steps 8,844,676; 15,475,287;
15,596,720; 16,267,744; 21,070,190; 24,287,917; 49,260,993; 49,261,017;
102,114,752; 104,265,065; 104,368,374; 104,371,057; and 104,931,074. Each
event has `old=unknown`: the resolver had no previous live-byte fingerprint
for those already materialized static SIR blocks. These are first live
resolutions/re-lowerings, not verified changes from one executed guest-code
version to another. There were zero confirmed changes with a known previous
signature, zero static AOT variant rebinds, and no resolver re-lowering after
104,931,074—before the `right` input window. Therefore this replay does **not**
validate an actual live overlay switch during gameplay. A route that loads and
executes a second code version at a reused address is still required.

The run log and manifest are `build/tmp/player-x-overlay-validation-steps2-20261025.log`
and `.json`; the control trace and capture are under
`build/tmp/control-trace-player-x-overlay-validation-steps2-20261025.csv` and
`build/artifacts/player-x-overlay-validation-steps2-20261025/`.

The captured frame is also confirmed to contain corrupted pixels in the raw
VDP1 framebuffer, before VDP2 composition. The saved diagnostic has display
bank 0 active while drawing into bank 1; its last VDP1 command list therefore
does not by itself identify which command produced the corrupt sprite currently
shown in bank 0. Reconstructing the five player-shaped 4-bpp tiles from the
captured VDP1 VRAM, including the command flips and VDP2 CRAM colors, reproduced
all 420 opaque pixels in bank 0 and in the final frame at the same coordinates.
This rules out the host compositor and the tested 4-bpp color decode as the
source of those pixels: the pattern is already present in the texture bytes
read by VDP1.

The frame-time VDP1 command list is reconstructed directly from
`frame_104998_vdp1_vram.bin`, not from the command list saved after the replay
finished. Player commands 155–159 read these 16x16 4-bpp tiles:

| Command | VDP1 source | Position | `IP.BIN` offset |
| --- | --- | --- | --- |
| 155 | `0x60100` | `(149,178)` | `0x1100` |
| 156 | `0x60180` | `(160,146)` | `0x1180` |
| 157 | `0x60280` | `(152,154)` | `0x1280` |
| 158 | `0x60200` | `(160,161)` | `0x1200` |
| 159 | `0x60080` | `(152,162)` | `0x1080` |

Each tile is 128 bytes. The descriptor copies `0x780` bytes. The full VDP1
destination `0x60000–0x6077F` exactly matches captured RAM
`0x06003000–0x0600377F`. Of those 1,920 bytes, 1,904 match `IP.BIN` offsets
`0x1000–0x177F`; 16 differ at relative offsets `0x18D`, `0x18F`,
`0x191–0x193`, `0x196–0x197`, `0x199–0x19B`, `0x19D`, `0x1B0–0x1B1`,
`0x1B3`, `0x1BD`, and `0x1DC`. The upload is overwhelmingly boot-image
code/data rather than player pixels. The bytes are already wrong in the VDP1
source region before host composition; this is not a renderer color-decode or
VDP2 compositing defect.

The frame's `ram_high.bin` also preserves the transient SCU DMA descriptor at
`0x060507C0`: count `0x780`, destination `0x25C60000`, source alias
`0x86003000`. It agrees with the texture bytes captured in VDP1. Static code
at `0x06012D3C` consumes 12-byte records beginning at `0x060507B4` (count at
`+0`, destination at `+4`, source at `+8`) and programs SCU DMA0. The record
writer at `0x06012E60` stores those fields and sets the source cache-bypass
alias. The builder around `0x0601B0A0` derives the source as
`0x06003000 + [0x06068598]`, stores the result at `0x060604DC`, and creates the
matching descriptor.

Code at `0x06010340` sets `R14=0`; one path through `0x060103A8` stores that
value to `[0x06068598]`, while a branch can skip the store. The frame snapshot
has `[0x06068598]=0` and `[0x060604DC]=0x06003000`, but this capture does not
show whether the store executed or whether the field remained zero from
initialization. The observed descriptor still selects the boot image's
address, and the source region lacks the expected sprite bytes at upload time.
Frame-capture runs now retain CPU and SCU DMA writes overlapping
`0x06003000–0x06003780` and writes to `[0x06068598]` in
`sprite-source-buffer-writes.csv`; SCU DMA destination transfers into the
buffer also appear in `scu-dma-sprite-source-transfers.txt`. Both include guest
step and block/caller. The next replay can identify which asset loader or
overlay should have supplied those bytes and whether the offset setter ran.

The AOT block view sharpens the interpretation of the zero field: the path at
`0x060103A8` loads a pointer from the literal at `0x06010464` (the captured
word there is `0x06068598`) and stores `R14`; the earlier init block sets
`R14=0`. The builder uses the same global through the pointer literal at
`0x0601B160`. This confirms a zero-initialization path, but does not prove the
separate texture buffer was filled or that zero is wrong. The immediate
failure remains the DMA reading boot-image bytes from `0x06003000`.

The disc image was then used to extract `PL00.ARC`, `PL01.ARC`, `PL02.ARC`,
`ST00_0_X.BIN`, `ST00_0_Z.BIN`, `ST04_0.BIN`, and `ST04_1.BIN` into
`build/rockman/`. Comparing the `PL00.ARC` payloads with the same frame's RAM
and video snapshots found exact, full-size matches:

| PL00 type | File payload offset | Size | Captured destination |
| --- | ---: | ---: | --- |
| 2 | `0x800` | `0x38E2C` | RAM `0x060BB040` |
| 0 | `0x3962C` | `0x300` | RAM `0x06002000` |
| 17 | `0x3992C` | `0x1B800` | VDP1 VRAM `0x8000` |
| 11 | `0x5512C` | `0x3000` | VDP2 VRAM `0x50000` |
| 27 | `0x5812C` | `0x4000` | VDP1 VRAM `0x78000` |
| 26 | `0x5C12C` | `0xE8C` | low RAM `0x002D0000` |
| 13 | `0x5CFB8` | `0x1038` | low RAM `0x002FE800` |
| 33 | `0x5DFF0` | `0x348C` | low RAM `0x002CC000` |
| 21 | `0x6147C` | `0x30606` | SCSP RAM `0x2A2EC` (mode 1) |

In the frame-time VDP1 traversal, 15 sprite commands (179–194) read inside
the loaded type-17 range. The five X-shaped commands 155–159 instead read
`0x60080–0x60280`, whose bytes come from `0x06003000`. Thus `PL00.ARC` was
loaded and its graphics are live elsewhere in the same frame; the remaining
failure is narrowed to the separate path that stages or selects the five
player tiles at VDP1 `0x60000`, rather than a missing disc read of the whole
package. The current captures still do not identify which routine should
populate that staging source or why its offset is zero.

A follow-up compared the other player and first-stage packages against the
same captured VDP1 memory. `PL02.ARC` has a type-17 payload at file offset
`0x39D40` whose full `0x1B800` bytes exactly match VDP1 `0x8000–0x237FF`, the
same bytes as `PL00.ARC`; this is shared stage imagery, not evidence for the
missing X animation. `PL00.ARC` type 27 at `0x5812C` is the `0x4000`-byte
interface sheet at VDP1 `0x78000`; its contact sheet shows the X portrait and
stage-select text, not a gameplay frame sequence. For the first stage,
`ST0_0_0.ARC[0x4DE00..0x82500]` exactly matches VDP1
`0x24074..0x58774` (`0x34700` bytes). That resident stage-graphics range is
still separate from the five X commands reading `0x60080–0x60280`.

The raw-byte search of `ST00_0_X.BIN` and `ST00_0_Z.BIN` found no nontrivial
128-byte blocks in RAM or VDP1 in this frame; the `_X` matches were in SCSP
RAM, consistent with audio data. This exact-byte check does not rule out
compressed or transformed graphics.

A follow-up exact comparison of the `ST00_1_X.BIN` stream against the valid
route's `scsp_ram.bin` snapshot found six complete 4 KiB blocks from file
offsets `0x5E000–0x63FFF` in the top 24 KiB of SCSP RAM. The blocks are
permuted across six slots:

| `ST00_1_X.BIN` offset | SCSP RAM offset | SCSP address |
| ---: | ---: | ---: |
| `0x5E000` | `0x7C000` | `0x05A7C000` |
| `0x5F000` | `0x7F000` | `0x05A7F000` |
| `0x60000` | `0x7A000` | `0x05A7A000` |
| `0x61000` | `0x7D000` | `0x05A7D000` |
| `0x62000` | `0x7B000` | `0x05A7B000` |
| `0x63000` | `0x7E000` | `0x05A7E000` |

All six 4 KiB comparisons are byte-for-byte exact, covering 192 128-byte
blocks. The file offsets are sectors 188–199 within `ST00_1_X.BIN`. The same
snapshot has no aligned 128-byte matches from this file in low/high work RAM or
VDP1 VRAM. This identifies the traced `0x25A7xxxx` destination family as the
SCSP data path, not the player tile source at `0x06003000`; audio is the likely
use, but the exact consumer still needs confirmation.

A broader extraction covered all 76 `.ARC` files in the ISO. It located
character art in two later first-stage packages: `ST0_1_0.ARC` type 14 at
`0x4C698` (`0x17000` bytes) and `ST0_1_1.ARC` type 35 at `0x800`
(`0x2E700` bytes). Their 4-bpp contact sheets are
`build/rockman/contacts/ST0_1_0_type14.png` and
`build/rockman/contacts/ST0_1_1_type35.png`; both show clear X character
graphics. The type-35 payload is arranged as 92 complete `0x800`-byte units
and a final `0x700` bytes. That chunk size is close to the observed `0x780`
VDP1 upload, which is 15 of the 16 128-byte tiles in a `0x800` unit. This is
a plausible frame/staging relationship, not a proven mapping: the archive
loader's destination and the selected frame offset still need to be traced.

An exact 128-byte-tile scan found 252 nonzero tiles from the type-14 payload
and 157 from type 35 somewhere in captured VDP1 VRAM, but none in the
`0x60000–0x607FF` player slot. The matching tiles are in other VDP1 ranges,
including the resident stage atlas; neither payload has nontrivial exact tile
matches in captured high/low RAM or SCSP RAM. Thus the visible character art
is present in disc resources and some related tiles are resident, while the
five commands still read boot-image bytes from their player slot. This does
not prove that the later packages were never loaded: a transformed copy or a
different staging destination remains possible.

The ISO extents place `ST0_0_0.ARC` at LBA 1492 (FAD 1642),
`ST0_1_0.ARC` at LBA 1825 (FAD 1975), and `ST0_1_1.ARC` at LBA 2099
(FAD 2249). The iteration-41 CD PLAY history records a 333-sector read from
FAD 1642, exactly the base archive size; its final-16-entry history does not
show the later two FADs. Since that history is truncated, it is a lead rather
than proof that those packages were skipped. No `0xC010–0xC050` sequence was
found in any of the 76 extracted `.ARC` files or the four extracted `ST00_0/1`
X/Z `.BIN` files. REA's current provider rejected the MDF image, raw SH-2
`0.BIN`, and proprietary `.ARC` package as unsupported formats, so these
results come from local archive extraction and byte-for-byte comparison with
the captured RAM/VRAM snapshots.

The raw typed-payload extraction is now reproducible with
`python tools/diagnostics/extract_x4_arc.py`. It indexes all 76 `.ARC` files,
copies each typed payload unchanged, preserves trailing bytes, and writes
source offsets, header fields, sizes and SHA-256 values to
`build/rockman/arc-extracted/manifest.json`. The current set contains 455
payloads (24,948,204 bytes) and 80,404 trailing bytes. This step does not
decompress or interpret type-specific data. The manifest confirms
`ST0_1_0.ARC` type 14 at `0x4C698`/`0x17000` bytes and `ST0_1_1.ARC` type 35
at `0x800`/`0x2E700` bytes.

The next offline extraction decodes the type-2 animation banks already tied to
the player sprite worker. `python tools/diagnostics/extract_x4_sprite_banks.py`
walks the bank's 32-bit pose descriptors, decodes each distinct word stream,
and records descriptor aliases, compressed spans and SHA-256 values in
`build/rockman/sprite-banks/manifest.json`. It found 237 descriptors / 143
unique sources in `PL00.ARC`, 254 / 121 in `PL01.ARC`, and 237 / 144 in
`PL02.ARC`: 728 descriptors and 408 decoded poses total. Unique output files
contain 830,976 bytes; counting descriptor aliases gives 1,616,896 decoded
bytes. Every descriptor's declared output length matched the independent
decoder, and every compressed stream ended within its type-2 payload. The
export does not claim a palette or runtime destination for every pose and
does not imply that `ST0_1_1.ARC` type 35 supplied the iteration-41 upload.
The known `PL00.ARC+0x380E` worker input is present as
`PL00/pose_007-src-00300E.bin` (1,920 bytes; SHA-256
`62f588c3fe16e56ae08dfdcf8ad7c8087fd3574bde4760c90773d3c5adbfd8b5`), and
the known `PL00.ARC+0x16FD4` source is `PL00/pose_066-src-0167D4.bin` (2,304
bytes; SHA-256 `4dba380897a2de69d7ca524db0e2457083f408efbed73a7c013b84261ecb7880`).

The type-1 payloads were also decoded structurally with
`python tools/diagnostics/extract_x4_type1_frames.py`. The expanded output in
`build/rockman/arc-decoded-all/` covers all 48 ARC files that contain type 1.
The bounded bank/frame/piece parser accepts 46 payloads: 311 bank references
(308 unique), 13,874 frame records, and 216,596 piece records including bank
aliases. Every frame's piece ranges are contiguous, remain inside its bank,
and end exactly at the next bank boundary or payload end. `ST0_0_0` bank
indices 11 and 12 alias the same source at type-1 offset `0x6C9C`.
`STE_0_0.ARC` and `STF_1_0.ARC` do not match this layout; their type-1 bytes
and parser status are preserved without forcing an interpretation. For the 46
accepted payloads, these checks support the bank/frame/piece structure but do
not pin graphic-code flags, atlas base, or coordinate interpretation; those
remain raw BE16 fields in the export.

For visual inspection across the archive set, run
`python tools/diagnostics/preview_x4_arc_graphics.py` produces 102 grayscale
index PNGs in `build/rockman/arc-previews-expanded/`: type 5 (45), type 14
(39), type 17 (3), type 27 (3), and type 35 (12). The index records each source
archive offset and hash. The preview
lays out packed nibbles as 16x16 tiles, 16 tiles per row, and scales indices to
grayscale. This reproduces the existing type-5/type-14/type-35 contact sheets;
the PL00 type-17 view shows the atlas already matched to VDP1 VRAM, and type 27
shows the interface sheet. Palette colors and exact in-game display remain
unknown, so type 17 and 27 previews remain exploratory.

`python tools/diagnostics/package_x4_arc_analysis.py --overwrite` bundles the
byte-exact payloads, decoded pose/frame data, and previews in
`build/rockman/x4-arc-analysis.zip`. REA accepts this standard ZIP container
and indexed it completely (959 graph nodes, 1,093 occurrences). REA only
inventories the ZIP directory; ARC semantics still come from the local
manifest and bounded decoders above.

The runtime now writes a frame-prefixed VDP1 command list and VRAM dump inside
the capture callback. This keeps the list and texture snapshot aligned even
when the displayed bank differs from the bank being drawn or the guest later
changes the command list. The existing `vdp1-command-list.txt` at the output
root remains the final post-run state. The command-list capture change compiled
in Release, but has not yet been used in a replay of the corrupted player
scene.

Iteration 46 used the iteration 41 button schedule and `--vblank-blocks
25000`, but it did not recreate the same gameplay state: its captured frame
360703 is a sky-only frame with no VDP1 sprites, and its final CD FAD was
65241, compared with 12965 in iteration 41. It reached the same final guest
block, but this is not evidence that the player scene or sprite glitch was
reproduced. Its separate VDP1-write trace observed DMA0 source alias
`0x86003000`; the frame 104998 descriptor and texture bytes provide the direct
evidence for the corrupted scene. Iteration 47 repeated the divergent route
and again ended at FAD 65241 with a sky-only capture and no VDP1 sprites. Its latest DMA history contains
repeated 832-byte copies from `0x06060594` to the VDP1 base `0x25C00000`, from
block `0x06034B5C` with caller `0x0F000018`; that is separate from the earlier
texture upload implicated by the sprite trace. The trace confirms the tail is
dominated by repeated VDP1-base transfers, so the runtime now keeps transfers
overlapping VDP1 texture VRAM separately in
`scu-dma-vdp1-texture-transfers.txt` (physical range `0x05C60000–0x05C7FFFF`,
including the `0x25Cxxxxx` alias). This filtered history helps avoid losing
texture transfers among the repeated command-list copies in the final history.

A prior room frame in `player-test-iter38-finish-opening/captured-frame.png`
shows X rendered normally; its run ended with 184 VDP1 commands, including 159
sprites. That comparison narrows the problem to the later scene/asset path, but
it does not yet distinguish bad stage data from stale or incomplete VDP1 work.

The iteration 41 layer diagnostics are under
`build/artifacts/player-test-iter41-start-stage/video-diagnostics/`:
`frame_104998_vdp1_bank0.png`, `frame_104998_vdp1_bank1.png`,
`frame_104998_final.png`, and the corresponding `frame_104998.txt` snapshot.

The disc inventory is now split into a route matrix for all 114 `STxx` `.BIN`
files across 80 numbered prefixes. It records each file's LBA and size, and
marks the `ST00_0_Z.BIN` and `ST04_0.BIN` starts observed in the repeated
route. The iteration-41 final FAD 12965 also maps to `ST00_0_X.BIN`; its
historical trace lacks per-sector records, so the matrix has not yet been
refreshed with that evidence. These prefixes are resource groups; they have
not yet been mapped to named gameplay phases. The matrix is
`build/artifacts/stage-resource-matrix-20261011.tsv`.

The launcher now supports `--trace-cd-sectors` with `--frame-dir`. It writes
`cd-play-trace.csv` for every PLAY range and `cd-sector-trace.csv` for each
sector consumed by PLAY or fetched by GET SECTOR. Iteration 48 logged 41 PLAYs
and 37,668 sectors with no dropped records, but reached only FAD 1642–1974 for
the first-stage resource; FADs 1975 and 2249 were never consumed. Its sky-only
capture had no VDP1 sprites or SCU DMA into the X sprite source range. The
iteration 41 route still needs to be recovered before those stage overlays can
be followed in a complete runtime trace; its FAD 12965 association only
identifies the X resource start, not the full read or its RAM destination.

## Remaining work

The launcher is a native x86-64 executable that runs the Saturn runtime and
executes the recorded opening/title and two phase-resource paths with static
native blocks. The module is trace-guided. The disc has 174 `ST*` files (114
`.BIN`, 60 `.ARC`) across 80 numbered `STxx` prefixes; only the `ST00` and
`ST04` resource starts have been correlated with recorded routes so far. Remaining
work includes the other stages and resource paths, version-aware same-address
overlays, a second SH-2 executor, and native SCSP/68K execution plus remaining
sound hardware. REA inspected the generated PE; its Windows Ghidra provider
could not analyze the MDF directly, so disc identity and inventory come from
the local `saturn-disc` parser.

## Player-route replay and AOT activation (2026-10-12)

The bounded repeat of the iteration-41 input schedule again took the divergent
route seen in iteration 48. Its single capture at block 490,000,000 is frame
360703 with sky only, empty VDP1 framebuffers, and no sprite commands. The run
completed 510 million JIT-native block executions with no interpreter
fallback, but it is not a replay of the corrupted-X frame.

The complete trace records 41 PLAYs and 37,668 sectors with no drops. PLAY 17
reads exactly FAD 1642–1974, the 333 sectors of `ST0_0_0.ARC`. Neither FAD
1975 nor 2249 is consumed in this route, so the guest does not request
`ST0_1_0.ARC` or `ST0_1_1.ARC` here. PLAY 18 requests 3,988 sectors at FAD
18061 (`ST00_0_Z.BIN`). PLAY 38 requests 5,004 sectors from FAD 42397; the
contiguous extents are `ST04_0.BIN` (192 sectors) plus `ST04_1.BIN` (4,812
sectors), both already extracted. This supports a route/resource-selection
explanation for the missing later first-stage packages, not a CD transfer
truncation. Whether that route difference caused the player glitch remains
unproven.

The final FAD 12965 recorded in iteration 41 maps to the start of
`ST00_0_X.BIN` (LBA 12815, 1,284 sectors). The current replay's PLAY 18 starts
at FAD 18061, the start of `ST00_0_Z.BIN` (LBA 17911). This points to a
different X/Zero resource-selection path as part of the divergence; the exact
SMPC event or guest state that caused it is not yet recorded. Existing
iteration 32 also starts at FAD 18061 and ends with 25 VDP1 commands and zero
sprites; iteration 41 instead reaches the FAD 12965 X stream and captures 35
sprites. This strengthens the route-divergence finding, but does not establish
that character choice caused the bad X texture.

At frame 360703, `[0x06068598]` is zero. The source range
`0x06003000–0x0600377F` has only the 72 initialization CPU stores (last at
step 123,325), no SCU writes, and still matches `IP.BIN+0x1000` except for 16
bytes. VDP1 VRAM `0x60000–0x6077F` is an exact copy of that source. The run
records 2,643 DMA uploads from `0x06003000` to VDP1 alias `0x25C60000` via the
guest path at `0x06012DBA` / block `0x06012DBC`. This proves the runtime copies
the selected bytes faithfully in this route; it does not show the X sprite,
so it is insufficient to patch the asset loader safely.

The selective texture trace also records 145 SCU uploads from the CD buffer
`0x25818000` to physical VDP1 texture VRAM `0x05C7xxxx`, each 2 KiB. Exact
byte matching against `ST04_0.BIN`, `ST04_1.BIN`, and the two first-stage ARC
overlays found no match in the sampled frame regions; the file-to-upload
offset mapping remains open. The full capture occupies 12,418,749 bytes and
preserves one frame, not a frame sequence.

The Release launcher is now configured to embed the existing trace-guided
module `build/rockman/rockman_player_stage_observed.cpp` through
`SATURNBOX_STATIC_MODULE_SOURCE`. A short 30-million-block replay verified
the active AOT path: 13,691 static functions were available; 26,762,812 block
steps executed statically, 3,237,188 through native JIT, and zero through the
interpreter. The build source is an ignored generated artifact; the CMake
option and launcher support are the tracked implementation. This advances the
x86-64 native path, while complete overlay coverage, the second SH-2 and native
SCSP/68K execution remain outstanding.

REA accepted the rebuilt launcher as PE x86-64 and its Ghidra workflow reported
8 segments, 14,545 procedures, and 1,770 strings. This describes the native
host launcher only; REA still does not analyze the MDF, raw Saturn `0.BIN`, or
the original SH-2 instruction stream. Those remain under `saturn-disc`,
`saturn-sh2`, and `saturn-aot` analysis.

## Resolver and AOT route experiments (2026-10-12)

A temporary replay using the older `RuntimeBlockResolver` ran for 30 minutes
and stopped at step 146,000,000 in block `0x0603084C`, before the first input at
step 380,000,000. Its log is 49,749 bytes. It did not reach the X/Zero route
selection, so it neither confirms nor rules out resolver behavior as the cause
of the divergence. The log is
`build/artifacts/player-stage-legacy-resolver-20261012/run.txt`.

The source was restored and Release rebuilt with
`SATURNBOX_STATIC_MODULE_SOURCE` set to
`build/rockman/rockman_player_stage_observed.cpp`. A bounded AOT comparison was
configured for 510,000,000 steps, one frame at step 490,000,000, CD-sector trace
only, no general trace, a 45-minute wall-clock limit, and a 100 MiB artifact
limit. The process hit the time limit before writing the frame or final summary;
`run.txt` contains only `VDP1 renderer: CPU reference`, and the output directory
`build/artifacts/player-stage-aot-route-20261012/` is 32 bytes. The last guest
step and selected route are unknown.

The saved iteration-41 and iteration-48 summaries show the same SCU schedule
(`period=25000`, `vblank=20400`, `vblank-out=20399`, `dropped=0`), so their
route difference is not explained by a changed VBlank period or dropped sector
records. CD command counts differ substantially (`0x10`: 99 vs. 41; `0x48`: 81
vs. 1171; `0x51`: 109143 vs. 75175), confirming different guest control flow;
the game's sampled SMPC state was not recorded.

Iteration 41's final CD summary records a PLAY at FAD 12965 with a 1,284-sector
filter range and 5,096 sectors requested/retained. Its last CD transfer was LBA
13241, 426 sectors after `ST00_0_X.BIN` begins at LBA 12815. This proves activity
on the X stream, but the absence of per-sector tracing in that iteration leaves
the complete transfer and final RAM destination unknown.

Since the iteration-41 build, `RuntimeBlockResolver` gained live-code snapshots
and fingerprints so it can invalidate resolved SH-2 blocks after overlay
mutation. That is a plausible semantic change to compare against the route
divergence. The added CD trace calls are gated by the capture flag and do not
change CD state. The legacy-resolver run stopped before the first input at step
380,000,000, so the resolver hypothesis remains untested at the route decision.

These experiments are inconclusive and do not justify a loader patch. The
directly established failure remains that iteration 41's five X commands read
tiles from `0x06003000`, which still contains almost entirely `IP.BIN` bytes;
the routine expected to populate that source was not observed. The visual fix
and any relationship to FADs 1975/2249 remain unresolved. The current resolver
and AOT CMake setting are restored, and no previous artifacts were removed.

## Selective input and CD trace (2026-10-12)

`saturn-sh2 boot` and `launch` now accept `--control-trace <events.csv>` and
`--control-trace-from-step N`. This opt-in trace records scripted pad changes,
SMPC reads/writes, and complete CD command packets at the CR4 commit, with the
SH-2 step, block, and caller. It omits high-volume CD status command `0x00`,
does not enable general MMIO snapshots, and retains at most 262,144 events; the
CSV reports any dropped records.

A one-million-step run with the existing ISO and `--pad1 start` verified the
trace path without producing a frame. From step 200,000 it recorded 1,730
events (226 SMPC reads, 89 SMPC writes, and 1,415 CD commands), with no drops.
The game issued INTBACK at step 350,029 and later read OREG bytes `F1 02 F7 FF
F0`; `OREG2=F7` reflects the active Start bit. The trace file is
`build/tmp/control-trace-boot-smoke-20261012.csv` (139,553 bytes). This proves
that the configured input reaches the game's SMPC reader, but does not reach
the X/Zero route decision or verify the sprite rendering.

A filtered route replay has now covered steps 375,000,000 through 510,000,000.
It retained only scripted pad events, OREG2 reads, and CD packets, plus one
frame at step 505,000,000. Its results and the still-open producer trace are
recorded below; the route did not reach the player stage.

## Route prefix through step 420,000,000 (2026-10-12)

A headless replay stopped at step 420,000,000 and retained control events from
step 375,000,000. Its CSV is 19,763,403 bytes with 262,144 retained events and
49,991 older events dropped; all three input changes remain in the retained
window. The file is `build/tmp/control-trace-route-prefix-20261012.csv`.

The game read the programmed controls back through OREG2: Start (`0x10`) as
`0xF7` at step 380,001,138, right (`0x01`) as `0x7F` at step 385,007,737, and A
(`0x20`) as `0xFB` at step 400,007,113. These first three inputs reached the
game's SMPC reader in this replay. The old iterations have no sampled OREG
values, so this alone does not explain the route difference between them.

The first post-Start PLAY is at step 380,008,582 for FAD 654, 46 sectors. Later
PLAYs request FAD 11,923 at step 380,556,509, FAD 48,713 at step 381,854,492,
and FAD 42,397 for 5,004 sectors at step 393,804,874. The last spans
`ST04_0.BIN` and `ST04_1.BIN`. The retained window ends at step 419,994,221 and
does not yet contain a PLAY at FAD 1642; the scripted input at step 420,000,000
was not applied before the run hit its step limit. This prefix narrows the
early route but does not yet explain why iteration 48 never consumed FADs 1975
and 2249.

## Filtered control replay through step 510,000,000 (2026-10-12)

`--control-trace-oreg2-only` now keeps only scripted pad changes, OREG2 SMPC
reads, and CD packets, preventing high-volume polling from evicting route edges.
The replay retained 194,535 events with no drops in a 14,800,626-byte CSV. It
recorded 155,518 OREG2 reads, 39,009 CD commands, and all eight input changes.
The one-million-step filtered smoke run also retained the expected Start value
`OREG2=0xF7`.

The full replay reached 510,000,000 steps and captured one frame at 505,000,000.
The game consumed Start, right, A, right, A, right, right+B, and release at
their programmed steps; the corresponding OREG2 values were `F7`, `7F`, `FB`,
`7F`, `FB`, `7F`, `7E`, and `FF`. Despite this, no PLAY at FAD 1642, 1975, or
2249 appears in the recorded step-375M-to-510M window. Its ten PLAYs were at
FADs 654, 11923, 48713, 749, 1051, 4378, 42397, 700, 65240, and 65241. The old
iteration-48 sector trace does show FAD 1642 for the 333-sector
`ST0_0_0.ARC`, but it has no OREG samples, so the route difference cannot yet
be assigned to a particular input. The command totals also differ (`0x48`:
1080 vs. 1171; `0x51`: 73906 vs. 75175), despite both summaries recording 41
PLAY commands.

The sole captured frame is sequence 329764 and contains 25 VDP1 commands, zero
sprites, and empty VDP1 banks/framebuffer. The source probe retained 72 CPU
stores in `0x06003000–0x0600377F`, all by step 123325, with no SCU DMA writing
that range. The same run records 2,643 RAM-to-VDP1 texture DMAs from
`0x06003000` to alias `0x25C60000` through caller `0x06012DBA` / block
`0x06012DBC`; the last three copies occurred at steps 418016701, 418024299, and
418042515. At the captured frame, the 1,920-byte source snapshot matches
`IP.BIN+0x1000` at 1,904 positions and matches the iteration-41 RAM snapshot at
all 1,920 positions. VDP1 VRAM `0x60000–0x6077F` exactly matches that source.
This confirms the RAM-to-DMA-to-VRAM transfer is faithful and places the known
bad payload upstream of DMA. It does not show player gameplay and does not
justify patching the loader yet.

The trace also records 145 2-KiB CD-buffer-to-VDP1 transfers from `0x25818000`
to `0x05C7xxxx`, between steps 12340932 and 389541042; their source file and
offset mapping remains open. The replay CSV is
`build/tmp/control-trace-route-filtered-20261012.csv` (14,800,626 bytes). Its
single-frame diagnostics occupy 11,140,811 bytes under
`build/artifacts/player-stage-control-filtered-20261012/`. The tail and smoke
traces plus the one-frame diagnostics total 45,384,735 bytes, below the
100-MiB capture budget. No older artifacts were removed.

## Valid X-stream replay through step 320,000,000 (2026-10-18)

The short route reproduced the valid PLAY at FAD 14,249 with CR3=`0x80` and
CR4=`0x0EE4` at step 253,109,169. At the 320M step limit the stream had reached
sector 3,628 of 3,812 (last FAD 17,876), so this replay does not yet establish
whether the stream completes or schedules the stage-art archives afterward.
The CD sector trace contains no reads from `ST0_1_0.ARC` (FAD 1975, 274
sectors) or `ST0_1_1.ARC` (FAD 2249, 94 sectors).

The frame captured at step 315M contains 43 VDP1 commands, including 17
sprites. However, 2,032 of 2,048 bytes in RAM `0x06003000–0x060037FF` still
match `IP.BIN+0x1000`, and VDP1 VRAM `0x60000–0x607FF` matches that RAM span.
The scene is rendering sprites, but the capture does not show correct X tiles
in the known player texture source. The lightweight FIFO trace contains 6,152
32-bit reads from `0x25818000` at block `0x060152DC`, with destinations in
SCSP alias `0x25A7xxxx`; it contains no reads from word FIFO `0x25898000`.
This is consistent with the previously mapped SCSP data path and does not
identify a player sprite transfer. Artifacts are under
`build/artifacts/player-stage-x-right-resource-complete-20261018/` and in
`build/tmp/player-stage-x-right-resource-complete-20261018.txt`.

Next, extend this exact input route beyond the 3,812-sector boundary and trace
the subsequent CD commands and sectors. If the two ARC extents remain absent,
advance the gameplay transition that should schedule them before changing the
loader or video pipeline.

## Repeated stage stream through step 330,000,000 (2026-10-18)

The 320M replay stopped before the valid FAD 14,249 PLAY reached its end. In
the follow-up, the 3,812-sector pass ended at FAD 18,060, cursor 3,811,
sequence 21,767. The next sector record (sequence 21,768) restarted at FAD
14,249, cursor 0. The control trace identifies the repeated 3,812-sector PLAY
at step 323,557,173, block `0x0603554C`, caller `0x060354C8`, with CR1=`0x1080`,
CR2=`0x37A9`, CR3=`0x80`, CR4=`0x0EE4`. At 330M, the repeated pass had reached
FAD 14,778, cursor 529. No sectors from stage-art extents FAD 1975 or 2249
appear anywhere in this trace.

The frame at step 328M (sequence 95,475) has 43 VDP1 commands, including 17
sprites. RAM `0x06003000–0x060037FF` still matches `IP.BIN+0x1000` at 2,032 of
2,048 bytes, and VDP1 VRAM `0x60000–0x607FF` exactly matches that RAM range.
Thus the repeated stream is not filling the known player texture source. The
filtered control trace retained 16,031 events without drops; no scripted input
occurred in that interval. Next, advance the gameplay transition and record
the first request for FAD 1975/2249 alongside writes to the player buffer.

## In-stage right-input replay through step 340,000,000 (2026-10-18)

The next replay held right from step 330M through 335M, then released it. The
filtered trace recorded both pad events and sampled OREG2 as `0x7F` for the
right press and `0xFF` after release. The frame captured at 335M (sequence
97,100) shows the stage scene shifted relative to the 328M frame. VDP1 command
count rose from 43 to 282 (17 to 255 sprites), confirming that the game
responded to right in the stage.

The player sprite remains corrupted. RAM `0x06003000–0x060037FF` still
matches `IP.BIN+0x1000` at 2,032 of 2,048 bytes; VDP1 VRAM
`0x60000–0x607FF` exactly matches that RAM span. At 340M the repeated FAD
14,249 stream had reached cursor 1,043; the CD sector trace still has no reads
from FAD 1975 or 2249. Thus the input path reaches gameplay, but the movement
does not trigger the stage-art reads or fill the known texture source. This
keeps the next investigation on the game-side GFS request/initialization path,
not a speculative CD, DMA or VDP1 change. Artifacts are under
`build/artifacts/player-stage-x-right-in-stage-20261018/` and in
`build/tmp/control-trace-x-right-in-stage-20261018.csv`.

The filtered DMA trace closes the rest of the render chain: SCU channel 0
copies from `0x06003000` to VDP1 alias `0x25C60000` through block
`0x06012DBC` / caller `0x06012DBA`, with counts `0x880` and `0x900`,
`from_cd=0`. The largest transfer range still matches `IP.BIN` byte for byte,
and VDP1 VRAM `0x60000–0x608FF` matches that RAM range. The X commands
continue to source `0x60000` and `0x60080`. Input, game response, DMA, and VDP1
are therefore observed end to end; the missing step is loading or selecting
the character art before DMA.

## GFS filename lookup probe through step 156,000,000 (2026-10-18)

The Release build added a bounded string capture at the GFS name matcher
(`0x0603AD4A`). The name key is the pointer in R5; R1 is a stack pointer and
must not be treated as a C string. The shortened replay recorded keys
`ST00_0_X.BIN` and `ST00_1_X.BIN` at steps 104,294,008/285 and
140,869,633/910. It counted 25 GFS resource-open and name-matcher entries,
all reaching the descriptor-ready path, with no not-ready or operation-error
path hits. Log: `build/tmp/player-stage-gfs-name-probe-20261018.txt`.

This run diverged before the matcher activity at 155.66M seen in the previous
longer trace (`0x060124B0` versus `0x0022A4AA` at step 155M). The short probe
therefore identifies the stream filename keys but does not identify the stage
ARC loader. It adds no evidence that `ST0_1_0.ARC` or `ST0_1_1.ARC` was
requested. Continue with the same capture configuration and route that
reaches the first-stage frame, record the string-comparison arguments at stage
initialization, and keep the known input-to-DMA path unchanged until the
upstream resource request is identified.

## Sprite producer confirmed from the iteration-41 snapshot (2026-10-19)

The missing `0x780` bytes are produced by the slave SH-2 routine at
`0x06016F98`. The pending mailbox contains callback/source/destination at
`0x060604D0/D8/DC`: `0x06016F98`, `0x060BE04E`, `0x06003000`.
The source is in the already resident **PL00.ARC type 2**, file offset
`0x380E`. All `0x504` consumed source bytes match `[0x380E,0x3D12)` of the
archive. Executing the original slave polling loop and decoder produces
exactly `0x780` bytes and clears the mailbox after 2,645 cooperative blocks. Interpreter,
JIT and independent file decoding agree on all 1,920 output bytes.

The runtime now shares cached/cache-through WRAM backing and executes the
firmware-selected slave entry cooperatively, with private CPU registers and
on-chip devices. MINIT resumes the slave until its next idle FRT poll. This corrects the
observed producer path without introducing a type-35 archive mapping or a
host implementation of the game's decompressor. Slave interrupts/exceptions
and service duration/dual-CPU cache/bus timing remain outside this polling subset.

Controlled rendering of the same captured command list and video memories
with only the decoded texture span replaced shows an intact blue X. Changed
pixels are confined to `[149,176) × [146,194)`. These are snapshot renders;
live boot-to-stage validation is tracked in
`ROCKMAN-X4-PLAYER-TEST-CHECKPOINT.md`. Reproducible diagnostics are
`saturn-x4-sprite-probe` and `tools/diagnostics/validate_x4_sprite_job.py`;
the proof is in `build/artifacts/player-x-slave-job-proof-20261019/`.

The corrected boot-to-stage replay completed 108M master blocks with zero
interpreter fallback. Frame 33671, captured at 106M, shows intact blue X.
Its complete `0x900`-byte upload matches both RAM and independent decoding of
`PL00.ARC+0x16FD4` byte for byte. See
`build/artifacts/player-x-slave-yield-scene-20261019/texture-validation.json`.
The earlier one-block-per-core replay is retained as a negative case: its
apparently improved image hid a partial new/previous pose upload. The capture
validator rejects that case and accepts the cooperative polling service.
