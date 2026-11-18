"""Caps word (both Shift keys) keeps going through hexpad input, so MAC
addresses can be typed in uppercase without holding Shift."""

from tests.e2e.harness import E2ETestCase


class TestCapsWordHexpad(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.hexpad = kb.key("Hexpad", sub="sticky")
        self.lshift = kb.key("Shift", sub="LShift")
        self.rshift = kb.key("Shift", sub="RShift")
        # Holding one Shift turns the other Shift key into caps word
        self.assertEqual(kb.binding(self.rshift, layer="LShift"), "&caps_word")
        self.assertEqual(kb.binding(self.lshift, layer="RShift"), "&caps_word")

    def caps_word(self, first=None, second=None):
        """Hold one Shift, tap the other: caps word on."""
        first, second = first or self.lshift, second or self.rshift
        with self.kb.hold(first):
            self.kb.tap(second)

    def type_hexpad(self, text):
        for char in text:
            self.kb.tap(self.hexpad, self.kb.key(char.upper(), layer="Hexpad"))

    def test_mac_address_letters(self):
        self.caps_word()
        self.type_hexpad("aa:bb:cc:dd:ee:ff")
        self.assertTyped("AA:BB:CC:DD:EE:FF")

    def test_mac_address_mixed(self):
        self.caps_word(self.rshift, self.lshift)
        self.type_hexpad("01:23:45:67:89:ab")
        self.assertTyped("01:23:45:67:89:AB")

    def test_caps_word_ends_at_space(self):
        """Caps word still ends normally, here on Space from the base layer."""
        kb = self.kb
        self.caps_word()
        self.type_hexpad("ab:cd")
        kb.tap(kb.key("Space"))
        self.type_hexpad("ef")
        self.assertTyped("AB:CD ef")


class TestCapsWordDash(E2ETestCase):
    """Caps word continues through '-' (CONSTANT-NAMES, UUID-LIKE-IDS)."""

    def caps_word(self):
        kb = self.kb
        with kb.hold(kb.key("Shift", sub="LShift")):
            kb.tap(kb.key("Shift", sub="RShift"))

    def test_dash_keeps_caps_word(self):
        self.caps_word()
        self.kb.type("foo-bar")
        self.assertTyped("FOO-BAR")

    def test_dash_and_underscore_mix(self):
        self.caps_word()
        self.kb.type("a-b_c")
        self.assertTyped("A-B_C")

    def test_space_still_ends_caps_word(self):
        self.caps_word()
        self.kb.type("ab-cd ef")
        self.assertTyped("AB-CD ef")
