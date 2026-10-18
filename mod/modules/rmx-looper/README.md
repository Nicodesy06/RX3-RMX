<!-- SPDX-License-Identifier: MPL-2.0 -->
# RMX X-PAD

The RMX-1000's **X-PAD FX** section on the touchscreen and the pads: **ROLL**, **KICK**, **SNARE**, **CLAP** and **HI HAT** on a touch strip from 1/8 to 2 beats, with **HOLD**, **OVERDUB**, **PITCH** and **VOLUME**. Standalone, no computer.

> **Status: implemented and tested offline only.** It compiles for the RX3, and its engines and module logic pass host tests. **It has not run on an RX3.** Do not rely on it in a set until the [hardware acceptance list](../../../RMX-FX.md#7-piano-di-test-su-hardware) has passed on your own player.

## The screen

Tap **RMX X-PAD** on the touchscreen (the tab where STEMS would be). One row runs across both halves, five and five, as on the RMX:

```
[ROLL 1][KICK][SNARE][CLAP][HI HAT][PITCH 0] | [1/8][1/4][1/2][1/1][2/1][VOL 80]
          the X-PAD FX buttons               |            the strip
```

1. **Choose** what the strip plays: tap ROLL or a sound. A sound also plays once at the tap.
2. **Touch the strip and slide** left and right. ROLL rolls the source at the section's length; a sound repeats at it. Sliding to another section changes the length on the next repeat; a roll keeps its start point, as the RMX does. Lift your finger and it stops, unless **HOLD** is on.
3. Hold **SHIFT** as your finger lands to play the strip an octave finer: 1/16 to 1/1.
4. **PITCH**: hold and drag, −12 to +12 semitones; tap to return to 0. It moves the sounds and the rolls together, as the RMX's PITCH knob does. (The track's own key is Key Shift's job.)
5. **VOL**: hold and drag, 0–100; tap to mute and unmute. It is the level of the sounds and of the OVERDUB pattern, its recorded ROLL track included; a live roll stays at the track's level.

ROLL's caption names its source: **ROLL 1**, **ROLL 2** or **ROLL M** (master). On a deck's source the roll sits on that deck's stream, before its EQ, Color FX and fader, and only on the bands chosen with ISOLATOR's FX buttons (all of them if ISOLATOR is not installed).

## The pads

While the panel is up, the eight pads of **both** decks belong to it, whatever pad mode is lit.

| Pad | Pressed | With SHIFT |
| --- | --- | --- |
| 1 | KICK | delete KICK's track |
| 2 | SNARE | delete SNARE's track |
| 3 | CLAP | delete CLAP's track |
| 4 | HI HAT | delete HI HAT's track |
| 5 | ROLL (select) | delete ROLL's track |
| 6 | HOLD on / off | PITCH −1 |
| 7 | OVERDUB | PITCH +1 |
| 8 | SOURCE: CH1 → CH2 → MST | PITCH 0 |

## OVERDUB, the RMX way

1. Press **OVERDUB**: it goes on standby (ROLL turns orange).
2. The first sound or roll you play starts a **4-beat pattern** from that moment, and everything you play is recorded into it, each sound and the ROLL on its own track. Tracks that hold something turn **red**, **blue** while they sound.
3. **HOLD + a sound** mutes or unmutes that sound's track. **OVERDUB + a sound** deletes it. (HOLD and OVERDUB act on release when nothing was pressed with them.)
4. Press **OVERDUB** again to stop the pattern and delete it.

## Your own sounds

The built-in KICK, SNARE, CLAP and HI HAT are synthesised. To use your own, copy `kick.wav`, `snare.wav`, `clap.wav` or `hat.wav` (16-bit PCM, mono or stereo, 22.05–96 kHz, at most 4 seconds) into `RX3_RUNTIME/rmx-looper/` on the drive. A file that does not fit keeps the built-in sound, and the session log says why.

## Limits

- **Tempo** comes from the player's own BPM accessor, read twenty times a second. Upstream has checked that accessor against the 1.19 binary but not watched it on a deck. If it reads outside 40–300 BPM, the X-PAD uses **120 BPM**. Whether it is the track BPM or the played BPM after the tempo fader is a hardware question; the session log prints it.
- **No beat grid**: a roll or a pattern starts the moment you touch, not on the next beat. Lengths follow the BPM, so repeats stay in time; their phase depends on your timing.
- **The AUX (third channel) knob does not drive VOL yet.** Its value is read by the player's firmware and its input path is unknown; the COLOR 2 probe logs the key codes and arguments that reach the hooked path, which is the first step to finding it.
- The touch strip follows **one finger**: the RX3 touchscreen layer this mod hooks reports one touch.
- A deck's roll crosses to the master bus (where OVERDUB records) through a small FIFO. Whether those run on one audio thread or two is not known on hardware; the session log reports FIFO underruns and skips.
- Pad lights change when the player refreshes them, which is not instant for every change.
- Uses about **18 MB** of RAM (three roll buffers of eight beats at 60 BPM, five 4-beat tracks, four sounds), plus up to 6 MB for sounds from the drive. With less than 64 MB free after that, the module steps aside and logs why; the rest of the mod still runs.
- An overload guard bypasses the module until restart if the master block keeps taking more than half its own duration.
- Not compatible with **Stems** (same touchscreen tab).

## Turning it off

Untick it in the app, or create `/tmp/rx3-rmx-looper.off` on the player with the diagnostic shell, then reinsert the drive. Power off, remove the drive and power on to return to stock.
