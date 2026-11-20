"""The bottom-row corner keys (far left and far right) are Super, like the
thumb Super keys: Super held + QWERTY layer, so shortcuts use QWERTY
positions.

Only the bindings are checked: pressing Super makes GNOME open the overview
(or run a Super shortcut), which steals the capture window's focus."""

from tests.e2e.harness import E2ETestCase
from adv360 import keymap  # tools/ is on sys.path (harness)

QWERTY = 1


class TestSuperCorners(E2ETestCase):

    def corners(self):
        """Far left and far right key of the bottom (lowest unrotated) row."""
        layout = keymap.load()["layout"]
        flat = [i for i, k in enumerate(layout) if k["rot"] == 0]
        bottom = max(layout[i]["y"] for i in flat)
        row = sorted((layout[i]["x"], i) for i in flat if layout[i]["y"] == bottom)
        return {"left": row[0][1], "right": row[-1][1]}

    def test_corners_are_super(self):
        kb = self.kb
        thumb_super = kb.binding(kb.key("Super", sub="QWERTY"))
        for side, pos in self.corners().items():
            with self.subTest(side=side):
                self.assertEqual(kb.layer("Base")["keys"][pos]["label"], "Super")
                self.assertEqual(kb.binding(pos), thumb_super)
