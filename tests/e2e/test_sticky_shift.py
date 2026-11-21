"""Sticky Shift: a Shift key tapped alone shifts the next key (within 1 s),
with its LShift/RShift layer; held, it is plain Shift; with Ctrl, Alt or
Super held it is never sticky. Tapped twice = caps word (the Shift layer's
Shift keys). zmk-modules/smart-keys sticky_shift.c."""

import time

from tests.e2e.harness import E2ETestCase


class TestStickyShift(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.lshift = kb.key("Shift", sub="LShift")
        self.rshift = kb.key("Shift", sub="RShift")
        self.ctrl = kb.key("Ctrl", sub="QWERTY")
        chars = kb.char_map()
        self.k = {ch: chars[ch][1] for ch in "abeh:b"}

    def test_tap_shift_shifts_next_key_only(self):
        kb = self.kb
        for shift in (self.lshift, self.rshift):
            kb.tap(shift)
            kb.tap(self.k["a"], self.k["b"])
            kb.tap(self.k["a"])
        self.assertTyped("AbaAba")

    def test_sticky_shift_uses_the_shift_layer(self):
        """':' shifted is '?' (LShift/RShift layer), sticky too."""
        kb = self.kb
        kb.tap(self.lshift)
        kb.tap(self.k[":"])
        kb.tap(self.rshift)
        kb.tap(self.k[":"])
        kb.tap(self.k[":"])
        self.assertTyped("??:")

    def test_sticky_shift_times_out_after_a_second(self):
        kb = self.kb
        kb.tap(self.lshift)
        time.sleep(1.2)
        kb.tap(self.k["a"])
        self.assertTyped("a")

    def test_roll_after_sticky_shift(self):
        """Shift tapped, then h and e rolled: only the h is shifted."""
        kb = self.kb
        kb.tap(self.lshift)
        with kb.fast():
            kb.press(self.k["h"])
            kb.press(self.k["e"])
            kb.release(self.k["h"])
            kb.release(self.k["e"])
        self.assertTyped("He")

    def test_held_shift_is_plain_shift(self):
        kb = self.kb
        with kb.hold(self.lshift):
            kb.tap(self.k["a"], self.k["b"])
        kb.tap(self.k["a"])
        self.assertTyped("ABa")

    def test_not_sticky_with_ctrl_held(self):
        kb = self.kb
        with kb.hold(self.ctrl):
            kb.tap(self.lshift)
        kb.tap(self.k["a"])
        self.assertTyped("a")

    def test_shift_released_before_next_key_still_shifts_once(self):
        """Press Shift, release it, then type: one capital, then normal."""
        kb = self.kb
        kb.press(self.lshift)
        kb.release(self.lshift)
        kb.tap(self.k["h"], self.k["e"])
        self.assertTyped("He")

    def test_caps_word_gesture_leaves_no_sticky_shift(self):
        """Hold one Shift, tap the other (caps word, types nothing): the held
        Shift is a plain hold, no sticky Shift + layer afterwards."""
        kb = self.kb
        with kb.hold(self.lshift):
            kb.tap(self.rshift)
        self.assertFalse(kb.layer_mask & (1 << 6), "LShift layer still on")
        kb.type("ab")
        self.assertTyped("AB")  # caps word, not a sticky Shift

    def test_tap_twice_is_caps_word(self):
        """While the sticky Shift is on, its Shift layer is too, where both
        Shift keys are caps word: Shift tapped twice = caps word."""
        kb = self.kb
        for first, second in ((self.lshift, self.lshift), (self.lshift, self.rshift)):
            kb.tap(first)
            kb.tap(second)
            kb.type("ab b")
        self.assertTyped("AB bAB b")

    def test_sticky_shift_through_hexpad(self):
        """The Hexpad key types nothing: the sticky Shift is still there for
        the compose letter after it (whole macro shifted)."""
        kb = self.kb
        kb.tap(self.lshift)
        kb.type("ö")
        kb.type("o")
        self.assertTyped("Öo")
