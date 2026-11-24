#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Firmware footprint delta report for the footprint workflow.

Compares two builds (base and PR) image by image and writes a Markdown report
to stdout. Each build directory holds one subdirectory per configuration
(variant), and each variant holds a <image>.elf / <image>.map pair per image:

    <build>/<variant>/<image>.elf     linked image (debug info may be stripped)
    <build>/<variant>/<image>.map     GNU ld map file from the same link

Everything is derived from those two files, standard library only:

* Region usage (the headline) reproduces what `ld --print-memory-usage`
  prints at the end of the build: for every memory region in the map's
  "Memory Configuration", the highest address any allocated output section
  occupies in it (by VMA, and by LMA for load copies such as .data), minus
  the region origin. Unlike summing program header sizes, this counts the
  load reservation of NOBITS-in-RAM sections and the holes ld leaves before
  fixed-address placements (the TF-M veneer region), so the number is the
  one the link fits or fails against, and the region length gives headroom.

* Attribution comes from the map's input sections: with -ffunction-sections
  and -fdata-sections an input section is one function or object, and every
  allocated byte belongs to one (or to linker fill). So unlike an ELF
  symbol-table diff it also covers string literals, static functions under
  their object file, and anonymous data, and it reconciles with the headline:
  what no input section accounts for is alignment padding, shown as such.
  Mergeable string literals are deduplicated across objects at link time and
  the map does not say whose survived, so they are one exact "merged" item.
  No DWARF is needed, unlike Zephyr's size_report.
"""

import argparse
import os
import re
import struct
import sys
from collections import defaultdict

SHT_NOBITS = 8
SHF_ALLOC = 0x2
SHF_TLS = 0x400

# Regions every report shows even when unused; others (IDT_LIST, ...) only
# when something occupies them in either build.
CORE_REGIONS = ("FLASH", "RAM")
WARN_PCT = 90.0


# --------------------------------------------------------------------------
# ELF: only the section headers are needed, to tell allocated output sections
# (and their type) from debug/metadata ones, which the map lists too.


def elf_sections(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        sys.exit(f"{path}: not an ELF file")
    is64 = data[4] == 2
    end = "<" if data[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(end + "Q", data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x3A)
        fmt = end + "IIQQQQIIQQ"
    else:
        shoff, = struct.unpack_from(end + "I", data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(end + "HHH", data, 0x2E)
        fmt = end + "IIIIIIIIII"
    hdrs = [struct.unpack_from(fmt, data, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]

    def name(off):
        return data[stroff + off:data.index(b"\0", stroff + off)].decode()

    out = {}
    for h in hdrs:
        sh_name, sh_type, sh_flags, sh_addr, _off, sh_size = h[:6]
        if sh_flags & SHF_ALLOC and sh_size:
            out[name(sh_name)] = (sh_type, sh_flags, sh_addr, sh_size)
    return out


# --------------------------------------------------------------------------
# Map file


_OUT_RE = re.compile(r"^(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?\s*$")
_IN_RE = re.compile(r"^ (\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+(\S.*))?$")
_ADDR_SIZE_RE = re.compile(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)(?:\s+load address 0x([0-9a-f]+))?(?:\s+(\S.*))?$")
_SYM_RE = re.compile(r"^\s+0x[0-9a-f]+\s+([A-Za-z_.$][\w.$]*)\s*$")
_REGION_RE = re.compile(r"^(\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)")


def parse_map(path):
    """Return (regions, output sections, input sections) from a GNU ld map.

    regions:  {name: (origin, length)}
    outputs:  [(name, vma, size, lma or None)]
    inputs:   [(output name, input name, vma, size, file, first symbol)]
              file is None for *fill* (linker padding).
    """
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()

    regions = {}
    i = 0
    while i < len(lines) and lines[i] != "Memory Configuration":
        i += 1
    i += 1
    while i < len(lines) and lines[i] != "Linker script and memory map":
        m = _REGION_RE.match(lines[i])
        if m and m.group(1) not in ("Name", "*default*"):
            regions[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
        i += 1
    if not regions:
        sys.exit(f"{path}: no Memory Configuration (not a GNU ld map?)")

    outputs, inputs = [], []
    cur = None
    pending_in = None  # input section name whose address/size wrapped
    pending_out = None  # output section name whose address/size wrapped
    last_in = None  # index of the last input section, for its first symbol
    for line in lines[i + 1:]:
        if pending_out is not None:
            m = _ADDR_SIZE_RE.match(line)
            name, pending_out = pending_out, None
            if m:
                cur = name
                lma = int(m.group(3), 16) if m.group(3) else None
                outputs.append((name, int(m.group(1), 16), int(m.group(2), 16), lma))
                continue
        if pending_in is not None:
            m = _ADDR_SIZE_RE.match(line)
            name, pending_in = pending_in, None
            if m and cur is not None:
                inputs.append([cur, name, int(m.group(1), 16), int(m.group(2), 16), m.group(4), None])
                last_in = len(inputs) - 1
                continue
        if not line:
            continue
        if line[0] not in " \t":
            last_in = None
            m = _OUT_RE.match(line)
            if m:
                cur = m.group(1)
                lma = int(m.group(4), 16) if m.group(4) else None
                outputs.append((cur, int(m.group(2), 16), int(m.group(3), 16), lma))
            elif re.match(r"^[.\w/$-][^\s]*$", line) and not line.startswith(("LOAD", "OUTPUT")):
                pending_out = line
            else:
                cur = None if line.startswith(("LOAD ", "OUTPUT(", "START GROUP", "END GROUP")) else cur
            continue
        if line.startswith(" *(") or line.startswith(" FILL") or line.startswith(" ["):
            continue
        if line.startswith(" ") and not line.startswith("  "):
            m = _IN_RE.match(line)
            if m:
                if cur is not None:
                    name = m.group(1)
                    file = m.group(4).strip() if m.group(4) else None
                    if name == "*fill*":
                        file = None
                    inputs.append([cur, name, int(m.group(2), 16), int(m.group(3), 16), file, None])
                    last_in = len(inputs) - 1
                continue
            tok = line.strip()
            if " " not in tok and not tok.startswith("*("):
                pending_in = tok
            continue
        m = _SYM_RE.match(line)
        if m and last_in is not None and inputs[last_in][5] is None:
            sym = m.group(1)
            if not sym.startswith("."):
                inputs[last_in][5] = sym
    return regions, outputs, inputs


# --------------------------------------------------------------------------
# Footprint model of one image


def region_of(regions, addr):
    for name, (origin, length) in regions.items():
        if origin <= addr < origin + length:
            return name
    return None


PADDING = "(padding and gaps)"


def component(file):
    """Group an object file (as the map spells it) into a component label."""
    if file is None:
        return PADDING
    m = re.match(r"^(.*?)(?:\(([^()]*)\))?$", file)
    a = m.group(1).replace("\\", "/")
    is_archive = m.group(2) is not None
    base = os.path.basename(a)
    parts = a.split("/")
    if base in ("libc.a", "libm.a", "libgcc.a", "libstdc++.a", "libnosys.a") or "-zephyr-eabi/" in a:
        return "toolchain (libc, libgcc)"
    if "nrfxlib" in parts:
        rest = [p for p in parts[parts.index("nrfxlib") + 1:] if p != "crypto"]
        return f"{rest[0]} (nrfxlib)"
    if "onomondo-uicc" in parts:
        return "SoftSIM: onomondo-uicc"
    if parts[:2] == ["modules", "softsim"] or "onomondo-softsim" in a:
        return "SoftSIM: nRF port"
    if parts[0] == "app":
        return "sample application"
    if parts[0] == "zephyr":
        if len(parts) > 2 and parts[1] in ("kernel", "arch", "drivers", "subsys", "lib", "soc"):
            return f"Zephyr {parts[1]}"
        return "Zephyr (generated, other)"
    if parts[0] == "modules" and len(parts) > 2:
        if parts[1] == "nrf":
            return f"NCS {parts[3]}" if len(parts) > 4 and parts[2] in ("lib", "subsys", "drivers", "modules") else "NCS"
        return parts[1]
    if parts[0] == "linker":
        return "(linker generated)"
    if is_archive:
        # TF-M and other non-Zephyr builds: one component per library.
        return re.sub(r"^lib|\.a$", "", base)
    return "(objects linked directly)"


LITERALS = "(string literals, merged)"
_MERGE_RE = re.compile(r"\.(str\d+\.\d+|cst\d+)$")

_SEC_PREFIX = re.compile(r"^\.?(text|rodata|data|bss|noinit|ramfunc|sdata|sbss|tdata|tbss)\.")


def item_name(in_name, sym, file):
    """A readable name for an input section: the function/object it holds."""
    if in_name == "*fill*":
        return "(padding)"
    obj = None
    if file:
        m = re.search(r"\(([^()]*)\)$", file)
        obj = m.group(1) if m else os.path.basename(file)
        obj = re.sub(r"\.obj$", "", obj)
    if in_name.startswith(".noinit.") and "/" in in_name:
        # .noinit."<path>.c".<n|name>: Zephyr's K_*_DEFINE buffers and stacks.
        label = sym or in_name.rsplit(".", 1)[-1]
    else:
        m = _SEC_PREFIX.match(in_name)
        label = in_name[m.end():] if m else (sym or f"({in_name})")
    return f"{label}  ({obj})" if obj else label


class Image:
    def __init__(self, elf, mapf):
        self.secs = elf_sections(elf)
        self.regions, outputs, inputs = parse_map(mapf)
        self.used = defaultdict(int)  # region -> bytes, as ld counts them
        # (region, component) -> bytes, (region, item key) -> bytes
        self.comp = defaultdict(int)
        self.items = defaultdict(int)
        self.outsec = defaultdict(int)  # (region, output section) -> bytes

        place = {}  # output section -> regions it occupies (VMA, then LMA)
        hi = {}
        for name, vma, size, lma in outputs:
            sec = self.secs.get(name)
            if sec is None or size == 0:
                continue  # debug/metadata or empty
            sh_type, sh_flags = sec[0], sec[1]
            if sh_flags & SHF_TLS and sh_type == SHT_NOBITS:
                continue  # .tbss occupies no memory of its own
            spots = []
            r = region_of(self.regions, vma)
            if r:
                spots.append(r)
                hi[r] = max(hi.get(r, 0), vma + size)
            if lma is not None and lma != vma:
                lr = region_of(self.regions, lma)
                if lr and lr != r:
                    spots.append(lr)
                    hi[lr] = max(hi.get(lr, 0), lma + size)
            place[name] = spots
            for s in spots:
                self.outsec[(s, name)] += size
        for r, top in hi.items():
            self.used[r] = top - self.regions[r][0]

        attributed = defaultdict(int)
        merged = set()
        for out, in_name, vma, size, file, sym in inputs:
            if size == 0 or out not in place:
                continue
            m = _MERGE_RE.search(in_name)
            if m:
                # Mergeable constants (string literals): ld lists the first
                # input of each merge group with the size of the whole merged
                # blob and every later one with its pre-merge size, though it
                # occupies nothing. Which object a deduplicated literal came
                # from is not recorded, so the blob is one item.
                group = (out, m.group(1))
                if group in merged:
                    continue
                merged.add(group)
                comp = label = LITERALS
            else:
                comp = component(file)
                label = item_name(in_name, sym, file)
            for r in place[out]:
                self.comp[(r, comp)] += size
                self.items[(r, label)] += size
                attributed[r] += size
        # Gaps between output sections (alignment) and holes before
        # fixed-address placements: in ld's count, in no input section.
        for r in self.used:
            gap = self.used[r] - attributed[r]
            if gap < 0:
                # Input sections claim more than the region holds: the map
                # is not laid out the way this parser expects.
                sys.exit(f"{mapf}: {r}: input sections total {attributed[r]} B, "
                         f"more than the {self.used[r]} B the region uses")
            if gap:
                self.comp[(r, PADDING)] += gap
        if not any(self.used.get(r) for r in CORE_REGIONS):
            sys.exit(f"{mapf}: nothing placed in {'/'.join(CORE_REGIONS)}")


# --------------------------------------------------------------------------
# Report


def kib(n):
    return f"{n / 1024:.1f} KiB"


def sign(n):
    return f"{n:+,d} B".replace(",", " ") if n else "0 B"


def region_order(regions):
    return [r for r in CORE_REGIONS if r in regions] + sorted(r for r in regions if r not in CORE_REGIONS)


def diff(a, b):
    keys = set(a) | set(b)
    return {k: b.get(k, 0) - a.get(k, 0) for k in keys if b.get(k, 0) != a.get(k, 0)}


def shown_regions(base, pr):
    return [r for r in region_order(pr.regions)
            if r in CORE_REGIONS or base.used.get(r) or pr.used.get(r)]


def headline_rows(name, base, pr, warnings):
    rows = []
    for r in shown_regions(base, pr):
        bu, pu = base.used.get(r, 0), pr.used.get(r, 0)
        length = pr.regions[r][1]
        pct = 100.0 * pu / length if length else 0.0
        flag = ""
        if pct >= WARN_PCT:
            flag = " ⚠️"
            warnings.append(f"{name} {r} is {pct:.1f}% full ({pu} of {length} bytes)")
        d = pu - bu
        rows.append(f"| {name} | {r} | {kib(bu)} | {kib(pu)} | {'**' + sign(d) + '**' if d else '0 B'} "
                    f"| {pct:.1f}% of {kib(length)}{flag} |")
    return rows


def share_line(name, base, pr, prefix):
    """One line: what the components named <prefix>* take of each region."""
    parts = []
    for r in CORE_REGIONS:
        b = sum(v for (rr, c), v in base.comp.items() if rr == r and c.startswith(prefix))
        p = sum(v for (rr, c), v in pr.comp.items() if rr == r and c.startswith(prefix))
        if b or p:
            parts.append(f"{kib(p)} {r}" + (f" ({sign(p - b)})" if p != b else ""))
    if not parts:
        return None
    return (f"{prefix} itself (nRF port + onomondo-uicc) in {name}: " + ", ".join(parts)
            + ", not counting its share of the merged string literals.")


def detail(name, base, pr, top):
    out = []
    regions = shown_regions(base, pr)
    comp = diff(base.comp, pr.comp)
    items = diff(base.items, pr.items)
    secs = diff(base.outsec, pr.outsec)
    if not (comp or items or secs):
        return out

    # Every component, with its size in the PR build and its change: changed
    # ones first by the size of the change, then the rest by size.
    cregs = [r for r in regions if any(k[0] == r for k in pr.comp)]
    names = {c for (_, c) in pr.comp} | {c for (_, c) in base.comp}

    def key(c):
        dmax = max((abs(comp.get((r, c), 0)) for r in cregs), default=0)
        return (0 if dmax else 1, -dmax, -sum(pr.comp.get((r, c), 0) for r in cregs), c)

    out.append("<details>")
    out.append(f"<summary>{name}: by component</summary>\n")
    out.append("| Component | " + " | ".join(f"{r} | Δ" for r in cregs) + " |")
    out.append("|---|" + "---:|---:|" * len(cregs))
    for c in sorted(names, key=key):
        cells = []
        for r in cregs:
            v, d = pr.comp.get((r, c), 0), comp.get((r, c), 0)
            cells += [kib(v) if v else "", f"**{sign(d)}**" if d else ""]
        out.append(f"| {c} | " + " | ".join(cells) + " |")
    out.append("\n</details>\n")

    # Largest changes per region, then the rest summed, so each region's
    # list adds up to its headline delta.
    out.append("<details>")
    out.append(f"<summary>{name}: largest changes (functions and objects)</summary>\n")
    out.append("```")
    for r in regions:
        ch = sorted(((d, k[1]) for k, d in items.items() if k[0] == r), key=lambda x: (-abs(x[0]), x[1]))
        gap = comp.get((r, PADDING), 0) - sum(d for d, label in ch if label == "(padding)")
        if gap:
            ch.append((gap, "(gaps between output sections)"))
            ch.sort(key=lambda x: (-abs(x[0]), x[1]))
        if not ch:
            continue
        out.append(f"{r} ({sign(pr.used.get(r, 0) - base.used.get(r, 0))}):")
        for d, label in ch[:top]:
            out.append(f"  {d:+8d} B  {label}")
        rest = ch[top:]
        if rest:
            out.append(f"  {sum(d for d, _ in rest):+8d} B  ({len(rest)} smaller changes)")
    out.append("```")
    out.append("\n</details>\n")

    if secs:
        out.append("<details>")
        out.append(f"<summary>{name}: output section changes</summary>\n")
        out.append("```")
        for (r, s), d in sorted(secs.items(), key=lambda x: (regions.index(x[0][0]) if x[0][0] in regions else 99, -abs(x[1]), x[0][1])):
            out.append(f"  {d:+8d} B  {r:<6} {s:<26} ({base.outsec.get((r, s), 0)} -> {pr.outsec.get((r, s), 0)})")
        out.append("```")
        out.append("\n</details>\n")
    return out


def load(base_dir, pr_dir, vdir, stem):
    paths = [os.path.join(d, vdir, stem + ext) for d in (base_dir, pr_dir) for ext in (".elf", ".map")]
    missing = [p for p in paths if not os.path.exists(p)]
    if missing:
        sys.exit("missing build output: " + ", ".join(missing))
    return Image(paths[0], paths[1]), Image(paths[2], paths[3])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base", help="base build directory")
    ap.add_argument("pr", help="PR build directory")
    ap.add_argument("--variant", action="append", default=[], metavar="DIR=TITLE",
                    help="variant subdirectory and its title, in report order; the first gets the full report")
    ap.add_argument("--image", action="append", default=[], metavar="STEM=TITLE",
                    help="image file stem and its title, in report order")
    ap.add_argument("--top", type=int, default=12, help="largest changes listed per region")
    ap.add_argument("--warnings", help="write region-almost-full warnings (first variant) here, one per line")
    args = ap.parse_args()

    variants = [v.split("=", 1) for v in args.variant] or [["default", "default"]]
    images = [i.split("=", 1) for i in args.image] or [["app", "app"]]
    warnings, out = [], []
    header = "| Image | Region | base | PR | Δ | PR use of region |\n|---|---|---:|---:|---:|---:|"

    # First variant: headline, SoftSIM's own share, and the full attribution.
    vdir, vtitle = variants[0]
    rows, details, notes = [], [], []
    primary = {}  # image -> item-level changes, to spot variants that repeat them
    for stem, ititle in images:
        base, pr = load(args.base, args.pr, vdir, stem)
        primary[stem] = (diff(base.items, pr.items), diff(base.used, pr.used))
        rows += headline_rows(ititle, base, pr, warnings)
        details += detail(ititle, base, pr, args.top)
        line = share_line(ititle, base, pr, "SoftSIM")
        if line:
            notes.append(line)
    out += [f"**{vtitle}**\n", header, *rows, ""]
    out += [f"{x}\n" for x in notes]
    out += details or ["No change in any image.\n"]

    # Other variants: a Δ-only summary row each, the detail collapsed.
    if len(variants) > 1:
        cols = []
        rest = []
        for vdir, vtitle in variants[1:]:
            cells, rows, details, same = {}, [], [], True
            for stem, ititle in images:
                base, pr = load(args.base, args.pr, vdir, stem)
                same &= diff(base.items, pr.items) == primary[stem][0]
                same &= diff(base.used, pr.used) == primary[stem][1]
                for r in shown_regions(base, pr):
                    if (ititle, r) not in cols:
                        cols.append((ititle, r))
                    cells[(ititle, r)] = pr.used.get(r, 0) - base.used.get(r, 0)
                rows += headline_rows(ititle, base, pr, [])
                details += detail(ititle, base, pr, args.top)
            rest.append((vtitle, cells, rows, [] if same else details, same and bool(details)))
        out.append("**Other configurations** (Δ only)\n")
        out.append("| Configuration | " + " | ".join(f"{i} {r}" for i, r in cols) + " | |")
        out.append("|---|" + "---:|" * len(cols) + "---|")
        for vtitle, cells, _, details, same in rest:
            note = "same changes as above" if same else ("detail below" if details else "")
            out.append(f"| {vtitle} | " + " | ".join(
                (f"**{sign(cells[c])}**" if cells[c] else "0 B") if c in cells else "" for c in cols) + f" | {note} |")
        out.append("")
        for vtitle, _, rows, details, _ in rest:
            if not details:
                continue
            out += ["<details>", f"<summary>{vtitle}: detail</summary>\n", header, *rows, "", *details, "</details>\n"]
    print("\n".join(out))
    if args.warnings:
        with open(args.warnings, "w") as f:
            f.write("".join(w + "\n" for w in warnings))


if __name__ == "__main__":
    main()
