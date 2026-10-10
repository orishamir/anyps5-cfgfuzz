#!/usr/bin/env python3
"""Delta-reduce a graph keeping: variant A -> class CA and variant B -> class CB (structurizer-only, cfgfuzz `one`).
usage: reduce2.py <cfgfuzz_A> <classA> <cfgfuzz_B> <classB> "<graph text>" <words>"""
import subprocess, sys
A, CA, B, CB, text, words = sys.argv[1:7]
def parse(t): return [[int(x) for x in f.split(",") if x] for f in t.split(";")]
def norm(s):
    new = {0: 0}; order = [0]; q = [0]
    while q:
        b = q.pop(0)
        for t in s[b]:
            if t < len(s) and t not in new: new[t] = len(order); order.append(t); q.append(t)
    out = []
    for b in order:
        l = []
        for t in s[b]:
            if new[t] not in l: l.append(new[t])
        out.append(l)
    return out
def txt(s): return ";".join(",".join(map(str, x)) for x in s)
def cls(binary, s):
    r = subprocess.run([binary, "one", txt(s), words], capture_output=True, text=True, timeout=60).stdout
    return r.split("\t")[0].strip() if r else "?"
def ok(s): return len(s) >= 2 and cls(A, s) == CA and cls(B, s) == CB
g = norm(parse(text)); assert ok(g), (cls(A, g), cls(B, g))
evals = 0; changed = True
while changed:
    changed = False
    for b in range(len(g)):
        if changed: break
        if len(g[b]) == 2:
            for k in range(2):
                c = [list(x) for x in g]; del c[b][k]; c = norm(c); evals += 1
                if ok(c): g, changed = c, True; break
    for b in range(1, len(g)):
        if changed: break
        if len(g[b]) == 1 and g[b][0] != b:
            t = g[b][0]; c = [[t if s == b else s for s in x] for x in g]; c[b] = []; c = norm(c); evals += 1
            if ok(c): g, changed = c, True
    for b in range(1, len(g)):
        if changed: break
        if g[b]:
            c = [list(x) for x in g]; c[b] = []; c = norm(c); evals += 1
            if ok(c): g, changed = c, True
print(f"reduced to {len(g)} blocks after {evals} evaluations: {txt(g)}")
print("{" + ", ".join("{" + ", ".join(map(str, x)) + "}" for x in g) + "}")
for name, binary in (("A", A), ("B", B)):
    r = subprocess.run([binary, "one", txt(g), words], capture_output=True, text=True).stdout.strip().split("\n")[0]
    print(f"  {name} {binary.split('/')[-1]}: {r[:140]}")
