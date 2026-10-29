# Vulkan VDP1 and VDP2 Rendering

SaturnBox keeps the CPU video renderer as the reference path. Vulkan is an
optional compute backend selected with `--renderer auto` or `--renderer
vulkan`; `auto` falls back to CPU when a compatible Vulkan device is absent.
The backend needs a Vulkan loader, a compute queue, coherent host-visible
staging memory, and device-local storage memory. The VDP1 shader also requires
`shaderFloat64` for the inverse bilinear mapping used by transformed quads.

## Current VDP1 GPU path

The VDP1 command walker publishes sprites, transformed textured quads,
polygons, and fills as an ordered batch. A compute invocation owns one output
pixel and replays that batch in command order. This preserves framebuffer
feedback for transparent drawing and blending without inter-command atomics.
The shader handles packed 4/8-bit and direct color texture modes, transparent
texels, flips, clipping, Gouraud shading, mesh dithering, shadow, half
luminance, and half transparency. Degenerate quads and VDP1 line commands use
the CPU reference path.

Each batch copies VRAM, the framebuffer snapshot, and commands through mapped
host-coherent staging buffers into device-local storage. The dispatch covers
the union of the visible primitive bounds rather than the full 320-by-256
surface. The framebuffer is copied back after the compute pass, and its fence
is waited only when the CPU or presenter needs the result. This lets SH-2
execution and CPU VDP2 composition overlap the compute work.

## Current VDP2 path and remaining work

VDP2 composition remains on the CPU. RBG0 now has a first affine bitmap path:
it reads a fixed rotation parameter A or B record from VDP2 VRAM, applies the
fixed-point matrix, center, translation, and per-dot step, then samples direct
RGB555 or RGB888 bitmap data. The result enters the existing per-pixel priority
and color-calculation pipeline and honors the RBG0 window. Screen-over repeat
and transparent-outside modes are handled; the screen-over pattern option
currently repeats the bitmap.

This RBG0 slice does not yet render cell-based rotation maps, coefficient-table
scaling/perspective, mode-2/mode-3 A/B selection, or RBG1. The VDP2 transform
and background sampling are still CPU-only. After validating the complete
rotation source modes, they can move to a second Vulkan compute pass without
changing the CPU reference.

The VDP1/VDP2 division follows the hardware roles: VDP1 rasterizes command
lists into its framebuffer; VDP2 samples background layers and combines them
with sprite pixels using priority, windows, and color-calculation controls.
Mednafen is used as a behavioral reference for the ordered VDP1 primitive
pipeline and VDP2 rotation-parameter sampling. SaturnBox implements its own
code; the Mednafen source is GPL-licensed, so copying implementation code
requires a separate license-compatibility review.

References:

- [Mednafen VDP1 polygon rasterization](https://github.com/libretro-mirrors/mednafen-git/blob/master/src/ss/vdp1_poly.cpp)
- [Mednafen VDP2 rendering and rotation sampling](https://github.com/libretro-mirrors/mednafen-git/blob/master/src/ss/vdp2_render.cpp)
- [Mednafen VDP2 rotation parameter definitions](https://github.com/libretro-mirrors/mednafen-git/blob/master/src/ss/vdp2.cpp)

## Current development host compatibility (2026-10-29)

The current host has an Intel Core i5-4590 (Haswell, 4th generation) and Intel
HD Graphics 4600, driver 20.19.15.4531 (September 2016, the last Haswell
driver). No `vulkan-1.dll` is installed, there is no
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` key, and the only Intel compute DLL
present is `igdrcl64.dll` (OpenCL). Intel's Vulkan setup guide lists
6th-generation Intel processors as the minimum for its Intel Graphics Vulkan
path
([Intel Vulkan guide](https://www.intel.com/content/dam/develop/external/us/en/documents/vulkan-on-unity-instructions-final.pdf)),
and Intel's Vulkan 1.2 announcement covers Skylake and Kaby Lake
([Khronos](https://www.khronos.org/news/archives/intel-releases-vulkan-1.2-drivers)).
This host has no supported Vulkan device; installing only the loader does not
add driver support.

That was verified end to end on 2026-10-29 with a portable runtime taken from
the Edge installation (`vulkan-1.dll`, `vk_swiftshader.dll`,
`vk_swiftshader_icd.json`, copied next to `saturn-sh2.exe` so no system install
is needed). `--renderer-check` reports the three states:

| estado | saída |
| --- | --- |
| sem loader | `loader: missing (vulkan-1.dll is not in the executable directory, the system directory or PATH)` |
| loader, sem ICD | `verdict: Vulkan compute unavailable: vkCreateInstance failed (VkResult -9)` |
| loader + SwiftShader | `verdict: Vulkan compute unavailable: Vulkan compute with shaderFloat64 unavailable` |

`VkResult -9` is `VK_ERROR_INCOMPATIBLE_DRIVER`, i.e. the loader found no
driver. SwiftShader is a Vulkan 1.0 software ICD and does not expose
`shaderFloat64`, which the VDP1 shader needs for the inverse bilinear mapping
of transformed quads, so the backend correctly refuses it instead of rendering
differently. `--renderer auto` (the default) prints the reason once and uses
the CPU reference path; `--renderer vulkan` fails with the same reason plus a
hint.

Options for exercising the Vulkan path, in increasing order of effort:

1. Run it on a machine with a supported GPU. The performance numbers in
   [`pt-BR/PERFORMANCE.md`](pt-BR/PERFORMANCE.md) come from an RTX 3080 host.
2. Stage a float64-capable software ICD (Mesa lavapipe) next to the executable
   and point `VK_ICD_FILENAMES` at it. It exercises the backend for
   correctness, not for speed.
3. Remove the `shaderFloat64` requirement with a float32 shader variant and
   parity tests against the CPU reference. That changes pixel behaviour on
   edge cases and needs its own validation.

Keep Vulkan optional and measure visual and audio behavior on the CPU path for
this machine.
