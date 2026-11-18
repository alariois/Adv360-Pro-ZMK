"""The Hexpad key: tap = one-shot layer (next key only), tap again = off,
hold = momentary; releasing it after Ctrl+Shift+U unicode entry sends the
Enter that finishes the entry."""

import time

from tests.e2e.harness import E2ETestCase

HEXPAD = 5


class TestHexpadLayer(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.hexpad = kb.key("Hexpad", sub="sticky")
        self.seven = kb.key("7", layer="Hexpad")
        self.base_char = kb.layer("Base")["keys"][self.seven]["label"].lower()
        self.unicode_key = kb.key("Ctrl+Shift+U", layer="Hexpad")

    def hexpad_active(self):
        return bool(self.kb.layer_mask & (1 << HEXPAD))

    def test_tap_is_one_shot(self):
        kb = self.kb
        kb.tap(self.hexpad, self.seven, self.seven)
        self.assertTyped("7" + self.base_char)

    def test_second_tap_turns_it_off(self):
        kb = self.kb
        kb.tap(self.hexpad, self.hexpad)
        self.assertFalse(self.hexpad_active())
        kb.tap(self.seven)
        self.assertTyped(self.base_char)

    def test_third_tap_turns_it_on_again(self):
        kb = self.kb
        kb.tap(self.hexpad, self.hexpad, self.hexpad)
        self.assertTrue(self.hexpad_active())
        kb.tap(self.seven)
        self.assertTyped("7")

    def test_times_out(self):
        kb = self.kb
        kb.tap(self.hexpad)
        time.sleep(1.3)
        self.assertFalse(self.hexpad_active())
        kb.tap(self.seven)
        self.assertTyped(self.base_char)

    def test_hold_is_momentary(self):
        kb = self.kb
        with kb.hold(self.hexpad):
            kb.tap(self.seven, self.seven)
        self.assertFalse(self.hexpad_active())
        kb.tap(self.seven)
        self.assertTyped("77" + self.base_char)

    def test_hold_without_unicode_sends_no_enter(self):
        kb = self.kb
        with kb.hold(self.hexpad):
            kb.tap(self.seven)
        self.assertTyped("7")

    def test_unicode_entry_finished_on_release(self):
        kb = self.kb
        with kb.hold(self.hexpad):
            kb.tap(self.unicode_key)
            for ch in "1F680":
                kb.tap(kb.key(ch, layer="Hexpad"))
        self.assertTyped("🚀")

    def test_unicode_entry_already_finished_gets_no_extra_enter(self):
        kb = self.kb
        with kb.hold(self.hexpad):
            kb.tap(self.unicode_key)
            for ch in "1F680":
                kb.tap(kb.key(ch, layer="Hexpad"))
            kb.tap(kb.key("Space"))  # finishes the entry
        kb.type("x")
        self.assertTyped("🚀x")

    def test_two_emoji_in_a_row(self):
        kb = self.kb
        for code in ("1F680", "1F600"):
            with kb.hold(self.hexpad):
                kb.tap(self.unicode_key)
                for ch in code:
                    kb.tap(kb.key(ch, layer="Hexpad"))
        self.assertTyped("🚀😀")
