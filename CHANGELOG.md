# Noether — changelog

What changed for players, version by version, from 0.3.0 (the first build
with the Reel) to the current release. The engineering detail behind each
entry lives in `memory.md`.

## 0.8.1 — current

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
