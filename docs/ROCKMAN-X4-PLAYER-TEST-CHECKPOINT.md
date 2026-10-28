# Rockman X4 Player-Route Checkpoint

This is the English checkpoint for the current player route. The detailed
iteration log is preserved in
[`pt-BR/ROCKMAN-X4-PLAYER-TEST-CHECKPOINT.md`](pt-BR/ROCKMAN-X4-PLAYER-TEST-CHECKPOINT.md).

## Current status

SaturnBox reaches the title and has captured a visible first-stage scene. A
scripted right-input replay reached the first X gameplay scene and confirmed
that the input travels through SMPC, gamepad normalization, and the player
movement handler. A 2026-10-27 CPU replay sampled X immediately before and
after a scripted right interval in one run and confirmed a positive position
delta. The 16.16 interpretation is strongly supported but still inferred.

The latest player-route evidence is from 2026-10-27:

- The VDP1 erase/change cycle was corrected and checked with paired
  172M-block replays. The released bank is clean in the new capture and the
  stale player poses in the previous capture disappear. The new
  [live debugger](pt-BR/LIVE-DEBUGGER.md) records eight pre/post frames,
  framebuffer phases, memory, input and audio counters on F8 or a file request.
  An [isolated SCSP driver probe](pt-BR/SCSP-DRIVER-PROBE.md) executes the real
  68000 driver, including timer IRQs and a verified volume command. The
  subsequent optional `--scsp-driver` backend adds direct PCM slots, envelopes
  and SMPC integration; a 25M-block ISO boot produced audio with 27 key-ons
  and no SDK errors. Gameplay cue fidelity and DSP remain unvalidated.
- A recorded live-control session completed the introductory stage and
  advanced into Frost Kibatodos's stage. It confirmed accumulated player
  poses, localized striped sprite corruption, and user-reported pillar
  occlusion and missing sound effects. The CPU frontend averaged 18.5453 FPS
  and ended with seven cumulative audio underruns. See the
  [live glitch audit](pt-BR/ROCKMAN-X4-LIVE-GLITCH-AUDIT.md) for evidence,
  implementation gaps, and the limits of the causal diagnosis.
- A 172-million-block CPU replay held right from 160M to 170M and captured
  gameplay frame 53,015 at 168M. X's raw position changed from `0x00C80000`
  before the press to `0x08A50000` before release: delta `0x07DD0000`, or
  +2,013 units under the inferred 16.16 format. At the end sample, right was
  still held and X speed was `0x00020000`.
- The replay reached `step-limit` at 172M after the BIOS handoff. The CPU
  reference renderer composed 76,800 non-black pixels at 320×240. Static code
  executed 49,652,178 blocks, the JIT executed 128,571,986, and there was no
  interpreter fallback.

Earlier evidence from 2026-10-26:

- A 30-million-block Vulkan replay reached handoff and reported 58.34 FPS on
  this host, six audio underruns, zero write errors, and 3,000 presented
  frames. These figures are from an instrumented run and are not a general
  performance guarantee.
- The interactive route reached the first stage with X visible. Vulkan ran at
  roughly 53–56 FPS during the scene. A brief arrow input was sent, but a
  visual capture alone did not prove movement.
- A 168-million-block replay with right input captured frame 52,997. The SMPC
  reported right pressed, the normalized input held the expected bit, and the
  player object selected rightward speed `0x00020000`. That run had only one
  position sample; the later CPU replay supplies the before/after comparison.
- The captured gameplay scene contained 224 VDP1 commands and 76,800
  non-black pixels. A `0xA80`-byte texture upload was traced to `PL00.ARC`;
  it was not evidence of a stage overlay swap.
- The audio runtime applies PCM parameter command `0x8A` when no DSP input is
  requested. Sequence playback still requires the SCSP 68K driver or an
  equivalent implementation.

The latest local title-screen capture is
`build-saturnbox/rockman/title-capture-single/captured-frame.png`.

## Confirmed input path

During the 2026-10-25 gameplay replay, SMPC register `0x20100025` changed from
`0xFF` to `0x7F` while right was pressed and returned to `0xFF` on release.
The game reads and normalizes the active-low input, remaps the right bit to
internal bit 1, and stores held/previous/pressed state in the player object.
The movement handler selects direction `0x10` and X speed `0x00020000` in the
captured state. In the 2026-10-27 replay, `obj+8` changed from
`0x00C80000` to `0x08A50000` during the 160M–170M right interval. The raw
positive delta proves that this position-like field advanced; interpreting it
as Q16.16 world position remains an evidence-backed inference.

The boot/launch CLI option `--x4-player-samples <csv>` records raw X position,
velocity, input state, and movement gates immediately before each scripted pad
change. The sample CSV for the measured route is
`build/tmp/x4-player-position-20261027-run1.csv`.

The launcher accepts a scripted pad sequence and can return the port to live
keyboard/XInput control after a selected block count with
`--pad-live-after-blocks`. The diagnostic launcher is
`tools/diagnostics/launch_x4_stage.ps1`. A user-controlled movement and combat
session was recorded on 2026-10-27, including completion of the first stage.

## Sprite and video evidence

An independent capture validator confirmed that the decompression worker was
idle and that `0x06003000` held 2,688 decompressed bytes from `PL00.ARC`. SCU
DMA copied `0xA80` bytes to `0x25C60000`; the destination matched one complete
pose in the resource. RAM later held a different pose because the source
buffer had been reused after upload. That difference does not indicate a
partial DMA.

The latest visual work also corrected Vulkan output coverage for 240-line
display modes. A CPU/Vulkan comparison failed for lower scanlines before the
fix and passed after it. This validates the affected scanlines, not every
gameplay effect or shadow.

## Audio and runtime limits

Interactive audio now measures elapsed time between VBlanks and produces
44.1 kHz PCM instead of assuming that every VBlank arrives at 60 Hz. This
keeps the audio clock aligned when rendering runs below 60 FPS. Buffers can
hold up to 500 ms of PCM, although longer delays can still stutter. A run with
the previous timing measured 38.28 FPS and 323 underruns; Vulkan with a
10,000-block VBlank interval measured 58.34 FPS and six underruns in a
30-million-block replay.

SCSP sound RAM captures match the extracted game sound driver and resources,
but SaturnBox does not execute the driver. PCM start/stop and a subset of PCM
parameter updates are modeled; sequence events, voices, envelopes, and the
SCSP 68K are not fully supported.

### 2026-10-27 stage capture: mesh and audio clock

The 320×240 capture at frame 53,013 is saved at
`build-saturnbox/rockman/stage-from-title-capture-20261027/captured-frame.png`.
Its layer dump identifies the yellow/black hatching as VDP1 polygon commands
169 and 170 (`PMOD=0x01C0`), which enable the hardware mesh pattern. The VDP1
manual defines mesh as alternating pixels ([VDP1 overview](https://www.infochunk.com/saturn/segahtml_en/hard/intr/hon/p03_30.htm));
this pattern moves with the scene and reveals the composed background through
its gaps. Keep it as-is unless a hardware reference shows a mismatch.

The deterministic route reported 12,000 presented frames and 8,820,000 SCSP
samples per channel, exactly 200 seconds at 44.1 kHz. The interactive frontend
uses elapsed time between VBlanks to keep the same output rate when rendering
falls below 60 FPS. The new live-control WAV is 44.1 kHz stereo, with
33,064,272 sample frames (749.76 seconds); the frontend averaged 15.99 FPS,
reported six underruns, and had no write errors or truncated blocks. An
eight-second sample is available at
`build-saturnbox/rockman/stage-audio-preview-120s-20261027.wav`. This verifies
the output clock and file format, but the replay is too slow to compare its
wall-clock duration directly with the deterministic 200-second route.

The captured sound command queue still contains `0x01` (Sequence Start),
`0x05` (Sequence Volume), `0x08` (Map Change), `0x0E` (Sequence Pan), and
`0x87` (Mixer Change), as defined by Sega's [sound-driver command reference](https://www.infochunk.com/saturn/segahtml_en/sndt/prog/drve/sndcmd0.htm).
These commands require the loaded 68K driver and its sequence/tone data. The
SCSP pitch word uses OCT/FNS over the 44.1 kHz sample source ([ST-241 driver manual](https://antime.kapsi.fi/sega/files/ST-241-042795.pdf));
the current PCM HLE cannot synchronize or reproduce those voices and
sequence-based effects yet.

### Current development host compatibility

The development host is an Intel Core i5-4590 (Haswell, 4th generation) with
Intel HD Graphics 4600 and driver 20.19.15.4531. The Windows Vulkan loader is
absent. Intel's Vulkan setup guide lists 6th-generation Intel processors as the
minimum for its Intel Graphics Vulkan path
([Intel Vulkan guide](https://www.intel.com/content/dam/develop/external/us/en/documents/vulkan-on-unity-instructions-final.pdf)).
The current host therefore uses the CPU reference renderer through
`--renderer auto`; installing only the loader would not add driver support.
The historical Vulkan measurements above are not reproducible on this host.
Further visual and audio measurements here should use the CPU renderer. SCSP
68K sequence and voice execution remains independent of the graphics backend.

## Next validation steps

1. Use the live debugger during player-controlled dash/shot/explosion tests
   to investigate remaining cases after the VDP1 fix; probe VDP2 special
   priority at the pillar overlap.
2. Follow the CD requests that load gameplay overlays and correlate each
   resource with VDP1 texture uploads.
3. Execute the loaded sound driver on a 68EC000 core and handle the pending
   sequence/tone commands.
4. Validate visible gameplay effects and sound events against a synchronized
   reference recording.

Related reports: [static route and AOT results](ROCKMAN-X4-STATIC-RECOMPILATION.md),
[title/video rendering](ROCKMAN-X4-TITLE-MAP-FIX.md), and
[FMV/post-FMV loading](ROCKMAN-X4-FMV-CHECKPOINT.md).
