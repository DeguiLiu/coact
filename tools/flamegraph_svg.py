#!/usr/bin/env python3
"""Render a coact folded-stacks file into an SVG flame graph.

Input format (one line per unique stack, as written by bench_hotpath):
    frame1;frame2;...;leaf <count>

Output: a self-contained SVG where bar width is proportional to sample count
and color is hashed from the function name. Root at the top, children stacked
below; frames are sorted by count (largest first).

Usage:
    python3 tools/flamegraph_svg.py in.folded out.svg [title]
"""
import hashlib
import sys


def parse(path):
    stacks = []
    for raw in open(path, "r", encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        frames_str, _, count_str = line.rpartition(" ")
        if not count_str.isdigit():
            continue
        stacks.append((frames_str.split(";"), int(count_str)))
    return stacks


def color_for(name):
    h = hashlib.md5(name.encode("utf-8")).hexdigest()
    hue = int(h[:4], 16) % 360
    sat = 55 + (int(h[4:6], 16) % 30)
    return f"hsl({hue} {sat}% 65%)"


class Node:
    __slots__ = ("name", "total", "children")

    def __init__(self, name):
        self.name = name
        self.total = 0
        self.children = {}


def build_tree(stacks):
    root = Node("<root>")
    for frames, count in stacks:
        node = root
        node.total += count
        for f in frames:
            if f not in node.children:
                node.children[f] = Node(f)
            node = node.children[f]
            node.total += count
    return root


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: flamegraph_svg.py in.folded out.svg [title]")
    stacks = parse(sys.argv[1])
    if not stacks:
        sys.exit("no stacks parsed from " + sys.argv[1])
    title = sys.argv[3] if len(sys.argv) > 3 else "coact hot path"
    root = build_tree(stacks)
    total = root.total

    WIDTH = 1200.0
    ROW_H = 16.0
    FONT = 10
    depth = 0

    def max_depth(node, d):
        nonlocal depth
        depth = max(depth, d + 1)
        for c in node.children.values():
            max_depth(c, d + 1)

    max_depth(root, 0)
    height = ROW_H * (depth + 1) + 40

    out = []
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" '
               f'height="{height}" font-family="monospace" font-size="{FONT}">')
    out.append(f'<text x="6" y="16" font-size="12">{title} '
               f'(total {total} samples)</text>')

    def render(node, x, y, width, prefix=""):
        if node.name != "<root>":
            label = node.name
            color = color_for(node.name)
            out.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(width, 0.5):.1f}" '
                f'height="{ROW_H - 1:.1f}" fill="{color}" stroke="#000" '
                f'stroke-width="0.25"/>'
            )
            label_w = len(label) * (FONT * 0.55)
            if label_w <= width - 4 and width > 30:
                out.append(
                    f'<text x="{x + 3:.1f}" y="{y + ROW_H - 4:.1f}" '
                    f'font-size="{FONT}">{label}</text>'
                )
            prefix = label if not prefix else label + ";" + prefix

        if not node.children:
            return
        kids = sorted(node.children.values(), key=lambda c: c.total, reverse=True)
        cur = x
        for c in kids:
            cw = width * (c.total / node.total) if node.total else 0.0
            render(c, cur, y + ROW_H, cw, prefix)
            cur += cw

    render(root, 0.0, 30.0, WIDTH)
    out.append("</svg>")
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print(f"wrote {sys.argv[2]} ({len(stacks)} unique stacks, {total} samples)")


if __name__ == "__main__":
    main()
