"""Static analysis of a Rockman X4 SCSP sound-RAM snapshot (``scsp_ram.bin``).

The snapshot is 512 KiB of SCSP sound RAM captured from a running session; the
Sega sound driver ``SDDRVS.TSK`` sits at offset 0 (68000 reset vectors in the
first two longwords).  This script only reads bytes: it never executes 68000
code and never builds the emulator.

Reported analyses
-----------------
1. Sequence-bank recovery.  Scans every word-aligned address for a bank table:
   a big-endian u16 entry count (1..128) followed by that many strictly
   increasing big-endian u32 entry offsets, each >= the end of the table and
   inside a plausible bank length.  Every candidate is listed; candidates whose
   first offset is exactly the end of the table (grade A) are accepted as banks,
   the rest are reported but rejected.  Candidates that overlap an accepted bank
   are deduplicated.
2. Per-entry records: bank, sequence number, address, size, SHA-256 and payload
   bytes.
3. "Families": entries that are byte-identical to another entry except for a
   bounded number of offsets.  Grouping is done by exact difference set (no
   transitive chaining), so each reported group lists precisely which offsets
   vary and which byte values occur there.
4. References to the effect-DSP register window (0x00100700-0x00100EFF) inside
   the 68K driver image: absolute longword operands, d16(An) displacements in
   0x700..0xEFF (with the last explicit long-immediate load of that address
   register reported as context), and every occurrence of the DSP start
   constant 0x00100BF0 as well as of the bare word 0x0BF0.
5. Heuristic search for DSP programs (128 four-word steps).  This is explicitly
   a heuristic: the SCSP has no memory-mapped DSP program area in sound RAM, so
   the only thing a dump can show is a *candidate* data block that parses as a
   field-valid program.

Only the Python standard library is used.  Hex dumps and JSON records can be
written under the directory given by ``--out-dir``.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

SCSP_RAM_SIZE = 0x80000
DRIVER_CODE_LO = 0x1000
DRIVER_CODE_HI = 0x6800
DRIVER_IMAGE_HI = 0x6800
SCSP_BASE = 0x00100000
DSP_WINDOW_LO = 0x700
DSP_WINDOW_HI = 0x0EFF
DSP_MPRO_LO = 0x800
DSP_MPRO_HI = 0x0BFF
DSP_START = 0x00100BF0
MAX_BANK_ENTRIES = 128
MAX_ENTRY_OFFSET = 0x8000
BASE_LOAD_WINDOW = 0x400
KNOWN_BANK3 = 0x29A1A


# --------------------------------------------------------------------------- #
# byte helpers
# --------------------------------------------------------------------------- #
def u16(data, offset):
    return struct.unpack_from(">H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from(">I", data, offset)[0]


def s16(data, offset):
    return struct.unpack_from(">h", data, offset)[0]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def hex_context(data, offset, before=8, after=10):
    start = max(0, offset - before)
    end = min(len(data), offset + after)
    return data[start:end].hex(" ")


# --------------------------------------------------------------------------- #
# 1. sequence-bank table scan
# --------------------------------------------------------------------------- #
def scan_bank_candidates(ram):
    """Return every word-aligned bank-table candidate, graded by plausibility.

    A candidate is a u16 count (1..128) at ``addr`` followed by ``count``
    strictly increasing u32 offsets.  Grade A: the offsets start exactly at the
    end of the table, so the table is immediately followed by its payload (this
    is what the real driver does).  Grade B: the table is followed by padding.
    Grade C: a single-entry table whose only offset is far past the table end,
    which is the shape random code/data produces.
    """
    candidates = []
    for addr in range(0, len(ram) - 6, 2):
        count = u16(ram, addr)
        if count < 1 or count > MAX_BANK_ENTRIES:
            continue
        table_end = 2 + 4 * count
        if addr + table_end > len(ram):
            continue
        offsets = [u32(ram, addr + 2 + 4 * index) for index in range(count)]
        if any(offset < table_end for offset in offsets):
            continue
        if any(left >= right for left, right in zip(offsets, offsets[1:])):
            continue
        if offsets[-1] >= MAX_ENTRY_OFFSET:
            continue
        if offsets[0] == table_end:
            grade = "A"
        elif count == 1:
            grade = "C"
        else:
            grade = "B"
        candidates.append({
            "address": addr,
            "count": count,
            "table_end": table_end,
            "offsets": offsets,
            "grade": grade,
        })
    return candidates


def recovered_banks(candidates, ram_size):
    """Accept grade-A candidates, deduplicate overlaps, and derive extents.

    Grade-A tables are accepted in address order; a later table that starts
    inside the previous bank's entry area is treated as a duplicate and dropped.
    The bank end is the start of the next bank, because the driver packs the
    five tables back to back (verified by the documented extent of bank 3).  The
    last bank has no following table, so its end is inferred from the modal
    entry size of that bank and flagged accordingly.
    """
    accepted = []
    for candidate in sorted((item for item in candidates if item["grade"] == "A"),
                            key=lambda item: item["address"]):
        start = candidate["address"]
        if accepted:
            previous = accepted[-1]
            previous_end = previous["address"] + previous["offsets"][-1]
            if start < previous_end:
                continue
        if start + candidate["offsets"][-1] > ram_size:
            continue
        accepted.append({**candidate, "_extent": start + candidate["offsets"][-1]})
    for index, bank in enumerate(accepted):
        start = bank["address"]
        if index + 1 < len(accepted):
            end = accepted[index + 1]["address"]
            inferred = False
        else:
            sizes = [b - a for a, b in zip(bank["offsets"], bank["offsets"][1:])]
            modal = max(set(sizes), key=sizes.count) if sizes else 2
            end = start + bank["offsets"][-1] + modal
            inferred = True
        bank["extent_end"] = end
        bank["extent_inferred"] = inferred
        bank["bank"] = index
        del bank["_extent"]
    return accepted


def collect_entries(ram, banks):
    entries = []
    for bank in banks:
        start = bank["address"]
        count = bank["count"]
        offsets = bank["offsets"]
        payload_end = bank["extent_end"] - start
        for number, offset in enumerate(offsets):
            end = offsets[number + 1] if number + 1 < count else payload_end
            if end <= offset or start + end > len(ram):
                continue
            payload = ram[start + offset:start + end]
            entries.append({
                "bank": bank["bank"],
                "sequence": number,
                "address": start + offset,
                "size": len(payload),
                "sha256": sha256(payload),
                "hex": payload.hex(),
            })
    return entries


def entry_structure(entries):
    """Check the recurring 8-byte prefix / 2-byte suffix of X4 sequence entries.

    The check is only a consistency signal for the recovered extents: it is not
    used to cut entries.
    """
    prefixes = {}
    suffixes = {}
    for entry in entries:
        payload = bytes.fromhex(entry["hex"])
        prefixes[payload[:8]] = prefixes.get(payload[:8], 0) + 1
        suffixes[payload[-2:]] = suffixes.get(payload[-2:], 0) + 1
    top_prefix = max(prefixes.items(), key=lambda item: item[1])
    top_suffix = max(suffixes.items(), key=lambda item: item[1])
    with_prefix = sum(1 for entry in entries
                      if bytes.fromhex(entry["hex"])[:8] == top_prefix[0])
    with_suffix = sum(1 for entry in entries
                      if bytes.fromhex(entry["hex"])[-2:] == top_suffix[0])
    return {
        "prefix": top_prefix[0].hex(" "),
        "prefix_matches": with_prefix,
        "suffix": top_suffix[0].hex(" "),
        "suffix_matches": with_suffix,
        "entries": len(entries),
    }


# --------------------------------------------------------------------------- #
# 3. families of near-identical entries
# --------------------------------------------------------------------------- #
def difference_families(entries, max_differences):
    """Group entries by exact difference set against a reference payload.

    Two entries belong to the same group when they have the same size, differ
    from the reference at the exact same set of offsets, and that set has at
    most ``max_differences`` offsets.  Differences are never chained, so every
    member of a group differs from the reference at exactly the listed offsets.
    """
    payloads = [bytes.fromhex(entry["hex"]) for entry in entries]
    groups = {}
    for reference_index, reference in enumerate(payloads):
        for other_index, other in enumerate(payloads):
            if len(other) != len(reference):
                continue
            offsets = tuple(index for index in range(len(reference))
                            if other[index] != reference[index])
            if len(offsets) > max_differences:
                continue
            template = bytes(0 if index in offsets else reference[index]
                             for index in range(len(reference)))
            key = (len(reference), offsets, template)
            groups.setdefault(key, set()).add(other_index)
    families = []
    for (size, offsets, _template), members in groups.items():
        if len(members) < 2:
            continue
        ordered = sorted(members)
        reference = ordered[0]
        varying = {}
        for offset in offsets:
            values = sorted({payloads[index][offset] for index in ordered})
            varying[offset] = values
        families.append({
            "size": size,
            "differences": list(offsets),
            "varying": {str(offset): [f"{value:02x}" for value in values]
                        for offset, values in varying.items()},
            "members": [{"bank": entries[index]["bank"],
                         "sequence": entries[index]["sequence"],
                         "address": entries[index]["address"]}
                        for index in ordered],
            "reference_sha256": entries[reference]["sha256"],
        })
    families.sort(key=lambda item: (-len(item["members"]), len(item["differences"]),
                                    item["size"]))
    return families


# --------------------------------------------------------------------------- #
# 4. DSP-window references in the driver image
# --------------------------------------------------------------------------- #
def ea_extension_words(word, size_long):
    """Extension-word count for the source EA encoded in bits 5..0 of a MOVE."""
    mode = (word >> 3) & 7
    register = word & 7
    if mode == 5:
        return 1
    if mode == 6:
        return 1
    if mode == 7:
        if register == 0:
            return 1
        if register == 1:
            return 2
        if register in (2, 3):
            return 1
        if register == 4:
            return 2 if size_long else 1
    return 0


def previous_word_ends_instruction_stream(ram, pos):
    """True when the word before ``pos`` carries a PC-relative displacement.

    The sliding d16 scan would otherwise read the displacement of a
    ``bra/jmp d16(PC)`` as an opcode and the following opcode word as a
    displacement.  Only the 16-bit (low byte 0x00) and 32-bit (0xFF) branch
    forms consume the following word; ``bra.s``/``bne.s`` do not.
    """
    if pos < 2:
        return False
    word = u16(ram, pos - 2)
    if (word & 0xF000) == 0x6000 and (word & 0xFF) in (0x00, 0xFF):
        return True
    return word in (0x4EBA, 0x4EFA)


def scan_dsp_window_displacements(ram):
    """d16(An) operands with a displacement in 0x0700..0x0EFF.

    Covers the 68K forms that matter for a register window: ``lea d16(An),Am``,
    MOVE with a d16 source or destination, and single-effective-address
    instructions whose EA field (bits 5..0) is mode 101.  A sliding sweep is
    used because the driver interleaves data tables with code; hits whose
    preceding word is a PC-relative displacement are discarded.
    """
    hits = []
    lo, hi = DRIVER_CODE_LO, DRIVER_CODE_HI
    for pos in range(lo, hi - 10, 2):
        if previous_word_ends_instruction_stream(ram, pos):
            continue
        word = u16(ram, pos)
        top = word & 0xF000
        if top in (0x1000, 0x2000, 0x3000):
            size_long = top == 0x2000
            source_mode = (word >> 3) & 7
            destination_mode = (word >> 6) & 7
            source_extension = ea_extension_words(word, size_long)
            if source_mode == 5:
                disp = u16(ram, pos + 2)
                if DSP_WINDOW_LO <= disp <= DSP_WINDOW_HI:
                    hits.append({
                        "offset": pos, "form": "move.source", "opcode": word,
                        "displacement": disp, "base_register": word & 7,
                        "kind": "byte" if top == 0x1000 else
                                "long" if top == 0x2000 else "word",
                        "confirmed": True,
                        "context": hex_context(ram, pos),
                    })
            if destination_mode == 5:
                location = pos + 2 + 2 * source_extension
                if location + 2 <= len(ram):
                    disp = u16(ram, location)
                    if DSP_WINDOW_LO <= disp <= DSP_WINDOW_HI:
                        hits.append({
                            "offset": pos, "form": "move.destination", "opcode": word,
                            "displacement": disp, "base_register": (word >> 9) & 7,
                            "kind": "byte" if top == 0x1000 else
                                    "long" if top == 0x2000 else "word",
                            "confirmed": True,
                            "context": hex_context(ram, pos),
                        })
            continue
        if (word & 0xF1C0) == 0x41C0 and ((word >> 3) & 7) == 5:
            disp = u16(ram, pos + 2)
            if DSP_WINDOW_LO <= disp <= DSP_WINDOW_HI:
                hits.append({
                    "offset": pos, "form": "lea", "opcode": word,
                    "displacement": disp, "base_register": word & 7,
                    "destination_register": (word >> 9) & 7,
                    "kind": "long",
                    "confirmed": True,
                    "context": hex_context(ram, pos),
                })
            continue
        if ((word >> 3) & 7) == 5 and top not in (0x0000, 0x4000, 0x5000,
                                                  0x6000, 0x7000, 0xE000):
            disp = u16(ram, pos + 2)
            if DSP_WINDOW_LO <= disp <= DSP_WINDOW_HI:
                # Any other opcode with a d16(An) effective address.  A sliding
                # sweep cannot tell this apart from the extension word of a
                # PC-indexed instruction, so the hit is reported unconfirmed.
                hits.append({
                    "offset": pos, "form": "single-ea", "opcode": word,
                    "displacement": disp, "base_register": word & 7,
                    "kind": "word",
                    "confirmed": False,
                    "context": hex_context(ram, pos),
                })
    return hits


def is_long_immediate_load_into(word, register):
    """``lea imm.l,An`` (0x41F9) or ``movea.l #imm,An`` (0x207C) for An."""
    if (word & 0xF1FF) not in (0x207C, 0x41F9):
        return False
    return ((word >> 9) & 7) == register


def is_address_register_write(word, register):
    """Any other ``An`` write that would invalidate a tracked base value."""
    if ((word >> 9) & 7) != register:
        return False
    top = word & 0xF000
    if top in (0x1000, 0x2000, 0x3000) and ((word >> 6) & 7) == 1:
        return True  # move.<size> <ea>,An  (0x0040 destination mode)
    if (word & 0xF1C0) in (0xD0C0, 0x90C0):  # adda / suba
        return True
    if (word & 0xF1C0) == 0x2040:  # movea.<size> <ea>,An
        return True
    return False


def last_base_load(ram, pos, register):
    """Last explicit long-immediate load of ``An`` before ``pos``, if any.

    Returns ``(offset, value)`` where ``value`` is ``None`` when the last write
    to that address register was not a recognisable constant load.
    """
    start = max(DRIVER_CODE_LO, pos - BASE_LOAD_WINDOW)
    if start % 2:
        start += 1
    found = None
    for location in range(start, pos, 2):
        word = u16(ram, location)
        if is_long_immediate_load_into(word, register):
            found = (location, u32(ram, location + 2))
        elif is_address_register_write(word, register):
            found = (location, None)
    return found


def annotate_base(hit, ram):
    base = last_base_load(ram, hit["offset"], hit["base_register"])
    hit["base_load"] = None
    if base is not None:
        hit["base_load"] = {"offset": base[0], "value": base[1]}
        if base[1] == SCSP_BASE:
            hit["target"] = SCSP_BASE + hit["displacement"]
            hit["correlated"] = True
            hit["region"] = dsp_region_name(hit["displacement"])
            return hit
    hit["target"] = None
    hit["correlated"] = False
    hit["region"] = None
    return hit


def dsp_region_name(offset):
    if DSP_MPRO_LO <= offset <= DSP_MPRO_HI:
        return "MPRO (DSP program)"
    if 0x700 <= offset <= 0x77F:
        return "COEF (DSP coefficients)"
    if 0x780 <= offset <= 0x7BF:
        return "MADRS (DSP address table)"
    if 0x7C0 <= offset <= 0x7FF:
        return "MADRS mirror / ignored writes"
    if 0xC00 <= offset <= 0xDFF:
        return "TEMP (read-only)"
    if 0xE00 <= offset <= 0xE7F:
        return "MEMS (read-only)"
    if 0xE80 <= offset <= 0xEBF:
        return "MIXS (read-only)"
    if 0xEC0 <= offset <= 0xEDF:
        return "EFREG (read-only)"
    if 0xEE0 <= offset <= 0xEE3:
        return "EXTS (read-only)"
    return "other DSP window"


def scan_absolute_dsp_operands(ram):
    """Longwords in 0x00100700..0x00100EFF anywhere in the RAM dump.

    Inside the code region the preceding instruction word is decoded when it is
    a known absolute-long consumer.
    """
    hits = []
    for pos in range(0, len(ram) - 3):
        value = u32(ram, pos)
        if not (0x00100700 <= value <= 0x00100EFF):
            continue
        in_code = pos % 2 == 0 and DRIVER_CODE_LO <= pos < DRIVER_CODE_HI
        decoded = None
        if in_code:
            previous = u16(ram, pos - 2)
            if (previous & 0xF1FF) == 0x207C:
                decoded = f"movea.l #${value:08X},a{(previous >> 9) & 7}"
            elif (previous & 0xF1FF) == 0x41F9:
                decoded = f"lea ${value:08X}.l,a{(previous >> 9) & 7}"
            elif (previous & 0xF1FF) in (0x1039, 0x2039, 0x3039):
                size = {0x1039: "b", 0x2039: "l", 0x3039: "w"}[(previous & 0xF1FF)]
                decoded = f"move.{size} ${value:08X}.l,d{(previous >> 9) & 7}"
        note = None
        if decoded is None and pos >= 2:
            previous = u16(ram, pos - 2)
            if (previous & 0xF000) == 0x6000 and (previous & 0xFF) in (0x00, 0xFF):
                note = ("high half is the 16-bit displacement of the branch at "
                        f"0x{pos - 2:05X}; the longword straddles two instructions")
        hits.append({
            "offset": pos,
            "value": value,
            "aligned": pos % 2 == 0,
            "in_code_region": in_code,
            "in_driver_image": pos < DRIVER_IMAGE_HI,
            "decoded": decoded,
            "note": note,
            "region": dsp_region_name(value - SCSP_BASE),
            "context": hex_context(ram, pos),
        })
    return hits


def scan_address_register_loads(ram):
    """Inventory of explicit long-immediate loads into address registers.

    ``lea $xxxxxxxx.l,An`` (0x41F9) and ``movea.l #$xxxxxxxx,An`` (0x207C) are
    the only ways this driver materialises a 32-bit pointer, so the inventory
    shows which registers can hold the SCSP base at all.
    """
    loads = {}
    for pos in range(DRIVER_CODE_LO, DRIVER_CODE_HI - 6, 2):
        word = u16(ram, pos)
        kind = None
        if (word & 0xF1FF) == 0x207C:
            kind = "movea.l"
        elif (word & 0xF1FF) == 0x41F9:
            kind = "lea"
        if kind is None:
            continue
        register = (word >> 9) & 7
        value = u32(ram, pos + 2)
        loads.setdefault(register, {}).setdefault(value, []).append((pos, kind))
    return loads


def scan_dsp_start_constant(ram):
    """Every longword equal to 0x00100BF0 and every bare word 0x0BF0."""
    longwords = [pos for pos in range(0, len(ram) - 3)
                 if u32(ram, pos) == DSP_START]
    words = []
    for pos in range(0, len(ram) - 1, 2):
        if u16(ram, pos) != 0x0BF0:
            continue
        enclosing = u32(ram, pos - 2) if pos >= 2 else None
        decoded = None
        if enclosing is not None and enclosing == DSP_START:
            decoded = "low half of 0x00100BF0"
        elif enclosing == 0x00000BF0:
            previous = u16(ram, pos - 4) if pos >= 4 else 0
            if (previous & 0xF1FF) == 0x207C:
                decoded = ("low half of movea.l #$00000BF0,a%d "
                           "(address register, not the DSP start register)"
                           % ((previous >> 9) & 7))
            elif (previous & 0xF1FF) == 0x203C:
                decoded = ("low half of move.l #$00000BF0,d%d "
                           "(data register, not the DSP start register)"
                           % ((previous >> 9) & 7))
            elif (previous & 0xF1FF) == 0x41F9:
                decoded = ("low half of lea $00000BF0.l,a%d "
                           "(address register, not the DSP start register)"
                           % ((previous >> 9) & 7))
            else:
                decoded = "low half of the longword 0x00000BF0"
        words.append({"offset": pos, "enclosing_longword": enclosing,
                      "decoded": decoded, "in_driver": pos < DRIVER_IMAGE_HI,
                      "context": hex_context(ram, pos)})
    return {"longwords": longwords, "words": words}


# --------------------------------------------------------------------------- #
# 5. heuristic DSP-program scan
# --------------------------------------------------------------------------- #
def analyse_dsp_program(words, start_word):
    """Score a 128-step (4 words each) block as an SCSP DSP program.

    Field layout is the one the emulator implements (``libs/saturn/src/
    scsp_dsp.cpp``, ported from MAME's ``scspdsp.cpp``):
      step word 0: bit15 reserved, TRA[14:8], TWT[7], TWA[6:0]
      step word 1: XSEL[15], YSEL[14:13], bit12 reserved, IRA[11:6],
                   IWT[5], IWA[4:0]
      step word 2: TABLE[15], MWT[14], MRD[13], EWT[12], EWA[11:8],
                   ADRL[7], FRCL[6], SHIFT[5:4], YRL[3], NEGB[2], ZERO[1],
                   BSEL[0]
      step word 3: bit15 (write format), COEF[14:9], MASA[6:2], ADREB[1],
                   NXADR[0]
    Validity used here: the two reserved bits are zero and IRA <= 0x31 (larger
    IRA values make the DSP abandon the sample, so they are not a program).
    """
    steps = 128
    nonzero = 0
    while nonzero < steps:
        base = start_word + nonzero * 4
        if not any(words[base:base + 4]):
            break
        word0, word1 = words[base], words[base + 1]
        if word0 & 0x8000 or word1 & 0x1000:
            return None
        if ((word1 >> 6) & 0x3F) > 0x31:
            return None
        nonzero += 1
    if nonzero == 0:
        return None
    # A program is loaded from step 0, so nothing may follow a zero step before
    # the highest non-zero step; restart needs the block to be contiguous.
    reads = writes = effect_writes = 0
    for index in range(nonzero):
        base = start_word + index * 4
        word2 = words[base + 2]
        reads += (word2 >> 13) & 1
        writes += (word2 >> 14) & 1
        effect_writes += (word2 >> 12) & 1
    last = words[start_word + (nonzero - 1) * 4 + 3]
    return {
        "steps": nonzero,
        "memory_reads": reads,
        "memory_writes": writes,
        "effect_writes": effect_writes,
        "loop_end": bool(last & 1),
    }


def scan_dsp_programs(ram, lo, hi, minimum_steps=4):
    """Sliding heuristic scan for DSP programs; see ``analyse_dsp_program``."""
    words = struct.unpack_from(f">{len(ram) // 2}H", ram)
    strict = []
    relaxed = []
    limit = min(hi, len(ram)) - 1024
    for address in range(lo if lo % 2 == 0 else lo + 1, limit, 2):
        start_word = address // 2
        if not any(words[start_word:start_word + 4]):
            continue
        result = analyse_dsp_program(words, start_word)
        if result is None or result["steps"] < minimum_steps:
            continue
        entry = {"address": address, **result}
        relaxed.append(entry)
        if (result["memory_reads"] + result["memory_writes"] >= 2
                and result["effect_writes"] >= 1 and result["loop_end"]):
            strict.append(entry)
    return strict, relaxed


# --------------------------------------------------------------------------- #
# report
# --------------------------------------------------------------------------- #
def analyse_file(path, max_differences, minimum_steps, dsp_scan_whole_ram):
    ram = path.read_bytes()
    report = {
        "path": str(path),
        "size": len(ram),
        "sha256": sha256(ram),
        "reset_sp": u32(ram, 0) if len(ram) >= 4 else None,
        "reset_pc": u32(ram, 4) if len(ram) >= 8 else None,
    }
    print(f"file: {path}")
    print(f"size: {len(ram)} (0x{len(ram):X})  sha256: {report['sha256']}")
    if len(ram) >= 8:
        print(f"68000 reset vectors: SP=0x{report['reset_sp']:08X} "
              f"PC=0x{report['reset_pc']:08X}")
    if len(ram) != SCSP_RAM_SIZE:
        print(f"warning: expected {SCSP_RAM_SIZE} bytes of sound RAM")

    # ---- 1. banks -----------------------------------------------------------
    candidates = scan_bank_candidates(ram)
    banks = recovered_banks(candidates, len(ram))
    accepted_addresses = {bank["address"] for bank in banks}
    report["bank_candidates"] = [{key: value for key, value in candidate.items()
                                  if key != "offsets"} for candidate in candidates]
    report["banks"] = banks
    print()
    print("[1] sequence-bank table scan (word-aligned, count 1..%d, "
          "first offset >= table end, increasing, last offset < 0x%X)"
          % (MAX_BANK_ENTRIES, MAX_ENTRY_OFFSET))
    print(f"    candidates: {len(candidates)}  "
          f"(grade A: {sum(1 for c in candidates if c['grade'] == 'A')}, "
          f"B: {sum(1 for c in candidates if c['grade'] == 'B')}, "
          f"C: {sum(1 for c in candidates if c['grade'] == 'C')})")
    print("    accepted banks (grade A, deduplicated):")
    total = 0
    for bank in banks:
        total += bank["count"]
        print("      bank %d  0x%05X  count %3d  table 0x%05X-0x%05X  "
              "first 0x%05X  last 0x%05X  extent 0x%05X-0x%05X%s"
              % (bank["bank"], bank["address"], bank["count"], bank["address"],
                 bank["address"] + bank["table_end"] - 1,
                 bank["address"] + bank["offsets"][0],
                 bank["address"] + bank["offsets"][-1],
                 bank["address"], bank["extent_end"],
                 " (end inferred)" if bank["extent_inferred"] else ""))
    print(f"    total entries: {total}")
    rejected = [c for c in candidates if c["address"] not in accepted_addresses]

    def overlapping_bank(candidate):
        for bank in banks:
            if bank["address"] <= candidate["address"] < bank["extent_end"]:
                return bank["bank"]
        return None

    inside = [c for c in rejected if overlapping_bank(c) is not None]
    print(f"    rejected/deduplicated candidates: {len(rejected)} "
          f"({len(inside)} of them start inside an accepted bank's entries)")
    for candidate in rejected[:12]:
        bank_index = overlapping_bank(candidate)
        print("      0x%05X count %3d first 0x%05X last 0x%05X grade %s%s"
              % (candidate["address"], candidate["count"],
                 candidate["offsets"][0], candidate["offsets"][-1],
                 candidate["grade"],
                 "" if bank_index is None else f"  [inside bank {bank_index}]"))
    if len(rejected) > 12:
        print(f"      ... {len(rejected) - 12} more (see JSON)")
    known = next((bank for bank in banks if bank["address"] == KNOWN_BANK3), None)
    print("    cross-check against the documented bank 3 (0x%05X):" % KNOWN_BANK3)
    if known is None:
        print("      NOT FOUND in this dump")
    else:
        print("      found with %d entries; documented value is 25" % known["count"])
        detail = next((entry for entry in collect_entries(ram, [known])
                       if entry["sequence"] == 2), None)
        if detail is not None:
            print("      entry 2 at 0x%05X size %d sha256 %s"
                  % (detail["address"], detail["size"], detail["sha256"]))
            expected = "c39b9b3303df5000fd7a72ca85c3d3b61532fe32562006a09914c6f5629f4040"
            print("      documented sha256 matches: %s"
                  % ("yes" if detail["sha256"] == expected else "NO"))

    # ---- 2. entries ---------------------------------------------------------
    entries = collect_entries(ram, banks)
    report["entries"] = entries
    structure = entry_structure(entries)
    report["entry_structure"] = structure
    print()
    print("[2] sequence entries (bank, sequence, address, size, sha256, bytes)")
    for entry in entries:
        payload = bytes.fromhex(entry["hex"])
        print("    bank %d seq %2d  0x%05X  %3d  %s  %s"
              % (entry["bank"], entry["sequence"], entry["address"],
                 entry["size"], entry["sha256"], entry["hex"]))
    print("    structure check: %d/%d entries share the 8-byte prefix %s; "
          "%d/%d share the 2-byte suffix %s"
          % (structure["prefix_matches"], structure["entries"],
             structure["prefix"], structure["suffix_matches"],
             structure["entries"], structure["suffix"]))
    histogram = {}
    for entry in entries:
        histogram.setdefault(entry["bank"], {}).setdefault(entry["size"], 0)
        histogram[entry["bank"]][entry["size"]] += 1
    report["size_histogram"] = histogram
    print("    entry-size histogram per bank:")
    for bank in banks:
        sizes = histogram.get(bank["bank"], {})
        listed = " ".join(f"{size}B x{count}" for size, count in sorted(sizes.items()))
        print(f"      bank {bank['bank']}: {listed}")

    # ---- 3. families --------------------------------------------------------
    families = difference_families(entries, max_differences)
    report["families"] = families
    print()
    print(f"[3] near-identical entries (exact difference set, <= {max_differences} "
          "differing offsets)")
    print(f"    groups with at least two members: {len(families)}")
    for index, family in enumerate(families):
        members = ", ".join(f"b{m['bank']}s{m['sequence']}" for m in family["members"])
        varying = ", ".join(
            "0x%02X in {%s}" % (int(offset), " ".join(values))
            for offset, values in family["varying"].items())
        print("    family %2d  size %3d  members %3d  differ at: %s"
              % (index, family["size"], len(family["members"]), varying))
        print("                members: %s" % members)
    if not families:
        print("    none")

    # ---- 4. DSP window references ------------------------------------------
    absolute = scan_absolute_dsp_operands(ram)
    displacements = scan_dsp_window_displacements(ram)
    for hit in displacements:
        annotate_base(hit, ram)
    loads = scan_address_register_loads(ram)
    start = scan_dsp_start_constant(ram)
    report["address_register_loads"] = {
        f"a{register}": {f"0x{value:08X}": [site[0] for site in sites]
                         for value, sites in sorted(values.items())}
        for register, values in sorted(loads.items())}
    report["dsp_absolute"] = absolute
    report["dsp_displacements"] = displacements
    report["dsp_start_constant"] = start
    confirmed = [hit for hit in displacements if hit["confirmed"]]
    unconfirmed = [hit for hit in displacements if not hit["confirmed"]]
    print()
    print("[4] references to the effect-DSP register window")
    print("    4.0 explicit long-immediate loads into address registers (context "
          "for 4.2)")
    for register, values in sorted(loads.items()):
        for value, sites in sorted(values.items()):
            print("      a%d <- 0x%08X  %-8s x%d  %s"
                  % (register, value, sites[0][1], len(sites),
                     " ".join("0x%05X" % site[0] for site in sites)))
    inside = [hit for hit in absolute if hit["in_code_region"]]
    outside = [hit for hit in absolute if not hit["in_code_region"]]
    print(f"    4.1 absolute longword operands in 0x00100700-0x00100EFF: "
          f"{len(absolute)} ({len(inside)} word-aligned in the code region, "
          f"{len(outside)} elsewhere)")
    for hit in inside:
        decoded = hit["decoded"] or "not an operand"
        suffix = "" if hit["decoded"] else "  (%s)" % (
            hit["note"] or "no absolute-long consumer before it")
        print("      0x%05X  value 0x%08X  %s%s  ctx[%s]"
              % (hit["offset"], hit["value"], decoded, suffix, hit["context"]))
    if outside:
        print("      elsewhere (unaligned in the driver image, or inside the "
              "sound data):")
        for hit in outside:
            where = ("unaligned, driver image" if hit["in_driver_image"]
                     else "sound data")
            print("      0x%05X  value 0x%08X  %-22s ctx[%s]"
                  % (hit["offset"], hit["value"], where, hit["context"]))
    print(f"    4.2 d16(An) displacements 0x700-0xEFF in the code region: "
          f"{len(confirmed)} decoded lea/MOVE, {len(unconfirmed)} other")
    for hit in confirmed:
        base = hit["base_load"]
        base_text = ("a%d <- 0x%08X at 0x%05X" % (hit["base_register"], base["value"],
                                                  base["offset"])
                     if base and base["value"] is not None
                     else ("a%d <- unknown at 0x%05X" % (hit["base_register"], base["offset"])
                           if base else "no base load in preceding 0x%X bytes"
                           % BASE_LOAD_WINDOW))
        target = "0x%08X -> %s" % (hit["target"], hit["region"]) \
            if hit["correlated"] else "target not provable here"
        print("      0x%05X  %-15s disp 0x%03X  %s  %s"
              % (hit["offset"], hit["form"], hit["displacement"], base_text, target))
        print("               ctx[%s]" % hit["context"])
    for hit in unconfirmed:
        print("      0x%05X  single-ea      disp 0x%03X  UNCONFIRMED "
              "(sliding sweep cannot exclude an extension word)  ctx[%s]"
              % (hit["offset"], hit["displacement"], hit["context"]))
    print("    4.3 DSP start constant 0x00100BF0: %d longword(s)%s"
          % (len(start["longwords"]),
             "" if start["longwords"] else " - absent from the whole 512 KiB dump"))
    for offset in start["longwords"]:
        print("      longword at 0x%05X" % offset)
    driver_words = [entry for entry in start["words"] if entry["in_driver"]]
    data_words = [entry for entry in start["words"] if not entry["in_driver"]]
    print("        bare word 0x0BF0: %d occurrence(s) at word-aligned offsets "
          "(%d in the driver image, %d in the sound data)"
          % (len(start["words"]), len(driver_words), len(data_words)))
    for entry in driver_words:
        print("      0x%05X  enclosing 0x%08X  %s  ctx[%s]"
              % (entry["offset"], entry["enclosing_longword"],
                 entry["decoded"] or "unclassified", entry["context"]))
    if data_words:
        print("      sound-data occurrences: %s%s"
              % (" ".join("0x%05X" % entry["offset"] for entry in data_words[:12]),
                 "" if len(data_words) <= 12 else " ..."))
    print("    4.4 raw words after each correlated data-region reference "
          "(run-length compressed, 0x100 bytes)")
    for hit in confirmed:
        if not hit["correlated"] or hit["region"] is None:
            continue
        if "MPRO" not in hit["region"] and "COEF" not in hit["region"]:
            continue
        runs = []
        end = min(len(ram), hit["offset"] + 0x100)
        for location in range(hit["offset"], end - 1, 2):
            word = u16(ram, location)
            if runs and runs[-1][0] == word:
                runs[-1][1] += 1
            else:
                runs.append([word, 1])
        text = " ".join("%04X" % word if count == 1 else "%04X x%d" % (word, count)
                        for word, count in runs[:14])
        print("      0x%05X (-> %s): %s%s"
              % (hit["offset"], hit["region"], text,
                 " ..." if len(runs) > 14 else ""))

    # ---- 5. DSP programs ----------------------------------------------------
    driver_strict, driver_relaxed = scan_dsp_programs(
        ram, 0, min(DRIVER_IMAGE_HI, len(ram)), minimum_steps)
    report["dsp_programs_driver_strict"] = driver_strict
    report["dsp_programs_driver_relaxed"] = driver_relaxed
    print()
    print("[5] heuristic DSP-program scan (128 steps of 4 words; reserved bits "
          "zero, IRA <= 0x31)")
    print("    This is a heuristic: sound RAM has no memory-mapped MPRO area, so "
          "a dump can")
    print("    only contain a candidate block; it does not prove the driver ever "
          "copied it to")
    print("    0x00100800-0x00100BFF.")
    print(f"    driver image 0x000000-0x{DRIVER_IMAGE_HI:06X}: "
          f"structured candidates (>=2 memory accesses, >=1 effect write, "
          f"loop end): {len(driver_strict)}")
    for hit in driver_strict[:20]:
        print("      0x%05X  steps %d  reads %d  writes %d  effect %d  loop end %s"
              % (hit["address"], hit["steps"], hit["memory_reads"],
                 hit["memory_writes"], hit["effect_writes"], hit["loop_end"]))
    with_memory = [hit for hit in driver_relaxed
                   if hit["memory_reads"] + hit["memory_writes"] >= 1]
    print(f"    driver image relaxed candidates (field-valid, >= {minimum_steps} "
          f"contiguous steps): {len(driver_relaxed)}, of which "
          f"{len(with_memory)} contain at least one delay-line access")
    for hit in with_memory[:20]:
        print("      0x%05X  steps %d  reads %d  writes %d  effect %d  loop end %s"
              % (hit["address"], hit["steps"], hit["memory_reads"],
                 hit["memory_writes"], hit["effect_writes"], hit["loop_end"]))
    if len(with_memory) > 20:
        print(f"      ... {len(with_memory) - 20} more (see JSON)")
    print("    note: the pointer/vector tables in 0x0000-0x0800 parse as long "
          "field-valid blocks")
    print("    because their high words are 0x0000; they contain no delay-line "
          "access.")
    if dsp_scan_whole_ram:
        whole_strict, whole_relaxed = scan_dsp_programs(
            ram, 0, len(ram), minimum_steps)
        outside = [hit for hit in whole_relaxed
                   if hit["address"] >= DRIVER_IMAGE_HI]
        report["dsp_programs_whole_strict"] = whole_strict
        report["dsp_programs_whole_relaxed_outside_driver"] = outside
        print(f"    whole dump: structured candidates {len(whole_strict)}; "
              f"relaxed candidates starting inside the driver image "
              f"{len(whole_relaxed) - len(outside)}; "
              f"relaxed candidates starting outside it {len(outside)}")
        for hit in whole_strict[:20]:
            print("      strict 0x%05X  steps %d  reads %d  writes %d  effect %d "
                  "loop end %s"
                  % (hit["address"], hit["steps"], hit["memory_reads"],
                     hit["memory_writes"], hit["effect_writes"], hit["loop_end"]))
        for hit in outside[:12]:
            print("      relaxed 0x%05X  steps %d  reads %d  writes %d  effect %d "
                  "loop end %s"
                  % (hit["address"], hit["steps"], hit["memory_reads"],
                     hit["memory_writes"], hit["effect_writes"], hit["loop_end"]))
    return report


def cross_dump_summary(reports):
    print()
    print("[6] cross-dump comparison")
    for report in reports:
        banks = report["banks"]
        print("    %-70s banks: %s entries: %d"
              % (report["path"],
                 " ".join("0x%05X/%d" % (b["address"], b["count"]) for b in banks),
                 sum(b["count"] for b in banks)))
    if len(reports) < 2:
        return
    base = reports[0]
    for other in reports[1:]:
        print(f"    {base['path']} vs {other['path']}:")
        base_entries = {(entry["bank"], entry["sequence"]): entry
                        for entry in base["entries"]}
        other_entries = {(entry["bank"], entry["sequence"]): entry
                         for entry in other["entries"]}
        shared = sorted(set(base_entries) & set(other_entries))
        differing = [key for key in shared
                     if base_entries[key]["sha256"] != other_entries[key]["sha256"]]
        print(f"      identical bank tables: "
              f"{[(b['address'], b['count']) for b in base['banks']] == [(b['address'], b['count']) for b in other['banks']]}")
        print(f"      shared entries: {len(shared)}, differing payloads: "
              f"{len(differing)}")
        for key in differing[:10]:
            print("        bank %d seq %d: %s vs %s"
                  % (key[0], key[1], base_entries[key]["sha256"][:16],
                     other_entries[key]["sha256"][:16]))
        if len(differing) > 10:
            print(f"        ... {len(differing) - 10} more")


def write_artifacts(reports, out_dir, max_differences, minimum_steps):
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "scsp-driver-analysis.json").write_text(
        json.dumps({"max_differences": max_differences,
                    "minimum_steps": minimum_steps,
                    "reports": reports}, indent=2) + "\n",
        encoding="utf-8")
    lines = ["bank,sequence,address,size,sha256,hex"]
    for report in reports:
        for entry in report["entries"]:
            lines.append("%d,%d,0x%05X,%d,%s,%s"
                         % (entry["bank"], entry["sequence"], entry["address"],
                            entry["size"], entry["sha256"], entry["hex"]))
    (out_dir / "scsp-driver-entries.csv").write_text("\n".join(lines) + "\n",
                                                    encoding="utf-8")
    print()
    print(f"artifacts written to {out_dir}")


def main():
    parser = argparse.ArgumentParser(
        description="Static analysis of an X4 SCSP sound-RAM dump.")
    parser.add_argument("dumps", nargs="+", type=Path,
                        help="one or more 524288-byte scsp_ram.bin dumps")
    parser.add_argument("--max-differences", type=int, default=6,
                        help="largest difference set still treated as a family "
                             "(default: 6)")
    parser.add_argument("--minimum-steps", type=int, default=4,
                        help="fewest non-zero steps for a DSP-program candidate "
                             "(default: 4)")
    parser.add_argument("--out-dir", type=Path, default=None,
                        help="write JSON/CSV records to this directory")
    parser.add_argument("--skip-whole-ram-dsp-scan", action="store_true",
                        help="only run the DSP-program heuristic on the driver "
                             "image")
    args = parser.parse_args()
    if args.max_differences < 0:
        parser.error("--max-differences must be non-negative")
    if args.minimum_steps < 1:
        parser.error("--minimum-steps must be positive")
    reports = []
    for path in args.dumps:
        if not path.is_file():
            parser.error(f"no such file: {path}")
        print("=" * 78)
        reports.append(analyse_file(path, args.max_differences, args.minimum_steps,
                                    not args.skip_whole_ram_dsp_scan))
        print()
    cross_dump_summary(reports)
    if args.out_dir is not None:
        write_artifacts(reports, args.out_dir, args.max_differences,
                        args.minimum_steps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
