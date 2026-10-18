<!-- SPDX-License-Identifier: MPL-2.0 -->
# ISOLATOR

A three-band **isolator** per deck on the touchscreen, and **FX LOW / MID / HI** buttons that choose which bands the effects act on, like a DJM-900NXS2's Beat FX band buttons.

> **Status: implemented and tested offline only.** It compiles for the RX3, and its engine and module logic pass host tests. **It has not run on an RX3.** Check it on your player before a set.

## Playing it

Tap **ISOLATOR** on the touchscreen (the STATUS position). Each deck gets a row:

```
[LOW 0] [MID KILL] [HIGH +4]   [FX LOW] [FX MID] [FX HI]
```

- **LOW / MID / HIGH**: tap to kill the band, tap again to bring it back at its level. Hold and drag to set the level, from kill to +6 dB, in half-decibel steps with a detent at 0 dB.
- **FX LOW / FX MID / FX HI**: which bands **COLOR 2** and the **RMX X-PAD**'s deck rolls act on for that deck, lit when in. Light FX LOW alone and a DUB ECHO or a roll touches only the bass; the rest of the track plays on dry. All lit, or none, is the whole signal. A change crossfades in 5 ms once the effect is idle, so it never clicks.

The crossovers sit at 250 Hz and 2.5 kHz (Linkwitz–Riley, 24 dB/octave, phase-aligned), so the three bands sum back to the original. With every band at 0 dB the isolator is a bypass, bit for bit. It sits after everything this mod does to a deck and before the player's own trim, EQ, Color FX and fader.

## Limits

- **It does not change the player's own Beat FX.** The RX3's Beat FX run inside its firmware, and choosing their bands needs reverse engineering of the player that has not been done; FX LOW / MID / HI apply to this mod's effects only.
- It does not replace the mixer's EQ knobs, which still work as stock in series after it.
- The touchscreen follows one finger at a time.
- Not compatible with **Samples** (same touchscreen tab).

## Turning it off

Untick it in the app, or create `/tmp/rx3-isolator.off` with the diagnostic shell and reinsert. Power off without the drive to return to stock.
