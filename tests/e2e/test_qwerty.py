"""The QWERTY layout (layout key, &to 1) stays on until the layout key is
pressed again: modifier keys must not drop back to Dvorak."""

from tests.e2e.harness import E2ETestCase

QWERTY = 1


class TestQwertyLayout(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.to_qwerty = kb.key("QWERTY", sub="to")
        self.to_base = kb.key("Base", layer="QWERTY", sub="to")
        self.qwerty_s = kb.key("S", layer="QWERTY")
        # Thumb modifiers as positions (Super is left out: tapping it alone
        # opens the GNOME overview and steals the focus).
        self.modifiers = {
            "left Ctrl": kb.key("Ctrl", sub="QWERTY", nth=0),
            "right Ctrl": kb.key("Ctrl", sub="QWERTY", nth=1),
            "left Alt": kb.key("Alt", sub="QWERTY"),
            "right Alt": kb.key("AltGr", sub="QWERTY"),
            "left Shift": kb.key("Shift", sub="LShift"),
        }

    def tearDown(self):
        if self.qwerty_active():
            self.kb.tap(self.to_base)
        super().tearDown()

    def qwerty_active(self):
        return bool(self.kb.layer_mask & (1 << QWERTY))

    def test_modifiers_keep_qwerty(self):
        kb = self.kb
        kb.tap(self.to_qwerty)
        self.assertTrue(self.qwerty_active(), "layout key didn't switch to QWERTY")
        kb.tap(self.qwerty_s)
        for name, pos in self.modifiers.items():
            kb.tap(pos)
            self.assertTrue(self.qwerty_active(), f"tapping {name} left the QWERTY layout")
            kb.tap(self.qwerty_s)
        self.assertTyped("s" * (1 + len(self.modifiers)))

    def test_layout_key_switches_back(self):
        kb = self.kb
        kb.tap(self.to_qwerty)
        kb.tap(self.qwerty_s)
        kb.tap(self.to_base)
        self.assertFalse(self.qwerty_active())
        kb.tap(self.qwerty_s)  # same key on Dvorak
        self.assertTyped("so")
