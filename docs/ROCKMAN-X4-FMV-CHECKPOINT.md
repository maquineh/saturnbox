# Rockman X4 FMV and Post-FMV Checkpoint

The full historical FMV investigation is preserved in
[`pt-BR/ROCKMAN-X4-FMV-CHECKPOINT.md`](pt-BR/ROCKMAN-X4-FMV-CHECKPOINT.md).
This English checkpoint records the verified findings and remaining limits.

## FMV completion

The earlier replay stalled at descriptor 27 because the game requested a
sector count while a periodic CD subcode report was still pending. The subcode
tick replaced the unread response; the game interpreted the periodic FAD
report as a sector count, capped the request at 17, and transferred the wrong
range. This was a CD communication-handshake race, not an image decoder, VDP2,
or DMA failure.

The CD runtime now protects the response packet from the CR1 command write
through the CR4 read, including register aliases. Sector production continues
while the periodic report waits for the response to be consumed. Command `0x52`
returns WAIT while the full requested range is unavailable and preserves the
last calculated size.

The Rockman X4 `LOGO.CPK` FILM has 139 STAB entries. The pair
`0xFFFFFFFF, 1` marks an audio sample; it is not an end-of-file marker. The
last video descriptor ends at `0x1DD5A8` (`0x8F0 + 0x1D8444 + 0x4874`).
The game compares the descriptor index with the table count, waits for
consumers, and then records completion state 5.

A clean 12-million-step replay consumed all 139 descriptors and began another
disc read after the FMV. The title-logo sequence rendered cleanly, including
the CAPCOM logo and fade-out. Frames 915, 1491, 1936, and 2103 were inspected;
the earlier horizontal tearing did not recur. The final producer cursor was
`0x1DD800` and the consumer cursor was `0x1DD5A8`, leaving only sector padding.
The last audio consumption was at step 8,167,882; each channel published
321,928 bytes. The FMV GFS callback stopped near step 8,671,931, and a new
`PLAY` at step 8,678,395 requested FAD 654 for 46 sectors.

## Post-FMV loading

The following 2,048-byte SCU DMA (`0x25818000 → 0x25E60000`) initially waited
because the game cleared `HIRQ.DRDY` before a manual GO while data remained in
the FIFO. SCU DMA now checks `fifoDataAvailable()`—active transfer and cursor
within payload—without reasserting HIRQ, changing transfer length, or creating
data. Regression coverage is named `transfersCdFifoThroughScuDma`.

The corrected 12-million-step replay completed the DMA at 2,048/2,048 bytes
with `count=0` and no pending CD descriptor. The post-FMV BIOS return at
`0x0022750C` is materialized from the downloaded `X4LOW.X4` image, allowing
execution to continue toward the title. The final capture had display on,
VDP1 active (43 command lists, 18 sprites), and VDP2 enabled.

The automatic `0.BIN` route reached the title after `PLAY` for `LOGO.CPK` and
auxiliary data reads. It did not issue `PLAY` for FAD 130776 (`OP.CPK`) in that
session. The ISO confirms that `OP.CPK` is present at LBA 130626 (FAD 130776)
with 1,708 STAB entries, but the automatic opening path has not been shown
playing that second animation. Do not inject a CD command to claim it was
reached.

## Title-screen caveat

After the FMV, the game reaches a black frame with VDP1/VDP2 enabled. A later
probe confirmed that the post-FMV `X4LOW.X4` routine copies VDP1 texture data,
but it does not populate the NBG0 title name map. The corrected VDP2 map base
is `0x70000`; that region was zero in the captured title frame. The FMV
completion is established, while a fully rendered title screen was not
established by these 2026-09-20 captures. A later SaturnBox replay did capture
a visible title screen; see the [roadmap](ROADMAP.md) and
[validation baseline](ROCKMAN-X4.md).

## Reproduction

The archived full report contains the complete PowerShell commands and
artifact paths. Use the same authorized 2448-byte-sector MDF, boot inputs,
`--continue-after-execdmp`, `--vblank-blocks 10000`, `--trace-game`, and
`--steps 12000000`; capture frames with `--frame-dir`. The original
artifacts are under `build/artifacts/fmv-handshake-verified-frames/` and
`build/artifacts/fmv-postbios-probe-12m.txt`.
