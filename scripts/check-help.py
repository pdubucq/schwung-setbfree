#!/usr/bin/env python3
"""Validate help.json against what the Shadow UI's help viewer actually reads
and actually draws.

Two separate failure modes, both silent on device:

1. SCHEMA. The loader's entire test is `if (helpData.children)`
   (shadow_ui.js getModuleHelpChildren). A file that is valid JSON but names
   its topics anything else -- `sections`, `pages`, `parameters` -- is
   discarded without a word and the viewer reports "No help content available
   for this module", exactly as if the file were absent. b5 shipped in that
   state: the old help.json was several KB under a `sections` key that nobody
   could ever read. A schwung catalog sweep in 2026-08 found 12 modules doing
   the same thing.

2. WIDTH. A help line is drawn, never wrapped and never truncated.
   drawScrollableText calls print(4, y, line) and print() walks the string one
   glyph at a time; pixels past x=127 are dropped by set_pixel with no error
   anywhere. An over-long line loses its tail with no ellipsis and nothing in
   the log.

   THE BUDGET IS PIXELS, NOT CHARACTERS. load_font trims every glyph to its own
   inked extent, so the atlas is fixed-pitch but the screen is proportional:
   '.' advances 3px, 'W' advances 6px. Counting characters against a flat 20
   both misses real overflows and reports lines that render perfectly.

   So the widths are measured from the FONT table in schwung's
   scripts/generate_font.py, which schwung_host.c names as the single source of
   truth for the atlas. That makes the width half of this check conditional on
   schwung being checked out next to b5 (or SCHWUNG_ROOT being set); the schema
   half always runs, because it needs nothing.

Usage: python3 scripts/check-help.py [path/to/help.json]
"""

import ast
import json
import os
import re
import sys

# Read from the code that draws, at the paths below, when schwung is present.
SCREEN_WIDTH = 128   # scrollable_text.mjs
ORIGIN_X = 4         # print(4, y, lines[i], 1)
CHAR_SPACING = 1     # load_font("font.png", 1)

failures = []


def fail(msg):
    failures.append(msg)


def find_schwung():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for cand in (os.environ.get("SCHWUNG_ROOT"), os.path.join(here, "..", "schwung")):
        if cand and os.path.isfile(os.path.join(cand, "scripts", "generate_font.py")):
            return os.path.abspath(cand)
    return None


def glyph_widths(schwung_root):
    """Advance width per character, trimmed the way load_font trims."""
    path = os.path.join(schwung_root, "scripts", "generate_font.py")
    with open(path, encoding="utf-8") as f:
        src = f.read()

    start = src.index("FONT = {")
    end = src.index("\n}", start) + 2
    # The block is a plain literal; generate_font.py imports PIL at module
    # scope, so evaluating the file itself would need a dependency we do not
    # have. literal_eval on the slice is exact and needs nothing.
    font = ast.literal_eval(src[start + len("FONT = "):end])

    m = re.search(r"CHAR_W,\s*CHAR_H\s*=\s*(\d+),\s*(\d+)", src)
    if not m:
        fail("could not read CHAR_W/CHAR_H from generate_font.py")
        return None
    cell_w, cell_h = int(m.group(1)), int(m.group(2))

    widths = {}
    for ch, rows in font.items():
        if len(ch) != 1 or len(rows) != cell_h:
            continue
        first, last = -1, -1
        for x in range(cell_w):
            if any(rows[y][x] != "." for y in range(cell_h)):
                if first == -1:
                    first = x
                last = x
        # A blank cell keeps the full cell width -- load_font still advances
        # the cursor, which is what makes ' ' a space.
        widths[ch] = cell_w if first == -1 else last - first + 1

    if len(widths) < 90:
        fail("only parsed %d glyphs from generate_font.py" % len(widths))
    if widths.get("W") != cell_w:
        fail("expected 'W' to be a full-cell glyph; the FONT parse is wrong")
    return widths


def right_edge(line, widths):
    """Rightmost inked pixel, mirroring print()/glyph() exactly."""
    x, last = ORIGIN_X, ORIGIN_X - 1
    for ch in line:
        w = widths.get(ch)
        if w is None:
            x += CHAR_SPACING     # glyph() miss: a bare gap, nothing drawn
            continue
        last = x + w - 1
        x += w + CHAR_SPACING
    return last


def walk(node, trail, widths, counts):
    here = trail + "/" + str(node.get("title", "?"))
    lines = node.get("lines", [])
    kids = node.get("children", [])

    if not isinstance(lines, list):
        fail(here + ": 'lines' is not an array")
        lines = []
    if not lines and not kids:
        fail(here + ": node has neither 'lines' nor 'children' -- it opens empty")

    for line in lines:
        if not isinstance(line, str):
            fail(here + ": non-string line %r" % (line,))
            continue
        counts["lines"] += 1
        if widths is None:
            continue
        edge = right_edge(line, widths)
        counts["widest"] = max(counts["widest"], edge)
        if edge > SCREEN_WIDTH - 1:
            fail("%s: runs to x=%d, screen ends at %d -- %s"
                 % (here, edge, SCREEN_WIDTH - 1, json.dumps(line)))
        for ch in line:
            if ch not in widths:
                fail("%s: %s has no glyph and draws as a %dpx gap -- %s"
                     % (here, json.dumps(ch), CHAR_SPACING, json.dumps(line)))

    for kid in kids:
        walk(kid, here, widths, counts)


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 \
        else os.path.join(repo, "src", "schwung_bfree", "help.json")

    if not os.path.isfile(path):
        print("FAIL: %s not found" % path)
        return 1

    with open(path, encoding="utf-8") as f:
        try:
            help_data = json.load(f)
        except ValueError as e:
            print("FAIL: %s is not valid JSON: %s" % (path, e))
            return 1
    print("ok   help.json parses")

    children = help_data.get("children")
    if not isinstance(children, list) or not children:
        for alias in ("sections", "pages", "topics", "parameters"):
            if alias in help_data:
                print("FAIL: top-level key is %r, but the loader reads only "
                      "'children' -- this file is discarded silently and the "
                      "viewer shows 'No help content available'" % alias)
                return 1
        print("FAIL: help.json needs a non-empty top-level 'children' array")
        return 1
    print("ok   top-level 'children' array (%d topics)" % len(children))

    schwung = find_schwung()
    widths = None
    if schwung:
        widths = glyph_widths(schwung)
        print("ok   glyph widths from %s" % os.path.join(schwung, "scripts", "generate_font.py"))
    else:
        print("skip line-width check (schwung not found next to b5; "
              "set SCHWUNG_ROOT to enable it)")

    counts = {"lines": 0, "widest": 0}
    for node in children:
        walk(node, "", widths, counts)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    if widths is None:
        print("ok   %d help lines, schema valid" % counts["lines"])
    else:
        print("ok   %d help lines, widest right edge x=%d of %d"
              % (counts["lines"], counts["widest"], SCREEN_WIDTH - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
