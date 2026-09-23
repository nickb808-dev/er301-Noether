# Noether

A seamless vari-speed loop recorder for the
[ER-301 Sound Computer](https://www.orthogonaldevices.com/er-301). One REC
button in the pedal-looper grammar, a loop that closes on the exact sample you
pressed it and wraps without a click, and playback as a tape head: continuous
speed from -4x to +4x, 0 = stopped, negative = reverse, pitch following speed.

Unit title: **Noether** · mnemonic **Nt** · stereo · current version **0.3.0**

Named for Emmy Noether. Sibling to Dirac, Planck, Bohr and Landau.

---

## What it does that the stock loopers do not

The stock Pedal Looper closes its loop on 128-sample frame boundaries and only
plays at 1x forward. Feedback Looper and Dub Looper are fixed-length punch-in
buffers. Noether records a loop of any length, closes it at the sample the REC
edge landed on, blends the tail of the take into its head so the wrap is
continuous at any speed or direction, and plays it back through a Hermite
tape head with a 40 ms speed glide.

## Quick start

1. Add Noether. It passes audio and has a 30 s stereo buffer ready.
2. Tap **rec** to start recording, tap it again to close the loop. It plays
   immediately, seamlessly: the close point is phase-matched to the start of
   the loop (searched over the last ~10 ms before your press).
3. Turn **speed**: +1 is the original, 2 is an octave up, 0.5 an octave down,
   0 stops, negative runs backwards. Patch CV for glides; patch a keyboard or
   sequencer into **V/oct** to play the loop chromatically.
4. Tap **rec** while playing to overdub; **sos** is the crossfader between the
   existing loop (1) and the live input (0). Tap **rec** again to stop.
5. Latch **ext** on and an overdub that reaches the end of the loop keeps
   going: the loop plays underneath while you add a second pass, and the next
   **rec** closes the longer loop (2 bars become 4).
6. **start** and **len** play a window of the loop. Both are fractions of the
   loop, both take CV; the window is latched at each wrap, so sweeping start
   steps through the recording rather than scrubbing.
7. **dry** is the live input level (always full while recording or
   overdubbing); **level** is the loop.
8. Quicksave or save the preset: the loop is written to
   `ER-301/noether/` on the card and comes back with the preset.
9. The sub-display button under the waveform, or the menu, clears the loop.
   The menu also picks the buffer length (10 / 30 / 60 s) and opens the buffer
   in the sample editor.

## The Reel

The loop is drawn as a ring: its thickness at each angle is the loudness at
that point in the loop, 12 o'clock is the seam, clockwise is forward. The
bright arc is the Start/Len window; eight ticks divide the loop into sections
and the one the head is in is brightest. The head is a dot that trails a
comet tail the further speed is from 1x (an octave away is four pixels), so
you can read the speed from across the room; on a musical ratio (1/4, 1/2, 1,
2, 4, either direction, or stopped) the head turns into a small hollow ring.
Recording winds the ring on from 12 o'clock, one turn per four seconds;
Extend winds the new material on outside the finished loop. The sub display
shows the speed scale with the same hollow-on-detent marker, the ratio and
semitone offset, the loop length and the section number.

## Controls

| Btn | Inlet | Range / default | What it does |
|---|---|---|---|
| rec | Rec | trigger | empty → record · recording → close & play · playing → overdub in · overdubbing → overdub out · extending → close |
| ext | Extend | latched | While on, an overdub past the loop end grows the loop (recorded at 1x, loop playing underneath). |
| speed | Speed | −4…+4 / 1 | Playback rate. 0 stops, negative reverses, pitch follows. Glides over 40 ms. Magnetic detents within 3 % of 1/4, 1/2, 1, 2, 4 and 0. Above 1x a gentle anti-alias filter engages. |
| V/oct | V/Oct | 1 V/oct / 0 | Summed into speed as octaves: 1 V doubles it. |
| start | Start | 0–1 / 0 | Playback window start, as a fraction of the loop. Latched at the next wrap. |
| len | Len | 0–1 / 1 | Playback window length, as a fraction of the loop (min ~5 ms). 1 = the whole loop and the natural seam. |
| sos | SOS | 0–1 / 0.5 | Sound-on-sound crossfader while overdubbing / extending: 0 replaces the loop with the input, 1 keeps the loop untouched, 0.5 blends. Ignored on the first take. |
| dry | Dry | 0–1 / 1 | Live input level. Forced to 1 while recording / overdubbing. |
| level | Level | 0–1 / 1 | Loop level. |

## Menu

Loop buffer 10 s / 30 s / 60 s (Tasks; recreate the buffer), Clear loop,
Attach pool buffer, Edit / save buffer (the stock sample editor).

## Loop persistence

On every preset save the loop (only the loop, not the whole buffer) is
written as `ER-301/noether/loop-xxxxxx-xxxx.wav` on the front card, and only
when it changed since the last save. Loading the preset loads that file into
the buffer and starts playing. Delete old files from that folder freely;
a preset whose file is gone simply comes back empty.

## Roadmap

See `LOOPER-PLAN.md` in the `dirac` repo. Next: undo (phase 3), clock input
with quantised record and re-sync plus an end-of-loop trigger output
(phase 4).

## Build

Same as Landau: `make docker-image`, `make swig-docker ER301_SDK=~/er-301`,
`make docker-build ER301_SDK=~/er-301`, `make pkg`. Or natively on Linux with
`make swig build TOOLCHAIN=native ER301_SDK=...`. `make hosttest` runs the
host harness (ident · length · seam · pulse · speed · sos · window · extend · voct · persist · detent · aa · viz · nan · cpu · asan).

Apache-2.0. See LICENSE and NOTICE.
