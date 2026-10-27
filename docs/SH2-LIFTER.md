# SH-2 Lifter

## Contract

The lifter accepts a `DecodedInstruction` and a `LiftContext`. It does not
decode words, recalculate PC-relative targets, or depend on LLVM. The intended
design is an `Opcode → LiftFn` table rather than a monolithic switch:

```cpp
using LiftFn = LiftResult (*)(const DecodedInstruction&, LiftContext&);
```

`LiftContext` provides register and `T/M/Q` flag access, the source PC, and a
`sir::Builder`. The first M1 slice uses a dispatch table and emits moves,
memory accesses, arithmetic/logical operations, and basic comparisons.
Block lowering emits control transfers, keeping delay slots outside the
instruction lifter.

Instructions that update flags (`ADDC`, `SUBC`, `DIV0S`, `DIV0U`, `DIV1`, shifts,
and rotates) use explicit `i1` values. Carry/borrow operations return the
result and flag separately.

## Delay slots

A delay slot belongs to a block terminator; the decoder neither executes nor
hides it. A block can represent the relationship explicitly:

```text
transfer: BRA target
slot:     ADD #1,R0
```

SIR lowering captures the values that determine the transfer before the slot,
executes the slot, and then emits the terminator. This preserves `T` for
`BT/S`/`BF/S`, the registered target of `JMP`/`JSR`/`RTS`, and the `PR` update
visible to a call's slot. Missing slots or slots containing another transfer
are rejected before any SIR is emitted.

`BlockLiftContext` resolves SH-2 addresses to SIR blocks and functions.
Unresolved indirect destinations remain dynamic; lowering does not invent a
target. The contract covers branches, calls with return sites, return through
`PR`, `RTE`, `TRAPA`, `SLEEP`, and explicit fallthrough.

`lowerCFG` creates stable identities for blocks and functions, connects a
`ModuleBuilder` to SH-2 architectural state, and materializes all recovered
blocks. Registers and flags are explicit SIR slots; writes are visible to
later instructions in the same block.

## Differential validation

`ReferenceExecutor` executes decoded SH-2 instructions directly on a copy of
`MachineState`. `compareBlock` executes another copy in the SIR interpreter
and compares registers, `T/M/Q`, memory, and the control-flow observation.
The reference executor returns `UnsupportedInstruction` for instructions
outside its subset rather than using SIR as its own reference.

At this stage, `RTE` and `TRAPA` are compared as architectural events. Full
exception-stack entry and exit belong to the runtime.

## Function recovery

M0 records entry points as proven candidates and `BSR` targets as
high-confidence candidates. Later function recovery can use direct calls,
resolved indirect calls, reachable `RTS` instructions, overlaps, and traces.
A function must not be marked certain when its evidence is only heuristic.

The CFG already performs conservative local constant propagation. It resolves
the common pattern `MOV.L @(disp,PC),Rn` followed by `JMP/JSR Rn` by reading a
big-endian longword from the constant pool. An intervening write invalidates
the value; targets outside the image remain external nodes in DOT output.
This recovers the AIP jump to the first program without making all discovery
speculative.

## Validation plan

Each lifter opcode has three validation levels:

1. Decoder: word → typed description.
2. Lifter: description → expected SIR.
3. Differential: reference SH-2 state versus SIR execution.

All three levels are active for representative arithmetic, flags, division,
memory, control-flow, and delay-slot cases. M0 tests cover decoding, target
calculation, control-flow classification, delay slots, recursive block
discovery, and call/return-site edges.

## Coverage report

`saturn-sh2 analyze` builds the reachable CFG and reports missing opcodes.
An invalid word in a delay slot receives `CRITICAL`; a word that ends block
discovery receives `HIGH`; occurrences that do not block the CFG receive
`MEDIUM`. The report also lists unresolved indirect transfers without
guessing their destinations.
