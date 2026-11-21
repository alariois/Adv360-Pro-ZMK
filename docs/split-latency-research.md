# Split latency research (November 2026)

Goal: keys of both halves always processed in the order they were pressed
("Repeat works in sync"), with the lowest possible latency. Everything below
was measured on the real keyboard (Advantage 360 Pro, nRF52840 both halves,
both on USB) with the test firmware.

## How it was measured

* `bin/split-bench` presses a right-half key (on the right half, over its own
  USB key-monitor port) and a left-half key at the same moment, 200-300
  times. The left half's key monitor timestamps when it sees each key (one
  clock):
  * split-timing off: the difference is the **raw split link latency**
  * split-timing on: `S` statistics give the **added delay** from press to
    processing, per half; a left-only phase shows what left keys alone wait
* e2e tests `test_split_timing.py`: letters and Repeat across the halves at
  3-30 ms gaps must come out in press order; the whole suite (80 tests) must
  pass.
* Experiment builds: `make test EXTRA_CMAKE="-DCONFIG_...=..."`,
  `ZEPHYR_PATCH=...` patches the Zephyr tree inside the build container.
* Not measurable with injected keys: the key switch, matrix scan and
  debounce (injection happens after them).

## Where the latency is (per key press, before this work)

| stage | latency | notes |
|---|---|---|
| debounce | **~15-16 ms** | `ZMK_KSCAN_DEBOUNCE_PRESS_MS=15` (Kinesis default), both halves |
| matrix scan | ~0.5 ms | interrupt, then 1 ms scans |
| split link (right half only) | 3-17.5 ms | BLE, 7.5 ms interval, see below |
| press-order delay (split-timing) | 20 ms fixed | added to every key so the right half's keys can't be overtaken |
| USB HID | ~0.5-1 ms | bInterval 1 ms (the nRF52840 is full-speed USB: 1 ms is the floor) |

## Split link: raw latency (split-timing off)

Right-half key seen by the left half, ms (n=300):

| link | min | median | p90 | p99 | max |
|---|---|---|---|---|---|
| BLE stock: 7.5 ms interval, peripheral latency 30, host advertising | 2.9 | 7.1 | 10.1 | 16.6 | 17.5 |
| + 2M PHY requested | 2.9 | 7.1 | 10.2 | 17.4 | 17.8 |
| no host advertising | 2.8 | 6.5 | 9.6 | 10.3 | 10.4 |
| peripheral latency 0 | 1.0 | 4.7 | 7.9 | 14.5 | 15.0 |
| no advertising + latency 0 | 1.0 | 4.4 | 7.4 | 8.3 | 8.4 |
| + 3.75 ms interval (patched Zephyr) | 0.9 | 2.9 | 4.3 | 4.6 | 4.7 |
| + 2.5 ms interval (patched Zephyr) | 1.0 | 2.3 | 3.3 | 3.5 | 3.5 |
| + 1.25 ms interval (patched Zephyr) | link connects, no data gets through | | | | |
| **raw radio** (split-radio, no Bluetooth) | **0.73** | **0.76** | 0.79 | 0.82 | **0.82** |

Findings:

* **Host advertising** (the left half advertises for Bluetooth hosts all the
  time when none is connected) collides with the split link: it causes the
  whole >10 ms tail.
* **Peripheral latency 30** (the right half may skip 30 connection events to
  save power) costs ~2.4 ms on every key: waking the link up from skipping
  apparently misses an event. Latency 0 = the right half's radio wakes every
  7.5 ms (small battery cost).
* **2M PHY**: the request didn't take effect (link stayed 1M); the possible
  gain is only the air time (~0.05 ms), not investigated further.
* **Intervals below the 7.5 ms Bluetooth minimum** work between our own two
  Zephyr controllers: the host check is skipped with
  `CONFIG_BT_CONN_PARAM_ANY`, one HCI check is patched
  (`zmk-modules/split-timing/experiments/short-interval.sh`). 2.5 ms is the
  practical floor (1.25 ms connects but ZMK's GATT traffic never completes).
  Not usable with real Bluetooth hosts (only for the split link).
* **"Fire and forget"**: BLE notifications (what ZMK uses) already have no
  reply at the GATT level; the link layer always acknowledges and
  retransmits (can't be turned off, and its retransmission is what makes
  delivery reliable). The latency comes from waiting for the next connection
  event, not from replies. Same for the radio: the ACK doesn't delay
  delivery, only decides about a retry.

## Press order (split-timing on)

Added delay, press -> processing (ms):

| setup | right keys | left keys alone | order (e2e, 3-30 ms gaps) |
|---|---|---|---|
| committed: fixed 20 ms delay | 20 | 20 | correct |
| tuned BLE + heartbeat + watermark (T9) | mean 5.8, max 8.5 | mean 9.4, max 17.5 | correct (80/80 twice) |
| 2.5 ms interval (patched) + 5 ms heartbeat (T5, before the fixes) | mean 3.2, max 18 | mean 3.7 | not checked |
| 2.5 ms interval (patched) + 2.5 ms heartbeat (T10) | **left half hung** (needed a manual reset) | | |
| **raw radio + 1 ms delay** | **mean 1.6, max 1.7** | **mean 1.1, max 1.1** | correct (80/80) |

How the BLE variant releases keys early (`ZMK_SPLIT_TIMING_WATERMARK`,
`ZMK_SPLIT_TIMING_HEARTBEAT_US`): the right half's messages arrive in order,
so a message stamped T proves every right-half key pressed before T has
arrived. A right-half key goes as soon as its own timestamp arrives; a
left-half key waits for a right-half message stamped at least 1 ms after it
(heartbeat every ~7.5 ms, randomized) or the 20 ms cap. Lessons:

* a heartbeat at exactly the connection interval locks to it (both from the
  same 32 kHz clock): every heartbeat then sees the same latency, the clock
  offset (smallest latency seen) was off by ~4 ms and keys came out of
  order. Randomizing the period (+-1/3) fixed it.
* heartbeats as fast as the interval (2.5 ms) overload the link together
  with ZMK's own notification per key: right-half keys queued up to 140 ms.
  Skipping a heartbeat when a timestamp went out recently fixed that.
* a cap shorter than the worst link latency (10 ms vs 8.4-10.4 ms) lets left
  keys overtake: keep the cap at 20 ms; the watermark makes the average low.
* fundamental on BLE: a left key waits about half an interval + the link
  latency + margin, ~9 ms at 7.5 ms.
* 2.5 ms interval + 2.5 ms heartbeat (T10) hung the left half within
  seconds (USB serial dead too, 1200-baud reset impossible, reset button
  needed). Not investigated: the out-of-spec interval is not worth it.

## Raw radio (`zmk-modules/split-radio`, `bin/flash.sh --radio`)

Both halves drive the nRF52840 radio directly (Nordic proprietary 2 Mbit,
one channel at 2450 MHz), no Bluetooth stack:

* right half sends each key event at once with its age in us; the left half
  computes the press time on its own clock (no clock sync needed) and acks
  every frame; unacked frames are rebuilt and resent after 50-350 us
* AES-128-CCM with a pre-shared key (`.split-radio-key`, generated by the
  Makefile, gitignored), AES in the ECB hardware (~30 us per frame), 4-byte
  MIC; per-boot handshake with a fresh challenge from the left half and
  increasing counters: recorded frames can't be replayed, ACKs can't be
  forged (so keys can't be suppressed either). CCM code checked against
  OpenSSL (`zmk-modules/split-radio/tests/ccm_test.c`).
  AES-CCM instead of ChaCha20-Poly1305: the nRF52840 has AES in hardware
  (ChaCha would be ~50 us in software, the CryptoCell that could do it needs
  Nordic's proprietary library).
* result: link 0.76 +- 0.05 ms, ~1 retry per 300 frames, press-order delay
  1 ms, all 80 e2e tests pass
* bugs found on the way: an address starting like the preamble (0xA?/0x5A)
  made the receiver lock on a bit early (~88% CRC failures); with the
  address included in the CRC every frame failed the CRC although the bytes
  arrived intact (now SKIPADDR; the address match + MIC cover it); the left
  half must not ack sooner than ~200 us or the right half isn't listening
  yet.

Costs / limits:

* **no Bluetooth at all** (Zephyr's open-source controller owns the radio;
  sharing it needs a timeslot API that only Nordic's SoftDevice Controller
  + MPSL in nRF Connect SDK has, i.e. moving ZMK to NCS): USB only, no
  Bluetooth hosts, no ZMK Studio over BLE
* the left half listens all the time (~5 mA; it's on USB in this mode), the
  right half keeps the crystal oscillator on (~0.25 mA, ~6 mAh/day)
* one fixed channel: Wi-Fi/Bluetooth interference means retries (each
  ~0.8 ms); no channel hopping yet
* Kinesis' LED code needs Bluetooth: built with `ZMK_RGB_UNDERGLOW=n` (no
  layer colors)
* the key is in both firmware files: anyone with the .uf2 can read it

## Other ideas considered

* **Debounce** (biggest single item, ~14 ms on every key, both halves):
  `CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS=1` reports a press after 2 stable
  scans; release debounce 5 ms still filters chatter. Can't be tested with
  injected keys: needs real typing to check for double letters.
* Wired link between the halves: none on this hardware.
* Both halves on USB and the host relaying the right half: needs a daemon,
  USB adds ~1-2 ms each way plus scheduling jitter.
* Optimistic processing (send left keys at once, fix up later): impossible,
  HID reports can't be taken back.
* Carrying the right half's keys only on the timestamped channel (one
  notification per key instead of two) would remove the 2.5 ms interval's
  remaining tail; needs replacing ZMK's split receive path.

## Recommendation

| option | latency added by the split | keeps | gives up |
|---|---|---|---|
| A. committed | 20 ms every key | everything | latency |
| B. tuned BLE (daily now) | right ~6, left ~9 (max 20) | Bluetooth hosts (advertising paused while on USB) | right half battery a bit (latency 0) |
| C. B + 2.5 ms interval | ~3-4 ms, but T10 hung the left half | | out-of-spec connection, patched Zephyr, not stable |
| D. raw radio | ~1-1.6 ms | USB typing, Studio over USB | all Bluetooth, layer LEDs, left half power |
| + debounce 1 ms | -14 ms on every key, any option | | needs a chatter check by typing |

## Decision and result (2026-11-21)

Bluetooth to computers isn't needed (the keyboard is always plugged in), so
the daily firmware uses the raw radio (option D) plus the 1 ms press
debounce. Made fit for daily use:

* right half on its battery: crystal oscillator only while active (on for
  1 s after the last frame), polls the left half every 100 ms (LED and
  backlight state come back in the acks), retries every 20-30 ms while the
  left half is gone for up to 5 s (it restarts in ~1 s), then slower up to
  1 s; a key press retries at once
* left half: releases right-half keys still held when the right half
  restarts or is silent for 1 s (no stuck keys)
* Kinesis' status LEDs and key backlight work as before (build-time patch,
  `zmk-modules/split-radio/patches/`); the Bluetooth-profile LED shows the
  link instead
* hardware watchdog on both halves; the system work queue stack went from
  1 KB to 4 KB after sticky Shift + split-timing overflowed it (the
  watchdog caught that hang; the unexplained hang of the tuned Bluetooth
  configuration above may well have been the same overflow)

Measured on the final test build (right half on USB):

| check | result |
|---|---|
| e2e suite (90 tests incl. cross-half order at 3-30 ms, sticky Shift) | pass (one caps-word test failed once in four full runs, then 66/66 in reruns) |
| press -> processing, left / right keys | mean 1.08 / 1.70 ms, max 2.9 / 2.1 ms |
| reboots: left, right, both (9 cycles, 30 presses per half each) | all keys arrive |
| right key held while the right half reboots | released on the left |
| 5 min soak, cross-half pairs at 0-30 ms | 2316 rounds, 0 hangs, ~2% frames retried |

Not measured: the right half on battery alone (the test setup needs its
USB port), its current draw, behavior under heavy 2.4 GHz interference.
