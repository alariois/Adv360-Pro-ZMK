# Adv360 Pro ZMK config

Personal ZMK config for the Kinesis Advantage 360 Pro, based on Kinesis'
`V3.0` branch (`upstream`), with a Dvorak-based programmer layout, local
builds, one-command flashing and end-to-end tests that drive the real
keyboard. **No Bluetooth** in the daily firmware: the halves talk over a
custom encrypted radio link (`zmk-modules/split-radio`, ~1 ms), the left
half is on USB, the right half runs on its battery.

**This keyboard is hardware-modified, and the firmware here matches it.**
After a water spill the right half's P0.07 died (column 2, whose only key is
the original Hexpad key, position 70). That key's column is now wired to
**P0.22** instead; `config/adv360_right.overlay` overrides
Kinesis' `col-gpios` accordingly (`config/boards/` stays untouched). On an
unmodified right half this firmware loses that one key. Never drive P0.07
on the right half again, and don't use P0.22 for anything else. Details and
diagnosis: `docs/keyfix.md`.

## Layout of the repo

| Path | What |
|---|---|
| `config/adv360.keymap` | The keymap: behaviors + 8 layers (Base, QWERTY, Keypad, Fn, Mod, Hexpad, LShift, RShift). Layer **numbers** matter: layer LED colors and `&mo`/`&sl`/`&to` refer to them |
| `config/user_macros.dtsi` | **Generated** by `node macro_gen.js`, never edit by hand |
| `macro_gen.js` | Generates per-key macros, compose macros (š ž õ ä ö ü °), shift-proof digits (`&unshifted_n0..9`) |
| `config/macros.dtsi`, `config/boards/` | Kinesis upstream, don't edit (keeps upstream merges clean) |
| `config/adv360.conf`, `config/adv360_right.overlay` | Enable the USB bootloader reset; the right half gets its own CDC ACM port. The overlay also has the **hardware mod**: the right half's column 2 on P0.22 instead of the dead P0.07 (see the top of this file). Never add `config/adv360.overlay`: ZMK picks only the first matching overlay and it would shadow `adv360_right.overlay` |
| `zmk-modules/usb-bootloader-reset` | Zephyr module: 1200 baud "touch" on a USB serial port reboots that half into the UF2 bootloader. Also the hardware watchdog (`CONFIG_ZMK_WATCHDOG`): a hung half resets itself within 4 s (reset reason in the key monitor's hello, `reset=2`), with breadcrumbs (`zmk_crumb()`, key-monitor `C`) |
| `zmk-modules/smart-keys` | Daily-firmware behaviors (central half only): `&lshift`/`&rshift` sticky Shift (`zmk,behavior-sticky-shift`: tapped alone = Shift + LShift/RShift layer for the next key that types something or 1 s; held or with another key pressed meanwhile = plain; with Ctrl/Alt/Super held = plain; tapped twice = caps word via the Shift layer), `&repeat` smart repeat key (also inside a Ctrl+Shift+U unicode entry: repeats the last hex digit; symbols replay with their recorded Shift, letters take the current Shift/caps word), `&sl_hex` one-shot Hexpad layer (tap again = off, Enter on release after unicode entry), `&caps_wd` caps word (compose letters Ö Š uppercase, lights the caps lock LED by `--wrap`ping `zmk_hid_indicators_get_current_profile`), mod-morph unmask on next key. Its listeners run before ZMK's own (module objects link first) and it observes HID modifier state by `--wrap`ping `zmk_hid_masked_modifiers_set/clear` and `zmk_hid_implicit_modifiers_press` |
| `zmk-modules/split-timing` | Left half: processes the key events of both halves in **press order**, `ZMK_SPLIT_TIMING_DELAY_MS` after each press (1 ms with split-radio, which gives exact press times; 20 ms over Bluetooth, where the right half notifies timestamps on its own GATT characteristic and the left half estimates the clock offset). Must be **first** in `EXTRA_MODULES` (its listener captures key events before all others). Bluetooth-only experiments: heartbeat/watermark, link tuning (`src/link_tuning.c`), `experiments/` |
| `zmk-modules/split-radio` | **The split link** (default build, `RADIO=1`): both halves drive the nRF52840 radio directly (Nordic 2 Mbit, one channel), AES-128-CCM with the pre-shared key from `.split-radio-key` (made by the Makefile, gitignored, both halves need the same key: flash both after it changes), per-boot handshake, replay protection. Right half: sends key events at once with their age, polls every 100 ms for the LED/backlight state, crystal on only while active, backs off when the left half is gone. Left half: always receiving, releases right-half keys if the right half restarts or goes silent. `patches/rgb_underglow.py` makes Kinesis' LED code (`rgb_underglow.c`, `backlight.c`) take its central path without Bluetooth. CCM checked against OpenSSL: `tests/ccm_test.c` |
| `zmk-modules/key-monitor` | **Test-only** module (snippet `key-monitor`): USB serial port that streams key positions (with device uptime)/layer changes and accepts injected presses. On both halves: on the right one it only injects/reports that half's keys, which then reach the left half over the real split link. Protocol in its `Kconfig` |
| `tools/adv360/` | Stdlib-only Python: `keymap.py` (keymap/layout parser + key labels), `device.py` (key-monitor connection), `fmt.py` (keymap formatter) |
| `bin/` | `build.sh` (runs in Docker), `flash.sh`, `keymap-ui`, `e2e`, `split-bench` (split latency benchmark), `split-soak` (minutes of cross-half presses, counts hangs), `matrix-probe` (raw key matrix scan of one half, bypassing ZMK: which pins see a key, pads that can't reach their level; test firmware) |
| `docs/keyfix.md` | Hardware repair of the original Hexpad key (position 70) after a water spill: how the matrix works, the raw matrix diagnosis (P0.07 dead), the fix (wire to P0.22, `col-gpios` override) |
| `docs/split-latency-research.md` | Measured latency of every split option (BLE tuning, short intervals, raw radio), what worked and what didn't |
| `tests/e2e/` | End-to-end tests against the real keyboard |

## Build and flash

```sh
make                       # daily firmware -> firmware/*-{left,right}-clique.uf2 (Docker)
make test                  # test firmware (key monitor on both halves) -> *-test-clique.uf2
make RADIO=0               # Bluetooth split instead of split-radio -> *-ble*-clique.uf2 (what CI builds)
bin/flash.sh               # build + flash both halves, no key presses needed (~10 s + build)
bin/flash.sh --no-build    # flash the newest build
bin/flash.sh --test        # build + flash the TEST firmware
bin/flash.sh --ble         # the Bluetooth flavor
bin/flash.sh --left|--right, --left-uf2 FILE, --right-uf2 FILE, --reset (settings-reset first)
```

- Both halves must run firmware built with the same `.split-radio-key`
  (`bin/flash.sh` flashes both). The right half must be on USB for flashing
  and testing; daily it runs on its battery.

- Flashing reboots each half into its bootloader by setting 1200 baud on its
  USB serial port, writes the `.uf2` to the `ADV360PRO` drive (matched to the
  half by USB serial number) and waits for it to come back.
- If a half has no serial port (old/other firmware) the script asks for
  `Mod` + bootloader key (Hotkey 1 left, Hotkey 3 right; the right one goes
  through the left half over the split link, so both halves need matching
  firmware), or double-tap the reset button with a paperclip.
- **The test firmware is a keylogger + remote keyboard for every local
  program.** Flash it only for testing; `bin/flash.sh` puts the daily build
  back. `bin/e2e --flash` re-flashes the test build.
- Never open a keyboard serial port without setting the speed explicitly
  (see `tools/adv360/device.py:open_raw`): Linux remembers the last speed
  per `ttyACM` number, and 1200 baud reboots the keyboard into its bootloader.

## Adding or changing a feature

Work test-first against the real keyboard:

1. **Write an e2e test** in `tests/e2e/test_<feature>.py` that describes the
   wanted behavior (see below). Run it on the current firmware with
   `bin/e2e -k <name>` and check that it fails for the expected reason.
2. **Change the keymap.**
   - Keys/layers: edit `config/adv360.keymap`, then run
     `python3 tools/adv360/fmt.py` to re-align the layer grids (CI checks it).
   - New generated macros/behaviors: edit `macro_gen.js`, run
     `node macro_gen.js` and commit both files (CI checks they match).
   - Prefer stock ZMK behaviors (mod-morph, sticky-key options, caps word
     `continue-list`, ...) over firmware code.
   - Global behavior settings (e.g. `&caps_word { continue-list = ...; };`)
     go at the top of the keymap, outside `/ { }`.
3. **Flash and test:** `bin/e2e --flash` (builds + flashes the test firmware,
   then runs all tests). Run the whole suite, not only the new test.
4. If a new behavior type shows a bad label in `bin/keymap-ui` or tests can't
   find a key by label, extend `Labeler` in `tools/adv360/keymap.py`.
5. Flash the daily firmware (`bin/flash.sh`) when done, commit.

Firmware C code (modules): anything that raises ZMK events (key presses)
must run on the system work queue like ZMK's own key handling, not on a
custom thread with a small stack (that froze the keyboard once). Code that
touches HID (`zmk_hid_*`) must be central-only
(`depends on !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL`): the right half has no
HID and fails to link. Add new modules to `EXTRA_MODULES` in
`bin/build.sh` and in `.github/workflows/build.yml`.

Keymap conventions that keep fast typing correct:
- The Base Shift keys are `&lshift`/`&rshift` (sticky Shift around
  `&macro_lshift_mo_6`/`&macro_rshift_mo_7`); tests find them as
  "Shift"/"LShift" and "Shift"/"RShift". Tests that tap Shift alone get a
  sticky Shift for the next key.
- Shift-proof keys (a key that ignores Shift) are mod-morphs generated as
  `&unshifted_<key>` by `macro_gen.js`, never "release Shift, tap, press
  Shift" macros (those race with the next key).
- Repeat is `&repeat`, the Hexpad key `&sl_hex 5`, caps word `&caps_wd`
  (never ZMK's `&caps_word`: no compose letters, no LED); tests find them by
  label ("Repeat", "Hexpad"/"sticky", "CapsWd"). On the Hexpad the Shift
  keys are mod-morphs `&hex_lshift`/`&hex_rshift` (other Shift held = caps
  word).
- Compose macros are `ScrollLock, accent, letter`; caps word and repeat
  rely on that shape to find the letter.

## E2E tests

```sh
bin/e2e                 # all tests (keyboard must run the test firmware)
bin/e2e --flash         # build + flash the test firmware first (after keymap changes)
bin/e2e -k hexpad -v    # tests whose name contains "hexpad", verbose
bin/e2e -x              # stop at the first failure
bin/e2e --skip compose_letter   # leave out tests whose name contains it (repeatable)
bin/e2e --left-only     # inject right-half keys on the left half: keymap without the split link
```

- Needs both halves on USB and an X11 desktop (`DISPLAY`; from a shell
  without it, e.g. Claude Code, use `DISPLAY=:1 bin/e2e`) or Hyprland
  (Wayland; detected by `HYPRLAND_INSTANCE_SIGNATURE`). The capture
  window is GTK (via the desktop's input method, IBus or fcitx5, like real
  apps, so compose and Ctrl+Shift+U unicode entry work); `bin/e2e` re-runs
  itself with `/usr/bin/python3` when the current Python has no PyGObject.
- A capture window ("Adv360 e2e capture") takes the keyboard focus. Focus is
  verified (GTK + `xdotool getactivewindow`, on Hyprland `hyprctl
  activewindow`) before **every** key press and the test aborts instead of
  typing elsewhere. Don't type while tests run.
- Tests check exactly what the OS received (`assertTyped`). Failure messages
  include the X key events and the keyboard's own event log with timings
  (`P <pos> <1|0>` key, `L <mask> <top>` layers).

Writing a test (`tests/e2e/harness.py` documents the API):

```python
from tests.e2e.harness import E2ETestCase

class TestThing(E2ETestCase):
    def test_shifted_a(self):
        kb = self.kb
        shift = kb.key("Shift", sub="LShift")   # find keys by label, not position
        with kb.hold(shift):                    # released even if the test fails
            kb.tap(kb.key("A"))
        self.assertTyped("A")
```

- `kb.key(label, layer="Base", sub=None, nth=0)` finds a key by its label in
  the keymap (the same labels `bin/keymap-ui` shows), so tests survive keys
  moving. `kb.binding(pos, layer)` gives the raw binding for sanity checks.
- `kb.type("text")` types text the way a person would: each character's
  key (plain, with Shift, Hexpad + key, or Shift + Hexpad + key for Ö Š ...)
  comes from the keymap labels (`kb.char_map()`).
- `kb.indicators() & kb.CAPS_LOCK_LED`: LED state as the keyboard shows it
  (key-monitor `I` command).
- `kb.tap(*positions)`, `kb.press/release(pos)`, `with kb.hold(*positions)`.
  Every press waits for the keyboard's acknowledgement and until follow-up
  events (macro layer changes) have settled: no sleeps in tests.
- Right-half keys are injected on the right half (`kb.right`, its own
  key-monitor port), so they arrive over the split link like real typing.
  A noisy 2.4 GHz channel shows up as "keyboard didn't acknowledge 'P <pos>
  1'" for right-half keys (key-monitor `R`: high `retries`/`resyncs`);
  `--left-only` then still tests the keymap. `kb.play([(delay, pos,
  pressed), ...])` plays a timed script without waiting in between, for
  cross-half races (see `test_split_timing.py`);
  `kb.left_times`/`kb.right_times` hold each
  half's device timestamps. The left half's key monitor reports keys when
  split-timing releases them for processing (~20 ms after the press).
- `with kb.fast(gap=0.03):` types without waiting for macros/layers to
  settle, to reproduce fast-typing races (rolls: press the next key before
  releasing the previous one, see `test_fast_typing.py`).
- For "every key" checks, loop over `kb.char_map()`, collect failures and
  assert once (see `TestRepeatEveryKey`).
- State is reset between tests: keys released, sticky layers timed out,
  Esc tapped (ends caps word, which is a toggle). If a feature adds other
  lingering state, extend `E2ETestCase.tearDown`.
- Expected text assumes the host uses the US keyboard layout; compose
  macros (š ž õ ...) need ScrollLock as the host's Compose key (XKB
  `compose:sclk`) and Esc must stay Escape. Hosts that remap keys for a
  laptop keyboard need a per-device override for the Kinesis (Hyprland:
  `hl.device({ name = "kinesis-corporation-adv360-pro-keyboard",
  kb_options = "compose:sclk" })`).

## Latency experiments

- `make test EXTRA_CMAKE="-DCONFIG_X=y ..."` builds with extra Kconfig,
  `ZEPHYR_PATCH=/app/extra-modules/...sh` runs a patch script on the Zephyr
  tree inside the build container first
  (`zmk-modules/split-timing/experiments/`).
- `bin/split-bench` (test firmware): raw split latency (split-timing off)
  or press-to-processing delays (`S` key-monitor command), plus `B`
  (Bluetooth link parameters) and `R` (split-radio counters).
- An experiment that hangs the left half also kills its USB serial port:
  the 1200-baud reset can't work, double-tap the reset button.

## Live keymap viewer

`bin/keymap-ui` serves http://127.0.0.1:8360/ and opens it: layout from the
keymap file (reloads on save), live layers and pressed keys, click keys to
press them (test firmware), text box with the result. It only shows the
keymap **file**, not what is flashed or changed in ZMK Studio.

## Gotchas

- ZMK handles keys on the system work queue: its stack is 4 KB
  (`CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` in `config/adv360.conf`). With the
  default 1 KB nested event handling (sticky Shift around a macro,
  split-timing releasing captured events) overflowed it and hung the left
  half.
- No Bluetooth in the daily firmware (`CONFIG_BT=n`): Kinesis code that
  assumes it gets the split-radio stand-ins (`zmk_split_bt_update_led/_bl`,
  `zmk_ble_active_profile_*`, `zmk_split_bt_peripheral_is_*`). The left LED
  that showed the Bluetooth profile shows the link: steady = right half
  connected.

- Docker mounts `zmk-modules/` at `/app/extra-modules`; never mount anything
  at `/app/modules` (that hides the Zephyr modules west installed there).
- `config/version.dtsi` is generated at build time (`&macro_ver`) and reset
  by `make`; plain `west build` without it fails on `macro_ver`.
- In CI the west workspace is the repo root: west owns `modules/`, `zephyr/`,
  `zmk/`; don't add repo directories with those names.
- Halves: left = central (`Adv360 Pro`), right = peripheral
  (`Adv360 Pro rt`). Keymap, layers and the key monitor live on the left.
