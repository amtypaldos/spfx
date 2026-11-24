#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""SP-1 FX flash-safety check for an app-only Zephyr image.

Run isolated:  python3 -I firmware/scripts/check_image.py --elf zephyr.elf --bin zephyr.bin

FAILS (exit 1) unless:
  * the first loadable byte / vector table is at 0x20000 (TE bootloader owns 0x0..0x1FFFF),
  * every loadable segment's flash image lies within 0x20000..0xFEFFF
    (nothing below 0x20000, nothing touching the 0xFF000 'storage' page),
  * the flash image size is <= 0xDF000,
  * the initial stack pointer (vector table word 0) is in RAM 0x20000000..0x2003FFFF,
  * the reset vector (word 1) is a Thumb address inside the image,
  * the .bin is byte-identical to the ELF's loadable bytes (objcopy --gap-fill 0xff layout),
  * with --reserve START:END (repeatable): no loadable byte lies in [START, END) (e.g. the
    settings journal pages 0xFD000..0xFEFFF of zephyr_sp1fx, which the image must never
    reach because flashing would overwrite them and the firmware erases them),
and reports flash and RAM use. Requires pyelftools. Writes nothing.
Limits come from firmware/boards/teenageengineering/stem_player/stem_player.dts.
"""

import argparse
import hashlib
import json
import struct
import sys

from elftools.elf.constants import SH_FLAGS
from elftools.elf.elffile import ELFFile

APP_START = 0x00020000
APP_MAX_SIZE = 0x000DF000
APP_END = APP_START + APP_MAX_SIZE          # 0xFF000, first byte of 'storage'
STORAGE = (0x000FF000, 0x00100000)
RAM_START = 0x20000000
RAM_END = 0x20040000                        # exclusive; 256 KiB


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--json", help="also write the report as JSON to this path")
    ap.add_argument("--reserve", action="append", default=[], metavar="START:END",
                    help="flash range [START, END) the image must not touch (hex ok)")
    args = ap.parse_args()
    reserved = []
    for r in args.reserve:
        try:
            a, b = (int(x, 0) for x in r.split(":"))
        except ValueError:
            print(f"ERROR: bad --reserve {r!r}", file=sys.stderr)
            return 1
        if not (APP_START <= a < b <= APP_END):
            print(f"ERROR: --reserve {r} must lie inside the app slot", file=sys.stderr)
            return 1
        reserved.append((a, b))

    errors = []
    report = {}

    with open(args.elf, "rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] != "EM_ARM":
            errors.append(f"ELF machine is {elf['e_machine']}, expected EM_ARM")

        loads = []
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD" or seg["p_filesz"] == 0:
                continue
            loads.append((seg["p_paddr"], seg["p_vaddr"], seg["p_filesz"],
                          seg["p_memsz"], seg.data()))
        if not loads:
            errors.append("no loadable segments with file data")
            return finish(errors, report, args)

        loads.sort(key=lambda s: s[0])
        for paddr, vaddr, filesz, memsz, _ in loads:
            end = paddr + filesz
            ok = APP_START <= paddr and end <= APP_END
            if not ok:
                errors.append(f"load segment LMA 0x{paddr:08x}..0x{end - 1:08x} "
                              f"outside 0x{APP_START:05x}..0x{APP_END - 1:05x}")
            if paddr < STORAGE[1] and end > STORAGE[0]:
                errors.append(f"load segment 0x{paddr:08x}..0x{end - 1:08x} overlaps storage page")
            if paddr < APP_START:
                errors.append(f"load segment at 0x{paddr:08x} would overwrite the bootloader")
            for a, b in reserved:
                if paddr < b and end > a:
                    errors.append(f"load segment 0x{paddr:08x}..0x{end - 1:08x} overlaps "
                                  f"reserved 0x{a:05x}..0x{b - 1:05x}")

        first = loads[0][0]
        last = max(p + n for p, _, n, _, _ in loads)
        image_size = last - first
        report["first_load_addr"] = f"0x{first:08x}"
        report["last_load_end"] = f"0x{last:08x}"
        report["flash_image_bytes"] = image_size
        report["flash_limit_bytes"] = APP_MAX_SIZE
        if reserved:
            report["reserved"] = " ".join(f"0x{a:05x}..0x{b - 1:05x}" for a, b in reserved)
            report["room_below_reserved"] = min(a for a, _ in reserved) - last
        if first != APP_START:
            errors.append(f"first loadable byte at 0x{first:08x}, expected 0x{APP_START:08x}")
        if image_size > APP_MAX_SIZE:
            errors.append(f"image size 0x{image_size:x} > 0x{APP_MAX_SIZE:x}")

        # Reconstruct the flash image the way Zephyr's objcopy does it:
        # -O binary --gap-fill 0xff, i.e. per allocated PROGBITS section at
        # its load address (LMA), gaps (incl. alignment padding) = 0xFF.
        image = bytearray(b"\xff" * image_size)
        segs = [s for s in elf.iter_segments()
                if s["p_type"] == "PT_LOAD" and s["p_filesz"] > 0]
        for sec in elf.iter_sections():
            if (not (sec["sh_flags"] & SH_FLAGS.SHF_ALLOC)
                    or sec["sh_type"] == "SHT_NOBITS" or sec["sh_size"] == 0):
                continue
            off = sec["sh_offset"]
            seg = next((s for s in segs
                        if s["p_offset"] <= off < s["p_offset"] + s["p_filesz"]), None)
            if seg is None:
                errors.append(f"section {sec.name} has file data outside any PT_LOAD")
                continue
            lma = seg["p_paddr"] + (off - seg["p_offset"])
            if not (first <= lma and lma + sec["sh_size"] <= last):
                errors.append(f"section {sec.name} LMA 0x{lma:08x} outside load range")
                continue
            image[lma - first:lma - first + sec["sh_size"]] = sec.data()

        symtab = elf.get_section_by_name(".symtab")
        vt = None
        if symtab is not None:
            syms = symtab.get_symbol_by_name("_vector_table")
            if syms:
                vt = syms[0]["st_value"]
        report["_vector_table"] = None if vt is None else f"0x{vt:08x}"
        if vt is None:
            errors.append("symbol _vector_table not found")
        elif vt != APP_START:
            errors.append(f"_vector_table at 0x{vt:08x}, expected 0x{APP_START:08x}")

        # RAM use: every allocated section whose run address is in RAM.
        ram_sections = {}
        for sec in elf.iter_sections():
            if not (sec["sh_flags"] & SH_FLAGS.SHF_ALLOC) or sec["sh_size"] == 0:
                continue
            a = sec["sh_addr"]
            if RAM_START <= a < RAM_END:
                ram_sections[sec.name] = sec["sh_size"]
                if a + sec["sh_size"] > RAM_END:
                    errors.append(f"section {sec.name} runs past end of RAM")
        ram_hi = max((s["sh_addr"] + s["sh_size"] for s in elf.iter_sections()
                      if (s["sh_flags"] & SH_FLAGS.SHF_ALLOC)
                      and RAM_START <= s["sh_addr"] < RAM_END), default=RAM_START)
        report["ram_static_bytes"] = ram_hi - RAM_START
        report["ram_sections_sum_bytes"] = sum(ram_sections.values())
        report["ram_limit_bytes"] = RAM_END - RAM_START

    if len(image) >= 8:
        sp, reset = struct.unpack_from("<II", image, 0)
        report["initial_sp"] = f"0x{sp:08x}"
        report["reset_vector"] = f"0x{reset:08x}"
        if not (RAM_START <= sp <= RAM_END - 1):
            errors.append(f"initial SP 0x{sp:08x} not in 0x{RAM_START:08x}..0x{RAM_END - 1:08x}")
        if not (reset & 1) or not (APP_START <= (reset & ~1) < APP_START + image_size):
            errors.append(f"reset vector 0x{reset:08x} not a Thumb address inside the image")
    else:
        errors.append("image shorter than 8 bytes")

    with open(args.bin, "rb") as f:
        binary = f.read()
    report["bin_bytes"] = len(binary)
    report["bin_sha256"] = hashlib.sha256(binary).hexdigest()
    if binary != bytes(image):
        diff = next((i for i in range(min(len(binary), len(image)))
                     if binary[i] != image[i]), min(len(binary), len(image)))
        errors.append(f".bin differs from ELF loadable bytes (bin {len(binary)} B, "
                      f"elf {len(image)} B, first difference at offset 0x{diff:x})")
    if len(binary) > APP_MAX_SIZE:
        errors.append(f".bin is {len(binary)} B > 0x{APP_MAX_SIZE:x}")

    return finish(errors, report, args)


def finish(errors, report, args) -> int:
    report["result"] = "PASS" if not errors else "FAIL"
    report["errors"] = errors
    for k, v in report.items():
        if k != "errors":
            print(f"{k:24s} {v}")
    if "flash_image_bytes" in report:
        print(f"{'flash_used':24s} {100.0 * report['flash_image_bytes'] / APP_MAX_SIZE:.2f}% of slot0")
    if "ram_static_bytes" in report:
        print(f"{'ram_used':24s} {100.0 * report['ram_static_bytes'] / report['ram_limit_bytes']:.2f}% of 256 KiB")
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
            f.write("\n")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
