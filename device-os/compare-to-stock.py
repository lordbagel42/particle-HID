#!/usr/bin/env python3
"""Proves which code differs between two Device OS system-part ELFs.

1. Functions (STT_FUNC): each instruction is reduced to mnemonic + operands.
   Branch/call targets become symbol+offset. Literal-pool words that hold a RAM
   or flash address are masked as ADDR, because LTO relays out data when any
   variable is added; constants are kept verbatim.
2. Dynalib tables (the ABI user firmware calls through): every entry is mapped
   to the exact function symbol it points at and compared in order.

Usage: device-os/compare-to-stock.py stock.elf patched.elf [function-to-show]

Build stock.elf from the same Device OS tag without the patch series. Output
should list only the functions the patches touch and no dynalib differences.
"""
import difflib
import re
import subprocess
import sys

TOOL = "/opt/tc/gcc-arm-none-eabi-10-2020-q4-major/bin/arm-none-eabi-"


def run(*args):
    return subprocess.run([TOOL + args[0], *args[1:]], capture_output=True, text=True, check=True).stdout


def is_addr(v):
    return 0x28000 <= v < 0x100000 or 0x20000000 <= v < 0x20040000


def func_names(elf):
    names = set()
    for line in run("readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
            names.add(p[7])
    return names


def func_ranges(elf):
    out = []
    for line in run("readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND" and p[0].rstrip(":").isdigit():
            out.append((int(p[1], 16) & ~1, int(p[2], 0), p[7]))
    out.sort()
    return out


def functions(elf):
    import bisect
    keep = func_names(elf)
    ranges = func_ranges(elf)
    starts = [r[0] for r in ranges]

    def target(m):
        a = int(m.group(1), 16)
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ranges[i][0] + max(ranges[i][1], 1):
            return "<%s+%#x>" % (ranges[i][2], a - ranges[i][0])
        return "<?%s>" % m.group(2)  # outside any function: keep objdump's label
    dis = run("objdump", "-d", "--no-show-raw-insn", elf)
    funcs, cur = {}, None
    head = re.compile(r"^[0-9a-f]+ <(.+)>:$")
    ins = re.compile(r"^\s*[0-9a-f]+:\s*(.*)$")
    for line in dis.splitlines():
        m = head.match(line)
        if m:
            cur = m.group(1) if m.group(1) in keep else None
            if cur:
                funcs[cur] = []
            continue
        m = ins.match(line)
        if not m or cur is None:
            continue
        text = m.group(1).split(";")[0].strip()
        text = re.sub(r"\b([0-9a-f]+) <([^>]+)>", target, text)  # branch targets by containing function
        w = re.match(r"\.word\s+0x([0-9a-f]+)", text)
        if w and is_addr(int(w.group(1), 16)):
            text = ".word ADDR"
        funcs[cur].append(text)
    return funcs


def dynalib_tables(elf):
    syms, addr_to_func = {}, {}
    for line in run("readelf", "-sW", elf).splitlines():
        p = line.split()
        if len(p) >= 8 and p[6] != "UND" and p[0].rstrip(":").isdigit():
            v, size = int(p[1], 16), int(p[2], 0)
            if p[3] == "OBJECT" and p[7].startswith("dynalib_"):
                syms[p[7]] = (v, size)
            if p[3] == "FUNC":
                addr_to_func.setdefault(v & ~1, p[7])
    data = {}
    for line in run("objdump", "-s", "-j", ".text", elf).splitlines():  # tables live in flash
        m = re.match(r"^ ([0-9a-f]+) ((?:[0-9a-f]{1,8} ?){1,4})", line)
        if m:
            base = int(m.group(1), 16)
            for i, word in enumerate(m.group(2).split()):
                data[base + 4 * i] = word
    tables = {}
    for name, (v, size) in syms.items():
        entries = []
        for off in range(0, size, 4):
            word = data.get(v + off)
            if word is None:
                entries.append("?")
                continue
            val = int.from_bytes(bytes.fromhex(word.ljust(8, "0")), "little")
            entries.append(addr_to_func.get(val & ~1, "0x%x" % val))
        tables[name] = entries
    return tables


def main(a, b, show=None):
    fa, fb = functions(a), functions(b)
    if show:
        for l in difflib.unified_diff(fa.get(show, []), fb.get(show, []), a, b, lineterm="", n=2):
            print(l)
        return
    changed = sorted(n for n in set(fa) & set(fb) if fa[n] != fb[n])
    print("functions: %d in %s, %d in %s" % (len(fa), a, len(fb), b))
    print("only in %s: %s" % (a, sorted(set(fa) - set(fb)) or "none"))
    print("only in %s: %s" % (b, sorted(set(fb) - set(fa)) or "none"))
    print("changed: %d" % len(changed))
    for n in changed:
        print("  %s  (%d -> %d instructions)" % (n, len(fa[n]), len(fb[n])))
    ta, tb = dynalib_tables(a), dynalib_tables(b)
    entries = sum(len(t) for t in ta.values())
    diff = [n for n in sorted(set(ta) | set(tb)) if ta.get(n) != tb.get(n)]
    print("dynalib tables: %d tables, %d entries; differing tables: %s" % (len(ta), entries, diff or "none"))
    for n in diff:
        for i, (x, y) in enumerate(zip(ta.get(n, []), tb.get(n, []))):
            if x != y:
                print("  %s[%d]: %s -> %s" % (n, i, x, y))


if __name__ == "__main__":
    main(*sys.argv[1:])
