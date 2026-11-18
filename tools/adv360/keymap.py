"""Parse the ZMK keymap, macros and physical layout of this repo.

Only what this repo's files actually use is supported: plain bindings (no
C preprocessor macros inside `bindings`), behavior nodes without nested
nodes, and one physical layout.
"""

import os
import re
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "config"
KEYMAP_FILE = CONFIG / "adv360.keymap"
BEHAVIOR_FILES = [KEYMAP_FILE, CONFIG / "macros.dtsi", CONFIG / "user_macros.dtsi"]
LAYOUT_FILE = CONFIG / "boards/arm/adv360/adv360-layouts.dtsi"
WATCHED_FILES = BEHAVIOR_FILES + [LAYOUT_FILE]


@dataclass
class Key:
    """Physical key, units are 1/100 key width, rotation in degrees."""
    w: float
    h: float
    x: float
    y: float
    rot: float = 0
    rx: float = 0
    ry: float = 0


@dataclass
class Layer:
    index: int
    name: str
    display_name: str
    bindings: list  # raw binding strings, e.g. "&kp A"


@dataclass
class Behavior:
    label: str
    compatible: str
    bindings: list = field(default_factory=list)  # list of binding groups
    display_name: str = ""


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def split_bindings(body):
    """'&kp A &mo 1 &trans' -> ['&kp A', '&mo 1', '&trans']"""
    return [" ".join(t.split()) for t in re.findall(r"&[\w-]+(?:\s+(?!&)[^\s&<>,]+)*", body)]


def parse_layout(path=LAYOUT_FILE):
    text = strip_comments(path.read_text())
    keys = []
    for m in re.finditer(r"<\s*&key_physical_attrs([^>]*)>", text):
        nums = [int(n) for n in re.findall(r"-?\d+", m.group(1))]
        w, h, x, y, rot, rx, ry = nums
        keys.append(Key(w, h, x, y, rot / 100, rx, ry))
    return keys


def parse_layers(path=KEYMAP_FILE):
    text = strip_comments(path.read_text())
    keymap = text[text.index("zmk,keymap"):]
    layers = []
    for m in re.finditer(r"([\w-]+)\s*\{([^{}]*?)\};", keymap):
        name, body = m.groups()
        b = re.search(r"bindings\s*=\s*<(.*?)>\s*;", body, flags=re.S)
        if not b:
            continue  # e.g. status = "reserved" layers
        d = re.search(r'display-name\s*=\s*"([^"]*)"', body)
        layers.append(Layer(len(layers), name, d.group(1) if d else name, split_bindings(b.group(1))))
    return layers


def parse_behaviors(paths=BEHAVIOR_FILES):
    behaviors = {}
    for path in paths:
        if not path.exists():
            continue
        text = strip_comments(path.read_text())
        for m in re.finditer(r"([\w-]+)\s*:\s*[\w-]+\s*\{([^{}]*)\}", text):
            label, body = m.groups()
            c = re.search(r'compatible\s*=\s*"([^"]+)"', body)
            if not c:
                continue
            b = re.search(r"bindings\s*=(.*?);", body, flags=re.S)
            groups = [split_bindings(g) for g in re.findall(r"<([^>]*)>", b.group(1))] if b else []
            d = re.search(r'display-name\s*=\s*"([^"]*)"', body)
            behaviors[label] = Behavior(label, c.group(1), groups, d.group(1) if d else "")
    return behaviors


# ---- labels ---------------------------------------------------------------

KEY_NAMES = {
    "COMMA": ",", "DOT": ".", "FSLH": "/", "SLASH": "/", "BSLH": "\\", "BACKSLASH": "\\",
    "SEMI": ";", "SEMICOLON": ";", "SQT": "'", "APOS": "'", "DQT": '"', "DOUBLE_QUOTES": '"',
    "LBKT": "[", "RBKT": "]", "LBRC": "{", "RBRC": "}", "LEFT_BRACE": "{", "RIGHT_BRACE": "}",
    "LPAR": "(", "RPAR": ")", "LEFT_PARENTHESIS": "(", "RIGHT_PARENTHESIS": ")",
    "MINUS": "-", "EQUAL": "=", "PLUS": "+", "ASTRK": "*", "ASTERISK": "*", "STAR": "*",
    "EXCL": "!", "EXCLAMATION": "!", "AT": "@", "AT_SIGN": "@", "HASH": "#", "POUND": "#",
    "DOLLAR": "$", "DLLR": "$", "PERCENT": "%", "PRCNT": "%", "CARET": "^", "AMPERSAND": "&", "AMPS": "&",
    "PIPE": "|", "TILDE": "~", "GRAVE": "`", "COLON": ":", "QUESTION": "?", "QMARK": "?",
    "LESS_THAN": "<", "LT": "<", "GREATER_THAN": ">", "GT": ">", "UNDERSCORE": "_", "UNDER": "_",
    "SPACE": "Space", "ENTER": "Enter", "RET": "Enter", "RETURN": "Enter", "BSPC": "⌫",
    "BACKSPACE": "⌫", "DEL": "Del", "DELETE": "Del", "TAB": "Tab", "ESC": "Esc", "ESCAPE": "Esc",
    "LEFT": "←", "RIGHT": "→", "UP": "↑", "DOWN": "↓", "HOME": "Home", "END": "End",
    "PG_UP": "PgUp", "PG_DN": "PgDn", "INS": "Ins", "INSERT": "Ins",
    "LSHFT": "Shift", "RSHFT": "Shift", "LSHIFT": "Shift", "RSHIFT": "Shift",
    "LCTRL": "Ctrl", "RCTRL": "Ctrl", "LALT": "Alt", "RALT": "AltGr", "LGUI": "Super", "RGUI": "Super",
    "CAPS": "Caps", "CAPSLOCK": "Caps", "PRINTSCREEN": "PrtSc", "PSCRN": "PrtSc",
    "SCROLLLOCK": "ScrLk", "SLCK": "ScrLk", "KP_NUM": "NumLk", "KP_NUMLOCK": "NumLk",
    "KP_EQUAL": "=", "KP_DIVIDE": "/", "KP_MULTIPLY": "*", "KP_MINUS": "-", "KP_PLUS": "+",
    "KP_ENTER": "Enter", "KP_DOT": ".", "C_VOL_UP": "Vol+", "C_VOL_DN": "Vol-", "C_MUTE": "Mute",
}
MOD_FUNCS = {"LS": "Shift", "RS": "Shift", "LC": "Ctrl", "RC": "Ctrl", "LA": "Alt", "RA": "AltGr",
             "LG": "Super", "RG": "Super"}
# This repo types accented letters with ScrollLock (compose) sequences.
COMPOSE = {('"', "o"): "ö", ('"', "a"): "ä", ('"', "u"): "ü", ("~", "o"): "õ",
           ("<", "s"): "š", ("<", "z"): "ž", ("o", "o"): "°"}
SHORT = {"&trans": "▽", "&none": "", "&key_repeat": "Repeat", "&caps_word": "CapsWd",
         "&bootloader": "Boot", "&sys_reset": "Reset", "&studio_unlock": "Studio",
         "&macro_ver": "Version"}


def key_label(code):
    """'LS(N1)' -> 'Shift+1', 'N1' -> '1', 'A' -> 'A'."""
    m = re.fullmatch(r"(\w\w)\((.+)\)", code)
    if m and m.group(1) in MOD_FUNCS:
        return f"{MOD_FUNCS[m.group(1)]}+{key_label(m.group(2))}"
    if re.fullmatch(r"N\d", code) or re.fullmatch(r"NUMBER_\d", code):
        return code[-1]
    if re.fullmatch(r"KP_N\d", code):
        return code[-1]
    return KEY_NAMES.get(code, code)


class Labeler:
    def __init__(self, layers, behaviors):
        self.layers = layers
        self.behaviors = behaviors

    def layer_name(self, idx):
        idx = int(idx)
        return self.layers[idx].display_name if idx < len(self.layers) else str(idx)

    def label(self, binding, depth=0):
        """Returns (main, sub): sub is a hint like the shifted symbol or 'hold'."""
        if binding in SHORT:
            return SHORT[binding], ""
        name, *params = binding.split()
        if name == "&kp" and params:
            return key_label(params[0]), ""
        if name in ("&mo", "&sl", "&to", "&tog") and params:
            kind = {"&mo": "hold", "&sl": "sticky", "&to": "to", "&tog": "toggle"}[name]
            return self.layer_name(params[0]), kind
        if name == "&bt":
            p = " ".join(params).replace("BT_", "")
            return "BT " + p.replace("SEL ", ""), "bluetooth"
        if name in ("&rgb_ug", "&bl") and params:
            what = params[0].replace("RGB_", "").replace("BL_", "").replace("_CMD", "")
            return ("RGB " if name == "&rgb_ug" else "Light ") + what.title(), "lighting"
        if name == "&stp":
            return "Battery", "status"
        beh = self.behaviors.get(name[1:])
        if beh and depth < 3:
            return self.behavior_label(beh, params, depth)
        return name[1:], " ".join(params)

    def behavior_label(self, beh, params, depth):
        flat = [b for g in beh.bindings for b in g]
        if beh.compatible == "zmk,behavior-mod-morph" and len(flat) >= 2:
            return self.label(flat[0], depth + 1)[0], self.label(flat[1], depth + 1)[0]
        if beh.compatible == "zmk,behavior-hold-tap" and len(params) >= 2:
            return key_label(params[1]), key_label(params[0])
        if beh.compatible == "zmk,behavior-macro":
            # Follow the macro's mode: bindings are tapped unless preceded
            # by &macro_press / &macro_release.
            mode, taps, pressed, layer = "tap", [], [], None
            for b in flat:
                name, *args = b.split()
                if name in ("&macro_tap", "&macro_press", "&macro_release"):
                    mode = name[len("&macro_"):]
                elif name == "&kp" and args:
                    (taps if mode == "tap" else pressed if mode == "press" else []).append(args[0])
                elif name == "&mo" and args and mode != "release":
                    layer = args[0]
            if layer is not None:
                return "+".join(key_label(k) for k in pressed) or "hold", self.layer_name(layer)
            if len(taps) == 3 and taps[0] in ("SCROLLLOCK", "SLCK"):
                seq = (key_label(taps[1]).lower(), key_label(taps[2]).lower())
                return COMPOSE.get(seq, "⎄" + "".join(seq)), "compose"
            if len(taps) == 1:
                return key_label(taps[0]), ""
            if beh.display_name:
                return beh.display_name, "macro"
            if 1 < len(taps) <= 4 and all(len(key_label(k)) == 1 for k in taps):
                return "".join(key_label(k) for k in taps), "macro"
        return beh.display_name or beh.label, beh.compatible.split("-")[-1]


def load():
    """Everything the UI needs, as a JSON-serializable dict."""
    layers = parse_layers()
    labeler = Labeler(layers, parse_behaviors())
    keys = parse_layout()
    out_layers = []
    for layer in layers:
        if len(layer.bindings) != len(keys):
            raise ValueError(f"layer {layer.name}: {len(layer.bindings)} bindings, layout has {len(keys)} keys")
        labels = [labeler.label(b) for b in layer.bindings]
        out_layers.append({
            "index": layer.index, "name": layer.name, "display_name": layer.display_name,
            "keys": [{"binding": b, "label": m, "sub": s} for b, (m, s) in zip(layer.bindings, labels)],
        })
    return {"layout": [k.__dict__ for k in keys], "layers": out_layers, "version": files_version()}


def files_version():
    """Changes whenever one of the parsed files changes."""
    return max((os.stat(p).st_mtime_ns for p in WATCHED_FILES if p.exists()), default=0)
