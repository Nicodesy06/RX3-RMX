<!-- SPDX-License-Identifier: MPL-2.0 -->
# COLOR 2 — a second Sound Color FX

Two Sound Color FX on a channel at once: the mixer's own, untouched, plus **COLOR 2**, a software FILTER, DUB ECHO, SPACE, CRUSH, NOISE or SWEEP in series before it.

> **Status: implemented and tested offline only.** It compiles for the RX3, and its engines and module logic pass host tests. **It has not run on an RX3**, and the effects are this project's own, not copies of Pioneer's. Check it on your player before a set.

## Playing it

1. Use the mixer's Color FX exactly as usual: button, COLOR knob, PARAMETER knob.
2. Tap **COLOR 2** on the touchscreen (the STATUS position). Each deck gets a row:
   `[<  DUB ECHO  >] [COLOR +40] [PARAM 50]`
3. `<` and `>` choose the second effect; tap its **name** to switch COLOR 2 on or off.
4. Hold and drag **COLOR** to set it, bipolar like the knob: left and right do different things, the centre is bypass. **Tap** COLOR to send it straight back to centre. Hold and drag **PARAM** to set the parameter; tap it to return to the middle.
5. Every effect remembers its own COLOR and PARAM, so switching back picks it up where you left it. An echo or reverb you leave keeps ringing out while the next effect comes in.

So FILTER on the mixer with DUB ECHO on COLOR 2, CRUSH + DUB ECHO, SWEEP + SPACE and any other pair work, and the two channels can use different second effects.

| COLOR 2 | Left | Right | PARAM |
| --- | --- | --- | --- |
| FILTER | low-pass | high-pass | resonance |
| DUB ECHO | echo, darker repeats | echo, thinner repeats | feedback; half-beat echo at the deck's BPM |
| SPACE | reverb, dark | reverb, bright | decay |
| CRUSH | bit and rate reduction, dark | the same, thin | tone |
| NOISE | low-passed noise | high-passed noise | level |
| SWEEP | beat-synced gate | band-pass sweep | gate rate / band width |

Order on a channel: deck → ROLL (if any) → **COLOR 2** → trim, EQ, **mixer Color FX**, fader.

## Using the Color FX buttons (after a probe)

The RX3's Color FX buttons and knobs are read by the player's own firmware. Their input codes are **not known yet**, and the COLOR knob's value is not readable by this mod at all. To map the buttons:

1. Create an empty file `RX3_RUNTIME/dual-color-fx/probe` on the drive and turn on **Troubleshooting report**.
2. Insert the drive, press each Color FX button once in the order SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER, wait ten seconds, power off.
3. The session log then holds lines like `fx2 probe key=0x4123 op=0 ch=1`. If the Color FX buttons appear there, write their six codes, in that order, on one line of `RX3_RUNTIME/dual-color-fx/buttons.txt`:
   ```
   # SPACE, DUB ECHO, SWEEP, NOISE, CRUSH, FILTER
   0x....,0x....,0x....,0x....,0x....,0x....
   ```
4. With that file, **SHIFT + a Color FX button** toggles that effect as COLOR 2 on both channels, and the mixer does not see that press. Without SHIFT the buttons work as stock.

If the buttons do not appear in the probe, they travel a path this mod does not hook, and mapping them needs more reverse engineering. Their lights are not driven by this module in either case.

## Limits

- COLOR 2's amount is set on the touchscreen: the physical COLOR knob drives only the mixer's own effect.
- DUB ECHO and SWEEP follow the deck's BPM through the same unverified accessor ROLL / LOOP uses; at an unreadable BPM they keep 120.
- About 1.6 MB of RAM for the echo and reverb memory.
- Not compatible with **Samples** (same touchscreen tab).

## Turning it off

Untick it in the app, or create `/tmp/rx3-dual-color-fx.off` with the diagnostic shell and reinsert. Power off without the drive to return to stock.
