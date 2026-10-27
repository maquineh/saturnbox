# Rockman X4 Validation Baseline

This page summarizes the SaturnBox validation target and links to the
specialized investigation reports. Detailed historical traces are preserved
in [`pt-BR/ROCKMAN-X4.md`](pt-BR/ROCKMAN-X4.md).

## Target image and boot path

The validated image is Rockman X4, Saturn release `T-1221G V1.002`. The
tested boot sequence uses the IP/AIP, the first executable `0.BIN`, and later
the dynamically loaded `X4LOW.X4` segment. The CD image used in the recorded
experiments is a 2448-byte-sector MDF; game images are not included in this
repository.

The IP is loaded at `0x06002000`, the AIP starts at `0x06002E40`, and the first
executable is loaded at `0x06009000`. The AIP transfers control to
`0x06010000`. `FIRST READ ADDRESS` is a load address and must not be treated as
the program entry point.

## Validated progress

- The boot harness reaches the first executable handoff and can continue into
  the game's CD-driven opening.
- Reference SH-2 execution, SIR interpretation, Win64 JIT, and static AOT paths
  have been compared on captured routes.
- A CD-backed replay reached the title screen showing `PRESS START BUTTON`.
  A later local route saved a visible 320×240 title frame near 19.5 million
  blocks; output returned to black by the 20-million-block limit.
- Static AOT covered the captured opening/title route for 20 million blocks
  without JIT or interpreter fallback. Extended captures also reached the
  first-stage resource stream and a 510-million-block `ST04_0.BIN` route.
- The current frontend can display a captured frame or open an interactive
  Win32 window. These results do not establish complete game compatibility,
  sustained 60 Hz, dual-SH-2 execution, or full SCSP emulation.

## Device and rendering scope

The runtime models CD Block commands, HIRQ/FIFO and ISO reads; SCU DMA; VDP1
command processing and software rendering; VDP2 backgrounds, cells, priorities,
and selected effects; SMPC digital pads; and a subset of SCSP audio behavior.
The SCSP 68K is not executed. Hardware timing and device behavior remain HLE
approximations.

The title-map investigation found an incorrect VDP2 name-table base for
16×16-character cells. The compositor now calculates the expected base
(`0x70000` for the captured configuration). Correcting the address did not
make NBG0 visible: the whole expected `0x70000–0x71FFF` range was zero in the
captured title frame, and the game had not issued the observed title-resource
read. See [the title-map investigation](ROCKMAN-X4-TITLE-MAP-FIX.md),
[NBG0 read trace](ROCKMAN-X4-NBG0-MAP-READ.md), and
[NBG0 producer trace](ROCKMAN-X4-NBG0-MAP-PRODUCER.md).

## Reproduction outline

Build SaturnBox as described in the root README, extract the IP/AIP and
`0.BIN` from an authorized image, then run `saturn-sh2 boot` with their base
addresses, the disc path, `--disc-sector-size 2448`, and a bounded `--steps`
value. Use `--continue-after-execdmp` and `--vblank-blocks` to run beyond the
initial handoff; add `--frame-dir` or `--frame-capture-after-blocks` to save
frames. Use `--interpreter` to repeat a route through the reference SIR engine.
The root README has the complete command syntax.

## Investigation reports

- [Static recompilation and route comparisons](ROCKMAN-X4-STATIC-RECOMPILATION.md)
- [Title-map and video rendering fixes](ROCKMAN-X4-TITLE-MAP-FIX.md)
- [FMV and post-FMV loading checkpoint](ROCKMAN-X4-FMV-CHECKPOINT.md)
- [First black-frame evidence](ROCKMAN-X4-BLACK-FRAME.md)
- [Player input and first-stage checkpoint](ROCKMAN-X4-PLAYER-TEST-CHECKPOINT.md)
- [NBG0 name-map reads](ROCKMAN-X4-NBG0-MAP-READ.md)
- [NBG0 map producers](ROCKMAN-X4-NBG0-MAP-PRODUCER.md)
