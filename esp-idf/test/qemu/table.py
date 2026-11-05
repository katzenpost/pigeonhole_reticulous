#!/usr/bin/env python3
# The Markdown table of what field_count printed in QEMU: per primitive, the instructions per
# call and its bytes, which are its function's and those of every function it calls in the
# app (not ROM's, such as memcpy), with the literals they load.
#   table.py <QEMU output> <objdump -d of the ELF> <nm -S of the ELF> [<name prefixes, comma-separated>]
import re
import sys

out, dis, sym = (open(p, errors="replace").read().replace("\r", "") for p in sys.argv[1:4])
only = [t for t in (sys.argv[4] if len(sys.argv) > 4 else "").split(",") if t]

fail = re.search(r"^KQ fail .*", out, re.M)
if fail or not re.search(r"^KQ done$", out, re.M):
    sys.exit(f"{fail[0] if fail else 'field_count did not finish'}; QEMU's output:\n{out[-3000:]}")

size = {int(a, 16): int(n, 16) for a, n in re.findall(r"^([0-9a-f]+) ([0-9a-f]+) [Tt] ", sym, re.M)}
calls, lits, fn = {}, {}, None  # per function: the addresses it calls, the literals it loads
for line in dis.splitlines():
    if m := re.match(r"([0-9a-f]+) <.*>:$", line):
        fn = int(m[1], 16)
        calls[fn], lits[fn] = set(), set()
    elif fn is not None:
        if m := re.search(r"\tcall(?:0|4|8|12)\s+([0-9a-f]+)", line):
            calls[fn].add(int(m[1], 16))
        if m := re.search(r"\tl32r\s+a\d+, ([0-9a-f]+) .*\(([0-9a-f]+)", line):
            lits[fn].add(int(m[1], 16))
            calls[fn].add(int(m[2], 16))  # a long call's target, when it is a function


def nbytes(f):
    seen, todo = set(), [f]
    while todo:
        g = todo.pop()
        if g in size and g not in seen:
            seen.add(g)
            todo += calls.get(g, ())
    return sum(size[g] for g in seen) + 4 * len(set().union(*(lits.get(g, set()) for g in seen)))


rows = [r for r in re.findall(r"^KQ count (\S+) 0x([0-9a-f]+) (\d+) (\d+)$", out, re.M)
        if not only or any(r[0].startswith(t) for t in only)]
if not rows:
    sys.exit(f"no primitive starts with {','.join(only)}; the names: "
             + " ".join(re.findall(r"^KQ count (\S+)", out, re.M)))
print("| primitive | instructions/call | bytes |\n|---|---:|---:|")
for name, addr, lo, hi in rows:
    print(f"| {name} | {lo if lo == hi else f'{lo}–{hi}'} | {nbytes(int(addr, 16))} |")

names = {r[0] for r in rows}
checks = [c for c in re.findall(r"^KQ check (\S+) (\d+) (\d+)$", out, re.M) if c[0] in names]
bad = [c for c in checks if c[2] != "0"]
if checks:
    print(f"\nOutputs bit-exact against *_ref: {len(checks) - len(bad)} of {len(checks)} primitives, "
          f"{checks[0][1]} chained calls each."
          + "".join(f"\n{n}: {d} of {k} outputs differ" for n, k, d in bad))
if any(lo != hi for _, _, lo, hi in rows):
    print("\nA range: the count depends on the data.")
sys.exit(1 if bad else 0)
