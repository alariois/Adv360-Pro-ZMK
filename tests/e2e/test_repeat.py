"""Repeat key: replays exactly what the last key typed - whole macros
(compose letters, unicode entry), with the modifiers as they were - and
works while the previous key is still held. Space is skipped."""

from tests.e2e.harness import E2ETestCase

NOT_TEXT = set("←→↑↓⌫")  # keys with one-character labels that don't type text


class RepeatTestCase(E2ETestCase):

    def setUp(self):
        super().setUp()
        kb = self.kb
        self.repeat = kb.key("Repeat")
        self.lshift = kb.key("Shift", sub="LShift")
        self.hexpad = kb.key("Hexpad", sub="sticky")

    def unicode(self, hex_digits):
        """Ctrl+Shift+U <hex> with the Hexpad held; releasing it sends Enter."""
        kb = self.kb
        with kb.hold(self.hexpad):
            kb.tap(kb.key("Ctrl+Shift+U", layer="Hexpad"))
            for ch in hex_digits:
                kb.tap(kb.key(ch.upper(), layer="Hexpad"))


class TestRepeatEveryKey(RepeatTestCase):
    """Every character the keymap types with one key repeats as itself."""

    def test_every_character(self):
        kb = self.kb
        failures = []
        for ch, (how, pos) in sorted(kb.char_map().items()):
            if ch in NOT_TEXT or ch == " ":
                continue
            self.capture.clear()
            kb.type(ch)
            kb.tap(self.repeat)
            got = self.capture.wait_for_text(ch * 2, timeout=1)
            if got != ch * 2:
                failures.append(f"{how} {ch!r} + repeat gave {got!r}")
            kb.wait_idle()
        self.assertFalse(failures, "\n  " + "\n  ".join(failures))

    def test_every_repeat_key(self):
        kb = self.kb
        keys = [i for i, k in enumerate(kb.layer("Base")["keys"]) if k["label"] == "Repeat"]
        self.assertGreaterEqual(len(keys), 2)
        for pos in keys:
            kb.type("x")
            kb.tap(pos)
        self.assertTyped("xx" * len(keys))


class TestRepeatMacros(RepeatTestCase):

    def test_compose_letters(self):
        for ch in "õäöüšž":
            self.kb.type(ch)
            self.kb.tap(self.repeat)
        self.assertTyped("õõääööüüššžž")

    def test_unicode_entry(self):
        self.unicode("1f680")
        self.kb.tap(self.repeat)
        self.assertTyped("🚀🚀")

    def test_repeat_twice(self):
        self.kb.type("õ")
        self.kb.tap(self.repeat, self.repeat)
        self.assertTyped("õõõ")


class TestRepeatModifiers(RepeatTestCase):

    def test_shifted_letter_repeats_shifted(self):
        self.kb.type("A")
        self.kb.tap(self.repeat)
        self.assertTyped("AA")

    def test_shifted_symbol_repeats_without_shift(self):
        self.kb.type(">")
        self.kb.tap(self.repeat)
        self.assertTyped(">>")

    def test_unshifted_equal_with_shift_still_held(self):
        """FNR== : '=' typed with Shift held, repeat while still holding it."""
        kb = self.kb
        with kb.hold(self.lshift):
            for ch in "FNR=":
                kb.tap(kb.char_map()[ch][1])
            kb.tap(self.repeat)
        self.assertTyped("FNR==")

    def test_grave_with_shift_held(self):
        kb = self.kb
        with kb.hold(self.lshift):
            kb.tap(kb.char_map()["`"][1])
            kb.tap(self.repeat)
        kb.tap(self.repeat)
        self.assertTyped("```")

    def test_replay_is_exact_even_if_shift_is_held(self):
        kb = self.kb
        kb.type("a")
        with kb.hold(self.lshift):
            kb.tap(self.repeat)
        self.assertTyped("aa")

    def test_modifier_and_layer_taps_dont_replace_last_key(self):
        kb = self.kb
        kb.type("q")
        kb.tap(self.lshift)
        kb.tap(kb.key("Ctrl", sub="QWERTY"))
        kb.tap(self.repeat)
        self.assertTyped("qq")


class TestRepeatSpace(RepeatTestCase):

    def test_space_is_skipped(self):
        self.kb.type("> ")
        self.kb.tap(self.repeat)
        self.kb.type("(")
        self.assertTyped("> >(")

    def test_several_spaces(self):
        self.kb.type("a  ")
        self.kb.tap(self.repeat)
        self.assertTyped("a  a")


class TestRepeatWhileHeld(RepeatTestCase):
    """Repeat pressed before the previous key is released."""

    def held(self, ch, order):
        """Hold the key for `ch`, press/release repeat and the key in `order`."""
        kb = self.kb
        how, pos = kb.char_map()[ch]
        shift = how == "shift"
        if shift:
            kb.press(self.lshift)
        if how == "hexpad":
            kb.tap(self.hexpad)
        kb.press(pos)
        for step in order:
            {"R+": lambda: kb.press(self.repeat), "R-": lambda: kb.release(self.repeat),
             "K-": lambda: kb.release(pos)}[step]()
        if shift:
            kb.release(self.lshift)
        self.assertTyped(ch * 2, msg=f"order {order}")

    def test_letter_release_repeat_first(self):
        self.held("l", ["R+", "R-", "K-"])

    def test_letter_release_key_first(self):
        self.held("l", ["R+", "K-", "R-"])

    def test_shifted_letter(self):
        self.held("L", ["R+", "R-", "K-"])

    def test_symbol(self):
        self.held(",", ["R+", "K-", "R-"])

    def test_shifted_symbol(self):
        self.held(">", ["R+", "R-", "K-"])

    def test_grave(self):
        self.held("`", ["R+", "R-", "K-"])

    def test_hexpad_digit(self):
        self.held("7", ["R+", "R-", "K-"])


class TestRepeatCapsWordAndLayers(RepeatTestCase):

    def caps_word(self):
        kb = self.kb
        with kb.hold(self.lshift):
            kb.tap(kb.key("Shift", sub="RShift"))

    def test_repeat_during_caps_word(self):
        self.caps_word()
        self.kb.type("ab")
        self.kb.tap(self.repeat)
        self.assertTyped("ABB")

    def test_repeat_after_caps_word_ended(self):
        """The letter was typed uppercase, so it repeats uppercase."""
        self.caps_word()
        self.kb.type("ab ")
        self.kb.tap(self.repeat)
        self.assertTyped("AB B")

    def test_fn_layer_degree(self):
        kb = self.kb
        kb.tap(kb.key("Fn", sub="sticky"), kb.key("°", layer="Fn"))
        kb.tap(self.repeat)
        self.assertTyped("°°")
