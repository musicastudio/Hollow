"""Finds text that does not fit its widget in a Hollow skin: static captions, every choice of a dropdown,
every label (or the widest number) a number field can show, and list cells. Widths come from the skin's
own font strips, the way the runtime measures them (Font::width), against the widget's rect less its pad.

    python tools/overflow.py [skin dir]      (FSVR's skin by default; prints one line per overflow)
"""
import glob
import json
import os
import sys

import numpy as np
from PIL import Image


def fonts(skin):
    out = {}
    for path in glob.glob(os.path.join(skin, "fonts", "*.png")):
        a = np.asarray(Image.open(path).convert("RGBA"))[:, :, 3]
        marks = np.nonzero(a[0])[0]
        if len(marks) == 256:   # a marker row: each mark starts a glyph
            xs = list(marks) + [a.shape[1]]
            out[os.path.basename(path)[:-4]] = ([xs[i + 1] - xs[i] for i in range(256)], a.shape[0] - 1)
        else:
            out[os.path.basename(path)[:-4]] = ([a.shape[1] // 256] * 256, a.shape[0])
    return out


def width(font, s):
    return sum(font[0][ord(c) if ord(c) < 256 else 63] for c in s)


def fmt(f, v):
    try:
        return f % v
    except (TypeError, ValueError):
        return str(v)


def shown(w, params, tables=None):
    """The texts a widget can show, one line each; None where it draws something the skin can't know."""
    kind, text = w.get("type"), (w.get("text") or {}).get("text", "")
    vt = w.get("valueText") if kind == "number" else None
    if vt and vt.get("table"):   # every cell of its table it can reach: its row (or all rows), its column (or all)
        t = (tables or {}).get(vt["table"]) or []
        rows = [t[vt["row"]]] if isinstance(vt.get("row"), int) and vt["row"] < len(t) else t
        return [c for r in rows for k, c in enumerate(r) if c and (not isinstance(vt.get("col"), int) or k == vt["col"])]
    if vt and vt.get("scale"):
        lo, hi = w.get("range") or [0, 1]
        return [fmt(vt.get("format") or "%g", lo * vt["scale"]), fmt(vt.get("format") or "%g", hi * vt["scale"])]
    if w.get("source") or w.get("key") or w.get("status"):
        return None
    if kind == "dropdown" and w.get("captionFromItem"):
        leaves = []

        def walk(items):
            for it in items or []:
                if it.get("items"):
                    walk(it["items"])
                elif not it.get("separator") and not it.get("action"):
                    leaves.append(it.get("short", it.get("label", "")))
        walk(w.get("items"))
        return leaves
    if kind == "number":
        p = params.get(w.get("param", "").replace("{part}", "p1").replace("{op}", "1").replace("{layer}", "v"))
        f = w.get("format") or (p or {}).get("format")
        if p and p.get("labels") and not w.get("format"):
            return list(p["labels"])
        lo, hi = (p["min"], p["max"]) if p else (w.get("range") or [0, 1])
        f = f or ("%.0f" if not p or p.get("steps", 0) > 0 else "%.2f")
        return [fmt(f, lo), fmt(f, hi)] + ([w["zeroText"]] if w.get("zeroText") else [])
    if kind in ("plate", "button", "dropdown", "textbox") and text:
        return text.replace("\\n", "\n").split("\n")
    return None


def check(skin):
    fs = fonts(skin)
    params = {p["id"]: p for p in json.load(open(os.path.join(skin, "params.json"), encoding="utf-8"))}
    tables = {os.path.basename(f)[:-5]: json.load(open(f, encoding="utf-8")) for f in glob.glob(os.path.join(skin, "data", "*.json"))}
    found = []
    for path in sorted(glob.glob(os.path.join(skin, "views", "*.json"))):
        view = os.path.basename(path)[:-5]
        for w in json.load(open(path, encoding="utf-8"))["widgets"]:
            x, y, rw, rh = w["rect"]
            pad = w.get("pad") or [0, 0, 0, 0]
            if w.get("type") == "list":
                row = fs.get((w.get("row") or {}).get("font"))
                cols = w.get("columns") or []
                for cells in w.get("rows") or []:
                    for c, cell in enumerate(cells):
                        if c >= len(cols) or cols[c].get("image") or cell.startswith("{data:") or not row:
                            continue
                        avail = cols[c]["width"] - pad[0] - pad[2]
                        if width(row, cell) > avail:
                            found.append((view, w["name"], cell, width(row, cell), avail))
                continue
            lines = shown(w, params, tables)
            font = fs.get((w.get("text") or {}).get("font"))
            if not lines or not font:
                continue
            avail = rw - pad[0] - pad[2]
            for ln in lines:
                if "{" in ln:   # a {var} template: its width is the value's, unknown here
                    continue
                if width(font, ln) > avail:
                    found.append((view, w["name"], ln, width(font, ln), avail))
            stacked = w.get("type") in ("plate", "button", "textbox") and len(lines) > 1   # a caption's lines, not a dropdown's choices
            if stacked and font[1] * (len(lines) - 0.5) > rh - pad[1] - pad[3]:
                found.append((view, w["name"], "(%d lines)" % len(lines), font[1] * len(lines), rh - pad[1] - pad[3]))
    return found


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    rows = check(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "FSVR", "plugin", "skin"))
    for view, name, text, tw, avail in rows:
        print("%-18s %-26s %4d > %-4d %s" % (view, name, tw, avail, text))
    print("%d overflows" % len(rows))
