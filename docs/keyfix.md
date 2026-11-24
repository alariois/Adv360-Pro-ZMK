# Broken key after the water spill: diagnosis and fix plan

Status 2026-11-24: diagnosis in progress, nothing soldered yet, no firmware
change for it yet. This file is the hand-over to continue on another PC.

## The key

- The **original Hexpad key** on the right thumb cluster stopped working
  after water was spilled on the keyboard. A second Hexpad key was added in
  the keymap as a workaround (the stock PgDn position, `&sl_hex 5`).
- Broken key: keymap **position 70** (right thumb, bottom row, the
  `&sl_hex 5` after `&kp BSPC` in the Base layer), matrix **RC(0,12)**.
- Right half (`config/boards/arm/adv360/adv360_right.dts`, `col-offset =
  <10>`): row index 0 = **P0.19**, column index 2 = **P0.07**.
- Matrix is `col2row`: column (output, P0.07) -> switch -> diode -> row
  (input with pull-down, P0.19).

## What is known

| Check | Result |
|---|---|
| Switch with a multimeter | works |
| Key monitor (test firmware) while pressing | **no position at all**: an open path, not a short to another line (a short would show up as a different key) |
| Diode replaced (with the one from the neighbouring position) | still dead: the diode wasn't the problem |
| Measured on top of the Holyiot module | the key press "comes in on the pin" (which pin exactly: **to confirm**) |
| Row 0 (P0.19) | fine: the new Hexpad key, Backspace, `:` `/` down up and the corner Super key are on it and work |
| Column 12 (P0.07) | serves **only this key**: a fault there shows nowhere else |

What the voltages mean (keyboard on, idle ZMK drives all columns high and
waits for a row to go high):

| Signal | Pin | Idle | Key held |
|---|---|---|---|
| column | P0.07 | ~3 V | ~3 V |
| row | P0.19 | ~0 V (pull-down) | ~2.4-2.7 V (3 V minus the diode drop) |

- If **P0.19 rises** when the key is held: the hardware path to the chip is
  intact. Most likely suspect then: **P0.07 can't drive properly while
  scanning** (after a press wakes the matrix, ZMK drives the columns one at a
  time and reads the rows; a weakened output can hold "high" idle but not
  work during the scan), or its solder joint under the module.
- If only **P0.07** was measured: it is high anyway, that proves nothing.

## Step 1: raw matrix test (firmware, not written yet)

Goal: find out for sure whether the chip sees the key, bypassing ZMK.

Implementation plan (test firmware only, `zmk-modules/key-monitor`):

- New key-monitor command `M` (works on both halves, the broken key is on
  the right one). For each column of this half's `kscan0` (pins from the
  devicetree: `row-gpios` / `col-gpios`, `GPIO_DT_SPEC_GET_BY_IDX` on
  `DT_NODELABEL(kscan0)`), with interrupts locked (a few us per column, so
  ZMK's scanning doesn't interfere):
  set all columns inactive, set this column active, wait ~10 us, read all
  rows, then restore the outputs as they were.
- Reply: `M c0=<row bits hex> c1=... c9=...` (bit n = row n pressed).
- Document the command in `zmk-modules/key-monitor/Kconfig` like the others.

Running it:

1. Plug the **right half into USB** (it normally runs on battery).
2. `bin/flash.sh --test` (builds + flashes the test firmware on both halves).
3. Hold the broken key, send `M` to the right half's key monitor, e.g.:
   ```sh
   /usr/bin/python3 -c "
   import sys,time; sys.path.insert(0,'tools')
   from adv360.device import KeyMonitor, RIGHT_PRODUCT
   L=[]; m=KeyMonitor(L.append, product=RIGHT_PRODUCT); m.connect(); m.start()
   m.send('M'); time.sleep(0.5); m.stop(); print([l for l in L if l.startswith('M')])"
   ```
   Compare with a working key on the same row (e.g. the new Hexpad key,
   position 68 = RC(0,10) = column index 0).
4. `bin/flash.sh` puts the normal firmware back.

Reading the result for the broken key (column index 2, row 0):

- `c2` has bit 0 set while held: the chip sees it, the hardware is fine and
  the problem is in software (unexpected: same firmware worked before the
  spill).
- `c2` stays 0 (and the working key shows up in `c0`): **P0.07 or its
  joint is the problem** -> step 2.

## Step 2: move the column to a free pin (wire + firmware)

### Pins

Used on the right half (from the board files): rows P0.19 P0.05 P0.31 P0.30
P0.29; columns P0.12 P1.09 **P0.07** P1.11 P1.10 P1.13 P1.15 P0.03 P0.02
P0.28; RGB LED data P0.20; backlight PWM P0.17; LED power switch P0.13;
battery ADC P0.04 (AIN2); 32 kHz crystal P0.00 P0.01; reset P0.18; NFC
P0.09 P0.10 (not GPIO unless reconfigured); USB and SWD on dedicated pins.

Free as far as the firmware knows:

- P0.06, P0.08, P0.11, P0.14, P0.15, P0.16, P0.21, P0.22, P0.23, P0.24,
  P0.25, P0.26, P0.27
- P1.00 - P1.08, P1.12, P1.14

Before using one:

1. The Holyiot module may not have a pad for every chip pin: check its pad
   labels / datasheet.
2. "Unused in firmware" isn't "unconnected on the PCB": with the keyboard
   off, check the pad has **no continuity to anything** (no trace, no short
   to GND or 3 V).
3. Prefer a pad close to the P0.07 pad or to the switch (shortest wire).
   Neighbouring numbers (P0.06, P0.08) are often neighbouring pads, check.
4. Check **P0.07 for a short** (keyboard off: P0.07 to GND and to 3 V). The
   firmware stops driving P0.07 after the change, so the old trace may stay
   connected - unless P0.07 is shorted, which would pull the new wire too:
   then cut the trace to the P0.07 pad.

### Wire

Thin wire from the free pad to the **column side of the broken key**: the
switch pin that is not connected to the diode (or anywhere on the column-12
trace, which goes only to this key).

### Firmware

One override in our own `config/adv360_right.overlay` (right half only;
Kinesis' `config/boards/` stays untouched): the whole `col-gpios` list with
the third entry changed, e.g. for P0.08:

```dts
/* Column 12 (the original Hexpad key, position 70): P0.07 stopped working
 * after a water spill, a wire connects the key's column to P0.08 instead.
 * docs/keyfix.md */
&kscan0 {
    col-gpios
        = <&gpio0 12 GPIO_ACTIVE_HIGH>
        , <&gpio1  9 GPIO_ACTIVE_HIGH>
        , <&gpio0  8 GPIO_ACTIVE_HIGH>
        , <&gpio1 11 GPIO_ACTIVE_HIGH>
        , <&gpio1 10 GPIO_ACTIVE_HIGH>
        , <&gpio1 13 GPIO_ACTIVE_HIGH>
        , <&gpio1 15 GPIO_ACTIVE_HIGH>
        , <&gpio0  3 GPIO_ACTIVE_HIGH>
        , <&gpio0  2 GPIO_ACTIVE_HIGH>
        , <&gpio0 28 GPIO_ACTIVE_HIGH>
        ;
};
```

Then `bin/flash.sh` (right half on USB), test the key with the key monitor
(`bin/flash.sh --test`, `bin/keymap-ui`) and type with it. If it works, the
workaround Hexpad key at position 68 can go back to `&macro_lalt_mo_1`
(see the comment above the Base layer in `config/adv360.keymap`).

## Continuing on another PC

- **`.split-radio-key` is not in git** (on purpose). Building on another PC
  makes a new key: the halves then only talk after **both** are flashed
  with that build (`bin/flash.sh` does both). Or copy `.split-radio-key`
  from the old PC to the repo root first.
- Flashing the right half needs it on USB.
- Tests need the test firmware and an X11 desktop (`DISPLAY=:1 bin/e2e`),
  see `CLAUDE.md`.
