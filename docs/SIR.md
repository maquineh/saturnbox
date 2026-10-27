# SIR — Saturn Intermediate Representation

SIR represents the Saturn program, not the PC. It is deliberately independent
of LLVM, x86-64, ARM64, Vulkan, SDL, and any other backend. The same program
can therefore be interpreted, analyzed, or converted to native code without
the SH-2 frontend knowing which implementation is selected.

## Planned levels

```text
SIR-L0  SH-2-like semantics, registers, and dynamic memory
SIR-L1  normalized CFG, promoted registers, classified addresses
SIR-L2  recognized Saturn hardware accesses
SIR-L3  high-level hardware-specific operations
```

M0 delivered the `sir::Builder` contract. M1 now materializes it as `Module`,
`Function`, `BasicBlock`, and `Instruction`. Values are numbered globally in
the module, blocks and functions have stable address-based identities, and
`ModuleBuilder` stores arithmetic, state, memory, and control flow without an
LLVM dependency. The initial static emitter generates host C++ per block; the
host C++ compiler produces x86-64 code that links into the runtime.

The first native backend compiles SIR blocks on demand to x64 code for Windows.
It supports 32-bit operations, carry/borrow, overflow, `DIV1`, conversions,
comparisons, state slots, and 8/16/32-bit memory. Memory accesses use
`MachineState` helpers, preserving big-endian RAM, MMIO interceptors, and
truncation of writes. In blocks that access memory, state slots are also
accessed in instruction order, allowing callbacks to observe and modify the
current state. Helper exceptions end a block and are rethrown in the C++
runtime, preserving prior effects; the block is not rerun in the interpreter.
Operations outside the supported subset cause the entire block to return to
the interpreter before execution begins.

The first 64 state slots use contiguous storage shared with native code.
Writes are immediately visible to memory callbacks; higher slots retain the
generic path. `stateSlots()` materializes a diagnostic snapshot with zero
values omitted, including when a callback requests it between native writes.
16/32-bit RAM loads and stores access a single page, with byte fallback when
crossing a page boundary or the end of the address space. `executeChain`
chains blocks with x64 jumps without growing the call stack. After each block,
the runtime commits state and calls a `ProgramExecutor` continuation. That
continuation resolves branches/calls/returns, handles `RTE` and BIOS callbacks,
publishes the next block's context, checks interrupts, and runs hooks. The JIT
then prepares its slots and jumps to the next block. The same path works during
boot/launch with MMIO and the scheduler, preserving addresses, callers, step
indices, and state snapshots for each access.

### Static native modules

`saturn-aot` builds CFGs from SH-2 segments and entry points, lowers reachable
blocks to SIR, and emits C++ functions plus a table ordered by address and SIR
signature. The CLI accepts multiple complete images with `--variant`; blocks
at the same address with different bodies get separate functions, and
identical signatures are emitted once. `StaticNativeBlockExecutor` compares
address and signature on first selection, then reuses the function while the
SIR body revision remains unchanged. Appending to or resetting a block
invalidates that selection, including negative results without a matching
static function. This avoids recalculating the signature on every execution
without losing version changes at the same address. `RuntimeBlockResolver`
compares code bytes when resolving an address and can replace a module block
body if an overlay changed. During boot, 4 KiB RAM-page generations invalidate
local resolution after a write; the next direct-target access revalidates its
bytes, and the block revision makes the static emitter select the matching
version. The 160M-step X replay on 2026-10-25 recorded 13 re-lowerings between
8,844,676 and 104,931,074, all with an unknown prior signature. These were
first resolutions against static SIR bodies, not evidence that active code
bytes had been replaced. No confirmed swap occurred during `right`
(122M–145M), and no rebind selected another AOT variant. A real overlay swap
during gameplay remains unverified; see
[`ROCKMAN-X4-STATIC-RECOMPILATION.md`](ROCKMAN-X4-STATIC-RECOMPILATION.md).
Missing or incompatible blocks continue to the JIT or, if it cannot execute
them, to the interpreter.

For incremental native compilation, `saturn-aot --output-dir` groups blocks by
64 KiB guest-address bank and emits one `.cpp` per bank, plus an aggregate
table and `saturnbox_sources.cmake`. The generator compares new text with each
existing file and preserves timestamps when content is unchanged. The
manifest removes only shards recorded by the previous run of this generator
that are no longer present. Set `SATURNBOX_STATIC_MODULE_DIR` to this
directory before configuring the launcher; the generated CMake list keeps the
source set synchronized. Decoder, CFG, and lowering still run on every AOT
invocation. This incremental step avoids recompiling unchanged C++ objects; it
does not yet cache guest analysis.

Generated C++ is compiled by the normal host compiler; the first validated
target is Windows PE x86-64. This static backend coexists with the JIT and the
Saturn HLE runtime. JIT tail chaining is disabled in mixed mode so each block
passes through static/JIT selection.

The Rockman X4 module uses `ip.bin`, `0.BIN`, and `X4LOW.X4`. In addition to
four initial entries, `--aot-entry-output` records indirect targets for
`saturn-aot --entry-list`, and `--aot-module-output` exports SIR accumulated
during a replay. That export contained 7,771 functions and 131,844 SIR
instructions; 56 unsupported blocks were omitted. During a 20-million-block
CD replay reaching the title, every step ran in the static module, with no JIT
or interpreter fallback. Diagnostics matched the interpreter except for the
static-execution counter. This validates the captured route, not full-game
recompilation. Entries, commands, and limitations are in the
[Rockman X4 static recompilation report](ROCKMAN-X4-STATIC-RECOMPILATION.md).
A second capture with a 14,000-block scheduler covered 30 million steps to the
first-stage stream; 11,232 static functions executed the route with no JIT or
fallback and diagnostics matching the interpreter. A 510-million-step route
added the stream started in `ST04_0.BIN`: 13,691 static functions executed the
entire route without JIT or fallback, and 65 shared records matched the
hybrid replay. Other stages, overlay versions, the second SH-2, and the SCSP
68K remain outside coverage.

The chain respects its block budget and stops when the program ends or the
next block cannot be compiled. If hooks for that successor already ran, the
fallback uses the prepared boundary without repeating effects. Hook/helper
exceptions are rethrown after leaving native code. Standalone `executeChain`
calls without a continuation remain limited to direct transfers without
memory interceptors.

### Win64 validation on 2026-10-04

- Release builds of `saturn-sh2` and `saturnbox-tests`; **109/109 tests
  passed**.
- Carry/borrow/overflow and 32 consecutive `DIV1` steps compared with the
  interpreter across 3,616 input combinations, in both pure blocks and blocks
  with memory. Cases include 32-bit limits, all Q/M/T combinations,
  non-canonical input flags, and deterministic pseudo-random inputs.
- Chaining compared with the reference executor for conditional branches,
  calls/returns, RAM, PC, different budgets, fallback, and a 200,000-block
  loop. MMIO, hooks, nested interrupts, `RTE`/BIOS-callback returns, missing
  interrupt targets, and boundary exceptions are also compared.
- Real Rockman X4 IP/AIP + `0.BIN` replay: 1,000,000 native blocks, 250
  compiled blocks, no fallback, and handoff to `0x06010000`. The
  `--interpreter` replay produced the same published CPU, resolver, device,
  and scheduler diagnostics; WAV SHA-256 matched:
  `f948bfe750270a5ec4f6d57b43cb94b61c40d3903b7b55e6d461d01806d33148`.
  Local artifacts: `build/artifacts/win64-jit-validation-20261004/`.

This initial replay uses `--steps 1000000 --continue-after-execdmp
--vblank-blocks 10000` and has no CD image. It does not demonstrate visual or
audio progress, all-path coverage, or dual-SH-2 execution. Native-block and
fallback counts are published by `run`, `boot`, and `launch`; `--interpreter`
repeats the same scenario with the reference engine.

### CD replay and interrupt chaining

The MDF image was found at `H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf`; its
SHA-256 matches the inventory:
`c1977179f386a2131cfdaf73417f4de8eb1f7fb984049518c2b2028330b09387`. Two
IP/AIP + `0.BIN` replays, one native and one with `--interpreter`, use the same
2448-byte-sector disc, `--steps 20000000 --continue-after-execdmp`,
`--vblank-blocks 10000`, Start at block 13,700,000, and capture at block
19,000,000.

| Native replay metric | Result |
| --- | ---: |
| Natively executed blocks | 20,000,000 |
| Blocks compiled on demand | 4,759 |
| Native-jump chained transfers | 19,999,999 |
| Blocks sent to the interpreter | 0 |
| Delivered interrupts | 504,846 |
| Frames produced by the runtime | 5,381 |

Both replays end at `step-limit`, reach the handoff, and progress through the
opening to the title screen showing `PRESS START BUTTON`. After normalizing
output paths and excluding backend counters, all 12,953 diagnostic lines
match. The 37 video, VDP1/VDP2 RAM, register, and GFS/DMA trace artifacts have
matching hashes. The captured frame is frame 5,154, with PPM SHA-256
`66f04890d954ef7d21725e8ae617c745fe6693365dfbccbb015ae557f0785608`. WAV
files are identical, SHA-256
`ad14173c0d49c1bd2071c0e1162747e9394b37fd3d5bf4c2fb68aeef89e28fb7`.

Local artifacts: `build/artifacts/win64-jit-cd-validation-20261004/`,
including `replay-comparison.json`, full logs, WAV files, PPM capture, and
`frame-native.png`. This validates boot, opening, and transition to the title.
It does not cover every game stage, dual-SH-2 execution, full HLE device
accuracy, or 60 Hz performance in the launcher; the replays are block-limited
and use extensive instrumentation.

### Native runtime optimization — 2026-10-04

`MachineState` directly reads the first 64 state slots and stores RAM in
dense, on-demand 64 KiB pages. Sparse RAM still excludes zero bytes, but is
materialized by `memory()` only for diagnostics. Interceptors run before RAM
backing; `readRaw8()` is reserved for observers that must avoid MMIO recursion.
Per-block register snapshots are optional and remain enabled for tracing and
BIOS calls.

In the same three-million-block CD boot, a headless run dropped from **19.598
seconds to 4.811 seconds** (about 4.07×). This is one run per version on this
host, including startup and on-demand compilation; it is not a benchmark of
all games or proof of a static-backend speedup.

The untraced launcher defers host composition until presentation. A
20-million-block replay **without Start** made 2,000 presentations/compositions
for 10,842 internal frames, averaging **49.145 FPS**, with 26 audio underruns,
no full-queue waits, no write errors, and no fallback. Before paged RAM without
write-through and deferred composition, an intermediate build in the same
session measured 13.13 FPS and 300 underruns. The 109 existing tests passed.
Compared final states and deterministic counters are in
`build/artifacts/performance-20261004/performance-result.json`.

The window uses 50 ms PCM prebuffering and backpressure. WinMM calls do not
hold the mutex used by the completion callback. Remaining cuts show that
production still cannot sustain the audio clock throughout the whole segment.
Natural completion, full `OP.CPK` visual fidelity, Vulkan/compute, and complete
static game emission remain pending.

## Architectural flags

SIR-L0 represents arithmetic results with flags as typed pairs
`{value: i32, flag: i1}`. The contract includes add-with-carry,
subtract-with-borrow, overflow detection, bit extraction, and logical or
arithmetic shifts. `DIV1` remains an L0 semantic primitive and returns
`{value, q, t}`; `M` is an explicit input and remains in architectural state.
This keeps `T/M/Q` observable to the future interpreter instead of lowering
the algorithm prematurely to host operations.

## Initial SH-2 → SIR mapping

| SH-2 | Planned SIR-L0 |
| --- | --- |
| `MOV Rm,Rn` | `copy.i32` + register write |
| `MOV #imm,Rn` | `const.i32` + register write |
| `MOV.B/W @Rm,Rn` | `load8/load16`, sign extension, write |
| `MOV.L @Rm,Rn` | `load32`, write |
| `MOV.* Rm,@Rn` | `store8/store16/store32` |
| `ADD`, `SUB`, logical ops | matching i32 operation |
| `CMP/*` | comparison and T-flag write |
| transfers | SIR branch/call emitted after delay slot |

Byte and word loads need sign extension before writing the SH-2 register. This
is a semantic requirement, not an optional optimization.

Calls carry an explicit return block, indirect jumps keep their target as
`i32`, and `RTS` receives the value of `PR` captured before the delay slot.
`RTE`, `TRAPA`, and `SLEEP` remain architectural SIR-L0 primitives.

## Interpreter

The interpreter executes a SIR block over `MachineState`, which contains state
slots and sparse big-endian memory. 32-bit arithmetic uses explicit wraparound;
carry, borrow, and overflow do not depend on host signed overflow. `DIV1`
follows the SH-2 manual's bitwise algorithm. The execution result separates
produced state from control-flow observations: branch, call, return, trap, or
exception return.

Zero values are not stored in sparse maps, but reading a missing slot or byte
returns zero. This makes differential state copies deterministic without
requiring a complete Saturn hardware map yet.

## Program execution and MMIO

`ProgramExecutor` chains blocks and limits execution by the number of
completed blocks (`maxSteps`), stopping loops without a thread or timer. For
indirect branches, calls, and returns, it asks a `BlockResolver` to resolve the
runtime-computed address. The SH-2 executor supplies a resolver that lowers a
new `MemoryMap` region on demand when the target was not in the initial CFG.

`MachineState` accepts a `MemoryInterceptor`. Sparse RAM remains the fallback,
but an interceptor can answer reads and consume writes without modifying RAM.
The Saturn implementation intercepts VDP1, VDP2, SCU, SCSP, and CD Block,
records width/address/value, and maintains a provisional deterministic
register bank.

## Metadata and analysis safety

Future versions will add source address, original word, CPU, cycles, symbols,
trace observations, and `Confidence`. An analysis without sufficient evidence
must preserve a dynamic operation or `unknown`, never invent a hardware call.
This separation allows static analysis and execution traces to be combined
later.
