"""Hexpad (sticky layer): digits stay digits even while Shift is held,
letters A-F follow Shift as usual."""

from tests.e2e.harness import E2ETestCase

DIGITS = "0123456789"
LETTERS = "abcdef"


class TestHexpad(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.hexpad = kb.key("Hexpad", sub="sticky")
        self.lshift = kb.key("Shift", sub="LShift")
        self.rshift = kb.key("Shift", sub="RShift")

    def hex_key(self, char):
        return self.kb.key(char.upper(), layer="Hexpad")

    def type_hex(self, chars, shift=None, shift_first=True):
        """Tap Hexpad + key for every char. With `shift`, hold that Shift key
        either around the whole sequence (shift_first) or only around the
        hex key after Hexpad was tapped."""
        kb = self.kb
        for char in chars:
            if shift is None:
                kb.tap(self.hexpad, self.hex_key(char))
            elif shift_first:
                with kb.hold(shift):
                    kb.tap(self.hexpad, self.hex_key(char))
            else:
                kb.tap(self.hexpad)
                with kb.hold(shift):
                    kb.tap(self.hex_key(char))

    # -- digits ------------------------------------------------------------

    def test_digits(self):
        self.type_hex(DIGITS)
        self.assertTyped(DIGITS)

    def test_digits_left_shift_held_first(self):
        self.type_hex(DIGITS, shift=self.lshift, shift_first=True)
        self.assertTyped(DIGITS)

    def test_digits_right_shift_held_first(self):
        self.type_hex(DIGITS, shift=self.rshift, shift_first=True)
        self.assertTyped(DIGITS)

    def test_digits_left_shift_after_hexpad(self):
        self.type_hex(DIGITS, shift=self.lshift, shift_first=False)
        self.assertTyped(DIGITS)

    def test_digits_right_shift_after_hexpad(self):
        self.type_hex(DIGITS, shift=self.rshift, shift_first=False)
        self.assertTyped(DIGITS)

    # -- letters keep normal Shift behaviour ---------------------------------

    def test_letters(self):
        self.type_hex(LETTERS)
        self.assertTyped(LETTERS)

    def test_letters_shifted(self):
        self.type_hex(LETTERS, shift=self.lshift, shift_first=True)
        self.assertTyped(LETTERS.upper())
