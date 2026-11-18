"""End-to-end test harness: drive the real keyboard, check what the OS types.

Needs the TEST firmware (bin/flash.sh --test) and an X11 desktop.

  * Keyboard  - injects key presses through the key-monitor port. Keys are
                looked up by their label in config/adv360.keymap, so tests
                survive keys being moved around.
  * Capture   - a Tk text window that is forced to the front and must hold
                the keyboard focus before anything is pressed; it records the
                resulting text and every raw key event.
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
from adv360.device import KeyMonitor  # noqa: E402


class E2EError(AssertionError):
    pass


# ---- the keyboard ---------------------------------------------------------

class Keyboard:
    """Presses keys on the real keyboard and waits for it to acknowledge each
    one, so tests are deterministic instead of sleep-based."""

    def __init__(self, monitor, km, gap=0.015, before_input=None):
        self.monitor = monitor
        self.layers = km["layers"]
        self.gap = gap  # pause after each press/release, lets the host see separate reports
        self.before_input = before_input or (lambda: None)
        self.events = queue.Queue()
        self.layer_mask = 0
        self.down = set()

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

    def binding(self, pos, layer="Base"):
        return self.layer(layer)["keys"][pos]["binding"]

    # -- device events ---------------------------------------------------------

    def on_message(self, line):
        if line.startswith("L "):
            self.layer_mask = int(line.split()[1], 16)
        self.events.put(line)

    def _wait_for(self, line, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                got = self.events.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                break
            if got == line:
                return
            if got.startswith("E ") or got == "disconnected":
                raise E2EError(f"keyboard said {got!r} while waiting for {line!r}")
        raise E2EError(f"keyboard didn't acknowledge {line!r} within {timeout}s")

    # -- pressing keys ---------------------------------------------------------

    def press(self, pos):
        self.before_input()
        self.monitor.press(pos, True)
        self._wait_for(f"P {pos} 1")
        self.down.add(pos)
        time.sleep(self.gap)

    def release(self, pos):
        self.monitor.press(pos, False)
        self._wait_for(f"P {pos} 0")
        self.down.discard(pos)
        time.sleep(self.gap)

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

    def release_all(self):
        for pos in list(self.down):
            try:
                self.release(pos)
            except E2EError:
                self.monitor.press(pos, False)
        self.down.clear()

    def wait_idle(self, timeout=3.0):
        """Wait until only the base layer is active (sticky layers time out)."""
        deadline = time.monotonic() + timeout
        while self.layer_mask and time.monotonic() < deadline:
            time.sleep(0.05)
        if self.layer_mask:
            raise E2EError(f"layers still active after {timeout}s: mask {self.layer_mask:#x}")


# ---- the focused text view ---------------------------------------------------

class Capture:
    """A Tk window with a text box that owns the keyboard focus. Everything the
    keyboard types lands here; key events are recorded too."""

    TITLE = "Adv360 e2e capture - don't touch the keyboard"

    def __init__(self):
        import tkinter as tk

        if not os.environ.get("DISPLAY"):
            raise E2EError("DISPLAY is not set: run the e2e tests from your desktop session")
        self.tk = tk
        self.root = tk.Tk()
        self.title = f"{self.TITLE} [{os.getpid()}]"
        self.root.title(self.title)
        self._window_id = None
        self.root.geometry("760x300+80+80")
        self.root.attributes("-topmost", True)
        self.label = tk.Label(self.root, text="", anchor="w", font=("TkDefaultFont", 11))
        self.label.pack(fill="x", padx=8, pady=(8, 0))
        self.text = tk.Text(self.root, font=("TkFixedFont", 14), wrap="char")
        self.text.pack(fill="both", expand=True, padx=8, pady=8)
        self.key_events = []
        for seq in ("<KeyPress>", "<KeyRelease>"):
            self.text.bind(seq, self._record, add="+")
        self.root.update()
        self.focus()

    def _record(self, e):
        self.key_events.append((e.type == self.tk.EventType.KeyPress and "down" or "up",
                                e.keysym, e.char, e.state))

    def pump(self, seconds=0.0):
        end = time.monotonic() + seconds
        while True:
            self.root.update()
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
        """The X window the window manager knows (not Tk's internal ids)."""
        if self._window_id is None:
            ids = self._xdotool("search", "--name", "^" + re.escape(self.title) + "$")
            self._window_id = int(ids[-1]) if ids else None
        return self._window_id

    def has_focus(self):
        self.root.update()
        if self.root.focus_get() is not self.text:
            return False
        active = self._xdotool("getactivewindow")
        return bool(active) and self.window_id is not None and int(active[0]) == self.window_id

    def focus(self, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.root.deiconify()
            self.root.lift()
            if self.window_id is not None:
                self._xdotool("windowactivate", str(self.window_id))
            self.text.focus_force()
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
        self.label.config(text=text)
        self.pump()

    def clear(self):
        self.pump(0.05)  # flush anything still in flight
        self.text.delete("1.0", "end")
        self.key_events.clear()

    def typed(self):
        self.pump()
        return self.text.get("1.0", "end-1c")

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
            self.root.destroy()
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

    @classmethod
    def close(cls):
        s = cls._instance
        if s:
            try:
                s.kb.release_all()
            finally:
                s.monitor.stop()
                s.capture.close()
            cls._instance = None


class E2ETestCase(unittest.TestCase):
    """Base class: self.kb (Keyboard), self.capture (Capture), assertTyped()."""

    def setUp(self):
        s = Session.get()
        self.kb, self.capture = s.kb, s.capture
        self.kb.wait_idle()
        self.capture.clear()
        self.capture.status(f"Running {self.id().split('.', 2)[-1]}")
        self.capture.ensure_focus()

    def tearDown(self):
        self.kb.release_all()
        try:
            self.kb.wait_idle()  # let sticky layers time out before the next test
        except E2EError:
            pass

    def assertTyped(self, expected, timeout=2.0, msg=None):
        got = self.capture.wait_for_text(expected, timeout)
        if got != expected:
            events = " ".join(f"{d}:{k}" for d, k, _, _ in self.capture.key_events
                              if d == "down")
            self.fail(f"typed {got!r}, expected {expected!r}"
                      + (f" ({msg})" if msg else "") + f"\n  key events: {events}")
