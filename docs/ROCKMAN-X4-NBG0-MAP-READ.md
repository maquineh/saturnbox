# NBG0 Name-Map Read Checkpoint

This report follows NBG0 name-map memory during the first black-frame capture
and the later title-load investigation. The detailed original trace log is
preserved in [`pt-BR/ROCKMAN-X4-NBG0-MAP-READ.md`](pt-BR/ROCKMAN-X4-NBG0-MAP-READ.md).

## Initial observation

The original capture tracked the expected map at `0x70000–0x71FFF` and the
name words actually read by the compositor. Forty-three nonzero words were
present while NBG0 was in bitmap mode (`CHCTLA=0x0042`). The game cleared them
at step `8673634`; NBG0 switched to cell mode (`CHCTLA=0x0101`) later. This
does not establish that those 43 values were title-map entries: while in
bitmap mode, `VCSTA=0x70000` also points at the region as a vertical-cell-scroll
table.

The compositor initially calculated the wrong base for the captured
configuration (`PNCN0=0`, `CHCTLA=0x0101`, `PLSZ=0x15`). For two-word names,
16×16 4-bpp characters, and a two-page horizontal plane, each page is
`0x1000` bytes and each plane is `0x2000` bytes. The Sega VDP2 table gives a
name-map base of `0x70000`; the renderer had used the 8×8-character multiplier,
producing a masked base of `0x40000`. The renderer now uses the character-size
dependent page size.

## Corrected capture

Two complete 12-million-step boots were run on 2026-09-26, one without input
and one with Start held. Their map and CD-command state traces were byte for
byte identical:

- The black-frame probe captured frame 2469 at step `11269999`.
- NBG0 read 143,360 name words from `0x70000` through `0x706CE`; all were zero.
- All 4,096 words in `0x70000–0x71FFF` were zero.
- At the capture, `CHCTLA=0x0101`, `BGON=0x000F`, and NBG0, VDP2, and final
  output had no nonblack pixels.
- All 139 FMV descriptors were consumed. The boot stopped at its step limit in
  block `0x060303CC`.
- The CD history contained `PLAY` for FAD 378, 276 sectors (`X4LOW.X4`), but
  no `PLAY` for FAD 130776 (`OP.CPK`). Holding Start from boot did not change
  the observed state.

This confirms that the compositor reads the corrected address and that no
name entries had been written there before the captured frame. It does not
identify which routine or resource should load the title map. The previous
`PLAY` at FAD 654 for 46 sectors matches `ASCII.ARC` (ISO LBA 504, 94,208
bytes). `CAPCOM.ARC` begins at LBA 550 (FAD 700); both files were extracted for
later inspection, but the capture does not establish which contains the map
or which command should load it.

The dynamic routine at `0x0022750C` copies 96 words from `X4LOW.X4` to
`0x25C7FF00` in VDP1 VRAM; this path does not write the NBG0 map.

## Title GFS wait

The partial trace ending at 11.9 million steps followed the title's queued
`SNDCOM.ARC` request. At step `11255502`, callback index 5 (`0x06016DA6`)
queued a pointer at `0x06043EB0` and set `count=1`; at step `11255503`, it set
`busy=1` at `0x0605E7F6`. The worker started at `0x0601CE72` and resolved GFS
name index `0x14` (20), then entered the read path.

The worker polls `0x06053354` against limit `0x800`. Status handling remains
active, but the completed-byte count stays zero. The path reaches
`0x0601CC7E` and keeps polling without returning to the next worker block
`0x0601C6C0`. No CD `GET DATA` (`0x61`) or `GET+DELETE` (`0x63`) request for the
queued title resource appears after it is enqueued. The queue remains
`index=0`, `count=1`, `busy=1`, with `SNDCOM.ARC` in its first slot.

The handle setup at `0x06036BD0` sees `handle+0x28=0`, initializes ring fields
to `+0x5C=0`, `+0x60=0xA3`, `+0x64=0`, and sets handle state 2. The subsequent
read at `0x06035DC2` takes the state-1 polling path and does not call the FMV
sector-transfer path at `0x0603A71A`. A later trace confirmed that the
post-enqueue title request still had no associated sector request and no
completed bytes.

The observed wait therefore sits after local handle setup/validation and
before the routine that starts the sector transfer. The next investigation
needs to find who should initialize `handle+0x28` or publish bytes to that
buffer. The map load and first sector of `SNDCOM.ARC` have not been reached.

## Captures and references

The original full traces are archived under `docs/pt-BR/`. The principal
capture directories were `build/artifacts/first-black-chain-20260926/`,
`build/artifacts/map-read-trace-20260926/`, and their Start-held equivalents.
See the [title-map fix](ROCKMAN-X4-TITLE-MAP-FIX.md),
[map producer trace](ROCKMAN-X4-NBG0-MAP-PRODUCER.md), and
[FMV checkpoint](ROCKMAN-X4-FMV-CHECKPOINT.md) for related findings.
