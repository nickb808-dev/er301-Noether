# Noether — changelog

What changed for players, version by version, from 0.3.0 (the first build
with the Reel) to the current release. The engineering detail behind each
entry lives in `memory.md`.

## 0.8.14 — current

- **Stop ends a take.** Pressing stop while recording used to do nothing to
  the recording: it kept going, and the loop it eventually closed into was
  silent and stuck. Now stop closes the take the way REC would: at once in
  free mode, on the next clock pulse in sync mode (so the loop stays a whole
  number of pulses). The loop waits, stopped; release stop to play it. Stop
  also ends an extension the same way, punches out a running overdub, and
  cancels a take or overdub that is waiting for the clock.
- **Stop in sync mode comes back in time.** Releasing stop now brings the
  loop back where the clock is, as if it had kept playing silently. Before,
  it resumed from where it stopped, out of time, until the next downbeat
  jumped it into place.
- **The bar readout follows the loop you hear.** It counted clock pulses, so
  it was right only at 1x: at half speed it showed bar 2 while you only ever
  heard bar 1. Now it reads the play head at any speed. While stopped it
  shows where the loop will come back in. During an extension it counts the
  bar being added (it used to freeze). A loop that doesn't end on a bar line
  now reads "beat 3/7" instead of calling its beats bars.

## 0.8.13

- **Fixes 0.8.10 breaking the ER-301's audio after a recording.** With
  0.8.10, closing a loop could overflow the small memory area (stack) the
  ER-301's audio thread works in. That corrupted memory beside it, and all
  audio stayed harshly distorted until a reboot, even with Noether bypassed.
  Noether's seam search kept a 1 KB list of measurements there, which had
  only just fitted since 0.7.1. It now lives inside Noether's own memory.
  The build now measures every audio function's stack use and refuses to
  package anything that could do this again.
- **Bars now goes up to 16** (it was 64), on the dial and by CV. More than
  16 bars wasn't musically manageable.
- Includes everything from 0.8.10: sync overdubs as clocked takes, extend
  following the clock, and the dub button. (0.8.11 and 0.8.12 were
  diagnostic builds, not releases.)

## 0.8.10 (do not use: see 0.8.13)

- **In sync mode, an overdub is another clocked take.** Press **rec** while
  the loop plays and the overdub waits for the next bar line, then punches
  in. With **Bars** set, it runs exactly that many bars and punches out by
  itself on a bar line. With Bars at 0, a second press punches out at the
  next bar line. Pressing again while it's waiting cancels it. The reel
  flashes **DUB** while it waits, and shows the bar count ("2/4") while a
  counted overdub runs. Free mode still punches in and out the moment you
  press.
- **New dub button: overdub right now.** Next to rec, **dub** punches an
  overdub in or out the moment you press it, in sync mode or not, and
  ignores Bars. So in sync mode you have both: rec for an overdub on the
  bar, dub for one on the spot. Dub also starts an overdub that rec has set
  waiting, and stops a counted one early. It does nothing when there's no
  loop yet. Like rec, it takes a gate or trigger from anywhere.
- **Extend keeps the loop in time with the clock.** In sync mode an
  extension now closes on a clock pulse (at the next bar line after you
  press rec, or by itself after Bars bars). The loop's bar count grows with
  it, so the bar readout and the re-sync follow the new length. Before, the
  re-sync kept using the old length and pulled the longer loop back to its
  start.

## 0.8.9

- **Bars counts bars.** A new menu list, **Clock pulses per bar** (1, 2, 3, 4,
  6, 8, 12, 16, 24 or 96), tells Noether how many clock pulses make one bar.
  Set it to 16 for a 16th-note clock, 4 for quarter notes, 96 for a 24 PPQN
  clock, or 3, 6 or 12 for 3/4 and 6/8 time. Then **Bars 4** records four
  real bars. The readouts count bars with the beat after them ("bar 2/4 .3"),
  and the reel's ticks mark bars. The default of 1 works exactly as before,
  and the setting is saved with the preset.

## 0.8.8

- **Quicksaves no longer ask you to save Noether's buffer.** Noether saves
  its loop to the card by itself, so its own buffers (the loop buffer and
  the undo buffer) now count as saved, and the "unsaved audio buffers"
  question doesn't come up because of them. A buffer you attach yourself
  from the sample pool is still treated as yours, and you'll still be asked
  about it.

## 0.8.7

- **A quicksaved loop plays again after a reboot, even if you also saved the
  buffer.** When the quicksave asks to save Noether's buffer and you say
  yes, after a reboot that buffer is loaded back from the card while Noether
  loads its own loop file beside it. Noether used to copy its loop in
  before the buffer had finished loading, and the loop was lost: the audio
  showed on screen, but nothing played. Noether now waits for both files.
  If its loop file is missing but the saved buffer is there, it plays the
  loop from the buffer.

## 0.8.6

- **The start of a loop is never altered.** To join a loop's end smoothly
  to its start, Noether used to blend the end *into the first moments of
  the loop*: 2.7 ms in free mode, 21 ms in sync mode (0.8.5). On sequenced
  or plucked material that softened the first note of every pass. Noether
  now keeps the few milliseconds of sound from just before you started
  recording, and blends the loop's *end* into that. The loop ends the way
  the original sound led into its first note, and that first note plays
  exactly as recorded, in free and sync mode.
- **The join adapts to the material.** Similar sound on both sides gets
  one crossfade shape; unrelated sound gets another that keeps the level
  steady instead of dipping.
- A sync loop that ends right at the end of the buffer now keeps its exact
  bar length.

## 0.8.5

- **No pop when you stop recording**, free or clocked. When a take closes,
  what you hear switches from the live input to the loop, and the two are
  usually out of phase. 0.8.4 made that switch over 5 ms, which on low
  notes is shorter than one cycle and comes out as a thump. It now takes
  21 ms along a smooth curve, and the Dry level follows the same curve.
- Pressing rec while a clear is still fading out starts a new take, as it
  should, instead of being swallowed.
- **Clocked loops no longer pop on every pass.** A clocked loop's length is
  set by the clock, so a held tone usually doesn't fit it exactly, and the
  end of the loop meets its start out of phase. That join was smoothed over
  2.7 ms, which on a pure tone is a pop every time the loop goes round. It
  is now smoothed over 21 ms. On a pure tone that lands exactly opposite in
  phase you may hear a short soft dip instead; on real material it's much
  milder. Free loops are unchanged: they still pick an in-phase cut.
- **Clear loop no longer clicks.** The loop now fades out over 5 ms instead
  of stopping dead. The buffer is then silenced a small piece at a time
  (about a second for 30 s) instead of all at once while the audio runs,
  which could cause a dropout in a busy patch.

## 0.8.4

- **No click when you close the first take.** While you record, you hear
  only your input. When you pressed rec to close, the loop started playing
  at full level from its first sample, and unless the input was silent at
  that moment the output jumped: a click. The loop now fades in over 5 ms
  (the same fade as Stop). This also covers clocked closes, a take that
  fills the buffer, and a loop restored from a preset.
- **No click when you end an Extend.** During an Extend you hear the loop
  one cycle back; when it closed, playback cut straight to the new start.
  It now crossfades over 8 ms.

## 0.8.3

- **What you hear while overdubbing is clean.** The overdub used to write at
  the play head and then read that spot straight back. So while DUB was on,
  you heard a second copy of your input with a grainy, sample-rate-reduced
  quality (and at 1x the input came through twice as loud). The write head
  now trails the play head by 3 samples, like the gap between tape heads:
  you hear the loop as it was plus your live input. What gets recorded is
  unchanged.
- Very quiet loop tails no longer turn into denormal numbers, which the
  ER-301's processor handles very slowly. This happened when overdubbing
  over silence for several minutes.

## 0.8.2

- **Overdubbing at any speed other than 1x no longer degrades the loop.**
  An overdub used to write to the nearest sample under the head. Below 1x
  several input samples landed on the same spot, so SOS was applied two
  (0.5x) or four (0.25x) times per pass and the input was sampled down with
  no filter. Above 1x, spots were skipped and never faded. It sounded like
  a sample-rate mismatch that got worse with every pass. The head now writes
  every spot it passes exactly once, like tape, with the input filtered
  below 1x. At exactly 1x nothing changes.
- **The speed glide now lands exactly on its target.** After any speed
  change, "1x" used to settle at 0.9999x, which made the head drift and
  set off the problem above even when the dial read 1.

## 0.8.1

- **bars** actually reaches the engine. In 0.8.0 the control was connected
  to an input the engine never opened, so a counted take would not have
  closed itself (or the unit would not have added).
- A counted take that you start while no clock is running now counts from
  the moment you pressed **rec**, instead of closing on the first pulse when
  the clock comes back.
- A counted take longer than the buffer closes where the buffer ends and
  no longer inherits the previous loop's bar count (which pulled the head
  back to the start at the wrong moments).
- A clocked take that ends right at the end of the buffer now gets its
  seam, and overdubs on it can be undone.
- **Attach pool buffer…** and preset loads rebuild the undo buffer to match
  the loop buffer's length and channels. Undo on a mismatched buffer is
  switched off rather than writing past the end of the undo buffer (a
  possible crash in a mono chain with a stereo buffer attached).

## 0.8.0

- New **bars** control (0–64, takes CV). In *sync* mode with bars set, one
  tap of **rec** records exactly that many clock pulses and closes the loop
  by itself; the Reel counts 1/4, 2/4 … while it records and the sub display
  reads *bar X/Y* as the loop plays (this replaces the old *N=* readout). A
  second tap still closes early on the next pulse. Bars 0 is the manual
  two-tap take as before; free mode and overdubs ignore it.

## 0.7.2

- Clear loop now also silences the buffer, so the waveform view goes flat
  along with the Reel instead of showing the old take.

## 0.7.1

- The seam no longer clicks on low-pitched or very pure material (a looped
  triangle wave showed it). When the loop closes, the phase-matched cut is
  now searched over the last ~40 ms before your press instead of ~10 ms, so
  one full period of anything down to bass fits the search.
- The cut prefers the point nearest your press among the good matches, so a
  loop is trimmed by at most about one period, not several.
- Short loops (down to about 25 ms) are phase-matched too; before, they were
  cut where pressed.

## 0.7.0

- Removed: **start**, **len**, the *Start/Len: sections* grid and the
  free-mode "jump to start" on a clock pulse. The unit is back to the
  Morphagene grammar — one loop, one head, speed and direction. Slicing
  belongs to Dirac.
- **speed** now runs −2 … +2 (was −4 … +4). Coarse dial steps are 0.25, so
  the slowing-down half of the range has real resolution; detents at ¼, ½,
  1, 2 and 0. V/oct is capped at 2x.
- The sub-display speed scale shows ¼ · ½ · 1 · 2.
- A clock pulse in *free* mode is a plain restart of the loop (skipped when
  the head is already at the seam, so a loop in time with its clock never
  retriggers itself). *sync* mode is unchanged.
- Presets saved by 0.6.x load fine: a saved window is ignored (the whole
  loop plays) and a saved speed above 2 lands at 2.

## 0.6.6

- Clock jumps behave like a sampler retrigger: the new position ramps in
  over 1 ms (attacks survive) while the old material tails out over 8 ms
  (no click on tonal material).

## 0.6.5

- Fixed an audible click on every clock pulse in *free* mode. A pulse that
  lands where the head already is no longer retriggers; other jumps fade
  properly.

## 0.6.4

- Loop persistence works on the device: a preset saved with a loop comes
  back playing it. The menu's status line now says what the loop file did
  ("saved …", "loaded N frames", or the reason it did not load).

## 0.6.3 / 0.6.1 / 0.5.1

- The word in the middle of the Reel (rec / REC / DUB / EXT / STOP / undo) is
  centred to the pixel and stays put while the unit scrolls on and off
  screen.

## 0.6.2

- Every step of saving and loading the loop file reports on line 4 of the
  menu sub display, and a "Reload loop file" task re-runs the load on demand.

## 0.6.0

- *(Superseded in 0.7.0.)* Start/Len could snap to the eight sections, and a
  free-mode clock pulse jumped to the start target.

## 0.5.0

- **clk** input. *free*: restart the loop on each pulse. *sync*: **rec**
  waits for the next pulse to start and to stop, so the loop is a whole
  number of clock periods (the Reel's REC blinks while it waits); every
  N-th pulse pulls the head back to the downbeat even at 2x or in reverse;
  the section ticks become the beats. Unplug the clock for four seconds and
  rec is immediate again.

## 0.4.0

- **undo**: swaps the last overdub or extend pass out, bit-exact, over a few
  blocks (the Reel says "undo" while it works); tap again to redo. A new
  pass replaces the undo point. The 60 s buffer doubles in memory for this.
- **stop** (latched): fades the loop out over 5 ms and holds the head; off
  again resumes from the same spot. Overdub writes pause while held.

## 0.3.0

- **The Reel**: the loop drawn as a ring whose thickness is the loudness at
  each point, 12 o'clock the seam, eight section ticks, a head dot with a
  comet tail that grows the further speed is from 1x (an octave away is
  four pixels) and turns hollow on a musical ratio. Recording winds the ring
  on from 12 o'clock; Extend winds new material on outside the loop.
- Sub display: speed scale with the same hollow-on-detent marker, ratio and
  semitone offset, loop length and section number.
- Magnetic detents: within 3 % of a musical ratio or 0, speed snaps to it.
- A gentle anti-alias filter engages above 1x.

## Before 0.3.0

- 0.2.0: Start/Len window (removed in 0.7.0), **ext** (an overdub past the
  end grows the loop), **V/oct** into speed, loop saved to
  `ER-301/noether/` with the preset.
- 0.1.1: the phase-matched seam — the close point is searched for the cut
  that best matches the start of the loop, so periodic material wraps
  without a click.
- 0.1.0: record / close / play / overdub / clear, sample-accurate close,
  continuous speed and direction, **sos** crossfader, **dry** / **level**,
  buffer length menu.
