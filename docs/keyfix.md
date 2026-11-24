# Broken key after the water spill: diagnosis and fix

Status 2026-11-24: **fixed.** P0.07 on the right half is dead (raw matrix
probe, below). A wire now connects the key's column to **P0.22**, and
`config/adv360_right.overlay` moves column 2 there. Verified with
`bin/matrix-probe`: P0.22 reads back high when driven and low when released
(`drv=3ff rel=0`, so the old net doesn't load it), and the key is seen on
row 0 x P0.22 at every settle time and reported by ZMK (position 70), alone
and together with other keys. The steps below are the diagnosis that led
there.

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
| Key monitor (test firmware) while pressing | **no position at all**: no other key shows up, so not a short to another matrix line |
| Diode replaced (with the one from the neighbouring position) | still dead: the diode wasn't the problem |
| Measured on top of the Holyiot module | the key press "comes in on the pin" (which pin exactly: **to confirm**) |
| Row 0 (P0.19) | fine: the new Hexpad key, Backspace, `:` `/` down up and the corner Super key are on it and work |
| Column 12 (P0.07) | serves **only this key**: a fault there shows nowhere else |
| Raw matrix probe, 2026-11-24 (`bin/matrix-probe`) | **P0.07 reads low while driven high**, with the standard and the high drive (H0H1), at every settle time (0 us - 1 ms), and also as an input with only the pull-up. The other 9 right columns and all 10 left columns (the left half's own P0.07 included) read back fine |

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

## Step 1: raw matrix test (`bin/matrix-probe`)

Goal: find out for sure whether the chip sees the key, bypassing ZMK.

Key-monitor command `M [<us>]` (test firmware, both halves; protocol in
`zmk-modules/key-monitor/Kconfig`): for each column of this half's matrix
(pins from the devicetree), with interrupts locked, drive only that column,
wait `<us>`, read all rows, restore the outputs. It also reports the rows as
found (`idle`: idle ZMK drives all columns, like the multimeter test) and
reads every column pad back while driven (`drv`/`rel`: a pin that can't
reach its level).

The settle time matters: ZMK reads the rows **0 us** after driving a column
(`CONFIG_ZMK_KSCAN_MATRIX_WAIT_BEFORE_INPUTS` defaults to 0). Corrosion that
adds resistance makes the row rise slowly: idle (all columns high) still
fires the row interrupt, but ZMK's scan reads too early and sees nothing,
which also looks like "no position at all".

Running it:

1. Plug the **right half into USB** (it normally runs on battery).
2. `bin/flash.sh --test` (builds + flashes the test firmware on both halves).
3. `bin/matrix-probe` (right half, watch mode): hold the broken key, then a
   working key on the same row for comparison (the new Hexpad key, position
   68 = RC(0,10) = column index 0). Every change is printed with the pins,
   the settle times at which the key is seen and what ZMK reports.
   `--once` prints one sweep with the raw replies.
4. `bin/flash.sh` puts the normal firmware back.

Reading the result for the broken key (position 70, row 0 P0.19 x column 2
P0.07):

| `bin/matrix-probe` says | Meaning | Next |
|---|---|---|
| seen at every wait | the chip sees it at 0 us like ZMK should | software (unexpected) |
| seen only from N us | slow path (corrosion) | `CONFIG_ZMK_KSCAN_MATRIX_WAIT_BEFORE_INPUTS` > N, or clean/rework the trace |
| row 0 high with all columns driven, no single column drives it | path intact when everything drives, P0.07 alone doesn't | P0.07 drive / joint -> step 2 |
| `col 2 P0.07: pad does NOT read back high` | the pin itself can't go high (damaged or shorted) | step 2, check P0.07 for a short |
| nothing pressed (and the working key shows up) | open path between P0.07 and the row | find the break (P0.07 pad -> switch); a wire from the P0.07 pad may be enough, else step 2 |

## Step 1b: find the short (multimeter, keyboard off)

The probe reads every pad back through the chip's own input buffer, so
"reads low" has two explanations:

- the column-12 net is held at ground: a short (corrosion along the trace,
  under the module), the pad then measures ~0 V with the keyboard on;
- the P0.07 cell inside the chip is damaged: its input reads low regardless,
  and its output is dead or too weak to lift the row through the switch and
  diode (13 kOhm pull-down). The pad can then still measure ~3 V idle with a
  multimeter and sag when the key is held.

The pad voltage with the keyboard on (idle ZMK drives all columns high) tells
them apart: ~3 V idle means no short, the damage is inside the module. A short
would also cost battery (idle ZMK drives P0.07 high into it).

With the right half **off and unplugged** (battery switch off):

1. Resistance from the P0.07 pad (on the module) to GND, and from the
   column-side pin of the broken switch to GND. Normal: open / megaohms
   (diode mode may show a chip-internal ESD diode one way, ~0.5-0.7 V).
   A few ohms to kilohms: the short.
2. If the switch pin is shorted: look for corrosion (white/green residue)
   along the trace and under the module edge near P0.07, clean with
   isopropyl alcohol and a soft brush, dry, measure again. Then
   `bin/matrix-probe --once`: `drv` back to `3ff` means fixed.
3. If cleaning doesn't help, cut the trace next to the switch and measure
   both sides: switch side clean -> the short is on the P0.07 side (pad,
   module, trace) -> step 2 with the wire straight to the switch. Switch side
   still shorted -> the problem is at the switch footprint itself.

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
4. **P0.07 is held low** (step 1, find where in step 1b): the old net
   would pull the new wire low too. The column-12 trace must be **cut**
   between the switch and the short, and the new wire go straight to the
   switch pin. The firmware stops driving P0.07 after the change.

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
(`bin/flash.sh --test`, `bin/keymap-ui`) and type with it. Done
2026-11-24: works, and the workaround Hexpad key at position 68 (the stock
PgDn position) is right Alt again (`&macro_ralt_mo_1`, `&kp RALT` on QWERTY).

## Continuing on another PC

- **`.split-radio-key` is not in git** (on purpose). Building on another PC
  makes a new key: the halves then only talk after **both** are flashed
  with that build (`bin/flash.sh` does both). Or copy `.split-radio-key`
  from the old PC to the repo root first.
- Flashing the right half needs it on USB.
- Tests need the test firmware and an X11 desktop (`DISPLAY=:1 bin/e2e`) or Hyprland,
  see `CLAUDE.md`.
