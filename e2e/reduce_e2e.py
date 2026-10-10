#!/usr/bin/env python3
"""Delta-reduce a CFG shape with the end-to-end pipeline as the oracle.

usage: reduce_e2e.py <e2e binary> "<succ lists>" <predicate substring> [words]
Keeps a transformation when the e2e result line (stage\\tdetails) still contains the predicate substring.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cfg2shader  # noqa: E402


def normalize(succ):
    n = len(succ)
    new = {0: 0}
    order = [0]
    queue = [0]
    while queue:
        b = queue.pop(0)
        for s in succ[b]:
            if s < n and s not in new:
                new[s] = len(order)
                order.append(s)
                queue.append(s)
    out = []
    for b in order:
        lst = []
        for s in succ[b]:
            t = new[s]
            if t not in lst:
                lst.append(t)
        out.append(lst)
    return out


def run(binary, succ):
    words = cfg2shader.assemble(cfg2shader.assembly(succ))
    with tempfile.TemporaryDirectory(prefix="reduce-e2e-") as d:
        inp = os.path.join(d, "in.words")
        outp = os.path.join(d, "out.results")
        open(inp, "w").write("g\t" + " ".join(f"{w:08x}" for w in words) + f"\t{len(succ)}\n")
        subprocess.run([binary, inp, outp, "20"], capture_output=True, text=True, timeout=120)
        line = open(outp).read().strip()
    return line


def main():
    binary, text, predicate = sys.argv[1], sys.argv[2], sys.argv[3]
    graph = normalize(cfg2shader.parse(text))
    first = run(binary, graph)
    assert predicate in first, f"predicate not in initial result: {first[:200]}"
    evals = 0

    def same(c):
        nonlocal evals
        if len(c) < 2:
            return False
        evals += 1
        try:
            return predicate in run(binary, c)
        except Exception:
            return False

    changed = True
    while changed:
        changed = False
        for b in range(len(graph)):
            if len(graph[b]) == 2 and not changed:
                for k in range(2):
                    c = [list(x) for x in graph]
                    del c[b][k]
                    c = normalize(c)
                    if same(c):
                        graph, changed = c, True
                        break
        for b in range(1, len(graph)):
            if changed:
                break
            if len(graph[b]) == 1 and graph[b][0] != b:
                t = graph[b][0]
                c = [[t if s == b else s for s in x] for x in graph]
                c[b] = []
                c = normalize(c)
                if same(c):
                    graph, changed = c, True
        for b in range(1, len(graph)):
            if changed:
                break
            if graph[b]:
                c = [list(x) for x in graph]
                c[b] = []
                c = normalize(c)
                if same(c):
                    graph, changed = c, True
    final = run(binary, graph)
    print(f"reduced to {len(graph)} blocks after {evals} evaluations")
    print(";".join(",".join(str(s) for s in x) for x in graph))
    print("{" + ", ".join("{" + ", ".join(str(s) for s in x) + "}" for x in graph) + "}")
    print(final[:300])


if __name__ == "__main__":
    main()
