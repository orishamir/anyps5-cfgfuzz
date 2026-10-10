#!/usr/bin/env python3
import collections, os, re
E = os.path.dirname(os.path.abspath(__file__))
def load(v):
    d = {}
    for line in open(os.path.join(E, f"sample_{v}.results")):
        c = line.rstrip("\n").split("\t")
        if len(c) >= 3: d[c[0]] = (c[1], c[2])
    return d
data = {v: load(v) for v in ("main", "pr2471", "both")}
classes = collections.OrderedDict()
for gid in data["main"]: classes.setdefault(gid.split(":")[0], []).append(gid)
def outcome(stage, msg):
    if stage == "STRUCTURIZE": return "structurizer budget" if "budget" in msg else "structurizer refused"
    return stage
print(f"{'class':14s} {'build':8s} " + " ".join(f"{c:>24s}" for c in ["DONE", "INVALID_SPIRV", "VALID_BUT_VERIFY_FAILED", "structurizer budget", "structurizer refused", "other"]))
for cls, ids in classes.items():
    for v in ("main", "pr2471", "both"):
        cnt = collections.Counter(outcome(*data[v][g]) for g in ids if g in data[v])
        known = ["DONE", "INVALID_SPIRV", "VALID_BUT_VERIFY_FAILED", "structurizer budget", "structurizer refused"]
        other = sum(n for k, n in cnt.items() if k not in known)
        print(f"{cls:14s} {v:8s} " + " ".join(f"{cnt.get(c, 0):24d}" for c in known) + f" {other:24d}")
    print()
print("=== validation messages (numbers stripped) ===")
msgs = collections.Counter()
for v in data:
    for g, (stage, msg) in data[v].items():
        if stage == "INVALID_SPIRV":
            m = re.sub(r"\d+", "#", msg); m = m[m.find("failed:") + 8:][:110]
            msgs[(v, m)] += 1
for (v, m), n in sorted(msgs.items(), key=lambda kv: (kv[0][1], kv[0][0])): print(f"  {n:4d} {v:8s} {m}")
