# Rockman X4: First Black Frame After Display Re-Enables

This checkpoint documents frame **2469** from a 12-million-block replay on
2026-09-20. The probe required a previously visible image, display-off, then
`TVMD=0x8010` with every output pixel black. It did not confuse the initial
boot black screen with the post-FMV transition and did not modify game flow or
rendering. The full original capture report is preserved in
[`pt-BR/ROCKMAN-X4-BLACK-FRAME.md`](pt-BR/ROCKMAN-X4-BLACK-FRAME.md).

## Captured state

| Area | Observation in the renderer |
| --- | --- |
| Registers | `PNCN0=0`, `CHCTLA=0x0101`, `PLSZ=0x15`, `MPOFN=0x111`, `MPABN0=MPCDN0=0x3030` |
| VRAM writes | 7,002 nonzero bytes at `0x53080–0x60FC6` |
| Initial NBG0 map address | `0x1C0000`, masked to `0x40000` |
| Name entries | 280 sampled at offsets `0x40000–0x406CC`; both words in every entry were zero |
| Map write history | All 42 sampled 32-byte regions were last written at frame 2192; current entries were zero |
| Characters | Character number zero; reads at `0x00000–0x0007F` were all zero; four regions last written at frame 2192 |
| Color | Palette/index zero, `CRAM[0]=0`; other CRAM positions `0x2–0x2FF` had 706 nonzero bytes |
| Window/priority | `WCTLA=0`, all 71,680 pixels allowed; `PRINA=0x0505`, NBG0 priority 5 and priority programming enabled |
| Composition | No nonblack pixels in decoded NBG0, NBG0 layer, VDP2, or final frame |
| VDP1 | Display bank 1 was black; bank 0 had 7,665 nonblack pixels. The capture did not explain the bank selection. |

The earliest observable failure was the zero name-map read. NBG0 was already
black before composition, so priority and window rules were not needed to
explain this frame. The capture did **not** prove that the map was absent at
the address the hardware should use, and it did not establish a global CRAM
fault.

## Corrected address validation

The first renderer used a name-map base calculated with the 8×8 character
multiplier. The Sega VDP2 address table gives the correct base as `0x70000` for
the captured setup: `PNCN0=0` (two-word names), `CHCTLA=0x0101` (NBG0 16×16,
4 bpp), and `PLSZ=0x15` (two horizontal pages). Each page is `0x1000` bytes
and each plane `0x2000` bytes. The original renderer calculated `0x1C0000`,
masked it to `0x40000`, and read the wrong region.

The corrected compositor reads 143,360 name words from `0x70000–0x706CE` in a
full capture; all are zero. All 8,192 bytes in `0x70000–0x71FFF` were also
zero. Fixing the address therefore corrected the renderer's lookup but did
not make NBG0 visible in this snapshot. See the
[NBG0 read trace](ROCKMAN-X4-NBG0-MAP-READ.md) and
[NBG0 producer trace](ROCKMAN-X4-NBG0-MAP-PRODUCER.md).

The write trace found 43 nonzero words written while NBG0 was in bitmap mode,
then cleared at step `8673634`; the corrected map range was still zero in cell
mode. Since `VCSTA=0x70000` during bitmap mode, these words may have been
vertical-cell-scroll data. The trace cannot distinguish clearing a populated
map from writing zeros over cells that were already empty.

## Artifacts and limits

The original capture directory was
`build/artifacts/first-black-chain-20260920/`. It includes full VRAM, CRAM,
register dumps, per-pixel CSV data, PPM images, summary JSON, and the trace log.
CSV addresses are calculated before VRAM/CRAM masks. The initial text report
included legacy bitmap fields that were not valid in cell mode; use the cell
CSV for this capture.

The reads came from the same `composeVdp2Cell` path used to render the layer,
rerun without mutation at the presentation boundary. They are not an
independent validation of Saturn address rules. A separate validator checked
the mode, dump sizes, original 71,680 reads, independent address calculation,
all 280 corrected entries, the full plane, write metadata, and SHA-256 hashes.
This validates the captured configuration and frame only; it is not a complete
test of zoom, page navigation, colors, or character addressing.

The first capture was evaluated with TypeSafe/JEV `jev-1.13.0`. Its selected
hypotheses were that zero name-map reads are the first observable point, that
the capture cannot distinguish an unloaded map from a wrong address, and that
the address formula should be checked against the specification. Those model
choices are not additional hardware evidence.

No functional VDP2, CD, DMA, or game-flow fix was applied as part of the
original black-frame probe. The later address correction only fixed the
renderer calculation; identifying the title-map producer remains an open
investigation.
