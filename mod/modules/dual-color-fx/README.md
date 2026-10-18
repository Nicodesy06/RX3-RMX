<!-- SPDX-License-Identifier: MPL-2.0 -->
# COLOR 2 — two Sound Color FX at once

Hold two of the mixer's six Color FX buttons together and both effects run: the first is the mixer's own, untouched; the second is **COLOR 2**, a software SPACE, DUB ECHO, SWEEP, NOISE, CRUSH or FILTER in series before it. There is no screen for it.

> **Status: implemented and tested offline only.** It compiles for the RX3, and its engines and module logic pass host tests. **It has not run on an RX3**, the effects are this project's own, not copies of Pioneer's, and **the buttons' input codes must first be found with the probe below**. Check it on your player before a set.

## Playing it

1. Press a Color FX button as usual: the mixer takes it, as stock.
2. **Keep it held and press a second one.** The mixer never sees that second press: its first effect stays, and the second effect starts as COLOR 2 on both channels.
3. To stop COLOR 2, play the same chord again, or tap the second effect's button on its own.

Every other press goes to the mixer untouched. FILTER + DUB ECHO, CRUSH + DUB ECHO, SWEEP + SPACE and any other pair work.

| COLOR 2 | What it does |
| --- | --- |
| SPACE | reverb |
| DUB ECHO | echo, a half beat at the deck's BPM |
| SWEEP | beat-synced gate / band-pass sweep |
| NOISE | filtered noise |
| CRUSH | bit and rate reduction |
| FILTER | low-pass / high-pass with resonance |

Order on a channel: deck → X-PAD roll (if any) → **COLOR 2** → trim, EQ, **mixer Color FX**, fader. With **ISOLATOR** installed, its FX LOW / MID / HI buttons choose which bands COLOR 2 acts on; an echo or reverb you stop keeps ringing out.

## Mapping the buttons (needed once)

The RX3's Color FX buttons are read by the player's own firmware, and their input codes are **not known yet**. To find them:

1. Create an empty file `RX3_RUNTIME/dual-color-fx/probe` on the drive and turn on **Troubleshooting report**.
2. Insert the drive. Press each Color FX button once in the mixer's order, SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER, then turn the COLOR knob and the AUX knob a little. Wait ten seconds, power off.
3. The session log then holds lines such as
   `fx2 probe key=0x4123 op=0 ch=1 n=0003 a=00000041 b=00000000 c=00000000` (inputs) and
   `fx2 probe led id=0x0052 ch=1` (lights the player switched).
4. If the buttons appear, write `RX3_RUNTIME/dual-color-fx/buttons.txt`:
   ```
   # SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER
   0x....,0x....,0x....,0x....,0x....,0x....
   leds=0x....,0x....,0x....,0x....,0x....,0x....
   depth=150
   ```
   The first line is required. `leds=` (optional) lights COLOR 2's button beside the mixer's own. `depth=` (optional, 0–200, 100 is centre, 150 by default) is how far COLOR 2 is turned, because the COLOR knob's value is not readable yet.

Without buttons.txt the module installs, logs that it has no codes, and the buttons work as stock. If the buttons do not appear in the probe, they travel a path this mod does not hook, and mapping them needs more reverse engineering of the player.

## Limits

- **The COLOR and PARAMETER knobs drive only the mixer's own effect.** COLOR 2 runs at the `depth=` set on the drive until the knobs' values are found (the probe's `a b c` arguments are where they may appear).
- DUB ECHO and SWEEP follow the deck's BPM through the same unverified accessor the RMX X-PAD uses; at an unreadable BPM they keep 120.
- About 1.6 MB of RAM for the echo and reverb memory.

## Turning it off

Untick it in the app, or create `/tmp/rx3-dual-color-fx.off` with the diagnostic shell and reinsert. Power off without the drive to return to stock.
