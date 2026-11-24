#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Firmware footprint delta report (Markdown) from GNU ld map files.

Reads <build>/<variant>/<image>.map for a base and a PR build. Region use matches
`ld --print-memory-usage`; attribution comes from the map's input sections and adds up to it.
"""

import argparse
import os
import re
import sys
from collections import defaultdict

CORE = ("FLASH", "RAM")
WARN_PCT = 90.0
PADDING = "(padding and gaps)"
LITERALS = "(string literals, merged)"
GAPS = "(gaps between output sections)"

ADDR = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?(?:\s+(\S.*?))?\s*$")
SYM = re.compile(r"^\s+0x[0-9a-f]+\s+([A-Za-z_$][\w.$]*)$")
REGION = re.compile(r"^(\w+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")
NON_ALLOC = re.compile(r"^\.(debug|comment|ARM\.attributes|gnu\.attributes|stab)|^\.?tbss$")
MERGE = re.compile(r"\.(str\d+\.\d+|cst\d+)$")
PREFIX = re.compile(r"^\.(text|rodata|data|bss|noinit|ramfunc)\.")


def parse_map(path):
    """Return regions {name: (origin, length)}, output sections and input sections."""
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()
    try:
        start, end = lines.index("Memory Configuration"), lines.index("Linker script and memory map")
    except ValueError:
        sys.exit(f"{path}: not a GNU ld map file")
    regions = {m[1]: (int(m[2], 16), int(m[3], 16))
               for m in map(REGION.match, lines[start:end]) if m and m[1] != "Name"}

    outputs, inputs = [], []  # (name, vma, size, lma) / [output, name, size, file, first symbol]
    cur = pending = None  # pending: (is_output, name) whose address/size wrapped to the next line

    def add(is_output, name, m):
        if is_output:
            outputs.append((name, int(m[1], 16), int(m[2], 16), int(m[3], 16) if m[3] else None))
        elif cur:
            inputs.append([cur, name, int(m[2], 16), m[4], None])

    for line in lines[end + 1:]:
        indent = len(line) - len(line.lstrip())
        if pending:
            (is_output, name), pending = pending, None
            if m := ADDR.match(line):
                add(is_output, name, m)
                continue
        if indent > 1:  # a symbol defined in the input section above: names Zephyr's .noinit buffers
            if (m := SYM.match(line)) and inputs and inputs[-1][0] == cur and inputs[-1][4] is None:
                inputs[-1][4] = m[1]
            continue
        tok = line.split(None, 1)
        if not tok or tok[0].startswith(("*(", "[")):
            continue
        if indent == 0:
            cur = tok[0]
        if len(tok) == 1:
            pending = (indent == 0, tok[0])
        elif m := ADDR.match(" " + tok[1]):
            add(indent == 0, tok[0], m)
    return regions, outputs, inputs


def component(file):
    """Group an object file, as the map spells it, into a component."""
    if file is None:
        return PADDING
    archive = file.split("(")[0] if file.endswith(")") else None
    parts = (archive or file).split("/")
    if "-zephyr-eabi" in file or parts[-1] in ("libc.a", "libm.a", "libgcc.a"):
        return "toolchain (libc, libgcc)"
    if "nrfxlib" in parts:
        return [p for p in parts[parts.index("nrfxlib") + 1:] if p != "crypto"][0] + " (nrfxlib)"
    if "onomondo-uicc" in parts:
        return "SoftSIM: onomondo-uicc"
    if parts[:2] == ["modules", "softsim"]:
        return "SoftSIM: nRF port"
    if parts[0] == "app":
        return "sample application"
    if parts[0] == "zephyr":
        return f"Zephyr {parts[1]}" if parts[1] in ("kernel", "arch", "drivers", "subsys", "lib", "soc") \
            and len(parts) > 2 else "Zephyr (generated, other)"
    if parts[0] == "modules" and len(parts) > 2:
        if parts[1] != "nrf":
            return parts[1]
        return f"NCS {parts[3]}" if len(parts) > 4 and parts[2] in ("lib", "subsys", "drivers", "modules") else "NCS"
    if parts[0] == "linker":
        return "(linker generated)"
    # TF-M and other non-Zephyr builds: one component per library.
    return re.sub(r"^lib|\.a$", "", parts[-1]) if archive else "(objects linked directly)"


def item_name(section, sym, file):
    """The function or object an input section holds, with its source file."""
    if file is None:
        return "(padding)"
    obj = re.sub(r"\.obj$", "", re.search(r"([^/()]+)\)?$", file)[1])
    if section.startswith(".noinit.") and "/" in section:  # .noinit.<path>.c.<n>: K_*_DEFINE buffers
        label = sym or section.rsplit(".", 1)[-1]
    else:
        m = PREFIX.match(section)
        label = section[m.end():] if m else sym or f"({section})"
    return f"{label}  ({obj})"


def region_of(regions, addr):
    return next((r for r, (o, n) in regions.items() if o <= addr < o + n), None)


class Image:
    def __init__(self, path):
        self.regions, outputs, inputs = parse_map(path)
        self.used, self.comp, self.items, self.outsec = {}, defaultdict(int), defaultdict(int), defaultdict(int)

        # Region use as ld counts it: the highest address an output section reaches in the region
        # (by VMA, and by LMA for load copies such as .data), minus the region origin.
        place = {}
        for name, vma, size, lma in outputs:
            if not size or NON_ALLOC.search(name):
                continue
            place[name] = set()
            for addr in {vma, vma if lma is None else lma}:
                if r := region_of(self.regions, addr):
                    place[name].add(r)
                    self.used[r] = max(self.used.get(r, 0), addr + size - self.regions[r][0])
                    self.outsec[(r, name)] += size

        merged, attributed = set(), defaultdict(int)
        for out, name, size, file, sym in inputs:
            if not size or out not in place:
                continue
            if m := MERGE.search(name):
                # ld lists a merge group's first input with the whole merged blob, the rest at their
                # pre-merge size though they take no space; whose literal survived is not recorded.
                if (out, m[1]) in merged:
                    continue
                merged.add((out, m[1]))
                comp = label = LITERALS
            else:
                comp, label = component(file), item_name(name, sym, file)
            for r in place[out]:
                self.comp[(r, comp)] += size
                self.items[(r, label)] += size
                attributed[r] += size

        for r, used in self.used.items():
            gap = used - attributed[r]
            if gap < 0:
                sys.exit(f"{path}: {r}: input sections total {attributed[r]} B, more than the {used} B used")
            if gap:
                self.comp[(r, PADDING)] += gap
                self.items[(r, GAPS)] += gap
        if not any(self.used.get(r) for r in CORE):
            sys.exit(f"{path}: nothing placed in {'/'.join(CORE)}")


def kib(n):
    return f"{n / 1024:.1f} KiB"


def sign(n, bold=False):
    s = f"{n:+,d} B".replace(",", " ") if n else "0 B"
    return f"**{s}**" if bold and n else s


def diff(a, b):
    return {k: b.get(k, 0) - a.get(k, 0) for k in set(a) | set(b) if b.get(k, 0) != a.get(k, 0)}


def regions(base, pr):
    """FLASH and RAM, then any other region either build uses."""
    return [r for r in CORE if r in pr.regions] + sorted(
        r for r in pr.regions if r not in CORE and (base.used.get(r) or pr.used.get(r)))


def headline_rows(title, base, pr, warnings):
    rows = []
    for r in regions(base, pr):
        bu, pu, length = base.used.get(r, 0), pr.used.get(r, 0), pr.regions[r][1]
        pct = 100.0 * pu / length
        if pct >= WARN_PCT:
            warnings.append(f"{title} {r} is {pct:.1f}% full ({pu} of {length} bytes)")
        rows.append(f"| {title} | {r} | {kib(bu)} | {kib(pu)} | {sign(pu - bu, True)} "
                    f"| {pct:.1f}% of {kib(length)}{' ⚠️' if pct >= WARN_PCT else ''} |")
    return rows


def softsim_share(title, base, pr):
    parts = []
    for r in CORE:
        b, p = (sum(v for (rr, c), v in im.comp.items() if rr == r and c.startswith("SoftSIM")) for im in (base, pr))
        if b or p:
            parts.append(f"{kib(p)} {r}" + (f" ({sign(p - b)})" if p != b else ""))
    return [f"SoftSIM itself (nRF port + onomondo-uicc) in {title}: {', '.join(parts)}, "
            "not counting its share of the merged string literals.\n"] if parts else []


def detail(title, base, pr, top):
    comp, items, secs = diff(base.comp, pr.comp), diff(base.items, pr.items), diff(base.outsec, pr.outsec)
    if not (comp or items or secs):
        return []
    regs = regions(base, pr)

    # Every component, changed ones first by the size of the change, then the rest by size.
    cregs = [r for r in regs if any(k[0] == r for k in pr.comp)]
    names = {c for _, c in pr.comp} | {c for _, c in base.comp}
    order = sorted(names, key=lambda c: (-max(abs(comp.get((r, c), 0)) for r in cregs),
                                         -sum(pr.comp.get((r, c), 0) for r in cregs), c))
    out = ["<details>", f"<summary>{title}: by component</summary>\n",
           "| Component | " + " | ".join(f"{r} | Δ" for r in cregs) + " |", "|---|" + "---:|---:|" * len(cregs)]
    for c in order:
        cells = [x for r in cregs for x in (kib(pr.comp[(r, c)]) if pr.comp.get((r, c)) else "",
                                            sign(comp.get((r, c), 0), True) if (r, c) in comp else "")]
        out.append(f"| {c} | " + " | ".join(cells) + " |")
    out += ["\n</details>\n"]

    # Largest changes per region, the rest summed, so each list adds up to the headline delta.
    out += ["<details>", f"<summary>{title}: largest changes (functions and objects)</summary>\n", "```"]
    for r in regs:
        ch = sorted(((d, k[1]) for k, d in items.items() if k[0] == r), key=lambda x: (-abs(x[0]), x[1]))
        if ch:
            out.append(f"{r} ({sign(pr.used.get(r, 0) - base.used.get(r, 0))}):")
            out += [f"  {d:+8d} B  {label}" for d, label in ch[:top]]
            if ch[top:]:
                out.append(f"  {sum(d for d, _ in ch[top:]):+8d} B  ({len(ch) - top} smaller changes)")
    out += ["```", "\n</details>\n"]

    if secs:
        out += ["<details>", f"<summary>{title}: output section changes</summary>\n", "```"]
        for (r, s), d in sorted(secs.items(), key=lambda x: (regs.index(x[0][0]), -abs(x[1]), x[0][1])):
            out.append(f"  {d:+8d} B  {r:<6} {s:<26} ({base.outsec.get((r, s), 0)} -> {pr.outsec.get((r, s), 0)})")
        out += ["```", "\n</details>\n"]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base", help="base build directory")
    ap.add_argument("pr", help="PR build directory")
    ap.add_argument("--variant", action="append", default=[], metavar="DIR=TITLE",
                    help="configuration subdirectory and title; the first gets the full report")
    ap.add_argument("--image", action="append", default=[], metavar="STEM=TITLE", help="map file stem and title")
    ap.add_argument("--top", type=int, default=12, help="largest changes listed per region")
    ap.add_argument("--warnings", help="write region-almost-full warnings (first variant) here")
    args = ap.parse_args()
    variants = [v.split("=", 1) for v in args.variant] or [["default", "default"]]
    images = [i.split("=", 1) for i in args.image] or [["app", "app"]]

    def load(vdir):
        for stem, title in images:
            yield title, *(Image(os.path.join(d, vdir, stem + ".map")) for d in (args.base, args.pr))

    header = ["| Image | Region | base | PR | Δ | PR use of region |", "|---|---|---:|---:|---:|---:|"]
    warnings, out, primary = [], [], {}
    rows, notes, details = [], [], []
    for title, base, pr in load(variants[0][0]):
        primary[title] = (diff(base.items, pr.items), diff(base.used, pr.used))
        rows += headline_rows(title, base, pr, warnings)
        notes += softsim_share(title, base, pr)
        details += detail(title, base, pr, args.top)
    out += [f"**{variants[0][1]}**\n", *header, *rows, "", *notes, *(details or ["No change in any image.\n"])]

    # Other configurations: one Δ row each; detail only where it differs from the first.
    cols, summary, extra = [], [], []
    for vdir, vtitle in variants[1:]:
        cells, rows, details, same = {}, [], [], True
        for title, base, pr in load(vdir):
            same &= (diff(base.items, pr.items), diff(base.used, pr.used)) == primary[title]
            for r in regions(base, pr):
                cols += [(title, r)] if (title, r) not in cols else []
                cells[(title, r)] = pr.used.get(r, 0) - base.used.get(r, 0)
            rows += headline_rows(title, base, pr, [])
            details += detail(title, base, pr, args.top)
        note = "" if not details else "same changes as above" if same else "detail below"
        summary.append((vtitle, cells, note))
        if details and not same:
            extra += ["<details>", f"<summary>{vtitle}: detail</summary>\n", *header, *rows, "", *details, "</details>\n"]
    if summary:
        out += ["**Other configurations** (Δ only)\n", "| Configuration | " + " | ".join(f"{i} {r}" for i, r in cols)
                + " | |", "|---|" + "---:|" * len(cols) + "---|"]
        out += [f"| {v} | " + " | ".join(sign(c[k], True) if k in c else "" for k in cols) + f" | {n} |"
                for v, c, n in summary]
        out += ["", *extra]
    print("\n".join(out))
    if args.warnings:
        with open(args.warnings, "w") as f:
            f.writelines(w + "\n" for w in warnings)


if __name__ == "__main__":
    main()
