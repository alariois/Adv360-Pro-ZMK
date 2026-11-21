"""Keys typed across the two halves keep their order.

The right half's key presses reach the left half (which runs the keymap)
over Bluetooth, a few ms to ~20 ms late. Keys pressed on the left half are
seen at once, so a left key pressed just after a right key used to win the
race. That matters most for Repeat: "s" (right) then Repeat (left) quickly
repeated the key *before* the s. zmk-modules/split-timing fixes it: the
right half timestamps its keys, the left half processes all keys in press
order, a fixed delay after each press.

These tests press right-half keys on the right half (its own key-monitor
port), so they travel over the real split link."""

import time

from tests.e2e.harness import E2ETestCase


def script(*keys, hold=0.05):
    """[(key, seconds after the previous key's press)] -> kb.play() steps;
    each key is held `hold` seconds (overlapping keys = rolls)."""
    events, t = [], 0.0
    for pos, gap in keys:
        t += gap
        events += [(t, pos, 1), (t + hold, pos, 0)]
    events.sort(key=lambda e: e[0])
    steps, prev = [], 0.0
    for at, pos, pressed in events:
        steps.append((at - prev, pos, pressed))
        prev = at
    return steps


class SplitTestCase(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        if kb.right is None:
            self.skipTest("the right half has no key monitor (flash the test firmware)")
        repeats = [i for i, k in enumerate(kb.layer("Base")["keys"]) if k["label"] == "Repeat"]
        self.left_repeat = next(p for p in repeats if p not in kb.right_keys)
        self.right_repeat = next(p for p in repeats if p in kb.right_keys)
        chars = kb.char_map()
        self.key = {ch: chars[ch][1] for ch in "sheoq"}
        for ch in "sh":
            assert self.key[ch] in kb.right_keys, f"{ch} should be on the right half"
        for ch in "eoq":
            assert self.key[ch] not in kb.right_keys, f"{ch} should be on the left half"

    def trials(self, cases, runs=6):
        """cases: [(name, steps, expected)]. Types "q" first so that a
        wrong repeat shows up as "qq..."; collects failures, asserts once."""
        kb = self.kb
        failures = []
        for name, steps, expected in cases:
            got = {}
            for _ in range(runs):
                self.capture.clear()
                kb.type("q")
                kb.play(steps)
                text = self.capture.wait_for_text("q" + expected, timeout=0.6)[1:]
                got[text] = got.get(text, 0) + 1
                kb.wait_idle()
            if set(got) != {expected}:
                failures.append(f"{name}: want {expected!r}, got {got}")
        self.assertFalse(failures, "\n  " + "\n  ".join(failures))


class TestRepeatAcrossHalves(SplitTestCase):

    # Gaps start at 3 ms: the two halves get their injected presses over
    # separate USB connections, about a millisecond apart either way.
    GAPS = (3, 5, 10, 20, 30)

    def test_repeat_on_left_right_after_right_key(self):
        """s (right) + Repeat (left) pressed 3-30 ms apart, then h: ssh."""
        k = self.key
        self.trials([
            (f"s, {gap}ms, left Repeat, 80ms, h",
             script((k["s"], 0), (self.left_repeat, gap / 1000), (k["h"], 0.08)), "ssh")
            for gap in self.GAPS])

    def test_key_right_after_left_repeat_is_not_repeated(self):
        """Repeat (left) then a right key soon after: the right key must not
        be taken for the key Repeat repeats."""
        k = self.key
        self.trials([
            (f"s, 100ms, left Repeat, {gap}ms, h",
             script((k["s"], 0), (self.left_repeat, 0.1), (k["h"], gap / 1000), hold=0.03),
             "ssh")
            for gap in self.GAPS])

    def test_repeat_on_right_after_left_key(self):
        """e (left) + Repeat (right) 3-30 ms apart, then o: eeo."""
        k = self.key
        self.trials([
            (f"e, {gap}ms, right Repeat, 80ms, o",
             script((k["e"], 0), (self.right_repeat, gap / 1000), (k["o"], 0.08)), "eeo")
            for gap in self.GAPS])

    def test_repeat_on_same_half(self):
        k = self.key
        self.trials([
            (f"s, {gap}ms, right Repeat, 80ms, h",
             script((k["s"], 0), (self.right_repeat, gap / 1000), (k["h"], 0.08)), "ssh")
            for gap in (0, 10, 30)] + [
            (f"e, {gap}ms, left Repeat, 80ms, o",
             script((k["e"], 0), (self.left_repeat, gap / 1000), (k["o"], 0.08)), "eeo")
            for gap in (0, 10, 30)])


class TestLettersAcrossHalves(SplitTestCase):

    def test_left_letter_right_after_right_letter(self):
        """s (right) then e (left) a few ms later: se, not es."""
        k = self.key
        self.trials([
            (f"s, {gap}ms, e", script((k["s"], 0), (k["e"], gap / 1000)), "se")
            for gap in self.GAPS[:3]] + [
            (f"e, {gap}ms, s", script((k["e"], 0), (k["s"], gap / 1000)), "es")
            for gap in self.GAPS[:3]])

    GAPS = TestRepeatAcrossHalves.GAPS


class TestSplitLatency(SplitTestCase):

    def test_right_keys_processed_in_time(self):
        """Right-half keys reach processing within the press-order cap
        (CONFIG_ZMK_SPLIT_TIMING_DELAY_MS, 20 ms) plus a little: the
        timestamp of every one of them arrived and was used (keys without
        a timestamp would wait the cap after their arrival instead)."""
        kb = self.kb
        shift = kb.key("Shift", sub="RShift")  # types nothing
        kb.monitor.send("S")  # reset split-timing's statistics
        time.sleep(0.2)
        for _ in range(60):
            kb.play([(0, shift, 1), (0.015, shift, 0)])
        before = len(kb.log)
        kb.monitor.send("S")
        deadline = time.monotonic() + 1
        line = ""
        while not line and time.monotonic() < deadline:
            line = next((l for _, l in kb.log[before:] if l.startswith("S ")), "")
            time.sleep(0.02)
        self.assertTrue(line, "no statistics from split-timing")
        remote = dict(kv.split("=") for kv in line.split("remote", 1)[1].split()
                      if "=" in kv)
        print(f"\n  split-timing: {line[2:]}")
        self.assertEqual(int(remote["n"]), 120, line)
        self.assertLess(int(remote["max"]), 22000, line)
