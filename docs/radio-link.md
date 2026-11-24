# Split radio link: lag bursts, status and next steps

Status 2026-11-24, end of the session in the office: the link no longer
blinks red or doubles keys, but some right-half key presses still lag a lot
(interference bursts). This file is the hand-over to continue on another PC.
Background and all measurements: `docs/split-latency-research.md`.

## First, on the other PC

1. **The keyboard may sit in its bootloader.** A flash was interrupted at
   15:43 (the keyboard was unplugged while `bin/flash.sh` waited): both
   halves had already been told to reboot into the bootloader. A half that
   shows up as an `ADV360PRO` drive instead of a keyboard: just run
   `bin/flash.sh` (it writes the drive it finds), or switch that half off and
   on to boot the firmware it still has.
2. **That firmware is the TEST build** (key monitor: a keylogger and remote
   keyboard for local programs), +8 dBm. Put the daily build back:
   `bin/flash.sh` (both halves on USB).
3. **`.split-radio-key` is per PC** (not in git): a build on the other PC
   uses that PC's key, so both halves must be flashed from there
   (`bin/flash.sh` does both).
4. **Check the right half's battery.** Unplugged at 15:36, it had no power at
   all (it came back with a cold boot, `reset=0`, when switched on): the
   battery switch was off or the battery is flat.
5. Host setup for the e2e tests: see `CLAUDE.md` (compose key etc.).

## What was found and fixed today (pushed)

| Symptom | Cause | Fix |
|---|---|---|
| right half blinks red | it handshook again after 10 lost frames (~10 ms of noise); Kinesis' LED code shows every handshake as "disconnected" | handshake only when the left half asks (`NEED_HELLO`) or after 1 s without an ack |
| doubled keys | each handshake restarted the event numbering: events that arrived but whose ack got lost came in again | numbering runs on for the whole right-half boot |
| slow recovery | the left half's challenge came from the hardware RNG inside the radio interrupt, too slow for the ack window (1498 HELLOs for 160 handshakes) | challenge = AES(boot seed, counter): one HELLO per handshake |
| losses under USB load | the right half switched to receiving in its interrupt handler, delayed by USB interrupts | hardware short TX -> RX |
| link breaks for seconds | 2450 MHz between Wi-Fi channels 6 and 11 (office access point on 11 at full signal), and at 0 dBm no margin | 2477 MHz, +8 dBm (A/B: 64-82 % failed tries at 0 dBm, 2-6 % at +8 dBm, no outages) |

## What is still wrong

At +8 dBm, right half **on battery**, ~2 minutes of typing: mostly clean
(0-1 CRC errors per 2 s at the left half), but one outage of 1.2 s with 26
CRC errors in 2 s: an interference burst. Such bursts are the lag you
still feel.

With the right half **on USB** and typing (test firmware, its key-monitor
port open): 20-77 CRC errors per 2 s and outages of 0.4-1 s. Suspected:
USB activity on the right half (one key-monitor line per key press, at the
same moment as the radio frame) disturbs its own radio. This morning's e2e
runs showed the same load dependence (idle 2.5 % losses, injected keys
96 %). The daily firmware has no key monitor and the right half normally
runs on battery, so daily use may not see this; the e2e results over the
radio do.

Not known yet: is the link **weak** (the right half's antenna: water damage,
or the new P0.22 wire running near the module's ceramic antenna) or is the
office just **noisy**? The next build answers that.

## Next steps

1. **Check the new RSSI measurement** (committed, compiled, never run on
   the keyboard: the flash was interrupted). `bin/flash.sh --test`, then
   `bin/link-log 60`: the `R sig`, `L sig` and `L noise` columns must fill
   in. `sig` = signal of the other half's valid frames, `noise` = the
   channel while the left half only listens (sampled every 5 ms, test
   builds only), both in -dBm since the previous sample.
2. **Measure the link budget.** Halves where they normally are, right half
   on battery, typing normally: `bin/link-log --left 300` (the left half's
   view; `--left` keeps the right half's port closed). Then with the right
   half on USB: `bin/link-log 300` (both views).
   - Expect the right half's frames at roughly -40 to -55 dBm at desk
     distance. **Much weaker (below -65 dBm):** the right half's RF path.
     Look at the P0.22 wire (route it away from the antenna end of the
     Holyiot module, keep it short and flat), spill residue on or around the
     module (isopropyl alcohol), and compare `L sig` (right -> left) with
     `R sig` (left -> right).
   - **Signal fine, noise high or bursty** (strongest noise within ~20 dB
     of the signal): interference. Compare at home and in the office.
3. **If interference stays the problem: frequency diversity.** Sketch (like
   Nordic's Gazell):
   - a short channel list, e.g. 2477 / 2425 / 2403 MHz (above Wi-Fi 11,
     between 1 and 6, below 1);
   - right half: after N failed tries on a channel, try the next one;
   - left half: no valid frame for ~150 ms (polls come every 100 ms): step
     to the next channel and stay long enough for the right half to cycle
     through all of them; acks name the channel in use;
   - both start on the first channel after boot; the handshake stays as is.
   Test with `bin/link-log` and the e2e suite over the radio; A/B against
   the current build (see Tools).
4. **USB on the right half:** if step 2 shows USB makes it worse, check it
   without the key monitor on the right half: test build on the left, daily
   build on the right (`bin/flash.sh --left-uf2 <test-left.uf2> --right-uf2
   <daily-right.uf2>`, both from the same commit and PC: same key, same
   radio code), then `bin/link-log --left` with the right half on USB and on
   battery. Cures if it matters: keep the right half on battery when testing
   over the radio, or delay the right half's key-monitor USB writes until
   its frame went out.
5. Daily firmware back (`bin/flash.sh`), update this file and
   `docs/split-latency-research.md` with the numbers, commit.

## Tools

- `bin/link-log [--left] [SECONDS] [INTERVAL]`: link counters per interval
  (acked frames, resends, handshakes, CRC errors, signal, noise) and a
  summary. Test firmware.
- Key monitor `R` (test firmware): the raw counters of one half.
- A/B two builds without rebuilding: every build lands in `firmware/` with a
  timestamp; `bin/flash.sh --left-uf2 A-left.uf2 --right-uf2 A-right.uf2`,
  log, then the same with B, then A again (the environment changes within
  minutes: never trust a single before/after).
- Build variants: `make test EXTRA_CMAKE="-DCONFIG_ZMK_SPLIT_RADIO_TX_POWER=0
  -DCONFIG_ZMK_SPLIT_RADIO_FREQUENCY=50"` (both halves need the same
  frequency).
- `bin/e2e --left-only` tests the keymap without the radio; plain `bin/e2e`
  includes the link (right-half keys go over the radio).
