# Saturn Disc Images

Saturn discs use CD-ROM, usually with an ISO 9660 filesystem. The reader in
`rust/saturn-disc` separates the physical sector layer from the filesystem
layer.

## Physical layout

A plain ISO has 2048-byte logical sectors. A BIN may contain 2352-byte raw
sectors:

| Format | ISO data within sector |
| --- | --- |
| ISO/MODE1/2048 | offset 0, 2048 bytes |
| MODE1/2352 | offset 16, 2048 bytes |
| MODE2/2352 | offset 24, 2048 bytes |
| MODE1/2448 (MDF with subchannels) | offset 16, 2048 bytes |

The reader looks for the Volume Descriptor at LBA 16 and checks the `CD001`
identifier. For CUE files, it selects the first data track (`MODE1/2048`,
`MODE1/2352`, or `MODE2/2352`) and honors `INDEX 01`.

MDF files may contain a prefix and 2448-byte sectors. The reader detects this
variant by searching for and validating the PVD at candidate offsets, keeping
the 96 subchannel bytes out of ISO data.

## ISO 9660

- LBA 16: Primary Volume Descriptor (type 1).
- PVD byte 156: root-directory record.
- Each directory record contains the starting LBA, size, flags, and name.
- Names commonly end in `;1`; the reader removes this suffix.
- The directory flag is `0x02` in the record flags.
- Numeric fields are stored in both little-endian and big-endian forms. The
  reader uses the little-endian copy, which is sufficient for the target discs.

Logical sector zero begins with the System ID `SEGA SEGASATURN`. `boot_info()`
exposes these fields, including IP size, stacks, and `FIRST READ ADDRESS`.
Header integers are big-endian, unlike the little-endian copy used in ISO 9660
directory records.

The boot ROM loads the IP at `0x06002000`. It transfers the first regular file
in the root directory (the Sega documentation's file identifier [2]) to
`FIRST READ ADDRESS`. This is a **load address, not an entry point**: the
Application Initial Program (AIP) inside the IP decides when and where to
transfer execution. `initial_program()` returns the complete IP, and
`first_read_file()` identifies the loaded file without relying on a filename
heuristic.

The AIP follows the System ID (`0x100`), security code (`0xD00`), and a
`0x20`-byte area-code object for each compatible region. Its offset is
`0xE00 + region_count * 0x20`; its address is that offset added to
`0x06002000`. `aip_offset()` and `aip_address()` calculate both values and
verify that the result remains inside the IP.

## Usage

```sh
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- list GAME.CUE
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- list GAME.CUE --extents
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- inspect GAME.CUE
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract GAME.CUE --output GAME.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract GAME.CUE 0.BIN --output GAME.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-ip GAME.CUE --output IP.BIN
cargo run --manifest-path rust/saturn-disc/Cargo.toml -- extract-aip GAME.CUE --output AIP.BIN
```

`list --extents` adds each file's starting LBA to the listing, allowing FADs
observed on the CD Block to be correlated with disc files.

An extracted file is ready for the SH-2 decoder. The next step analyzes AIP
control flow to find where it enters the first file instead of assuming that
the load address is also the entry point.

The extracted AIP can be inspected by the C++ frontend using the address
reported by `inspect` or `extract-aip`:

```sh
saturn-sh2 decode AIP.BIN --base 0x06002E40
saturn-sh2 cfg AIP.BIN --base 0x06002E40 --entry 0x06002E40 --output aip.dot
```
