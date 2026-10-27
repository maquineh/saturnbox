# Writes to NBG0 Range `0x70000–0x71FFF`

This report traces writes to the physical address range calculated as the
NBG0 plane for the first black-frame configuration:
`0x25E70000–0x25E71FFF` (VDP2 offsets `0x70000–0x71FFF`). The same region was
used as the vertical-cell-scroll table while NBG0 was in bitmap mode. The
trace records SH-2 step, block, caller, registers, previous/new value, and any
SCU-DMA transfer associated with the write context.

Interpretation updates are in the [title-map report](ROCKMAN-X4-TITLE-MAP-FIX.md).
The historical capture also labels `cpu-store` for some DMA writes whose
descriptor exceeded the older history limit.

## Capture

```powershell
& build/tools/saturn-sh2/Release/saturn-sh2.exe boot build/rockman/aip.bin `
  --aip-base 0x06002E40 --ip build/rockman/ip.bin --ip-base 0x06002000 `
  --program build/rockman/0.BIN --program-base 0x06009000 `
  --entry 0x06002E40 --expected-entry 0x06010000 `
  --disc 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf' --disc-sector-size 2448 `
  --continue-after-execdmp --vblank-blocks 10000 --trace-game --steps 12000000 `
  --vdp2-map-trace build/artifacts/map-producer-trace-20260920-e
```

Files: `vdp2-map-producer.csv` and `vdp2-map-dma-transfers.csv` under
`build/artifacts/map-producer-trace-20260920-e/`. The CSV associates the active
SH-2 context with each transfer record. A DMA attribution requires matching
step, block, and caller, preventing a CPU write from being assigned to an
earlier DMA that covered all of VRAM.

Each row also includes `previous`, read immediately before the write, and
`changed`. This distinguishes a zero write that clears a cell from a zero
rewrite of an already-zero cell.

## Results

| Producer (block / caller) | Steps | Writes | Values written | Interpretation |
| --- | ---: | ---: | --- | --- |
| CPU, `0x06002FD4` / `0x06002FD4` | `106945–108992` | 2,048 × 32-bit | all `0x00000000` | all were already zero |
| SCU DMA channel 0, context `0x06034B5C` / `0x0F000018` | `502806` | 4,096 × 16-bit | all `0x0000` | all were already zero |
| CPU, `0x0601667C` / `0x06016672` and `0x0601667C` | `515553–515574` | 44 | `0x0000`, then `0x0400` through `0xAC00` | 43 zero-to-nonzero changes |
| CPU, `0x06034B5C` / `0x0F000018` | `8673634` | 4,096 × 16-bit | all `0x0000` | 43 nonzero values were cleared |
| CPU, `0x06012DBC` / `0x06012DBA` | `8675512` and `11257939` | 8,192 × 16-bit | all `0x0000` | all were already zero |

There were 18,476 writes total: 14,380 by CPU and 4,096 by DMA. Of these,
18,433 values were zero and 43 were nonzero. There were 86 effective changes:
43 cells changed from zero to nonzero during loading, and the same 43 changed
from `0x0400–0xAC00` to zero during cleanup at step `8673634`. The other
18,390 zero writes targeted cells that were already zero.

The 43 nonzero values came from block `0x0601667C`. The first value is
`0x0400` at `0x25E7000C`; subsequent values occur two entries per step, from
`0x0800` through `0xAC00`, at offsets up to `0x7015C`. Cleanup clears cells at
offsets `0x7000E–0x7015E`; `previous` records each nonzero value before
`0x0000` is written.

The matching DMA is history entry 4, channel 0:

```text
source      = 0x06009F6C
destination = 0x25E00000
count       = 0x80000 bytes
add         = 0x1
mode        = 0x107
map writes  = 4096 halfwords, all 0x0000
context     = step 502806, block 0x06034B5C, caller 0x0F000018
```

With `add=0x1`, the destination advances in 2-byte units while the source
remains fixed in this mode. The event therefore fills the NBG0 range from one
zero source word; it is not a copy of varying map data. The source is not the
CD FIFO (`source=0x06009F6C`).

## Interpretation

The capture shows the CPU writing 43 nonzero values into this region while
NBG0 is in bitmap mode, then clearing those same positions at step `8673634`.
The DMA and other cleanup routines rewrite zeros over cells already cleared.
Because `VCSTA=0x70000` during bitmap mode, the writes may be scroll data; the
trace does not prove that the CPU loaded and then erased the title name map.
The later read capture, with the address corrected, shows the range still
zero in cell mode. The routine or resource that should populate the title map
has not yet been identified.
