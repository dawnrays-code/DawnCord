#!/usr/bin/env python3
"""Extract function names from a PS Vita core dump (psp2core / .psp2dmp).

Prints a short, text-only crash summary: which thread crashed, why, and
the function names behind PC, LR and the likely return addresses found on
the stack. It never prints raw memory contents, so the output is safe to
paste anywhere.

Usage:
    python crash_symbols.py <psp2core file> <unstripped elf>

The psp2core file lives on the Vita in ux0:data/ (name looks like
psp2core-1234567890-0x001-DawnCord.psp2dmp). The unstripped ELF is the
DawnCord-debug-elf artifact that CI builds for the exact release installed
on the console. No VitaSDK or extra Python packages needed.
"""

import argparse
import bisect
import gzip
import os
import struct
import sys

PT_LOAD = 1
PT_NOTE = 4
PF_X = 1
SHT_SYMTAB = 2
STT_FUNC = 2

STOP_REASONS = {
    0x30002: "Undefined instruction exception",
    0x30003: "Prefetch abort exception",
    0x30004: "Data abort exception",
    0x60080: "Division by zero",
}


def u16(data, off):
    return struct.unpack_from("<H", data, off)[0]


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def c_str(data, off):
    end = data.index(b"\x00", off)
    return data[off:end].decode("ascii", "replace")


class Elf32:
    """Minimal little-endian ELF32 reader (enough for core + app ELF)."""

    def __init__(self, data):
        if data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        if data[4] != 1 or data[5] != 1:
            raise ValueError("expected 32-bit little-endian ELF")
        self.data = data
        (self.e_phoff, self.e_shoff) = struct.unpack_from("<II", data, 28)
        (self.e_phentsize, self.e_phnum,
         self.e_shentsize, self.e_shnum, self.e_shstrndx) = \
            struct.unpack_from("<HHHHH", data, 42)

    def phdrs(self):
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            (p_type, p_offset, p_vaddr, _paddr, p_filesz,
             _memsz, p_flags, _align) = struct.unpack_from("<8I", self.data, off)
            yield p_type, p_offset, p_vaddr, p_filesz, p_flags

    def shdrs(self):
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            yield struct.unpack_from("<10I", self.data, off)


def parse_notes(elf):
    """Return {name: desc_bytes} from every PT_NOTE segment."""
    notes = {}
    for p_type, p_offset, _vaddr, p_filesz, _flags in elf.phdrs():
        if p_type != PT_NOTE:
            continue
        data = elf.data[p_offset:p_offset + p_filesz]
        off = 0
        while off + 12 <= len(data):
            namesz, descsz, _ntype = struct.unpack_from("<3I", data, off)
            off += 12
            name = data[off:off + namesz].rstrip(b"\x00").decode("ascii", "replace")
            off += (namesz + 3) & ~3
            notes[name] = data[off:off + descsz]
            off += (descsz + 3) & ~3
    return notes


class Module:
    def __init__(self, name):
        self.name = name
        self.segments = []  # (num, attr, start, size)


class Thread:
    def __init__(self, uid, name, status, stop_reason, pc):
        self.uid = uid
        self.name = name
        self.status = status
        self.stop_reason = stop_reason
        self.pc = pc
        self.gpr = None


class CoreDump:
    def __init__(self, path):
        with open(path, "rb") as f:
            raw = f.read()
        if raw[:2] == b"\x1f\x8b":
            raw = gzip.decompress(raw)
        self.elf = Elf32(raw)
        self.notes = parse_notes(self.elf)
        for required in ("MODULE_INFO", "THREAD_INFO", "THREAD_REG_INFO"):
            if required not in self.notes:
                raise ValueError("core dump is missing the {} note".format(required))
        self._parse_modules()
        self._parse_threads()
        self._parse_regs()
        self.loads = [(vaddr, off, filesz)
                      for p_type, off, vaddr, filesz, _f in self.elf.phdrs()
                      if p_type == PT_LOAD]

    def _parse_modules(self):
        data = self.notes["MODULE_INFO"]
        num = u32(data, 4)
        off = 8
        self.modules = []
        for _ in range(num):
            mod = Module(c_str(data, off + 0x24))
            num_segs = u32(data, off + 0x4C)
            off += 0x50
            for n in range(num_segs):
                mod.segments.append((n + 1, u32(data, off + 4),
                                     u32(data, off + 8), u32(data, off + 12)))
                off += 0x14
            off += 0x10
            self.modules.append(mod)

    def _parse_threads(self):
        data = self.notes["THREAD_INFO"]
        num = u32(data, 4)
        off = 8
        self.threads = []
        for _ in range(num):
            sz = u32(data, off)
            entry = data[off:off + sz]
            self.threads.append(Thread(u32(entry, 4), c_str(entry, 8),
                                       u16(entry, 0x30), u32(entry, 0x74),
                                       u32(entry, 0x9C)))
            off += sz

    def _parse_regs(self):
        data = self.notes["THREAD_REG_INFO"]
        num = u32(data, 4)
        off = 8
        by_uid = {t.uid: t for t in self.threads}
        for _ in range(num):
            sz = u32(data, off)
            entry = data[off:off + sz]
            tid = u32(entry, 4)
            if tid in by_uid:
                by_uid[tid].gpr = [u32(entry, 8 + 4 * i) for i in range(16)]
            off += sz

    def locate(self, vaddr):
        """Return (module, seg_num, seg_attr, offset) or None."""
        for mod in self.modules:
            for num, attr, start, size in mod.segments:
                if start <= vaddr < start + size:
                    return mod, num, attr, vaddr - start
        return None

    def read_vaddr(self, vaddr, size):
        for seg_vaddr, file_off, filesz in self.loads:
            if seg_vaddr <= vaddr < seg_vaddr + filesz:
                take = min(size, seg_vaddr + filesz - vaddr)
                start = file_off + (vaddr - seg_vaddr)
                return self.elf.data[start:start + take]
        return b""


class SymbolTable:
    """FUNC symbols from the unstripped app ELF, looked up by address."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.elf = Elf32(f.read())
        self.rx_vaddr = None
        for p_type, _off, p_vaddr, _sz, p_flags in self.elf.phdrs():
            if p_type == PT_LOAD and (p_flags & PF_X) and self.rx_vaddr is None:
                self.rx_vaddr = p_vaddr
        if self.rx_vaddr is None:
            raise ValueError("no executable segment in the ELF")

        shdrs = list(self.elf.shdrs())
        syms = []
        for sh in shdrs:
            if sh[1] != SHT_SYMTAB:
                continue
            strtab = shdrs[sh[6]]
            strdata = self.elf.data[strtab[4]:strtab[4] + strtab[5]]
            count = sh[5] // 16
            for i in range(count):
                st_name, st_value, st_size, st_info = \
                    struct.unpack_from("<IIIB", self.elf.data, sh[4] + i * 16)
                if st_info & 0xF != STT_FUNC or st_name == 0:
                    continue
                end = strdata.index(b"\x00", st_name)
                name = strdata[st_name:end].decode("ascii", "replace")
                syms.append((st_value & ~1, st_size, name))
        if not syms:
            raise ValueError("no function symbols found (is this the "
                             "unstripped debug ELF, not the VPK one?)")
        syms.sort()
        self.addrs = [s[0] for s in syms]
        self.syms = syms

    def lookup(self, seg_offset):
        """seg_offset is the offset inside the app's exec segment."""
        addr = (self.rx_vaddr + seg_offset) & ~1
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        base, size, name = self.syms[i]
        delta = addr - base
        if size and delta >= size:
            return None
        if delta > 0x4000:  # sizeless symbol too far away to trust
            return None
        return "{} +0x{:x}".format(name, delta)


def pick_app_module(core, elf_path, override):
    if override:
        for mod in core.modules:
            if mod.name.lower() == override.lower():
                return mod
        sys.exit("error: no module named '{}' in the dump (found: {})".format(
            override, ", ".join(m.name for m in core.modules)))
    stem = os.path.splitext(os.path.basename(elf_path))[0].lower()
    non_sce = [m for m in core.modules if not m.name.startswith("Sce")]
    for mod in non_sce:
        if stem in mod.name.lower() or mod.name.lower().endswith(".elf"):
            return mod
    if len(non_sce) == 1:
        return non_sce[0]
    return None


def describe(core, app_mod, symtab, label, vaddr):
    loc = core.locate(vaddr)
    if not loc:
        return "{}: (address outside every module)".format(label)
    mod, seg_num, _attr, offset = loc
    where = "{} seg{}".format(mod.name, seg_num)
    if mod is app_mod and seg_num == 1:
        sym = symtab.lookup(offset)
        if sym:
            return "{}: {}  [{}]".format(label, sym, where)
    return "{}: {} (system)".format(label, where)


def stack_callers(core, app_mod, symtab, thread, max_frames):
    """Scan words above SP for return addresses; best-effort backtrace."""
    sp = thread.gpr[13]
    data = core.read_vaddr(sp, 0x600)
    frames, last = [], None
    for i in range(0, len(data) - 3, 4):
        word = u32(data, i)
        loc = core.locate(word & ~1)
        if not loc:
            continue
        mod, seg_num, attr, offset = loc
        if attr & 0xF != 5:  # only addresses inside executable segments
            continue
        if mod is app_mod and seg_num == 1:
            entry = symtab.lookup(offset) or "{} seg1 (no symbol)".format(mod.name)
        else:
            entry = "{} (system)".format(mod.name)
        if entry != last:
            frames.append(entry)
            last = entry
        if len(frames) >= max_frames:
            break
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("corefile", help="psp2core-*.psp2dmp from ux0:data/")
    parser.add_argument("elffile", help="unstripped ELF (DawnCord-debug-elf artifact)")
    parser.add_argument("--module", help="app module name in the dump, if autodetect fails")
    parser.add_argument("--max-frames", type=int, default=12)
    args = parser.parse_args()

    core = CoreDump(args.corefile)
    symtab = SymbolTable(args.elffile)
    app_mod = pick_app_module(core, args.elffile, args.module)

    print("== DawnCord crash summary (function names only) ==")
    if app_mod is None:
        print("warning: could not tell which module is the app; "
              "pass --module NAME. Modules in dump:")
        for mod in core.modules:
            print("  " + mod.name)

    crashed = [t for t in core.threads if t.stop_reason != 0]
    if not crashed:
        # GPU crashes look exactly like this: the GXM driver kills the app
        # without any CPU exception, so every thread is just parked. Where
        # they were parked still tells us which phase of the frame was in
        # flight, so print it all.
        print("No CPU exception in this dump (typical of a GPU crash: the")
        print("graphics driver died, the CPU threads were just waiting).")
        print("Parked positions at the moment of death:")
        for t in core.threads:
            print()
            print("Thread '{}' (status 0x{:x}):".format(t.name, t.status))
            print("  " + describe(core, app_mod, symtab, "PC", t.pc))
            if t.gpr:
                print("  " + describe(core, app_mod, symtab, "LR", t.gpr[14]))
                frames = stack_callers(core, app_mod, symtab, t, args.max_frames)
                if frames:
                    print("  Likely call path (innermost first):")
                    for fr in frames:
                        print("    " + fr)
        return

    for t in crashed:
        reason = STOP_REASONS.get(t.stop_reason,
                                  "stop reason 0x{:x}".format(t.stop_reason))
        print()
        print("Thread '{}' crashed: {}".format(t.name, reason))
        print("  " + describe(core, app_mod, symtab, "PC", t.pc))
        if t.gpr:
            print("  " + describe(core, app_mod, symtab, "LR", t.gpr[14]))
            print("  Possible callers on the stack (innermost first):")
            frames = stack_callers(core, app_mod, symtab, t, args.max_frames)
            if frames:
                for fr in frames:
                    print("    " + fr)
            else:
                print("    (none found)")
        else:
            print("  (no registers recorded for this thread)")


if __name__ == "__main__":
    main()
