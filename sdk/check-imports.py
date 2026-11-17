#!/usr/bin/env python3
"""
check-imports.py - will these Windows programs load on Windows XP?

    check-imports.py <dir with the XP system DLLs> <exe or dll> ...

Reads the import table of each PE file and reports every function that the
corresponding DLL in the given directory does not export, and every DLL
that is not there at all.  The DLLs are the ones from the target system
(copy them from its system32).  Also checks the header: XP (5.1) loads only
32-bit x86 executables whose OS and subsystem versions are 5.1 or lower.
Exit status 1 if anything is wrong.

Functions a program looks up itself with GetProcAddress do not show up
here; that is the usual way code falls back gracefully on older systems.
"""
import os
import struct
import sys


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError("%s: not a PE file" % path)
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("%s: not a PE file" % path)
        self.machine, nsect = struct.unpack_from("<HH", d, pe + 4)
        optsize, = struct.unpack_from("<H", d, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", d, opt)
        self.pe32plus = magic == 0x20B
        # MajorOperatingSystemVersion .. MinorSubsystemVersion
        (self.os_major, self.os_minor, _, _, self.subsys_major,
         self.subsys_minor) = struct.unpack_from("<6H", d, opt + 40)
        ddir = opt + (112 if self.pe32plus else 96)
        self.dirs = [struct.unpack_from("<II", d, ddir + 8 * i)
                     for i in range(16)]
        self.sections = []
        s = opt + optsize
        for i in range(nsect):
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((va, max(vsize, rsize), raw))
            s += 40

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError("RVA %x outside the image" % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")

    def exports(self):
        rva, size = self.dirs[0]
        if rva == 0:
            return set()
        o = self.off(rva)
        nnames, = struct.unpack_from("<I", self.data, o + 24)
        names_rva, = struct.unpack_from("<I", self.data, o + 32)
        no = self.off(names_rva)
        return {self.cstr(struct.unpack_from("<I", self.data, no + 4 * i)[0])
                for i in range(nnames)}

    def imports(self):
        """[(dll, [function name or '#ordinal'])] incl. delay-loaded ones."""
        result = []
        thunk_size = 8 if self.pe32plus else 4
        ord_flag = 1 << (63 if self.pe32plus else 31)

        def names(table_rva):
            out = []
            t = self.off(table_rva)
            while True:
                v = struct.unpack_from("<Q" if self.pe32plus else "<I",
                                       self.data, t)[0]
                if v == 0:
                    return out
                if v & ord_flag:
                    out.append("#%d" % (v & 0xFFFF))
                else:
                    out.append(self.cstr((v & 0x7FFFFFFF) + 2))
                t += thunk_size

        rva, size = self.dirs[1]
        if rva:
            o = self.off(rva)
            while True:
                ilt, _, _, name, iat = struct.unpack_from("<IIIII",
                                                          self.data, o)
                if name == 0:
                    break
                result.append((self.cstr(name), names(ilt or iat)))
                o += 20
        rva, size = self.dirs[13]          # delay-load imports
        if rva:
            o = self.off(rva)
            while True:
                fields = struct.unpack_from("<IIIIIIII", self.data, o)
                if fields[1] == 0:
                    break
                result.append((self.cstr(fields[1]) + " (delay)",
                               names(fields[4])))
                o += 32
        return result


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    dlldir = sys.argv[1]
    cache = {}
    bad = 0
    for path in sys.argv[2:]:
        missing = []
        pe = PE(path)
        if pe.machine != 0x14C:
            missing.append("machine type %#x, not 32-bit x86" % pe.machine)
        for what, major, minor in (("OS", pe.os_major, pe.os_minor),
                                   ("subsystem", pe.subsys_major,
                                    pe.subsys_minor)):
            if (major, minor) > (5, 1):
                missing.append("%s version %d.%d in the header (XP is 5.1; "
                               "link with --major-%s-version=5 "
                               "--minor-%s-version=1)" %
                               (what, major, minor,
                                "os" if what == "OS" else "subsystem",
                                "os" if what == "OS" else "subsystem"))
        for dll, funcs in pe.imports():
            key = dll.replace(" (delay)", "").lower()
            if key.startswith("api-ms-win-"):
                missing.append("%s (an API set: Windows 7 and later)" % dll)
                continue
            # the system's DLLs, else one that ships next to the program
            local = os.path.join(os.path.dirname(os.path.abspath(path)), key)
            p = os.path.join(dlldir, key)
            if not os.path.exists(p) and os.path.exists(local):
                p = local
            if p not in cache:
                cache[p] = PE(p).exports() if os.path.exists(p) else None
            exp = cache[p]
            if exp is None:
                missing.append("%s (DLL not found)" % dll)
                continue
            for f in funcs:
                if not f.startswith("#") and f not in exp:
                    missing.append("%s!%s" % (dll, f))
        name = os.path.basename(path)
        if missing:
            bad = 1
            print("%s: %d problems" % (name, len(missing)))
            for m in missing:
                print("    " + m)
        else:
            print("%s: ok" % name)
    return bad


if __name__ == "__main__":
    sys.exit(main())
