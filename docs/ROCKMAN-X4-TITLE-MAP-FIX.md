# Rockman X4: movie cleanup and the title map

Investigation continued from the uncommitted NBG0 producer/reader traces on
2026-09-27. The original captures and reports are retained. This report corrects
two earlier interpretations: the 43 nonzero values are not a title map, and an
unmatched DMA-history join does not prove a CPU store.

## Why the 43 entries disappear

`0x06016C1C` calls the movie player `0x06015B24` for `LOGO.CPK`.
Its setup calls `0x060166C4`, which calls `0x06016444` at `0x06016752`.
The loop at `0x0601667C` writes 44 longwords, starting with zero, at
`0x25E70004 + i*8`, with value `i*0x400` (`i=0..43`). Thus there are exactly
43 nonzero stores, ending at `0x25E7015C = 0xAC00`. At this point
`CHCTLA=0x0042` and `VCSTA=0x70000`: this is the movie's vertical cell scroll
table in bitmap mode, not pattern names for the title.

The exit path at `0x06015D70` tests movie status and the Start press bit.
`0x06015D8C` calls cleanup at `0x06015DFC`. Cleanup disables display, sets the
register shadow's `CHCTLA` to `0x0101` at `0x06015E80`, and requests a full
`0x80000`-byte VRAM zero fill through `0x06034A84` / `0x06034AD8`.
The block at **`0x06034B5C` is a DMA-start block**: its store at `0x06034B68`
writes `0x0101` to SCU channel enable (`0x25FE0010 + channel*0x20`). The VRAM
writes inherit this CPU context from the bus interceptor. The clear at step
8673634 therefore intentionally removes the movie table as part of a full
VRAM clear.

`0x060192AC` also queues ordinary map clears through `0x06012E60`; the zero
source at `0x06045128` and map addresses in the following table are not asset
data. `0x06012DBC` is another DMA-start path. Earlier CSV files labelled some
of these writes `cpu-store` because transfer history stops at 1024 records.
New CSV files use `unmatched` when no descriptor was retained. The old files
remain unchanged as historical evidence.

The first `CHCTLA=0x0101` frame is an intermediate blank setup before the
opening movie and title assets. Preserving the scroll table or copying it back
would corrupt the intended cell map. A blank map at that first transition is
not evidence that a title map was lost.

## Where the title map comes from

The main-program state machine loads `ASCII.ARC`, then `X4LOW.X4` at
`0x00220000` (`0x06016C90`), and queues `SNDCOM.ARC` (`0x06016D3C`).
Once the sound load and fade finish, state 8 at `0x06016E1E` switches the
coroutine to **`0x00220700`** through `0x060102BE` / `0x06010064`.
The overlay's state 0 plays `OP.CPK` through the same movie player.
State 1 dispatches through `0x002210D0` to title setup **`0x00220800`**.

Title setup sets stage/substage to `14/0` at `0x00220822..28` and calls
`0x0601D17C`. Its filename builder selects **`STE_0_0.ARC`**, loaded through
`0x0601C920` at `0x0601D498`. `CAPCOM.ARC` belongs to a different path.
The title is assembled from the overlay and archive, not loaded as a flat
`0x70000` map blob:

| Data | Source and destination |
| --- | --- |
| Screen-block layout | `X4LOW.X4` table `0x0028C7EC`, stage-14 entry at `0x0028C85C`, points to `0x002904F7`; copied to `0x0607C000` by `0x0601A7EC` |
| 16x16 block indices | Archive kind 4 to `0x00200000`, then `0x0601696C` copies to `0x0607D000` |
| Pattern-name definitions | Archive kind 3 to `0x0609D000` |
| Character pixels | Archive kind 5 to VDP2 `0x25E00000` |
| Palette | Archive kind 10 to `0x0605DF5C` |

`0x0601D540..52` installs the three map pointers at
`0x0600D024/28/2C`. `0x0601A7EC -> 0x06019C48 -> 0x06019D92 -> 0x06019FC8`
expands the layout and tile indices into VDP2 pattern names. In particular,
`0x0601A182..B4` reads a tile word, indexes the name definitions with its low
14 bits, combines flip bits, and stores the resulting longword through R7.
The plane address table at `0x06045130` supplies `0x25E70000` for NBG0.

## Changes

* CD positional SEEK now transitions from SEEK (4) to PAUSE (1) on the next
  subcode tick. The synchronous response remains intact until CR4 is read.
  SEEK/PAUSE/STOP disable the old PLAY sector producer while retaining buffered
  partitions. Previously the guest status loop at `0x0603C420..466` waited
  forever, leaving `SNDCOM.ARC` busy without issuing its next PLAY.
* The SH-2 runtime resolver can recover reached blocks from live machine
  memory when they are outside the boot image. It decodes only the reached
  straight-line block and its delay slot, avoiding speculative overlay data.
  The runner supplies that memory. The old `0x0022750C` return/copy substitute
  is removed: this is real overlay code and now executes normally. Its short
  copy targets VDP1 `0x25C7FF00`, not the title map.
* The existing uncommitted renderer correction is retained: 16x16 characters
  use one quarter of the 8x8 pattern-name page size, and map bases wrap within
  512 KiB VRAM. For `MPOFN=0x0111`, `MPABN0=0x3030`, two-word names and a
  2x1-page plane, NBG0 starts at `0x70000`, not the old wrapped `0x40000`.
* The diagnostic runner accepts `--pad-after-blocks N` with `--pad1/--pad2`
  to reproduce a button press after boot rather than holding it throughout
  initialization. Unmatched map writes are no longer mislabelled CPU stores.
* Title-transition validation exposed a separate input blocker: SMPC marked
  its first peripheral packet as a continuation, echoed port modes into the
  wrong SR bits, and never delivered INTBACK completion to SCU. The guest
  handler `0x06033E0A` / `0x06033F3C` consequently could not publish the Start
  state consumed at `0x06015D7A`. The first packet now has PDL=1, NPE=0 and
  mode bits in SR[3:0]; completion requests vector `0x47`, and IREG0 BREAK
  stops the armed exchange. The game's mode-2 request at `0x060107E8` also
  requires the initial status packet and a bit-7 CONTINUE before peripheral
  data. That handshake is now honored; RTC/SMEM contents remain zero in this
  HLE subset. Existing SMPC tests cover both packets, the status fields,
  masked interrupt retention, and absence of refresh/interrupt after BREAK.

The seek behavior follows Sega's CD technical bulletin: positional SEEK ends
in PAUSE ([Sega technical bulletins](https://antime.kapsi.fi/sega/files/Sattechs.pdf)).
No title bytes, game-state shortcuts, or suppressed clears are introduced.
The SMPC fields and completion interrupt follow the
[SMPC manual, section 3.1, figure 3.13](https://antime.kapsi.fi/sega/files/ST-169-R1-072694.pdf).

## Validation and preserved captures

Release build and CTest pass (90 tests). New tests cover seek completion,
response acknowledgement, retained sectors/no stale PLAY production, and
execution of an overlay loaded after initial discovery, including PC-relative
data, a delayed branch, zero instruction bytes, and return to static code.
The existing renderer matrix covers NBG0/NBG1, both character and name sizes,
and three plane layouts.

Captures under `build/artifacts/` are intentionally untracked:

* `title-seek-fix-20260927-run.txt`: sound read completes (163 sectors from
  FAD 12606), exposing the subsequent missing-overlay-execution boundary.
* `title-overlay-fix-20260927-run.txt`: intermediate failed experiment; not a
  successful title validation.
* `title-final-fix-20260927-run.txt` and matching map CSV directory: normal
  execution reaches the opening movie after the two runtime fixes.
* `title-final-start-20260927-run.txt`: Start held since boot; remains in the
  opening movie and does not establish successful title presentation.
* `title-final-delayed-start-20260927-run.txt` and matching map CSV directory:
  delayed Start at block 13700000 before the SMPC correction; also stays in OP.
* `title-smpc-status-only-partial-20260927-run.txt`: partial investigation with
  corrected packet status but no completion interrupt; deliberately stopped.
* `title-smpc-interrupt-20260927-run.txt` and matching CSV directory: completion
  interrupts alone still do not satisfy the guest's status+peripheral request.
* `title-map-complete-input-20260927-run.txt` and matching CSV directory:
  completed 10-million-block run with Start held, using the complete input fix.
* **`title-map-final-20260927-run.txt`** and matching CSV directory: completed
  20-million-block run with normal logo playback and Start applied at block
  13700000 during `OP.CPK`. This is the final transition validation.

Static evidence is also saved as `title-map-code-path-20260927.txt` (annotated
instructions) and `title-map-sources-20260927.json` (asset SHA-256 hashes,
archive descriptors/destinations, and title layout bytes).

Reproduction uses the existing boot command from the producer report, with
`--steps 20000000 --pad1 start --pad-after-blocks 13700000`, both
`--vdp2-map-trace` and `--vdp2-map-read-trace` pointing to a fresh directory.

### Final replay result

The CD PLAY history reaches FAD **11923, 112 sectors**, the title archive.
The first nonzero title-map store occurs at block step **16327256**:
`0x0601A182` (store instruction `0x0601A1B4`) writes **`0x00574484`** to
**`0x25E71208`**. This value is in the kind-3 definitions at archive file
offset `0x1084`; that resource starts at `0xC00`. There are **234 nonzero map
stores**, all matching the archive definitions after the guest's flip-bit
composition. The independent validator passes for both completed input runs:

```powershell
python tools/diagnostics/validate_title_map_trace.py `
  build/artifacts/title-map-final-20260927 build/artifacts/STE_0_0.ARC
```

Frame **4440**, step **16328051**, already has nonzero map words with
`CHCTLA=0x0101`, `BGON=0x0003`, and NBG0 enabled. At the end of the run the
map retains **164 nonzero 16-bit words**. Thus the earlier permanent empty-map
condition is gone without modifying the game's clear or resource-loading code.

The renderer's later NBG0 reads stay in the corrected `0x70000..0x71FFF`
plane. Those visible samples are still zero: populated names begin in the
second page, and the game subsequently sets `BGON=0x0006` (NBG0 disabled).
This verifies resource provenance and map availability, not a pixel-perfect
title-screen rendering. The composed frame is nonblack, but that alone does
not establish which layer supplies its pixels. Unskipped `OP.CPK` audio/
playback completion was not fixed or certified here; the final run uses the
game's own Start-skip path.

Per-run `validated-source.json` files retain the validator's exact results.
At that milestone, Release CTest passed 90 tests and `git diff --check` passed.

## VDP1 sprite replay (2026-10-03)

The release harness completed 31,020,000 block steps and captured frame 23,020
after applying Start during the title transition. The command-list walk reported
99 commands: 74 sprites and 5 polygons. The VDP1 banks contain visible pixels
through y=109; the VDP2 registers report `BGON=0x0003`, with NBG0 empty and
NBG1 active across the 320x224 output.

The replay exercised the fixed-sprite path, but its captured frame showed a
dark field with horizontal strips. That image predates correction of the
`CMDSIZE` field order: commands with `size=0x0210` were logged as 128×2, though
the hardware fields describe 16×16 cells. The old pixels therefore cannot
validate the corrected sprite reads or the final title composition. The
snapshot records `PRINA=0x0206`, so explicit VDP2 priorities were enabled;
NBG0 was empty and NBG1 was active. Its `priority_configured=0` field came from
a diagnostic omission: the memory-capture path did not copy the configured
flag. The snapshot now records that flag for every capture and a unit test
covers it. At that point a fresh replay was still needed to inspect the
corrected cell rendering and compare VDP2 layers; the successful replay is
recorded below. The VDP1 final overlay remains simplified.

The new shape-0 test deliberately gives B/C/D distant coordinates and checks
that they do not stretch the sprite; the scaled-sprite test now uses shape 1
with zoom point 0. The first capture also exposed a reversed `CMDSIZE` decode:
`0x0210` is a 16x16 cell, not a 128x2 strip. The renderer and command-list
diagnostic now decode X from bits 13–8 and Y from bits 7–0; a regression test
checks the last pixel of a 16x16 pattern. Shape-1 non-zero zoom points and the
full distorted-sprite mapping remain outside this implementation slice.

### Replay after the size-field fix (2026-10-03)

The memory spike came from `MmioInterceptor` retaining every access with a
copy of the SH-2 execution-context map. The 31.02-million-step replay recorded
33,334,685 accesses, which had driven the earlier run above 18 GiB. MMIO
history now retains only the newest 262,144 entries in chronological order
while keeping a separate total count. The same replay completed with a sampled
working set of about 0.57 GiB and captured frame 23,020 at step 31,000,000.

The corrected `size=0x0210` commands are logged as 16x16 cells, and the logo is
now legible instead of appearing as horizontal strips. The isolated VDP2
images show NBG1 filling all 320x224 pixels with `#000020`, while NBG0 is
empty. VDP1 bank 0 has 29,863 non-zero pixels through y=109; its fill rectangle
and logo occupy the upper part of the output, while the lower part reveals the
NBG1 background. `BGON=0x0003`, `PRINA=0x0206`, and
`priority_configured=1` confirm that both layers are enabled and VDP2
priorities are configured. This validates the corrected sprite read and
separates the visible layers for this frame; the remaining VDP1 polygon and
distorted-sprite behavior is still simplified.

The retained MMIO window covers the final portion of the replay, including the
VDP2 register and map writes around the capture. The full VDP1 VRAM dump and
per-layer PPMs are in
`build/artifacts/title-video-vdp1-bounded-20261003/`. Release CTest and the
MSVC AddressSanitizer Debug CTest both pass all 93 tests. The separate
`ENABLE_SANITIZERS` enables ASan+UBSan with GCC/Clang in `-fsanitize` mode and
ASan with MSVC; this host's MSVC does not implement UBSan.

### VDP2 multi-page plane and transition capture (2026-10-03)

A focused replay with `--trace-game` captured frame **4441** near the title-map
transition. It reached 16,400,000 block steps; the frame was armed at step
16,328,000. The producer trace again records the first title name at step
**16,327,256**, address `0x25E71208`, value `0x00574484`. At the composition
sample for frame 4440 (step 16,328,051), the `0x70000–0x71FFF` region contains
12 non-zero 16-bit words. They form six two-word names at `0x71208`, `0x71288`,
`0x71308`, `0x71388`, `0x71408`, and `0x71488`.

The map was present, but outside the visible NBG0 sample coordinates. At this
frame `PLSZ=0x0015` selects a 2x1-page NBG0 plane, `CHCTLA=0x0101` selects
16x16 characters, and `PNCN0=0` selects two-word names. The Sega manual gives
4 KiB per 16x16/two-word page and says pages in a multi-page plane are stored
consecutively ([PNT page layout, §4.6](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_60.htm),
[plane layout, §4.7](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_70.htm),
[map address calculation, §4.8](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_80.htm)).
Thus page 1 begins at `0x71000`; the first name at `0x71208` is row 4,
column 2 of that page. With 16x16 characters, those names start at source
coordinate `(544, 64)`. `SCXIN0=0x00C2`, `SCYIN0=0`, and unit coordinate
increments put the 320-pixel viewport at x=194..513, so the six names begin
30 pixels beyond its right edge.

This exposed a renderer issue: it had used the A/B/C/D map registers as though
they selected each page. They select the four planes; `PLSZ` pages within each
plane are consecutive. The cell compositor now picks the 2x2 plane from the
plane dimensions and then adds the row-major page offset. A regression test
checks page 1 within a 2x1 plane, plane B, and page 3 within a 2x2 plane. The
Release build and CTest pass.

After the fix, NBG0 still contributes no pixels to this capture because its
names start beyond the viewport. The renderer records 143,360 NBG0 name-word
reads, all zero. NBG1 supplies a uniform `#000020` background over 320x224;
VDP1 bank 0 supplies 29,884 non-zero pixels in the upper 110 rows. The final
frame differs from the isolated VDP2 frame at exactly those 29,884 pixels,
matching the VDP1 bank-0 mask. The captured RGB pixels are byte-for-byte equal
before and after the page-address correction, as expected for this viewport.

The corrected replay artifacts are in
`build/artifacts/title-vdp2-multipage-fixed-20261003/`; the frame preview is
`frames/captured-frame.png`, and the register/layer snapshot is
`frames/video-diagnostics/frame_004441.txt`. The full map write, map read, and
layer images are kept beside them for continued comparison.

### VDP1 command traversal and sprite format (2026-10-04)

The previous `vdp1-command-list.txt` was a scan of every non-empty 32-byte
block in VDP1 VRAM. It included unrelated texture data after the command-list
terminator, so it did not represent execution order. The trace writer now walks
NEXT/ASSIGN/CALL/RETURN links from offset zero, labels skipped and state-setting
commands, stops at END, and leaves the complete memory image in
`vdp1-vram.bin`.

Walking the captured memory independently produced 99 commands before END:
16 skipped commands, 74 sprites, five polygons, four clipping/local-coordinate
commands, no lines, and no malformed links. All 74 sprites are normal shape-0
sprites with `CMDSIZE=0x0210` (16x16), `PMOD=0x0080`, and color mode 0. This is
the VDP1 4-bit color-bank format: the high nibble of each source byte is the
left pixel, zero is transparent with SPD clear, and ECD is disabled by PMOD bit
7. The Sega manuals describe this encoding in [VDP1 §6.3, character patterns](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p06_36.htm)
and [VDP1 §6.3, end-code handling](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p06_34.htm).

At the captured frame, VDP2 has `SPCTL=0x0625` (sprite type 5, mixed palette/RGB
mode), `CRAOFB=0`, `PRISB=0x0405`, and `PRINA=0x0206`. The sprite words formed
from `CMDCOLR=0x3000`–`0x3030` and the 4-bit texels carry sprite selector 3;
`PRISB` assigns that selector priority 4, above NBG1's priority 2. The old
draw-time palette shortcut happened to produce that frame's layer order, but
discarded sprite priority bits before composition. The runtime now keeps the
16-bit VDP1 framebuffer codes through drawing, resolves RGB or CRAM data at
composition, and compares each sprite pixel's selected 3-bit priority against
the VDP2 layer priority at the same pixel. It follows [VDP2 §9.1, sprite data](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p09_10.htm)
and [§9.2, priority selection](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p09_20.htm).

The first polygon clears the region y=110–239 with color zero. The other four
are the screen-edge border strips at x/y -16..-1 and 320..336/240..256; none
adds visible pixels in the 320x224 capture. Thus distorted sprites and polygon
rasterization are not the next visual blocker for this title frame.

### Per-pixel VDP1/VDP2 priority composition (2026-10-04)

The compositor now preserves the VDP1 16-bit framebuffer codes, including
palette-bank/CLUT values and sprite priority selector bits. At scan-out it
decodes mixed RGB555 versus palette format from `SPCLMD`/the code MSB, selects
one of eight `SxPRIN` values from the sprite type's priority bits, applies
`SPCAOS` with the CRAM-mode-specific `0x200`/`0x400` word stride, and compares
that priority against the per-pixel priority buffer produced by VDP2 layers.
Transparent palette texels leave earlier VDP1 commands and the VDP2 pixel
underneath them intact, and priority zero stays transparent. Equal priorities
use the manual's layer order: sprite above RBG0 and NBG0, then NBG1–NBG3
([VDP2 §11.1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p11_10.htm)).
The `SPCAOS` offset follows [VDP2 §10.1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p10_10.htm).
A regression test draws a palette sprite over NBG0. NBG0 priority 5 hides the
sprite at priority 4; lowering NBG0 to 3 reveals it; at priority 4 the sprite
wins the documented tie. Its transparent neighbor continues to show the blue
NBG0 pixel, verifies that a zero sprite priority stays transparent, and reads
back raw code `0x3001` from the VDP1 framebuffer. A second test gives NBG0 and
NBG1 equal priority and confirms NBG0 wins the tie. The sprite test also sets
`SPCAOS=1` and reads the color from CRAM word `0x201`, checking the offset
calculation.

The replay completed **16,400,000 block steps** and captured frame **4441**
armed at step **16,328,000**. At this frame `SPCTL=0x0625`, `PRISB=0x0704`,
and `PRINA=0x0206`: sprite selector 3 maps to priority 7, above NBG1's
priority 2; NBG0 is empty and NBG1 fills the frame with `#000020`. Per-pixel
comparison between `frame_004441_final.ppm` and `frame_004441_vdp2.ppm` finds
**38,964 changed pixels**, all of which match the decoded VDP1 bank-0 pixel at
that coordinate. This includes black sprite pixels, so the difference count
is larger than the 37,992 non-black pixels in the diagnostic bank image.
The corrected frame visibly shows the Rockman X4 logo and its colored stripe
art over the dark NBG1 field. At the 15-million-step progress sample, the
runtime working set was about 192 MiB.

Artifacts, including per-layer images, CRAM/VRAM/register dumps and map
read/write traces, are in
`build/artifacts/title-sprite-priority-20261004/`. Release CTest and the
MSVC AddressSanitizer Debug CTest both pass all 96 test cases.

### VDP2 color calculation and shadows (2026-10-04)

Composition now keeps the highest-priority pixel and the immediately lower
candidate per screen coordinate. VDP1 is inserted into those two candidates
before color calculation, so a sprite that falls between VDP2 layers can still
be the second image. This bounds retained compositor state to one fixed-size
record per output pixel instead of accumulating full-frame history.

For register-level color calculation (`SFCCMD=0`), `CCCTL` enables NBG0–NBG3
and sprite calculation. `CCMD=0` uses the selected five-bit ratio with the
manual's 32-step weights; `CCMD=1` adds the two image channels and saturates at
255. `CCRTMD` selects the ratio from the top or immediately lower image. Sprite
types 0–7 now decode their per-dot `CC` fields and use `CCRSA`–`CCRSD`; `SPCCEN`
and `SPCCCS`/`SPCCN` gate the operation. The behavior follows [VDP2 §9.2](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p09_20.htm),
[§12.1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p12_11.htm),
and [§15.3](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p15_30.htm).

The compositor also recognizes normal shadow color codes and palette-only MSB
sprite/transparent shadows for sprite types 2–7. Shadows take effect only when
the sprite wins priority; `SDCTL` selects NBG0–NBG3 or BACK, and transparent
shadows additionally require `TPSDSL` and the immediately lower priority, as
specified by [VDP2 §14.1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p14_10.htm).

The new regressions cover weighted NBG blending, ratio selection from the
second image (`CCRTMD`), saturating addition, sprite ratio selection and
condition gates, normal shadows, MSB sprite shadows, and the transparent-shadow
gate. Release CTest and MSVC AddressSanitizer Debug CTest both pass **98/98**
tests; the MSVC toolchain does not provide UBSan.

A fresh replay captured frame **4441** at block step **16,328,000** with
`SPCTL=0x0625`, `PRINA=0x0206`, and `CCCTL=0x0107`. NBG1 remains `#000020`, BACK
is black, and `CCMD=1` adds those values, so color calculation correctly leaves
the VDP2 frame unchanged. `SPCCEN` is clear, so sprites stay opaque. The new
final, VDP2, and VDP1-bank PPMs are byte-identical to the previous replay; the
final image still differs from VDP2 at **38,964** sprite pixels, with the same
decoded sprite colors. This replay therefore checks for regressions in the
real title state, while the focused tests exercise the blend and shadow paths.
The captured preview and logs are in
`build/artifacts/vdp2-color-shadow-20261004/`; the frame image is
`video-diagnostics/frame_004441_final.png`.

For `SFCCMD=1–3`, the layer decoders now retain the special color-calculation
decision per output pixel. Cell mode reads the auxiliary SCC bit for one-word
pattern names and the CC bit from two-word pattern names. Bitmap mode reads the
SCC bit from `BMPNA`. `SFCCMD=2` additionally checks the selected A/B special
function code against the dot's lower four color-code bits; `SFCCMD=3` checks
the CRAM data MSB for palette pixels and enables calculation for RGB pixels.
The flag is applied only when that NBG is the top image, matching the VDP2
rule. This follows [§4.6](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_60.htm),
[§4.9](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p04_90.htm),
[§10.3](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p10_30.htm),
and [§12.2](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p12_20.htm).

With `EXCCEN` enabled and no line-color screen inserted, the compositor now
uses the second image's `CCEN` to blend the second and third images at 1:1,
then performs the regular top/second calculation. This is the three-image case
in [§12.1](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p12_12.htm).
The four-image case remains future work because it also requires line-color
screen composition, which this runtime does not yet provide.

### Unskipped OP.CPK video and audio (2026-10-05)

The opening file contains 1,708 FILM descriptors, a 320x176 Cinepak image,
and 359 planar signed-8-bit stereo audio records at 22,050 Hz (89.933 seconds).
An unskipped diagnostic capture showed valid anime pixels in NBG0, hidden by
NBG2/NBG3 interpreting the same VRAM as cells. The compositor now honors the
VDP2 fetch-slot exclusions for high-color NBG0/NBG1 modes. The regression
also checks that excluded planes become visible when the conflicting mode
is disabled.

The next capture stalled at descriptor 46 because PCM Start was posted beyond
the eight command packets, at 0x25A00790. Sound Initial (10h) and Total Volume
(82h) were left pending even though the batch flag was acknowledged. Modeling
those commands lets the original SH-2 queue reset its packet pointer and
consume the OP PCM Start (850090E0,60000000,78000000,00000000). Sequence/DSP
synthesis remains outside this HLE. The stereo output also now follows the
original CPK transfer's right-plane-first sound buffer layout.

A 133.333-second mixed-output WAV then exposed repeated fragments separated
by 65,536 / 22,050 = 2.972154 seconds: old PCM data was read after a buffer wrap.
The CD producer delivered only one data sector per 75 Hz report, limiting data
to 150 KiB/s. Saturn data reads use double speed (150 sectors/s); the runtime
now produces two data sectors per report, preserving range termination,
retained-buffer capacity and command reply protection. The drive also continues
producing sectors while the host drains the FIFO: the transfer holds a separate
snapshot, so host reads must not suspend replenishment of CD buffer RAM.
The [Sega technical bulletin](https://antime.kapsi.fi/sega/files/Sattechs.pdf)
documents maximum/double speed for CD-ROM data reads. A regression checks
150 sectors in 75 reports and the one-sector final batch.

Double-speed CD alone did not remove all repeated fragments: a 10,000-block
display budget left the original CPK decoder too little foreground work to
refill PCM in time. At 20,000 blocks/frame, the first 21.2 seconds match
99.9985% of the source stereo samples (allowing one PCM16 quantization unit).
This is a deterministic scheduling budget, not an accurately modeled SH-2
overclock.

CD reports now remain at 75 Hz when the display budget changes; the previous
10,000-block cap accelerated the drive at larger budgets. The SH-2 FRT also
uses the same guest-time scale as display/audio, with fractional ticks and
the phi/8, /32 and /128 prescalers. The launcher uses the
[Sega NTSC 320-dot clock of 26.8741 MHz](https://www.infochunk.com/saturn/segahtml_en/hard/intr/hon/p03_10.htm).
Changing FRT alone did not correct the PCM repeats. Dynamic dot-clock/PAL
changes and instruction-cycle timing remain outside this scheduler.

The final unskipped replay uses **14,000 blocks/frame**, now the launch default,
after removing per-register helper calls/map updates from the native backend
and repeated page lookups from word RAM accesses. Both changes preserve MMIO
observability and diagnostic snapshots, including page crossings and sparse
state slots. The 20,000-block replay's mixed WAV before and after the register
optimization is byte-identical (SHA-256
`a71459b33c68ce2056d73ea3725dc14d17b1ccdb69e4ff4ebed3d858066b565e`).

| Replay | Average FPS | Audio underruns |
| --- | ---: | ---: |
| 20,000 blocks/frame, original register path | 39.9611 | 425 |
| 20,000 blocks/frame, direct native registers | 46.6383 | 310 |
| 14,000 blocks/frame, direct registers and word RAM access | 56.1091 | 25 |

The last replay completed 105 million native blocks and 7,500 display/audio
frames, with zero interpreter fallback and zero waveOut write errors. The
anime opening reached its natural end and continued to the title. All
89.933243 seconds of source PCM align at one fixed offset (12.35 seconds),
with **99.998487%** of stereo sample frames matching to one PCM16 unit. Only
30 sample frames differ beyond that tolerance at buffer transitions; this
still needs refinement. There are no multi-second repeated fragments. The
peak/clipping statistics for the entire mixed WAV include post-opening audio
and are not measurements of OP.CPK alone.

Release validation passes **116/116 tests**. Stable 60 FPS, zero underruns,
full SCSP/sequence synthesis, Vulkan/compute and whole-game static emission
remain pending. This runtime still uses demand-compiled Win64 blocks.

Reference extraction, WAV comparisons and replay logs are local artifacts in
`build/artifacts/op-audio-20261005/`, `op-full-audio-20261005/`,
`op-cd2x-audio-20261005/`, `op-final-audio-20261005/`,
`op-native-state-audio-20261005/` and `op-budget14-audio-20261005/`.
The extracted reference is only used for validation;
the executable plays samples supplied by the original game through sound RAM.

### Windows presentation pacing (2026-10-05)

A fresh 105-million-block / 7,500-frame replay measured the remaining host
jitter without changing the 14,000-block guest budget. Window painting averaged
0.133 ms, while pacing sleeps of at most 1 ms overshot by 7.638 ms on average
(p99 bucket upper bound 17 ms). Presentation intervals had p95/p99 bucket upper
bounds of 31/36 ms. The mixed PCM WAV still matched the opening reference at
the same fixed 12.35-second offset.

The frontend now pairs `timeBeginPeriod(1)` with `timeEndPeriod(1)` for the
session, as required by the
[WinMM timer API](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod).
The first timer/refill experiment reduced mean sleep overshoot to 0.811 ms,
but still counted 29 underruns. Timer precision alone did not protect the PCM
reserve. The final pacing path checks the existing three-buffer / 50 ms reserve
before waiting, producing the next frame immediately when that reserve runs
low or playback is paused. It waits before publishing the image, so normal
presentation intervals do not inherit the next frame's processing variation.
PCM order, sample rate, conversion and the eight-buffer capacity are unchanged.

BIOS-service hooks also reject ordinary game addresses before searching their
service table; MMIO execution context and timer updates still run at every block
boundary. Fixed-size timing histograms report presentation/paint/sleep percentiles
and maximum intervals without collecting an unbounded per-frame trace.

| Same 7,500-frame replay | Baseline | Final pacing |
| --- | ---: | ---: |
| Average FPS | 56.1085 | 56.3302 |
| Presentation p95 bucket upper bound (ms) | 31 | 29 |
| Presentation p99 bucket upper bound (ms) | 36 | 30 |
| Mean pacing sleep overshoot (ms) | 7.63845 | 0.799232 |
| Audio underruns | 15 | 18 |
| Audio write errors | 0 | 0 |

The 1 ms histogram bins give percentile upper bounds, rather than exact
percentiles. This improves presentation regularity, with similar average
throughput; it does not establish stable 60 FPS or fewer underruns. The final
run had 961 queue-recovery frames, 38 paused-refill frames and a maximum
presentation interval of 403.264 ms. Remaining long CPU/host stalls need
further profiling, including first-use block compilation and composition.

Both mixed WAVs are byte-identical (SHA-256
`5ee1aa2cb1a3670dd52a6bf444bb425beaa085b4d9b15ddd0e3a7587357a3ba6`),
and the full opening retains **99.998487%** source-sample agreement at the
fixed offset. The final run completed 105 million native blocks with zero
interpreter fallback. Release validation remains **116/116 tests**.
Logs and WAV comparisons are in `build/artifacts/stutter-baseline-20261005/`,
`stutter-timer-refill-20261005/` and `stutter-final-20261005/`.

### VDP1 Vulkan compute and interactive controls (2026-10-05)

The runtime now accepts `--renderer auto|cpu|vulkan`; `launch` defaults to
auto and reports a CPU fallback reason when Vulkan initialization fails.
Explicit Vulkan selection reports an error instead of replaying CPU commands.
The shader is embedded SPIR-V targeting Vulkan 1.1 and requires `shaderFloat64`.
The SDK is optional: the build can fetch pinned Khronos headers and glslang.
`SATURNBOX_VULKAN=OFF` retains a dependency-free CPU renderer build.

Sprites and nondegenerate VDP1 quads are packed into ordered compute batches.
Each invocation owns one output pixel and evaluates the commands in order,
retaining destination-color feedback for RGB effects. The quad mapping uses
inverse bilinear UV across the entire polygon, avoiding a texture discontinuity
at an independent triangle diagonal. It mirrors the current CPU reference;
this is not a claim of complete Saturn rasterization accuracy. Lines and
collapsed quads remain on the CPU, after completion of earlier GPU commands.
Legacy rectangle fills retain their CPU clipping and zero-color semantics.

VRAM and command snapshots remain immutable until completion. Submission
returns before the fence; guest SH-2 execution and VDP2 composition can overlap
GPU work. Framebuffer reads/writes, CPU primitives, another draw and VDP1
composition wait before consuming the result. The framebuffer bank is captured
at submission. Guest draw-end scheduling retains the existing logical timing;
GPU completion is enforced at framebuffer consumers. VDP2 and Win32/GDI
presentation still run on the CPU; there is one GPU batch in flight.

The first replay exposed very expensive CPU reads from uncached Vulkan memory.
The framebuffer allocation now prefers host-cached coherent memory. A cached
but synchronous replay reached 55.4812 average FPS and 64 audio underruns.
Asynchronous submission improved that replay to 56.7183 FPS and 17 underruns,
with presentation p95/p99 bucket upper bounds of 27/30 ms. These single runs
do not establish a statistically significant gain or stable 60 FPS. The async
run processed 36,255 GPU batches and 234,329 primitives over 7,500 frames;
fence waits totaled 3,764 ms and host copies 8,615 ms. Removing those copies
and moving composition/presentation to Vulkan remain performance work.

Both replays completed 105 million native blocks at the same 14,000-block
frame budget. Their entire mixed WAV is byte-identical to the previous CPU
replay (SHA-256 `5ee1aa2cb1a3670dd52a6bf444bb425beaa085b4d9b15ddd0e3a7587357a3ba6`).
Logs/WAVs are in `build/artifacts/vulkan-cached-opening-20261005/` and
`build/artifacts/vulkan-async-opening-20261005/`. Both Vulkan and CPU-only
Release builds compiled; the unit test suite was not rerun for this milestone.

The final replay after the legacy-fill correction completed the same 7,500
frames at **56.6997 FPS**, with **16 underruns**, zero audio write errors and
p95/p99 presentation bucket upper bounds of 27/29 ms. It produced the same
mixed-WAV hash. Fence waits totaled 3,807 ms and host copies 8,633 ms;
the maximum frame interval was 416 ms, so isolated stalls remain.
Artifacts are in `build/artifacts/vulkan-final-opening-20261005/`.

The Win32 window now receives keyboard input and two XInput controllers through
the existing SMPC pad path. F1 displays the bindings; focus loss clears keyboard
state and releases live buttons. Absent controllers are polled less often,
reconnection is detected and opposite directions cancel. Explicit CLI pad
scripts retain priority over live input. Hardware gamepad behavior still needs
an attached-controller session. README lists keyboard and controller bindings.
Keyboard taps are retained for at least two presentation frames and until
a peripheral packet encodes the pad. A down/up pair received in the same
message pump therefore survives the status/RTC phase; focus loss also clears
this latch. The measured opening replay used no live buttons.
In the final interactive session, a short Enter tap during OP.CPK took the
game's skip path to the title, and F1 displayed the controls overlay. Repeated
Start taps at the title did not establish menu entry; this remains unvalidated.
The session is recorded in `build/artifacts/vulkan-controls-20261005/`.

The longer interactive session reached the title but showed approximately
31--36 FPS there. The 56.7 FPS opening average must not be treated as gameplay
throughput. Phase progression and visual correctness beyond the opening
remain to be established.

### Red background and retained-frame flicker (2026-10-05)

The subsequent visual review found that the apparent title mixed old movie
pixels with the current loading screen. It did not certify arrival at the
real title/menu. Replays with a Start pulse at blocks 13,700,000--14,000,000
and a capture at block 24,500,000 reproduced retained pixels in both CPU and
Vulkan. All eight captured surfaces (BACK, four NBG layers, both VDP1 banks
and final composition) were byte-identical between renderers.

The command list contains a zero-color polygon with `PMOD=0x00C0`, covering
rows 110--239. SPD=1 disables transparent-pixel rejection, so this command
must write zero to the framebuffer. Both plot paths incorrectly rejected all
zero colors, and the raw CPU framebuffer writer rejected them again. The
correction honors SPD in CPU and compute, allowing the game's own polygon
to remove old pixels. Ordinary transparent texels still preserve prior data.
See [VDP1 color calculations and SPD](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p06_37.htm).

The runtime also lacked VBlank erase. It now writes EWDR inside the EWLR/EWRR
window on the display side before a bank exchange, bypassing command clipping.
TVMR.VBE enables recurring VBlank erase; manual FBCR erase is consumed once.
GPU writes to that bank complete before CPU erase. The current implementation
covers the existing 16-bit, 512x256 framebuffer path; other display formats
and precise scanline timing remain outside this milestone. The behavior follows
[VDP1 framebuffer switching](https://www.infochunk.com/saturn/segahtml_en/hard/vdp1/hon/p04_12.htm)
and [Yabause's software erase path](https://github.com/Yabause/yabause/blob/master/yabause/src/vidsoft.c).

VDP2 32-bit register writes now publish both consecutive 16-bit registers,
matching [Vdp2WriteLong](https://github.com/Yabause/yabause/blob/master/yabause/src/vdp2.c).
The shader's legacy fills again follow CPU clipping/transparent-zero behavior;
its Gouraud interpolation now truncates like the CPU reference. The previous
milestone's claim that legacy CPU fills bypass clipping and clear zero was
incorrect: those calls used the pixel writer's clipping and zero rejection.

One-shot frame capture now retains the interactive deferred-composition path,
while full traces and frame sequences keep reference composition. The corrected
capture at sequence 4821 contains 14,547 nonblack display-bank pixels, compared
with 70,413 before. It shows the loading logo on black, without old background
or Start/copyright fragments. CPU and Vulkan again match all eight surfaces.
The NBG layers are still black at this point, so this establishes clean loading
presentation, not completed title assets or playable menu/phases.

Artifacts: `build/artifacts/title-glitch-{cpu,vulkan}-20261006/` and
`title-glitch-final-{cpu,vulkan}-20261006/`. Vulkan and CPU-only Release builds
compiled. The unit test suite was not run for this correction; stable 60 FPS,
full title progression and hardware controller validation remain pending.

The later live session exposed a separate VDP2 fault: the loaded cell maps
were scrambled, with solid red rectangles. Character numbers were multiplied
by the cell byte count and again by four for 16x16 characters. VDP2 uses
fixed 32-byte character address units, followed by the individual 8x8 cells.
The cell renderer now uses this base, decodes the one-word supplementary
character bits, and applies horizontal/vertical flips to the entire character.
Palette dots use their index to determine transparency, including BGON's
transparency-disable bits; a nonzero dot referencing black CRAM remains opaque.
Scroll-layer scratch buffers use alpha zero as the empty-dot sentinel, so
opaque black also survives the priority merge and color calculation.
Palette addressing also includes CRAOFA and the correct 256-color palette bits.
These formats follow the primary implementation in
[Yabause's Vdp2PatternAddr and Vdp2FetchPixel](https://github.com/Yabause/yabause/blob/master/yabause/src/vidsoft.c).

Cell-map dimensions are powers of two. Row coordinates and plane addresses
are now computed outside the pixel loop; masks and shifts replace per-dot
division and modulo. This keeps the VDP2 compositor on CPU for this milestone.

The later deterministic captures use block 49,000,000 (sequence 39805).
`title-glitch-loaded-vulkan-20261006` and
`title-glitch-cell-fixed-vulkan-20261006` contain byte-identical VDP2 VRAM,
CRAM and register dumps. The earlier render has scrambled cells and solid
red regions; the corrected render has a continuous city/machinery background.
The first live cell-corrected session likewise remained visually stable at
about 35--37 FPS after skipping the movie. Start did not establish menu entry.
The coherent background alone does not certify the complete title: its logo,
menu transition and stages still need progression investigation.

The final live build with coordinate masks and hoisted row calculations
showed 52.2 FPS in the same loaded scene, versus about 35--37 FPS before
that optimization. These are window-counter observations, not a sustained
60 FPS benchmark or gameplay validation. The session remains available in
`build/artifacts/title-glitch-optimized-window-20261006/`.
Windows HID enumeration did not identify an Xbox/XInput-named controller;
physical button/axis behavior has not been validated.

This is the first GPU VDP1 runtime stage for the static recompilation project.
Whole-game static emission, GPU VDP2 composition, Vulkan presentation and
stable 60 FPS remain pending; the current game session uses demand Win64 blocks.

## First-stage loading and proportions (2026-10-06)

The first-stage report exposed additional rendering faults. NBG2/NBG3 scroll
now reads SCXN2/SCYN2 at `0x090/0x092` and SCXN3/SCYN3 at `0x094/0x096`.
The old NBG2 offsets read NBG1 zoom registers. In the captured stage, NBG2
requests X=40, Y=256; correcting these registers restores the ground/platform
placement in the raw layer render.

The output height follows normal, noninterlaced TVMD selections: 224, 240 or
256 lines. This game selects `TVMD=0x8010`, with a VDP1 system clip of 319x239,
so its frame is 320x240. Previously the last 16 rows were clipped. The Windows
presenter now fits the frame within the client area while preserving its
width/height ratio; resizing adds black borders instead of stretching sprites.
High-resolution and interlaced modes are still outside this implementation.
The height decode follows
[Sega's TVMD description](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p02_10.htm)
and [Yabause's software resolution selection](https://github.com/Yabause/yabause/blob/master/yabause/src/vidsoft.c).

VDP2 color offset is now applied after color calculation, using the winning
layer's CLOFEN/CLOFSL bank, signed nine-bit RGB offsets and channel saturation.
This follows
[Sega's color-offset specification](https://www.infochunk.com/saturn/segahtml_en/hard/vdp2/hon/p13_10.htm).
The captured transition has `CLOFEN=0x7F` and RGB offsets `0xFF00` (-256).
Consequently the corrected final image is black while loading is held, although
the diagnostic VDP2 layers contain the city and platform. The previous visible
background was evidence of loaded assets, not proof that gameplay had begun.
No game flags or fade registers are patched to bypass that wait.

CD selectors now share a 200-sector capacity. Get Buffer Size returns the actual
free count, and BFUL is recomputed as a level when HIRQ is read, including after
an acknowledgement or sector deletion. These semantics follow
[Yabause's CD buffer implementation](https://github.com/Yabause/yabause/blob/master/yabause/src/cs2.c).
This corrects the device contract but has not resolved the loading stall.

Deterministic 30-million-block replays with Start held at blocks 13,700,000--
14,000,000 reach the stage resources `PL01.ARC` and `ST0_0_0.ARC`. The next PLAY
starts at FAD 18061 for 3988 sectors (`ST00_0_Z.BIN` plus `ST00_1_Z.BIN`).
It stops advancing at 200 retained sectors; the guest polls the two partitions
without starting the next host sector transfer. The CPU still executes blocks
and services interrupts, with no interpreter fallback. The VDP1 list contains
25 commands, five clear polygons and zero sprites. Player/enemy proportions
cannot yet be assessed from that list. Earlier PCM/GFS callback activity ends
before this final PLAY, so it does not establish a working stage music stream.

One-shot diagnostics now save matching low/high work RAM, SH-2 state slots,
SCSP RAM and VDP1 command VRAM. Focused probes cover PCM/GFS callbacks and the
host-transfer preparation path; CD output includes all nonzero opcode totals.
Artifacts are under `build/artifacts/frozen-scene-scroll-fixed-20261006/`,
`stage-stream-probe-cpu-20261006/`, `stage-transfer-probe-20261006/` and
`stage-height-cpu-20261006/`. Release builds compile with Vulkan and CPU-only
VDP1. No unit tests were added or run for this correction.

The captures at block 29,000,000 (sequence 10913) are 320x240 in both builds.
All nine saved PPM surfaces have identical pixels between CPU and Vulkan;
low/high work RAM and SCSP RAM dumps are also byte-identical. This comparison
covers the captured loading state, whose VDP1 list contains no actor sprites.

Remaining: unblock the stage streaming/transition, observe actual actor command
lists and validate controls in gameplay. Stable 60 FPS, complete gameplay and
whole-game static emission are not established by these captures.

## First-stage CD notification fix (2026-10-06)

The follow-up trace locates the stalled transfer after the sector-count query,
not in its response. `0x0603C898` loads the actual count from the stack before
returning it; snapshots taken at block entry show R0=0 but SP+0 contains the
retained count. The two-file GFS stream receives that count correctly. Its
transfer quota remains zero because the CD position notification is lost.

CR1 writes incorrectly cleared HIRQ.SCDQ. GFS submits sector-count commands
before checking the periodic event, so this erased the notification needed by
`0x06037B88` to set the stream position-update flag. The controller stayed at
the starting FAD; its transfer task never acquired a quota, and the CD buffer
filled without being drained. CR1 now preserves SCDQ until the guest explicitly
acknowledges it through HIRQ. Unread command packets remain protected by
`commandPending_`, including ticks between CR1 submission and CR4 consumption.
The independent HIRQ acknowledgement follows
[Yabause's CD register semantics](https://github.com/Yabause/yabause/blob/master/yabause/src/cs2.c).
No guest flags, fade values or PCM data are patched.

Release builds with Vulkan and CPU VDP1 completed the same 30-million-block
replay. Both report zero interpreter fallback and matching CD activity:

* The stage PLAY advances to 446/3988 sectors, beyond the previous 200-sector
  stall, with data retained in the second partition.
* Host GET and DELETE commands resume; the full replay transfers 3373 sectors
  and deletes 3371. These totals include boot, movie and earlier resource reads.
* The position callback still executes at block 29,990,401; the GFS/PCM callback
  executes at 29,992,045/29,992,046. The quota setter receives `0x7FFFFFFF`.
* The final VDP1 list has 111 commands, including 83 sprites and eight polygons,
  versus zero sprites in the stalled replay. This count includes HUD and other
  sprites and does not establish that every actor is rendered correctly.

The capture armed at block 26,500,000 (sequence 5332) shows the stage HUD and
foreground effects with the fade released. CPU and Vulkan produce byte-identical
320x240 frame captures, command lists and VDP1 VRAM. The image still has visual
defects; correct sprite colors and the complete stage are not yet validated.
Artifacts: `build/artifacts/stage-scdq-latch-20261006/` and
`build/artifacts/stage-scdq-latch-cpu-20261006/`. Diagnostic probes now include
stack output words, separate file-query records and the position/quota path.
No unit tests were added or run for this correction.

A longer Vulkan replay completes 45 million blocks with zero interpreter
fallback. At its end, the stage stream has advanced to 1216/3988 sectors and
the VDP1 list contains 247 sprites. The capture at block 42,000,000 (sequence
8186) shows a changed camera position and effects, establishing progression
beyond the earlier image. Its `PRESS START BUTTON` overlay identifies the
attract-mode stage demonstration, not a validated player-controlled session.
Some sprite textures remain visibly corrupt. Artifacts are in
`build/artifacts/stage-scdq-progress-45m-20261006/`. The initial streaming stall
is resolved in these replays; gameplay controls, actor rendering and completion
of the full game remain pending.
