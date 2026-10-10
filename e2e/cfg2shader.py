#!/usr/bin/env python3
"""Turn CFG shapes (cfgfuzz graph lines) into real RDNA compute-shader code words via LLVM's assembler.

usage: cfg2shader.py <in.graphs> <out.words> [--ids 1,2,3]
Each output line: <id>\t<hex words space-separated>\t<block count>

Block i executes `v_add_f32 v1, <i+1>.0, v1` (non-empty, distinct), conditional blocks compare the
thread id in v0 against a per-block constant so branches are not constant-foldable, return blocks store
v1 and end the program. Blocks are laid out greedily so a conditional's false target falls through
when possible (no trampoline block); otherwise an `s_branch` trampoline is emitted.
"""
import os
import subprocess
import sys
import tempfile

MC = "/usr/lib/llvm-21/bin/llvm-mc"
OBJCOPY = "/usr/lib/llvm-21/bin/llvm-objcopy"
MCFLAGS = ["--triple=amdgcn", "--mcpu=gfx1013", "--mattr=+wavefrontsize64", "-filetype=obj"]


def parse(text):
    return [[int(x) for x in field.split(",") if x] for field in text.split(";")]


def layout(succ):
    """Greedy order: after a conditional block, place its false target next if unplaced."""
    order, placed = [], set()
    stack = [0]
    while stack:
        b = stack.pop()
        if b in placed:
            continue
        order.append(b)
        placed.add(b)
        s = succ[b]
        if len(s) == 2:
            # push true first so false is popped (placed) next
            stack.append(s[0])
            stack.append(s[1])
        elif len(s) == 1:
            stack.append(s[0])
    for b in range(len(succ)):
        if b not in placed:
            order.append(b)
    # the decoder requires the program to end in s_endpgm: move a return block to the end if needed
    if succ[order[-1]]:
        returns = [b for b in order if not succ[b]]
        if returns:
            order.remove(returns[-1])
            order.append(returns[-1])
    return order


PAD = 0  # extra v_add instructions per block (each adds ~60 estimated SPIR-V words)


def assembly(succ):
    order = layout(succ)
    nxt = {order[i]: (order[i + 1] if i + 1 < len(order) else None) for i in range(len(order))}
    lines = [".text"]
    for b in order:
        s = succ[b]
        lines.append(f"B{b}:")
        lines.append(f"  v_add_f32 v1, {float(b + 1):.1f}, v1" if b + 1 in (1, 2, 4) else f"  v_add_f32 v1, 0x{0x3f800000 + b * 0x10000:x}, v1")
        for k in range(PAD):
            lines.append(f"  v_add_f32 v1, 0x{0x3f000000 + k * 0x1000 + b * 0x10:x}, v1")
        if not s:
            lines.append("  buffer_store_dword v1, off, s[4:7], 0")
            lines.append("  s_endpgm")
        elif len(s) == 1:
            if nxt[b] != s[0]:
                lines.append(f"  s_branch B{s[0]}")
        else:
            t, f = s
            lines.append(f"  v_cmp_gt_u32_e32 vcc, {b + 3}, v0")
            lines.append(f"  s_cbranch_vccnz B{t}")
            if nxt[b] != f:
                lines.append(f"  s_branch B{f}")
    return "\n".join(lines) + "\n"


def assemble(asm_text):
    with tempfile.TemporaryDirectory(prefix="cfg2shader-") as d:
        src = os.path.join(d, "g.s")
        obj = os.path.join(d, "g.o")
        binp = os.path.join(d, "g.bin")
        open(src, "w").write(asm_text)
        r = subprocess.run([MC, *MCFLAGS, src, "-o", obj], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("llvm-mc: " + r.stderr.strip()[:200])
        r = subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.text", obj, binp], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("objcopy: " + r.stderr.strip()[:200])
        data = open(binp, "rb").read()
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    return words


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ids = None
    global PAD
    args = sys.argv[3:]
    if "--pad" in args:
        PAD = int(args[args.index("--pad") + 1])
    if "--ids" in args:
        ids = set(args[args.index("--ids") + 1].split(","))
    n = 0
    with open(dst, "w") as out:
        for line in open(src):
            gid, text = line.rstrip("\n").split("\t")[:2]
            if ids is not None and gid not in ids:
                continue
            succ = parse(text)
            try:
                words = assemble(assembly(succ))
            except RuntimeError as e:
                print(f"{gid}: {e}", file=sys.stderr)
                continue
            out.write(f"{gid}\t{' '.join(f'{w:08x}' for w in words)}\t{len(succ)}\n")
            n += 1
    print(f"assembled {n} shaders", file=sys.stderr)


if __name__ == "__main__":
    main()
