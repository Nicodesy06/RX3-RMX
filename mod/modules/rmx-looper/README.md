<!-- SPDX-License-Identifier: MPL-2.0 -->
# ROLL / LOOP FX

An RMX-1000-style **roll** on the performance pads and a **live loop** on its own bus, with **HOLD**, **OVERDUB** layers, **UNDO**, **CLEAR** and a level. Standalone, no computer.

> **Status: implemented and tested offline only.** It compiles for the RX3, and its engines and module logic pass host tests. **It has not run on an RX3.** Do not rely on it in a set until the [hardware acceptance list](../../../RMX-FX.md#7-piano-di-test-su-hardware) has passed on your own player.

## Playing it

Tap **RMX** on the touchscreen (the tab where STEMS would be). While the ROLL / LOOP panel is up, the eight pads of **both** decks belong to it, whatever pad mode is lit.

| Pad | Held | SHIFT + pad |
| --- | --- | --- |
| 1 | roll 1/16 beat | HOLD on/off |
| 2 | roll 1/8 | LOOP: record, then stop / play |
| 3 | roll 1/4 | OVERDUB on/off |
| 4 | roll 1/2 | UNDO the last overdub (then CLEAR) |
| 5 | roll 1 beat | CLEAR the loop |
| 6 | roll 2 beats | SOURCE: CH1 → CH2 → MST |
| 7 | roll 4 beats | loop level −10 |
| 8 | roll 8 beats | loop level +10 |

- **Roll**: hold a pad. The slice starts where you pressed and repeats at that division of the deck's BPM; the track keeps playing underneath, so letting go lands where the track would have been. Press another pad while holding to change the division from the same start. With **HOLD** on, the roll keeps going after you lift your finger; turn HOLD off to release it.
- A deck's pads roll **that deck** (before its EQ, Color FX and fader, so you can filter a roll). With SOURCE on **MST** they roll the **master bus** instead.
- **Loop**: SHIFT + pad 2 records one loop of the current division (at least 1/4 beat) from SOURCE: CH1 or CH2 (that deck after rolls and COLOR 2, before its fader) or MST (the master). It then plays on its own bus, into the master, at its own level: changing tracks, stopping the deck or moving faders does not touch it.
- **Overdub**: SHIFT + pad 3 adds SOURCE into the loop pass after pass until you press it again. Each overdub is a layer; UNDO removes the newest. Four layers are kept for undo; older ones are folded into the loop.
- Roll something, then overdub it: that is the RMX way to build a pattern.

The touchscreen row does the same things: `< SOURCE >`, `< DIVISION >`, ROLL (a latched roll of SOURCE), HOLD, REC/STOP/PLAY, OVERDUB, UNDO/CLEAR, and the level (tap to mute, hold and drag to set).

## Limits

- **Tempo** comes from the player's own BPM accessor, read ten times a second. Upstream has checked that accessor against the 1.19 binary but not watched it on a deck. If it reads outside 40–300 BPM, rolls use **120 BPM**. Whether it is the track BPM or the played BPM after the tempo fader is a hardware question; the session log prints it.
- **No beat grid yet**: a roll starts the moment you press, not on the next beat. Its length follows the BPM, so repeats stay in time with the track; their phase depends on your timing. A loop's length is fixed when it is recorded and does not follow later tempo changes.
- The loop's CH1/CH2 source crosses from the deck stream to the master bus through a small FIFO. Whether those run on one audio thread or two is not known on hardware; the session log reports FIFO underruns and skips so it can be checked.
- Pad lights change when the player refreshes them, which is not instant for every change.
- Uses about **28 MB** of RAM (three roll buffers and six loop buffers, eight beats at 60 BPM). With less than 64 MB free after that, the module steps aside and logs why; the rest of the mod still runs.
- An overload guard bypasses the module until restart if the master block keeps taking more than half its own duration.
- Not compatible with **Stems** (same touchscreen tab).

## Turning it off

Untick it in the app, or create `/tmp/rx3-rmx-looper.off` on the player with the diagnostic shell, then reinsert the drive. Power off, remove the drive and power on to return to stock.
