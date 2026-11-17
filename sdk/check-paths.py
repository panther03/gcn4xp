#!/usr/bin/env python3
"""
check-paths.py - does anything in a directory tree name the machine it was
built on?

    check-paths.py <dir> <string> ...

Looks for each string (case-insensitive, with either slash direction, as
8-bit text and as UTF-16) in every file below <dir>, and lists the files
that contain one.  Exit status 1 if any does.  prepare-sdk.sh passes the
repository path and the user's home directory, so that no build path or
user name ends up in the SDK.
"""
import os
import sys


def forms(s):
    out = set()
    for v in (s, s.replace("\\", "/"), s.replace("/", "\\")):
        v = v.lower()
        out.add(v.encode("latin-1", "ignore"))
        out.add(v.encode("utf-16-le"))
    return [f for f in out if len(f) >= 6]


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    root = sys.argv[1]
    needles = []
    for s in sys.argv[2:]:
        if s:
            needles += forms(s)
    bad = 0
    for dirpath, _, files in os.walk(root):
        for name in files:
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                data = f.read().lower()
            hits = [n for n in needles if n in data]
            if hits:
                bad = 1
                shown = hits[0].decode("utf-16-le" if b"\0" in hits[0]
                                       else "latin-1")
                print("%s: contains %s" % (os.path.relpath(path, root), shown))
    return bad


if __name__ == "__main__":
    sys.exit(main())
