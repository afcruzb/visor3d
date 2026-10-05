#!/usr/bin/env python3
"""Generate test STL files: a torus with roughly N triangles (binary and/or ASCII).

usage: gen_test_stl.py OUT.stl [--tris N] [--ascii]
"""
import argparse
import math
import struct


def torus(n_tris, R=40.0, r=15.0):
    seg = max(3, int(math.sqrt(n_tris / 2)))
    pts = []
    for i in range(seg):
        u = 2 * math.pi * i / seg
        row = []
        for j in range(seg):
            v = 2 * math.pi * j / seg
            row.append(((R + r * math.cos(v)) * math.cos(u),
                        (R + r * math.cos(v)) * math.sin(u),
                        r * math.sin(v) + r))
        pts.append(row)
    for i in range(seg):
        for j in range(seg):
            a, b = pts[i][j], pts[(i + 1) % seg][j]
            c, d = pts[(i + 1) % seg][(j + 1) % seg], pts[i][(j + 1) % seg]
            yield a, b, c
            yield a, c, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--tris", type=int, default=200_000)
    ap.add_argument("--ascii", action="store_true")
    args = ap.parse_args()
    tris = list(torus(args.tris))
    if args.ascii:
        with open(args.out, "w") as f:
            f.write("solid torus\n")
            for t in tris:
                f.write(" facet normal 0 0 0\n  outer loop\n")
                for p in t:
                    f.write("   vertex %e %e %e\n" % p)
                f.write("  endloop\n endfacet\n")
            f.write("endsolid torus\n")
    else:
        with open(args.out, "wb") as f:
            f.write(b"solid binary torus".ljust(80, b" "))
            f.write(struct.pack("<I", len(tris)))
            rec = struct.Struct("<12fH")
            for a, b, c in tris:
                f.write(rec.pack(0, 0, 0, *a, *b, *c, 0))
    print(f"{args.out}: {len(tris)} triángulos")


if __name__ == "__main__":
    main()
