#!/usr/bin/env python3
"""Compare structurizer fuzz results across variants and block-size configs."""
import collections
import glob
import os
import re
import sys

RES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "results")
CORPUS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus.graphs")
VARIANTS = ["main", "pr2471", "alxc20", "both"]


def key_of(cls, msg):
    return cls + "|" + re.sub(r"\d+", "#", msg)


def load(variant, words):
    out = {}
    path = os.path.join(RES, f"{variant}_w{words}.results")
    if not os.path.exists(path):
        return out
    for line in open(path):
        cols = line.rstrip("\n").split("\t")
        if len(cols) < 4:
            continue
        out[cols[0]] = (cols[1], cols[2], int(cols[3]))
    return out


def main():
    corpus = {}
    for line in open(CORPUS):
        gid, text = line.rstrip("\n").split("\t")
        corpus[gid] = text
    for words in (0, 16):
        data = {v: load(v, words) for v in VARIANTS}
        if not data["main"]:
            continue
        ids = sorted(data["main"], key=int)
        print(f"\n################ words per block = {words}   graphs = {len(ids)} ################")
        print(f"{'variant':8s} " + " ".join(f"{c:>9s}" for c in ["OK", "BUDGET", "REFUSED", "INVARIANT", "TIMEOUT", "CRASH"]))
        for v in VARIANTS:
            if not data[v]:
                continue
            cnt = collections.Counter(data[v][g][0] for g in ids if g in data[v])
            print(f"{v:8s} " + " ".join(f"{cnt.get(c, 0):9d}" for c in ["OK", "BUDGET", "REFUSED", "INVARIANT", "TIMEOUT", "CRASH"]))
        # transitions relative to main
        for v in VARIANTS[1:]:
            if not data[v]:
                continue
            fixed = [g for g in ids if g in data[v] and data["main"][g][0] != "OK" and data[v][g][0] == "OK"]
            regressed = [g for g in ids if g in data[v] and data["main"][g][0] == "OK" and data[v][g][0] != "OK"]
            changed_fail = [g for g in ids if g in data[v] and data["main"][g][0] != "OK" and data[v][g][0] != "OK" and key_of(*data["main"][g][:2]) != key_of(*data[v][g][:2])]
            print(f"\n  {v} vs main: fixed {len(fixed)}, regressed {len(regressed)}, failure-class changed {len(changed_fail)}")
            for g in regressed[:12]:
                print(f"    REGRESSION graph {g} ({data[v][g][2]} blocks): main={data['main'][g][0]} -> {v}={data[v][g][0]} {data[v][g][1][:110]}")
                print(f"        {corpus[g]}")
            if len(regressed) > 12:
                print(f"    ... {len(regressed) - 12} more regressions")
        # remaining failure classes per variant
        for v in VARIANTS:
            if not data[v]:
                continue
            classes = collections.defaultdict(list)
            for g in ids:
                if g in data[v] and data[v][g][0] != "OK":
                    classes[key_of(*data[v][g][:2])].append(g)
            print(f"\n  --- {v}: {sum(len(x) for x in classes.values())} failing graphs in {len(classes)} classes ---")
            for k, gs in sorted(classes.items(), key=lambda kv: -len(kv[1])):
                smallest = min(gs, key=lambda g: data[v][g][2])
                print(f"    {len(gs):6d}  {k[:120]}")
                print(f"            smallest: graph {smallest} ({data[v][smallest][2]} blocks): {corpus[smallest]}")
            # serious classes get all examples listed
            for g in ids:
                if g in data[v] and data[v][g][0] in ("INVARIANT", "TIMEOUT", "CRASH"):
                    print(f"    !! {data[v][g][0]} graph {g}: {data[v][g][1][:140]}\n        {corpus[g]}")


if __name__ == "__main__":
    main()
