#!/usr/bin/env python3
"""Instruction-count emulation of the SP-1 FX DSP on a Cortex-M4F model (SIMULATED).

Runs tools/fxprof_emu/harness.c (the same cases as the fxprof firmware image) in the
Unicorn emulator and counts the Thumb-2 instructions executed per stereo frame, plus a
rough Cortex-M4 cycle estimate from a per-instruction cost table (zero-wait-state memory,
no interrupts, no flash cache misses). It is a STATIC MODEL, not a measurement: use it to
compare code versions and find hot spots; only the DWT counts from the fxprof image on the
device are real numbers.

Needs: pip install unicorn capstone pyelftools  (tested with unicorn 2.1.4, capstone 5/6)
Usage: tools/fxprof_emu/build.sh && python3 tools/fxprof_emu/fxprof_emu.py [elf] [--blocks N]
"""
import argparse
import bisect
import collections
import sys

from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs
from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_SP)
try:
    from unicorn.arm_const import UC_CPU_ARM_CORTEX_M4
except ImportError:  # older unicorn
    UC_CPU_ARM_CORTEX_M4 = None

RET = 0x0FFF0000  # return trampoline ("b ." in its own page)

# Rough Cortex-M4 cycle costs (ARM DDI 0439 / FPU TRM), 1 cycle if not listed.
LOAD_STORE = ("ldr", "str", "vldr", "vstr")
MULTI = ("push", "pop", "ldm", "stm", "vpush", "vpop", "vldm", "vstm")
SLOW = {"vdiv": 14, "vsqrt": 14, "sdiv": 6, "udiv": 6,
        "vmla": 3, "vmls": 3, "vnmla": 3, "vnmls": 3, "vfma": 3, "vfms": 3}
BRANCHY = ("b", "bl", "bx", "blx", "cbz", "cbnz", "tbb", "tbh")


def base_cost(insn):
    m = insn.mnemonic.split(".")[0]
    for k, v in SLOW.items():
        if m.startswith(k):
            return v
    if m.startswith(MULTI):
        n = insn.op_str.count(",") + 1
        if "-" in insn.op_str:  # register range such as {d8-d12}
            n = 5
        return 1 + n
    if m.startswith(LOAD_STORE):
        return 2
    return 1


class Emu:
    def __init__(self, path):
        with open(path, "rb") as f:
            elf = ELFFile(f)
            self.syms = {}
            funcs = []
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                if s.name:
                    self.syms.setdefault(s.name, s["st_value"])
                    if s["st_info"]["type"] == "STT_FUNC":
                        funcs.append((s["st_value"] & ~1, s.name))
            # static functions share names (every effect has a "process"): tag them by address
            src = {}
            cur = None
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                if s["st_info"]["type"] == "STT_FILE":
                    cur = s.name
                elif s["st_info"]["type"] == "STT_FUNC" and s["st_info"]["bind"] == "STB_LOCAL":
                    src[s["st_value"] & ~1] = cur
            segs = [(seg["p_vaddr"], seg["p_paddr"], seg.data(), seg["p_memsz"])
                    for seg in elf.iter_segments() if seg["p_type"] == "PT_LOAD"]
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        if UC_CPU_ARM_CORTEX_M4 is not None:
            try:
                self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
            except Exception:
                pass
        self.uc.mem_map(0x00000000, 0x00100000)
        self.uc.mem_map(0x20000000, 0x00100000)
        self.uc.mem_map(RET, 0x1000)
        self.uc.mem_write(RET, b"\xfe\xe7")
        for vaddr, paddr, data, memsz in segs:
            self.uc.mem_write(vaddr, data)  # .data copied to its RAM address directly
        self.sp = 0x20100000 - 16
        self.cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
        self.dec = {}
        self.count = False
        self.n = 0
        self.cyc = 0
        self.prev = None
        self.by_fn = collections.Counter()
        self.fn_starts = sorted((a, f"{src[a]}:{n}" if a in src else n) for a, n in funcs)
        self.fn_keys = [a for a, _ in self.fn_starts]
        self.profile = False
        self.uc.hook_add(UC_HOOK_CODE, self._hook)

    def _hook(self, uc, addr, size, _):
        if not self.count:
            return
        d = self.dec.get(addr)
        if d is None:
            code = bytes(uc.mem_read(addr, size))
            ins = next(self.cs.disasm(code, addr), None)
            c = base_cost(ins) if ins else 1
            br = bool(ins) and (ins.mnemonic.split(".")[0] in BRANCHY or "pc" in ins.op_str.split(",")[0])
            d = (c, br, size)
            self.dec[addr] = d
        if self.prev is not None:
            pa, pc_, pbr, psz = self.prev
            if pa + psz != addr:  # control transfer taken: pipeline refill
                self.cyc += 2
        self.n += 1
        self.cyc += d[0]
        if self.profile:
            i = bisect.bisect_right(self.fn_keys, addr) - 1
            self.by_fn[self.fn_starts[i][1] if i >= 0 else "?"] += d[0]
        self.prev = (addr, d[0], d[1], d[2])

    def call(self, name, arg=0):
        fn = self.syms[name]
        self.uc.reg_write(UC_ARM_REG_SP, self.sp)
        self.uc.reg_write(UC_ARM_REG_R0, arg)
        self.uc.reg_write(UC_ARM_REG_LR, RET | 1)
        self.prev = None
        self.uc.emu_start(fn | 1, RET, count=200_000_000)
        return self.uc.reg_read(UC_ARM_REG_R0)

    def enable_fpu(self):
        # CPACR: full access to CP10/CP11 (Unicorn models the SCS register)
        try:
            self.uc.mem_map(0xE000E000, 0x1000)
        except Exception:
            pass
        self.uc.mem_write(0xE000ED88, (0xF << 20).to_bytes(4, "little"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", nargs="?", default="build/fxprof_emu/harness.elf")
    ap.add_argument("--blocks", type=int, default=40)
    ap.add_argument("--warmup", type=int, default=8)
    ap.add_argument("--case", type=int, default=-1)
    ap.add_argument("--profile", action="store_true", help="cycles by function (est.)")
    a = ap.parse_args()
    e = Emu(a.elf)
    e.enable_fpu()
    e.profile = a.profile
    n = e.call("h_ncases")
    cases = range(n) if a.case < 0 else [a.case]
    print("SIMULATED (instruction-count model, not hardware). per stereo frame:")
    print(f"{'case':>4} {'name':<12} {'kind':<6} {'instr':>8} {'est_cyc':>8} {'max_cyc':>8}")
    names = sys.modules[__name__]
    for i in cases:
        e.call("h_setup", i)
        for _ in range(a.warmup):
            e.call("h_prep")
            e.call("h_block")
        tot_n = tot_c = 0
        mx = 0
        for _ in range(a.blocks):
            e.call("h_prep")
            e.n = e.cyc = 0
            e.count = True
            e.call("h_block")
            e.count = False
            tot_n += e.n
            tot_c += e.cyc
            mx = max(mx, e.cyc)
        fr = 64.0 * a.blocks
        label = case_label(i, n)
        print(f"{i:>4} {label[0]:<12} {label[1]:<6} {tot_n / fr:>8.1f} {tot_c / fr:>8.1f} {mx / 64.0:>8.1f}")
        if a.profile:
            for fn, c in e.by_fn.most_common(12):
                print(f"       {fn:<28} {c / fr:8.1f} cyc/frame")
            e.by_fn.clear()
        sys.stdout.flush()


FX = ["None", "Filter", "Chorus", "Delay", "Reverb", "Saturation", "Flanger",
      "Bitcrusher", "Tremolo", "Compressor", "TapeWobble"]
PRESETS = ["DubStation", "BoomBap", "House", "LoFiTape", "Ambient", "Preset6"]


def case_label(i, n):
    if i == 0:
        return ("Clean", "clean")
    if i <= 30:
        k = i - 1
        return (FX[1 + k // 3], ["def", "worst", "auto"][k % 3])
    return (PRESETS[i - 31] if i - 31 < len(PRESETS) else f"preset{i - 31}", "preset")


if __name__ == "__main__":
    main()
