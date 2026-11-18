"""Fast typing: a symbol followed quickly by a letter must come out in
order and with the right case, both for taps and for rolls (pressing the
letter before releasing the symbol key), with or without Shift held."""

import time

from tests.e2e.harness import E2ETestCase


class TestFastTyping(E2ETestCase):

    def symbols(self, how):
        """Single-key printable ASCII symbols typed `how` ("plain"/"shift")."""
        return sorted((ch, pos) for ch, (h, pos) in self.kb.char_map().items()
                      if h == how and not ch.isalnum() and ch.isascii()
                      and ch.isprintable() and ch != " ")

    def check_all(self, how, play):
        """play(symbol_pos, letter_pos) for every symbol; collect failures."""
        kb = self.kb
        lshift, letter = kb.key("Shift", sub="LShift"), kb.key("L")
        failures = []
        for ch, pos in self.symbols(how):
            self.capture.clear()
            if how == "shift":
                with kb.hold(lshift):
                    play(pos, letter)
                expected = ch + "L"
            else:
                play(pos, letter)
                expected = ch + "l"
            got = self.capture.wait_for_text(expected, timeout=1)
            if got != expected:
                failures.append(f"{expected!r} came out as {got!r}")
            kb.wait_idle()
        self.assertFalse(failures, "\n  " + "\n  ".join(failures))

    def roll(self, gap):
        def play(a, b):
            kb = self.kb
            with kb.fast():
                kb.press(a)
                time.sleep(gap)
                kb.press(b)
                kb.release(a)
                kb.release(b)
        return play

    def taps(self, gap):
        def play(a, b):
            with self.kb.fast(gap):
                self.kb.tap(a, b, hold=gap)
        return play

    def test_shifted_symbol_rolled_into_letter_10ms(self):
        self.check_all("shift", self.roll(0.01))

    def test_shifted_symbol_rolled_into_letter_60ms(self):
        self.check_all("shift", self.roll(0.06))

    def test_shifted_symbol_tapped_fast(self):
        self.check_all("shift", self.taps(0.0))

    def test_plain_symbol_rolled_into_letter(self):
        self.check_all("plain", self.roll(0.02))

    def test_plain_symbol_tapped_fast(self):
        self.check_all("plain", self.taps(0.0))

    def test_grave_then_L(self):
        """The reported case: ` then L, typed fast with Shift held."""
        kb = self.kb
        grave = kb.char_map()["`"][1]
        with kb.hold(kb.key("Shift", sub="LShift")):
            self.roll(0.03)(grave, kb.key("L"))
        self.assertTyped("`L")
