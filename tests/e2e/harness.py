"""End-to-end test harness: drive the real keyboard, check what the OS types.

Needs the TEST firmware (bin/flash.sh --test) and an X11 desktop or Hyprland.

  * Keyboard  - injects key presses through the key-monitor ports. Keys of
                the right half are pressed on the right half (its own
                key-monitor port), so they reach the left half over the
                split link with the real Bluetooth latency. Keys are looked
                up by their label in config/adv360.keymap, so tests survive
                keys being moved around.
  * Capture   - a GTK text window (same input method as your apps, so
                compose and Ctrl+Shift+U unicode entry work) that is forced to
                the front and must hold the keyboard focus before anything is
                pressed; it records the resulting text and every key event.
  * E2ETestCase - unittest base class wiring both together.

Writing a test:

    class TestSomething(E2ETestCase):
        def test_it(self):
            kb = self.kb
            kb.tap(kb.key("Hexpad", layer="Base"))
            with kb.hold(kb.key("Shift", layer="Base", sub="LShift")):
                kb.tap(kb.key("7", layer="Hexpad"))
            self.assertTyped("7")
"""

import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
import unittest
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from adv360 import keymap  # noqa: E402
from adv360.device import RIGHT_PRODUCT, KeyMonitor  # noqa: E402


class E2EError(AssertionError):
    pass


# ---- the keyboard ---------------------------------------------------------

class Keyboard:
    """Presses keys on the real keyboard and waits for it to acknowledge each
    one, so tests are deterministic instead of sleep-based."""

    def __init__(self, monitor, km, quiet=0.03, before_input=None):
        self.monitor = monitor
        self.right = None  # the right half's key monitor, if it has one
        # (pos, pressed, uptime us) as each half saw its keys; the left
        # half sees the right half's keys when they arrive over the link
        self.left_times, self.right_times = [], []
        self.layers = km["layers"]
        layout = km["layout"]
        middle = (min(k["x"] for k in layout) + max(k["x"] + k["w"] for k in layout)) / 2
        self.right_keys = {i for i, k in enumerate(layout) if k["x"] + k["w"] / 2 > middle}
        # After each press/release, wait until the keyboard has been quiet
        # this long: macros (e.g. the Shift keys: Shift + layer) finish
        # asynchronously and report their layer changes a few ms later.
        self.quiet = quiet
        self.before_input = before_input or (lambda: None)
        self.events = queue.Queue()
        self.log = []  # (time, line) of everything the keyboard said, for failure messages
        self.layer_mask = 0
        self.down = set()
        self._fast = False
        self._gap = 0.0

    # -- finding keys --------------------------------------------------------

    def layer(self, name):
        for layer in self.layers:
            if name in (layer["display_name"], layer["name"], layer["index"]):
                return layer
        raise E2EError(f"no layer {name!r} in the keymap")

    def key(self, label, layer="Base", sub=None, nth=0):
        """Position of the nth key on `layer` whose label (and sub label, if
        given) matches, e.g. key("Shift", sub="RShift"), key("7", "Hexpad")."""
        L = self.layer(layer)
        found = [i for i, k in enumerate(L["keys"])
                 if k["label"] == label and (sub is None or k["sub"] == sub)]
        if len(found) <= nth:
            raise E2EError(f"no key labelled {label!r}{f' / {sub!r}' if sub else ''} "
                           f"on layer {L['display_name']!r}")
        return found[nth]

    def char_map(self):
        """{char: (how, position)} for every character the keymap can type
        with one key: how is "plain" (Base layer), "shift" (Base key with
        left Shift held), "hexpad" (tap the Hexpad key first) or
        "hexpad-shift" (both: uppercase compose letters). Built from
        the labels, so it follows keymap changes."""
        if getattr(self, "_char_map", None) is None:
            base, lshift, hexpad = (self.layer(n)["keys"] for n in ("Base", "LShift", "Hexpad"))
            chars = {" ": ("plain", self.key("Space"))}
            for pos, k in enumerate(base):
                label, sub = k["label"], k["sub"]
                if len(label) == 1:
                    chars.setdefault(label.lower() if label.isalpha() else label, ("plain", pos))
                over = lshift[pos]
                if over["binding"] != "&trans":
                    shifted = over["label"] if len(over["label"]) == 1 else None
                elif label.isalpha() and len(label) == 1:
                    shifted = label.upper()
                else:
                    shifted = sub if len(sub) == 1 else None
                if shifted:
                    chars.setdefault(shifted, ("shift", pos))
            for pos, k in enumerate(hexpad):
                label = k["label"]
                if len(label) == 1 and k["binding"] != "&trans":
                    chars.setdefault(label.lower() if label.isalpha() else label, ("hexpad", pos))
                    if label.isalpha():
                        chars.setdefault(label.upper(), ("hexpad-shift", pos))
            self._char_map = chars
        return self._char_map

    def type(self, text):
        """Type text the way a person would on this keymap (see char_map)."""
        chars = self.char_map()
        for ch in text:
            if ch not in chars:
                raise E2EError(f"don't know how to type {ch!r} with one key")
            how, pos = chars[ch]
            if how == "plain":
                self.tap(pos)
            elif how == "shift":
                with self.hold(self.key("Shift", sub="LShift")):
                    self.tap(pos)
            elif how == "hexpad-shift":
                with self.hold(self.key("Shift", sub="LShift")):
                    self.tap(self.key("Hexpad", sub="sticky"), pos)
            else:
                self.tap(self.key("Hexpad", sub="sticky"), pos)

    def binding(self, pos, layer="Base"):
        return self.layer(layer)["keys"][pos]["binding"]

    # -- device events ---------------------------------------------------------

    def on_message(self, line):
        self.log.append((time.monotonic(), line))
        line = self._device_time(line, self.left_times)
        if line.startswith("L "):
            self.layer_mask = int(line.split()[1], 16)
        self.events.put(line)

    @staticmethod
    def _device_time(line, times):
        """'P <pos> <state> <us>' -> 'P <pos> <state>', noting the time."""
        parts = line.split()
        if parts[:1] == ["P"] and len(parts) == 4:
            times.append((int(parts[1]), parts[2] == "1", int(parts[3])))
            return " ".join(parts[:3])
        return line

    def on_right_message(self, line):
        self.log.append((time.monotonic(), "R " + line))
        self._device_time(line, self.right_times)

    def _wait_for(self, line, timeout=2.0, prefix=False):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                got = self.events.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                break
            if got == line or (prefix and got.startswith(line)):
                return got
            if got.startswith("E ") or got == "disconnected":
                raise E2EError(f"keyboard said {got!r} while waiting for {line!r}")
        raise E2EError(f"keyboard didn't acknowledge {line!r} within {timeout}s")

    def _settle(self, max_wait=0.5):
        """Consume follow-up events (layer changes from macros, sticky
        layers, ...) until nothing arrives for `self.quiet` seconds."""
        if self._fast:
            time.sleep(self._gap)
            return
        deadline = time.monotonic() + max_wait
        while time.monotonic() < deadline:
            try:
                got = self.events.get(timeout=self.quiet)
            except queue.Empty:
                return
            if got.startswith("E ") or got == "disconnected":
                raise E2EError(f"keyboard said {got!r}")

    CAPS_LOCK_LED = 0x2

    def indicators(self):
        """HID indicator bits the keyboard LEDs show (CAPS_LOCK_LED, ...)."""
        self.monitor.send("I")
        return int(self._wait_for("I ", prefix=True).split()[1], 16)

    # -- pressing keys ---------------------------------------------------------

    def half(self, pos):
        """Key monitor that presses `pos` like a person would: the right
        half's own one for its keys (over the split link), else the left."""
        if self.right is not None and pos in self.right_keys:
            return self.right
        return self.monitor

    def press(self, pos):
        self.before_input()
        self.half(pos).press(pos, True)
        self._wait_for(f"P {pos} 1")
        self.down.add(pos)
        self._settle()

    def release(self, pos):
        self.half(pos).press(pos, False)
        self._wait_for(f"P {pos} 0")
        self.down.discard(pos)
        self._settle()

    def play(self, steps, timeout=2.0):
        """Timed key script, like a person typing: steps are
        (seconds after the previous step, position, pressed). Nothing waits
        for the keyboard in between, so presses can overtake each other on
        the way to the left half as in real typing; afterwards every step
        must have been acknowledged. Returns [(pos, pressed, sent, acked)]
        with host times, e.g. to measure the split latency."""
        done = []
        self.before_input()  # (slow: the clock starts after it)
        at = time.monotonic()
        for delay, pos, pressed in steps:
            at += delay
            while (now := time.monotonic()) < at:
                time.sleep(min(0.0005, at - now))
            self.half(pos).press(pos, pressed)
            done.append([pos, bool(pressed), time.monotonic(), None])
            (self.down.add if pressed else self.down.discard)(pos)
        deadline = time.monotonic() + timeout
        while any(d[3] is None for d in done) and time.monotonic() < deadline:
            try:
                got = self.events.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                break
            if got.startswith("E ") or got == "disconnected":
                raise E2EError(f"keyboard said {got!r} during play()")
            for d in done:
                if d[3] is None and got == f"P {d[0]} {int(d[1])}":
                    d[3] = time.monotonic()
                    break
        missing = [f"P {d[0]} {int(d[1])}" for d in done if d[3] is None]
        if missing:
            raise E2EError(f"keyboard didn't acknowledge {missing} within {timeout}s")
        self._settle()
        return [tuple(d) for d in done]

    def tap(self, *positions, hold=0.02):
        for pos in positions:
            self.press(pos)
            time.sleep(hold)
            self.release(pos)

    @contextmanager
    def hold(self, *positions):
        """with kb.hold(shift): kb.tap(x)  - keys are released even on failure."""
        for pos in positions:
            self.press(pos)
        try:
            yield
        finally:
            for pos in reversed(positions):
                if pos in self.down:
                    self.release(pos)

    @contextmanager
    def fast(self, gap=0.0):
        """Type fast: wait for each acknowledgement (a few ms) plus `gap`
        seconds, but not for macros or layer changes to settle. For
        reproducing fast-typing races (a quick human is ~0.03-0.06 s)."""
        self._fast, self._gap = True, gap
        try:
            yield
        finally:
            self._fast, self._gap = False, 0.0

    def release_all(self):
        for pos in list(self.down):
            try:
                self.release(pos)
            except E2EError:
                self.half(pos).press(pos, False)
        self.down.clear()

    def wait_idle(self, timeout=3.0):
        """Wait until only the base layer is active (sticky layers time out).
        Bit 0 is ignored: after &to 0 ZMK reports the base layer explicitly."""
        deadline = time.monotonic() + timeout
        while self.layer_mask & ~1 and time.monotonic() < deadline:
            time.sleep(0.05)
        if self.layer_mask & ~1:
            raise E2EError(f"layers still active after {timeout}s: mask {self.layer_mask:#x}")


# ---- the focused text view ---------------------------------------------------

class Capture:
    """A GTK window with a text view that owns the keyboard focus. Everything
    the keyboard types lands here, through the same input method (IBus,
    fcitx5) as in normal GTK apps, so compose sequences and Ctrl+Shift+U
    unicode entry work. Key events are recorded too.

    Focus is checked with the window manager before every key press: on X11
    with xdotool, on Hyprland (Wayland, where xdotool sees no native windows)
    with hyprctl."""

    TITLE = "Adv360 e2e capture"

    def __init__(self):
        self.hyprland = bool(os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"))
        if not (self.hyprland or os.environ.get("DISPLAY")):
            raise E2EError("DISPLAY is not set: run the e2e tests from your desktop session")
        import gi
        gi.require_version("Gtk", "3.0")
        gi.require_version("Gdk", "3.0")
        from gi.repository import Gdk, Gtk
        self.Gtk, self.Gdk = Gtk, Gdk

        self.title = f"{self.TITLE} {os.getpid()}"
        self._window_id = None
        self.key_events = []

        self.win = Gtk.Window(title=self.title)
        self.win.set_default_size(760, 300)
        self.win.move(80, 80)
        self.win.set_keep_above(True)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, margin=8)
        self.label = Gtk.Label(xalign=0)
        self.view = Gtk.TextView(monospace=True, wrap_mode=Gtk.WrapMode.CHAR)
        self.view.connect("key-press-event", self._record, "down")
        self.view.connect("key-release-event", self._record, "up")
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.add(self.view)
        box.pack_start(self.label, False, False, 0)
        box.pack_start(scroll, True, True, 0)
        self.win.add(box)
        self.win.show_all()
        self.pump(0.3)
        self.focus()

    def _record(self, widget, event, kind):
        self.key_events.append((kind, self.Gdk.keyval_name(event.keyval) or str(event.keyval),
                                "", int(event.state)))
        return False  # let the text view (and IBus) handle it

    def pump(self, seconds=0.0):
        Gtk = self.Gtk
        end = time.monotonic() + seconds
        while True:
            while Gtk.events_pending():
                Gtk.main_iteration_do(False)
            if time.monotonic() >= end:
                return
            time.sleep(0.005)

    @staticmethod
    def _xdotool(*args):
        try:
            return subprocess.run(["xdotool", *args], capture_output=True, text=True,
                                  timeout=2).stdout.split()
        except (OSError, subprocess.TimeoutExpired):
            return []

    @property
    def window_id(self):
        """The X window the window manager knows."""
        if self._window_id is None:
            ids = self._xdotool("search", "--name", "^" + re.escape(self.title) + "$")
            self._window_id = int(ids[-1]) if ids else None
        return self._window_id

    @staticmethod
    def _hyprctl(*args):
        try:
            r = subprocess.run(["hyprctl", *args], capture_output=True, text=True, timeout=2)
            return r.stdout
        except (OSError, subprocess.TimeoutExpired):
            return ""

    def _hypr_focused(self):
        """Hyprland's own idea of the focused window is this one."""
        try:
            active = json.loads(self._hyprctl("-j", "activewindow") or "{}")
        except ValueError:
            return False
        return active.get("pid") == os.getpid() and active.get("title") == self.title

    def _hypr_activate(self):
        # Lua config (Hyprland >= 0.55); older versions take the plain dispatcher
        out = self._hyprctl("dispatch", f'hl.dsp.focus({{ window = "pid:{os.getpid()}" }})')
        if out.startswith("error"):
            self._hyprctl("dispatch", "focuswindow", f"pid:{os.getpid()}")

    def has_focus(self):
        self.pump()
        if not (self.win.is_active() and self.view.has_focus()):
            return False
        if self.hyprland:
            return self._hypr_focused()
        active = self._xdotool("getactivewindow")
        return bool(active) and self.window_id is not None and int(active[0]) == self.window_id

    def focus(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.win.present()
            if self.hyprland:
                self._hypr_activate()
            elif self.window_id is not None:
                self._xdotool("windowactivate", str(self.window_id))
            self.view.grab_focus()
            self.pump(0.1)
            if self.has_focus():
                return
        raise E2EError("couldn't give the capture window the keyboard focus, "
                       "refusing to type into another window")

    def ensure_focus(self):
        """Called before every key press: never type into another window."""
        if not self.has_focus():
            self.focus(timeout=1.0)

    def status(self, text):
        self.label.set_text(f"{text}  -  don't touch the keyboard")
        self.pump()

    def clear(self):
        self.pump(0.05)  # flush anything still in flight
        self.view.get_buffer().set_text("")
        self.key_events.clear()

    def typed(self):
        self.pump()
        buf = self.view.get_buffer()
        return buf.get_text(buf.get_start_iter(), buf.get_end_iter(), True)

    def wait_for_text(self, expected, timeout=2.0, settle=0.15):
        """Wait until the text equals `expected` (and stays so for `settle` s,
        to catch trailing extra characters). Returns what was typed."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.typed() == expected:
                self.pump(settle)
                return self.typed()
            self.pump(0.02)
        return self.typed()

    def close(self):
        try:
            self.win.destroy()
            self.pump(0.05)
        except Exception:
            pass


# ---- shared session + unittest base class ----------------------------------------

class Session:
    """One keyboard connection and one capture window for the whole run."""
    _instance = None
    _error = None

    @classmethod
    def get(cls):
        if cls._error:  # don't retry (and open windows) for every test
            raise cls._error
        if cls._instance is None:
            try:
                cls._instance = cls()
            except Exception as e:
                cls._error = e if isinstance(e, E2EError) else E2EError(f"setup failed: {e!r}")
                raise cls._error
        return cls._instance

    def __init__(self):
        self.km = keymap.load()
        self.capture = Capture()
        try:
            self._connect()
        except Exception:
            self.capture.close()
            raise

    def _connect(self):
        self.kb = Keyboard(None, self.km, before_input=self.capture.ensure_focus)
        self.monitor = KeyMonitor(self.kb.on_message)
        self.kb.monitor = self.monitor
        if not self.monitor.connect():
            raise E2EError("no key-monitor port found: flash the test firmware (bin/flash.sh --test)")
        if "inject=1" not in (self.monitor.hello or ""):
            raise E2EError(f"firmware doesn't allow injection: {self.monitor.hello}")
        self.monitor.start()
        # Optional: right-half keys are then pressed on the right half.
        # ADV360_E2E_LEFT_ONLY (bin/e2e --left-only): inject them on the left
        # half too, i.e. test the keymap without the split link (a noisy
        # radio channel makes right-half keys arrive late or not at all).
        self.right = None
        if os.environ.get("ADV360_E2E_LEFT_ONLY"):
            print("e2e: --left-only: right-half keys are injected on the left half, "
                  "the split link is not tested", file=sys.stderr)
            return
        right = KeyMonitor(self.kb.on_right_message, product=RIGHT_PRODUCT)
        if right.connect() and "inject=1" in (right.hello or ""):
            self.right = self.kb.right = right.start()
        else:
            right.close()
            print("e2e: the right half has no key monitor (old test firmware?), "
                  "its keys are injected on the left half", file=sys.stderr)

    @classmethod
    def close(cls):
        s = cls._instance
        if s:
            try:
                s.kb.release_all()
            finally:
                s.monitor.stop()
                if s.right is not None:
                    s.right.stop()
                s.capture.close()
            cls._instance = None


class E2ETestCase(unittest.TestCase):
    """Base class: self.kb (Keyboard), self.capture (Capture), assertTyped()."""

    def setUp(self):
        s = Session.get()
        self.kb, self.capture = s.kb, s.capture
        self.kb.wait_idle()
        self.capture.clear()
        self.kb.log.clear()
        self.kb.left_times.clear()
        self.kb.right_times.clear()
        self.capture.status(f"Running {self.id().split('.', 2)[-1]}")
        self.capture.ensure_focus()

    # Tapped after every test to end "until the next key" state such as caps
    # word, which a later test would otherwise toggle off instead of on.
    # Esc does nothing in the capture window.
    RESET_KEY = ("Esc", "Base")

    def tearDown(self):
        kb = self.kb
        kb.release_all()
        try:
            kb.wait_idle()  # let sticky layers time out
            kb.tap(kb.key(*self.RESET_KEY))
        except E2EError:
            pass

    def assertTyped(self, expected, timeout=2.0, msg=None):
        got = self.capture.wait_for_text(expected, timeout)
        if got != expected:
            events = " ".join(f"{d}:{k}" for d, k, _, _ in self.capture.key_events
                              if d == "down")
            t0 = self.kb.log[0][0] if self.kb.log else 0
            device = " ".join(f"[{(t - t0) * 1000:.0f}ms {line}]" for t, line in self.kb.log)
            self.fail(f"typed {got!r}, expected {expected!r}"
                      + (f" ({msg})" if msg else "")
                      + f"\n  key events: {events}\n  keyboard: {device}")
