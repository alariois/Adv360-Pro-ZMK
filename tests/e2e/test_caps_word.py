"""Caps word (both Shift keys, on the Base layer or the Hexpad): keeps
going through hexpad input (MAC addresses), '-' and compose letters
(Ö Š ...), and lights the caps lock LED while on."""

from tests.e2e.harness import E2ETestCase


class TestCapsWordHexpad(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.hexpad = kb.key("Hexpad", sub="sticky")
        self.lshift = kb.key("Shift", sub="LShift")
        self.rshift = kb.key("Shift", sub="RShift")
        # Holding one Shift turns the other Shift key into caps word
        self.assertEqual(kb.binding(self.rshift, layer="LShift"), "&caps_wd")
        self.assertEqual(kb.binding(self.lshift, layer="RShift"), "&caps_wd")

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


class CapsWordTestCase(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.lshift = kb.key("Shift", sub="LShift")
        self.rshift = kb.key("Shift", sub="RShift")
        self.hexpad = kb.key("Hexpad", sub="sticky")

    def caps_word(self):
        with self.kb.hold(self.lshift):
            self.kb.tap(self.rshift)

    def assertLed(self, on):
        lit = bool(self.kb.indicators() & self.kb.CAPS_LOCK_LED)
        self.assertEqual(lit, on, f"caps lock LED should be {'on' if on else 'off'}")


class TestCapsWordComposeLetters(CapsWordTestCase):

    def test_compose_letters_uppercase(self):
        self.caps_word()
        self.kb.type("õäöüšž")
        self.assertTyped("ÕÄÖÜŠŽ")

    def test_compose_letters_keep_caps_word(self):
        self.caps_word()
        self.kb.type("söö-öö x")
        self.assertTyped("SÖÖ-ÖÖ x")

    def test_degree_is_not_a_letter(self):
        """° is ScrollLock o o: nothing to uppercase, caps word continues."""
        kb = self.kb
        self.caps_word()
        kb.type("a")
        kb.tap(kb.key("Fn", sub="sticky"), kb.key("°", layer="Fn"))
        kb.type("b")
        self.assertTyped("A°B")


class TestCapsWordOnHexpad(CapsWordTestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.hex_lshift = kb.key("Shift", layer="Hexpad", sub="CapsWd", nth=0)
        self.hex_rshift = kb.key("Shift", layer="Hexpad", sub="CapsWd", nth=1)
        self.assertEqual(kb.binding(self.hex_lshift, layer="Hexpad"), "&hex_lshift")

    def hexpad_caps_word(self, first, second):
        kb = self.kb
        with kb.hold(self.hexpad):
            with kb.hold(first):
                kb.tap(second)
            for ch in "ab:12":  # Shift released: only caps word uppercases
                kb.tap(kb.key(ch.upper(), layer="Hexpad"))

    def test_left_then_right(self):
        self.hexpad_caps_word(self.hex_lshift, self.hex_rshift)
        self.assertTyped("AB:12")

    def test_right_then_left(self):
        self.hexpad_caps_word(self.hex_rshift, self.hex_lshift)
        self.assertTyped("AB:12")

    def test_continues_after_hexpad_released(self):
        self.hexpad_caps_word(self.hex_lshift, self.hex_rshift)
        self.kb.type("x-y")
        self.assertTyped("AB:12X-Y")

    def test_hexpad_shift_still_shifts(self):
        kb = self.kb
        with kb.hold(self.hexpad, self.hex_lshift):
            kb.tap(kb.key("-", layer="Hexpad"))
        with kb.hold(self.hexpad, self.hex_rshift):
            kb.tap(kb.key("+", layer="Hexpad"))
        self.assertTyped("_+")


class TestCapsWordLed(CapsWordTestCase):

    def test_led_follows_caps_word(self):
        self.assertLed(False)
        self.caps_word()
        self.assertLed(True)
        self.kb.type("ab ")
        self.assertLed(False)
        self.assertTyped("AB ")

    def test_led_off_when_toggled_off(self):
        self.caps_word()
        self.assertLed(True)
        self.caps_word()
        self.assertLed(False)

    def test_led_on_from_hexpad(self):
        kb = self.kb
        with kb.hold(self.hexpad, kb.key("Shift", layer="Hexpad", sub="CapsWd", nth=0)):
            kb.tap(kb.key("Shift", layer="Hexpad", sub="CapsWd", nth=1))
        self.assertLed(True)
