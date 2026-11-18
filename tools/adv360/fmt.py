"""Re-align the layer bindings in config/adv360.keymap on the V3 76-key grid.

    python3 tools/adv360/fmt.py [--check]
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adv360.keymap import KEYMAP_FILE, split_bindings  # noqa: E402

# Grid columns per row: 0-6 left main, 7-10 left thumb, 11-14 right thumb, 15-21 right main.
GRID = [
    list(range(0, 7)) + list(range(15, 22)),
    list(range(0, 7)) + list(range(15, 22)),
    list(range(0, 7)) + [9, 10, 11, 12] + list(range(15, 22)),
    list(range(0, 6)) + [10, 11] + list(range(16, 22)),
    list(range(0, 5)) + [8, 9, 10, 11, 12, 13] + list(range(17, 22)),
]
INDENT = " " * 8


def format_bindings(tokens):
    if len(tokens) != sum(map(len, GRID)):
        raise ValueError(f"{len(tokens)} bindings, expected {sum(map(len, GRID))}")
    rows, i = [], 0
    for cols in GRID:
        rows.append(dict(zip(cols, tokens[i:i + len(cols)])))
        i += len(cols)
    width = {c: max((len(r[c]) for r in rows if c in r), default=0) for c in range(22)}
    return "\n".join(INDENT + " ".join(r.get(c, "").ljust(width[c]) for c in range(22) if width[c]).rstrip()
                     for r in rows)


def format_keymap(text):
    keymap_at = text.index("zmk,keymap")

    def repl(m):
        if m.start() < keymap_at or "//" in m.group(2) or "/*" in m.group(2):
            return m.group(0)  # leave anything with comments alone
        return f"{m.group(1)}\n{format_bindings(split_bindings(m.group(2)))}\n{m.group(3)}"

    return re.sub(r"(bindings = <)(.*?)\n(\s*>;)", repl, text, flags=re.S)


if __name__ == "__main__":
    text = KEYMAP_FILE.read_text()
    new = format_keymap(text)
    if "--check" in sys.argv:
        sys.exit(0 if new == text else "keymap is not formatted: run python3 tools/adv360/fmt.py")
    KEYMAP_FILE.write_text(new)
