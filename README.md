# Noether

A seamless vari-speed loop recorder for the
[ER-301 Sound Computer](https://www.orthogonaldevices.com/er-301). One REC
button in the pedal-looper grammar, a loop that closes on the exact sample you
pressed it and wraps without a click, and playback as a tape head: continuous
speed from -2x to +2x, 0 = stopped, negative = reverse, pitch following speed.

Unit title: **Noether** · mnemonic **Nt** · stereo · current version **0.8.14**

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
   the loop (searched over the last ~40 ms before your press, so one full period of anything down to bass fits).
3. Turn **speed**: +1 is the original, 2 is an octave up, 0.5 an octave down,
   0 stops, negative runs backwards. Patch CV for glides; patch a keyboard or
   sequencer into **V/oct** to play the loop chromatically.
4. Tap **rec** while playing to overdub; **sos** is the crossfader between the
   existing loop (1) and the live input (0). Tap **rec** again to stop.
   Didn't like it? Tap **undo**: the pass is swapped out over the next few
   blocks (the reel says "undo" while it works). Tap **undo** again to bring
   it back.
5. Latch **ext** on and an overdub that reaches the end of the loop keeps
   going: the loop plays underneath while you add a second pass, and the next
   **rec** closes the longer loop (2 bars become 4).
6. **dry** is the live input level (always full while recording or
   overdubbing); **level** is the loop. **stop** (latched) fades the loop out
   and holds the head where it is; off again resumes from that spot.
7. Patch a clock into **clk**. In the menu, *Clk input* = *free*: every
   pulse restarts the loop from the seam (skipped when the head is already
   there, so a loop in time with its clock never retriggers itself; the
   restart ramps in over 1 ms while the old position tails out over 8 ms).
   *sync*: **rec** waits for the next pulse to start and to stop, so the
   loop is a whole number of clock periods (the reel's REC blinks while it
   waits), the section ticks become the beats, and every N-th pulse pulls
   the head back to the downbeat even at 2x or in reverse. Set **bars** to
   4 and one tap does it all: the take starts on the next pulse and closes
   itself after four (the reel counts 1/4, 2/4 … while it records; a second
   tap still closes early). The sub display then reads *bar 2/4* as the
   loop plays. Unplug the clock for four seconds and rec is immediate again.
8. Quicksave or save the preset: the loop is written to
   `ER-301/noether/` on the card and comes back with the preset.
9. The sub-display button under the waveform, or the menu, clears the loop.
   The menu also picks the buffer length (10 / 30 / 60 s) and opens the buffer
   in the sample editor.

## The Reel

The loop is drawn as a ring: its thickness at each angle is the loudness at
that point in the loop, 12 o'clock is the seam, clockwise is forward. Eight
ticks divide the loop into sections (the beats, when clocked) and the one the
head is in is brightest. The head is a dot that trails a
comet tail the further speed is from 1x (an octave away is four pixels), so
you can read the speed from across the room; on a musical ratio (1/4, 1/2, 1,
2, either direction, or stopped) the head turns into a small hollow ring.
Recording winds the ring on from 12 o'clock, one turn per four seconds;
Extend winds the new material on outside the finished loop. The sub display
shows the speed scale with the same hollow-on-detent marker, the ratio and
semitone offset, the loop length and the section number.

## Controls

| Btn | Inlet | Range / default | What it does |
|---|---|---|---|
| rec | Rec | trigger | empty → record · recording → close & play · playing → overdub in · overdubbing → overdub out · extending → close |
| undo | Undo | trigger | Swaps the last overdub / extend pass out (bit-exact, restored over a few blocks from the head outward); again = redo. A new pass replaces the undo point. |
| stop | Stop | latched | Fades the loop out (5 ms) and holds the head; off resumes from the same place (sync mode: where the clock is, so the loop comes back in time). Pressed during a take it closes the take as rec would (free: at once; sync: on the next pulse) and the loop waits, stopped. It also ends an extension, punches out an overdub, and cancels a take or overdub waiting for the clock. |
| clk | Clk | trigger | Menu *free*: restart the loop. Menu *sync*: quantise rec to the clock, loop = N periods, re-sync every N-th pulse. |
| bars | Bars | 0–16 / 0 | Sync mode only: with N > 0 one tap of rec records exactly N clock pulses and closes the loop by itself. 0 = manual (tap to start, tap to stop). Takes CV, so a sequencer can set the loop length. In sync mode an overdub is also a take of N bars, in on a bar line. Ignored in free mode. |
| ext | Extend | latched | While on, an overdub past the loop end grows the loop (recorded at 1x, loop playing underneath). |
| speed | Speed | −2…+2 / 1 | Playback rate. 0 stops, negative reverses, pitch follows. Coarse steps of 0.25, fine below. Glides over 40 ms. Magnetic detents within 3 % of 1/4, 1/2, 1, 2 and 0. Above 1x a gentle anti-alias filter engages. |
| V/oct | V/Oct | 1 V/oct / 0 | Summed into speed as octaves: 1 V doubles it (capped at 2x). |
| sos | SOS | 0–1 / 0.5 | Sound-on-sound crossfader while overdubbing / extending: 0 replaces the loop with the input, 1 keeps the loop untouched, 0.5 blends. Ignored on the first take. |
| dry | Dry | 0–1 / 1 | Live input level. Forced to 1 while recording / overdubbing. |
| level | Level | 0–1 / 1 | Loop level. |

## Menu

Clk input free / sync. Loop buffer 10 s / 30 s / 60 s (Tasks; recreate the buffer and its undo
buffer — undo doubles the memory, 60 s stereo is 46 MB), Clear loop, Attach
pool buffer, Edit / save buffer (the stock sample editor).

## Loop persistence

On every preset save the loop (only the loop, not the whole buffer) is
written as `ER-301/noether/loop-xxxxxx-xxxx.wav` on the front card, and only
when it changed since the last save. Loading the preset loads that file into
the buffer and starts playing. Delete old files from that folder freely;
a preset whose file is gone simply comes back empty.

## Changes

See `CHANGELOG.md`.

## Roadmap

See `LOOPER-PLAN.md` in the `dirac` repo. Next: the end-of-loop trigger
output (its own flash), the shared-buffer workflow with Dirac, and a steeper
anti-alias filter if 2x sounds harsh.

### Research: loop a section of the loop

Play only part of the main loop, such as a quarter or one bar of four, and
loop that part. This is zooming in, not slicing: one head, one region, and
the whole loop stays intact underneath.

- **Why it came up:** at 0.5x in sync mode, the downbeat re-sync already
  plays only the first half of the loop, over and over. It sounds like a
  feature waiting to be chosen on purpose.
- **What went wrong before:** 0.6.x had a Start / Len window. It was
  removed in 0.7.0 because every wrap at a window edge was a crossfaded
  jump, which clicked or softened attacks. It also pulled Noether toward
  sequenced slices, which belong to Dirac. A section needs edges as clean
  as the main seam. One idea: phase-match each section's end to its start
  (the findSeam search) once, when the section is chosen, instead of
  crossfading on every pass.
- **Grammar to explore:** a Size control (1, 1/2, 1/4, 1/8 of the loop; in
  sync mode, whole bars) and a Position control, latched at the section's
  end. Morphagene's Gene Size / Slide is the nearest reference. Avoid a
  per-step sequencer input that turns it back into slicing.
- **Open questions:**
  - Does overdub write only inside the section (a punch region), or is the
    section a playback-only zoom?
  - In sync mode, does a section of B bars re-sync every B bars?
  - What do the ring and the bar readout show? The old window arc is a
    starting point.

## Build

Same as Landau: `make docker-image`, `make swig-docker ER301_SDK=~/er-301`,
`make docker-build ER301_SDK=~/er-301`, `make pkg`. Or natively on Linux with
`make swig build TOOLCHAIN=native ER301_SDK=...`. `make hosttest` runs the
host harness (ident · length · seam · pulse · tri · bars · speed · sos · extend · voct · persist · detent · aa · viz · undo · stop · clock · bars · dub · close · seamq · clean · syncdub · stoprec · barview · nan · cpu · asan).

Apache-2.0. See LICENSE and NOTICE.
