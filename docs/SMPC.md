# Minimal SMPC Runtime

The runtime implements a deterministic subset of the SMPC sufficient to
configure digital pads and observe the protocol used by the game.

## Implemented contract

- SH-2 byte-addressed registers at `0x20100001..0x20100063`.
- `IREG0..IREG6`, `COMREG`, `OREG0..OREG31`, `SR`, and `SF`.
- `INTBACK` (`0x10`) executes immediately and is resampled at `VBlank-IN` if
  the request remains armed.
- `SYSRES`, `RESENAB`, and `RESDISA` commands are represented as explicit
  state.
- Two ports, connect/disconnect state, and a digital pad with ID `0x02`.
- Saturn pad buttons are active-low across the two pad bytes.

The first peripheral packet sets PDL=1 and NPE=0, using the port modes in
`SR[3:0]`. `INTBACK` publishes SCU interrupt `0x47`. When `IREG0` requests
status, this packet precedes peripheral data and waits for CONTINUE (bit 7
toggle); BREAK (bit 6) ends the exchange. RTC/SMEM return zero in this subset.

Immediate execution preserves the protocol visible to code reading OREGs.
The VBlank scheduler refresh adds time-based sampling without changing the
SH-2/SIR byte-only interface.

## Configuration

```text
--pad1 up,left,a
--pad2 none
```

Supported names: `up`, `down`, `left`, `right`, `start`, `a`, `b`, `c`, `x`,
`y`, `z`, `l`, and `r`.

## References

- SEGA, *SMPC User's Manual*, ST-169-R1-072694, sections 1.2 and 3.1.
  <https://docs.exodusemulator.com/Archives/SegaDTSMirror/antime/docs/ST-169-R1-072694.pdf>
- SEGA, *System Program User's Manual*, SMPC commands and system services.
  <https://www.infochunk.com/saturn/segahtml_en/prgg/sysg/prog/hon/p02_11.htm>
