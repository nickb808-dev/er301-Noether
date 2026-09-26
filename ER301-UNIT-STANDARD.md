# The ER-301 unit standard

**This file is mirrored, byte for byte, in every unit repo** (`dirac`,
`landau`, and whatever comes next). It is the accumulated cost of building
them — every rule below is here because breaking it produced a specific,
diagnosable failure on real hardware, and each one names that failure so it
is obvious what you lose by ignoring it.

Read this before writing a line of a new unit. Then read the most recent
unit's `Landau.h` / `Landau.lua` and copy their shape.

*Maintained by: the unit repos themselves. If you learn something new, add it
here first, then mirror the file into the other repos in the same commit.*

---

## 0. The one-sentence version

**The Lua layer is not a language you can guess, and the audio thread is not
a place you can do work.** Almost every failure so far has been one of those
two mistakes.

---

## 1. Lua: copy, never invent

Every ER-301 unit failure that has ever *bricked the load* came from
assuming an API existed.

> `Landau.lua:33: attempt to call a nil value (method 'addGainBiasBranch')`
>
> The `.so` had loaded fine. The C++ was never at fault. Four more invented
> APIs were found in the same audit.

**The rule: if an `app.*` or `self:*` call does not already appear in a
shipped unit's Lua, you may not use it.** Verify mechanically before every
release:

```bash
grep -oE '\b(app\.[A-Za-z]+|self:[a-zA-Z]+)' assets/NewUnit.lua | sort -u > /tmp/new
grep -oE '\b(app\.[A-Za-z]+|self:[a-zA-Z]+)' ../dirac/assets/Dirac.lua | sort -u > /tmp/known
comm -23 /tmp/new /tmp/known      # must be empty, or every line justified
```

The idioms that are actually proven:

| Need | The only correct shape |
|---|---|
| A CV-able parameter | `app.GainBias()` + `app.MinMax()` + `connect()` + `self:addMonoBranch()` |
| 1 V/oct pitch | `app.ConstantOffset()` + the `Pitch` view control — **never** a GainBias |
| A gate / trigger | `app.Comparator()` + the `Gate` view control — **never** a dial |
| Choose a file from the card | `SamplePool.chooseFileFromCard(self.loadInfo.id, cb)` |
| Attach a sample that may be MULTI-FILE | `head:setSample(sample.pSample, sample.slices.pSlices)` — the slices carry the per-file boundaries |
| Persist an attached sample | `SamplePool.serializeSample` / `deserializeSample` in the unit's `serialize`/`deserialize` |
| Choose from the pool | `SamplePoolInterface(self.loadInfo.id, "choose")`, then `subscribe("done")` |
| Anything expensive from the menu | `Task { task = function() ... end }` — see §2 |

A branch with no view control is unpatchable and therefore a bug — Landau's
`fm` shipped that way once. Every branch you add needs a control.

## 1b. When you need an SDK type across the SWIG boundary

Landau needed `setSample(od::Sample*, od::Slices*)` — the multi-file attach —
but its header hides every `od::` include behind the SWIGLUA guard.

Forward-declare the types **above** the guard:

```cpp
namespace od { class Sample; class Slices; }   // outside #ifndef SWIGLUA
```

SWIG treats them as opaque pointers and matches them against the wrappers the
SDK's own module already generates. The full definition goes in the `.cpp`
(`#include <od/audio/Slices.h>`), which SWIG never parses.

The alternative is to inherit from the SDK class that already declares the
method — `od::SliceHead`, which is how the stock SingleCycle gets it. That is
more proven, but it also inherits that class's inlets and options whether you
want them or not. Weigh the baggage against the risk.

## 1c. Copying a CALL is not copying an INTERFACE

`SingleCycle.lua` shows the multi-file attach in one line:

```lua
osc:setSample(sample.pSample, sample.slices.pSlices)
```

Landau copied it and locked up the device with no crash log. The line was
right; everything around it was missing, and none of it is visible from the
call site:

- **`od::Slices` is `ReferenceCounted`.** `SliceHead::setSample` calls
  `attach()` on the new list, `release()` on the old, and releases again in
  its destructor. Using it without a reference means using memory someone
  else may free.
- **`od::Slices` is `Lockable`, and its "read" accessors write.**
  `getIntervalCount()` calls `refreshIntervals()`, which rebuilds two
  vectors. Unlocked, on an object shared with the sample pool, that corrupts
  it.

**When adopting an SDK type, read how the SDK's own code USES it** — the base
class's `.cpp`, not just the header or the call site. Ownership, locking and
lifetime never appear in the one line you copied.

**And read the usage BEFORE inferring an obligation from the header.**
`od::Slices` is `Lockable`, and `getIntervalCount()` internally writes — from
which Landau concluded it must hold the mutex. But `SingleCycle::process()`
calls that same accessor **unlocked, on the audio thread, every block**. The
obligation did not exist; the "fix" only added a deadlock risk. Headers
describe what is POSSIBLE, call sites describe what is EXPECTED. The
reference counting was real and the locking was not, and only the
implementation could tell the two apart.

And note what this failure mode does to your evidence: an unlocked write is a
**hard fault, and a hard fault writes no log**. A Lua error always produces
one. So "the crash log is stale / empty" is not an obstacle to diagnosis — it
is itself the diagnosis, and it says the fault is below Lua.

## 1a. If Lua calls it, SWIG must be able to SEE it

The engine header wraps its `od::` members in `#ifndef SWIGLUA`, because SWIG
cannot parse them. **SWIG binds only what it can see.** A method declared
inside that guard compiles, links, packages and installs perfectly — and is
simply absent from Lua:

> `Landau.lua:123: attempt to call a nil value (method 'getCutMode')`
>
> A crash on the device, the instant the user opens the menu, with nothing
> wrong in the C++. Landau shipped it in v0.1.3, v0.1.4 **and** v0.1.5 —
> three releases, because a build, a full test run and a code read all pass.

This is the mirror image of §1. There, the Lua named something that did not
exist; here, the C++ has it and the binding layer silently drops it. Both
produce a nil method call, and neither is caught by anything but running it.

So the header has an explicit, labelled section **above** the guard:

```cpp
class Landau : public od::Head {
public:
    Landau();
    virtual ~Landau();

    /* ── THE LUA-CALLABLE API — everything Lua calls must be declared here ── */
    void setCut(int mode);          // declaration only, plain types only:
    int  getCutMode();              // a body would touch members SWIG cannot parse

#ifndef SWIGLUA
    ...everything else...
```

Methods inherited from `od::Head` (`setSample`) or `od::Object` (`getOption`)
are already wrapped by the SDK and do not belong there.

**Declare EVERY arity you want, including ones you inherit.** SWIG does not
merge a derived declaration with an inherited overload — it shadows it.

> Landau declared `setSample(Sample*, Slices*)` for the multi-file attach. The
> generated wrapper read `SWIG_check_num_args(...,3,3)` and the inherited
> one-argument `od::Head::setSample` **vanished from Lua**. Declaring both
> arities makes SWIG emit a proper dispatcher.

Read the generated wrapper when you add anything to the Lua-callable section.
It is where a declaration becomes a binding, and neither the compiler nor the
test suite looks there. And if moving a declaration out from behind the guard
costs you an `override` keyword, replace that check with a test that calls
through a base-class pointer — otherwise a signature drift silently runs the
base version.

**Gate it in the Makefile.** `tools/check-swig-api.sh` extracts every
`head:method()` the Lua calls and fails the build unless each one is outside
the guarded regions. `make pkg` and `make hosttest` both depend on it, so a
package cannot be produced with this bug in it. The script is mirrored
alongside this document; copy it into every new unit.

(Two notes from writing it: a header has **two** `SWIGLUA` guards — one around
the includes — so the check must track nesting rather than cutting at the
first match. And verify the checker against the real bug by reintroducing it;
the first attempt at that test silently patched a *comment* that mentioned the
guard, and reported a false pass.)

## 2. The audio thread does no work. None.

`process()` runs every 128 samples: **2.67 ms at 48 kHz, shared with every
other unit in the chain.** The temptation is always the same — an option
changed, so rebuild something.

> Landau's `Table` option originally rebuilt all 512 waves inside
> `process()`: ~100 ms, roughly 40 audio callbacks, in one block.
> Amortizing it over 64 blocks was tried next and a single block **still**
> cost 3.3 ms on a desktop x86 — worse than the entire budget, before you
> account for the ER-301's ARM being far slower.

Amortization is not the fix. It only shrinks the constant. **The fix is to
restructure so the work does not exist.**

Landau's version of that: the cube is stored once as a 2048-point wave plus a
band-limited chain of halvings, and `Table` just chooses which level counts as
the base — because a mip level of *n* points already *is* an n-point table.
The expensive operation became an integer.

When work genuinely cannot be removed, move the *thread*, not the schedule:

- **`setSample()` runs on the Lua/UI thread.** A synchronous rebuild there is
  fine, and both units do exactly that.
- **A menu `Task` runs on the Lua/UI thread.** So anything costly that the
  user triggers from the menu must be a Task, not an `OptionControl` —
  an OptionControl's new value is only ever observed by `process()`.
  (This is why Landau's *Cut at* is three menu entries rather than a toggle.)

The audio thread may read a table while the UI thread rewrites it. That is
acceptable **only** when every buffer is pre-allocated and every index stays
in range, so the worst case is a moment of mixed content rather than a fault.
Which leads directly to:

## 3. Allocate once, for the worst case

Never resize a buffer the audio thread might be reading — that is a
use-after-free, not a glitch. Size for the maximum at construction, and let
options select a *prefix* or a *level* of what is already there.

**But do not allocate for a setting nobody has chosen.** Landau stored a
2.00 MB mip level — half its entire memory — that only its 2048 table setting
ever read, so it was dead weight at the default. The resolution is to allocate
it lazily from a menu **Task** (§2), in this order:

```
allocate  ->  mark it present  ->  fill it  ->  only THEN point the reader at it
```

and **never free it again**, because a block already in flight may be reading.
Paying once per session for a setting you actually chose is fine; paying
always for one you did not is not.

Where a rarely-used level has to be derivable, stage the source in a small
shared buffer and build from that, so the result is bit-identical whether or
not the optional level exists.

## 3c. A sample is not ready when you get it

**This is the one that cost a week.** Landau could not attach ANY sample —
single file or folder — and the device froze with no crash log. Eight
hypotheses missed it.



`od::Sample` carries two counts:

```cpp
uint32_t mSampleCount;       // what the buffer was ALLOCATED for
uint32_t mSampleLoadCount;   // how much has actually been LOADED
```

They differ while a file is still coming off the card. **Streaming units never
notice** — they read a few samples per block, long after the attach. A unit
that reads the WHOLE buffer in the attach callback lands straight in the gap.

> Landau used `mSampleCount` and could not load a single file, let alone a
> folder. Seven code-reading theories missed it; the SDK header says it
> plainly.

Use `mSampleLoadCount` as the length, and if it is short, **wait** — poll on
the UI thread and build when the file has arrived. And if your unit builds
from a sample rather than streaming it, assume every difference between it
and the stock units matters, because that is the class of bug you are in.

## 3b. Copy what you keep; never hold a pointer into firmware memory

If a unit reads an attached sample ONCE and bakes it into its own tables, copy
the audio out at attach time. Do not retain `mpSample->mpData` and read it
again later.

> Landau kept that pointer and re-read it from `setCut()` and every rebuild.
> The buffer belongs to the sample pool: it can be freed, moved, or still be
> loading. A stale read is a hard fault, and a hard fault writes no log.

The copy is bounded by what you can actually address — Landau needs at most
512 waves x 2048 points, 2 MB as int16, whatever the file size — and it makes
dangling, reallocation and async-load races impossible rather than unlikely.
Resample each cycle to a uniform length as you copy: the store becomes a plain
grid, and unequal-length sources stop being a special case.

Streaming units do not have this problem, which is why copying from one is not
enough (§1c).

## 3a. Deriving the same geometry twice is an out-of-bounds waiting to happen

If one function validates a buffer and another re-derives its length, they
will eventually disagree — and the one that runs in a loop is the one that
walks off the end.

> Landau's `prepareSource` checked an attached sample's pointer, channel
> count and length. `buildSampleWave` then read `mpSample`'s fields again
> directly, once per wave, 512 times.

Validate once, publish the validated view as members, and make every consumer
use only those. Nothing downstream should be able to see the raw header.

**This matters most for units that SNAPSHOT rather than stream.** A streaming
unit reads a few samples per block and survives being slightly wrong; a unit
that sweeps a whole buffer to bake tables turns a wrong length into a fault
that takes the device down with no log written.

## 4. Defaults must be exact bypasses

A control at its default must be *bit-identical* to the feature not existing.
Not "close" — identical. It makes every feature independently verifiable and
makes regressions obvious.

This is a design constraint, not just a test: Landau's roton family exists in
the form it does partly because depth 0 gives a linear dispersion curve, which
is a pure time shift, which leaves the waveform untouched. A physically
meaningful bypass.

## 5. Sanitize every input, every block

`-ffast-math` is on. It **optimizes away `x == x`**, so the usual NaN idiom
silently does nothing. Use a bit-test on the exponent:

```cpp
static inline float sanitize(float x);   // bit-tests the exponent field
```

Rules:

- Sanitize **at the read**, not deep in the DSP: `sanitize(mFooIn.buffer()[0])`.
- Order matters: `softLimit(sanitize(x))`, never the reverse.
- `std::min`/`std::max` do **not** reliably clamp NaN, and `int(NaN)` is
  undefined behaviour. Clamping is not sanitizing.
- **Accumulators need their own guard.** A NaN that reaches a phase or filter
  accumulator is *sticky* — it survives forever and the unit stays broken
  until it is deleted. Guard with a positive test, which is false for NaN:
  `if (!(p >= 0.0 && p < 1.0)) p = 0.0;`

There is a `nan` test suite for this: poison every input port with NaN and
inf in turn, require the output to stay finite, then require the unit to
recover. Copy it.

## 5a. The ARM has no hardware integer divide

The ER-301's Cortex-A8 has **no integer divide instruction**. Every `%` and
every integer `/` is a library call (`__aeabi_idivmod`), tens of cycles.

> Landau's mip builder ran `j %= nPrev` inside its tap loop: 33 taps × 4080
> outputs × 512 waves = **69 million modulos**, and most of a 70 ms load time.
> Changing it to `& (nPrev - 1)` — every length there is a power of two — was
> the single largest optimization in the unit.

**And the host cannot see this.** x86 divides in hardware, so a loop full of
modulos benchmarks fine on your machine and stalls the device. Landau's mip
builder was fixed for exactly this — and then a new resample loop reintroduced
it (2.1 million modulos at load) in the release that moved the failure to load
time. **Grep for `%` and `/` after writing any new loop, and put a timing gate
on every path that can run long, not just the ones you already fixed.**

So: **keep buffer lengths powers of two and wrap with a mask.** Where a length
is genuinely arbitrary (a file length), walk it monotonically and wrap with a
conditional subtract instead. Hoist float divides out of loops as a
reciprocal. Grep for `%` and `/` in anything the audio thread or the
constructor touches — it is the first thing to check when something is slow.

## 5b. The display can cost more than the audio

`process()` runs 375 times a second. The panel refreshes at about 30. Anything
recomputed for the display every block is doing roughly 12× the work it needs.

> Landau redrew 128 trilinear cube reads plus two 128-point neighbour waves
> every block — about three times the cost of the audio it was illustrating.
> Throttling to every 12th block (~31 fps) more than halved total CPU.

Throttle the expensive part; keep cheap scalars (cursor positions, levels)
live every block. And expect this to break tests that read display state after
a single `process()` — that is the throttle working, so give the harness a
`settleViz()` helper rather than removing the throttle.

**Then stop.** Once throttled, the display is a single-digit share of the
total — measured on Landau at 7%, where deleting it entirely would have saved
less than a third of what one hoisted expression in the audio path did.
Simplifying the graphics further trades real information on screen for
almost nothing. Measure against a display-free build before you cut anything.

## 5c. Hoist anything that is constant for the block

Controls are read once per block. If a derived quantity depends only on
control values, it is a block constant — computing it inside a per-sample or
per-corner loop multiplies it by up to a thousand.

> Landau's `Res` recomputed its polyline point count and scale inside
> `readWave` — up to 1024 times a block, from a value that changes once.
> Hoisting it cut the worst-case block cost by 22%.

Profile **per control**, not just overall: the expensive one is rarely the one
you would guess, and an overall figure hides it.

## 6. Build tables once — and measure them

Windows, mip chains, dispersion curves, lookup tables: built in the
constructor, never in the render loop.

**Know the closed form.** Landau's Chebyshev family computed
`cos(ord * acos(cos(ph)))` — an `acosf` per sample, the most expensive call in
the builder — for an expression that is *identically* `cos(ord * ph)`, because
`T_n(cos x) = cos(n x)` is the definition of the polynomial. And because a
single-cycle wave is exactly N points with N a power of two,
`sin(2*pi*h*i/N)` is an exact table index, not an approximation. Most build
loops do not need a transcendental at all.

**Dump and diff whenever you "optimize" a builder.** Landau's rewrite was
verified by dumping all 512 waves before and after: five families came out
bit-identical, two within one int16 LSB — and one, the quantizing family, had
changed audibly, because a 1e-7 interpolation difference flips a sample into
the next quantization level. A test suite that still passes is not evidence
that the sound is unchanged.

**Test the feature on the material it will actually meet.** Landau's warp
control was rewritten twice. Version two passed a convincing test on a sine
and moved a sawtooth's spectral centroid by 5% across the entire dial — it
did nothing on the waves anyone would really play, and would have shipped as
the same "this control does nothing" complaint that prompted the rewrite. If
a feature is for a wavetable oscillator, test it on rich waves, not on the
one waveform that flatters the algorithm.

**Measure LEVEL, not only spectrum.** The first version of that control
halved the output and emptied the fundamental. The spectrum analysis said it
was working; on the device it read as broken, because a control that mostly
turns the volume down is indistinguishable from one that does nothing. Assert
that a shaping control preserves loudness.

**Use the right instrument for the claim.** Two ways Landau's warp test lied
before it worked: a fixed "high band" reference, when the feature's whole
point is a peak that MOVES (at low settings it sat inside the reference band,
so the result read as "darker"); and probing a harmonic spectrum on a linear
grid, where the probes land between the harmonics and the answer depends on
where they happen to fall. Evaluate the DFT at exact multiples of f0 instead.
Then assert the actual claim — "the peak is at harmonic 1/k" — not merely
that something changed.

**A threshold near the measurement floor is not a threshold.** Landau's
rebuild guard compared the worst block against "8x a steady block" — but a
steady block is about one clock tick, so the bar was eight ticks, inside
scheduler noise, and the test flapped. Express the limit in terms of what you
are actually protecting (a fraction of the audio budget), not as a multiple of
a quantity that rounds to 1.

**When two readings of the data are genuinely indistinguishable, do not add
a heuristic.** Landau can be handed a file that is either 256 frames of 2048
samples or 512 of 1024 — same length, no signal that separates them. Two
detectors were measured (a divisor search and a straight pairwise comparison
between the only two candidates); the pairwise one went 0 for 4 with two exact
ties. The answer is not a cleverer metric: pick the reading that is more often
right, make the override cheap and STICKY, and display what was decided so a
wrong guess is visible rather than merely audible.

**When you have shipped a hypothesis and a test is pending, WAIT FOR THE
TEST.** Landau's sample loading was fixed by waiting for `mSampleLoadCount`
— and one turn later I argued the fix "overstated", reasoning that an
allocated-but-unloaded buffer returns zeros and therefore cannot fault. The
logic was clean and the conclusion was wrong; the pending test vindicated the
original fix. A chain of "therefore"s about a platform you have already
misread eight times is not evidence, and it is least trustworthy exactly where
it feels most confident.

**Ask for the experiment that PARTITIONS the space, not the one that confirms
your guess.** Landau's sample loading survived seven code-reading theories.
What broke it open was two five-minute tests: *does a single file also fail?*
(killing the entire multi-file hypothesis) and *does another unit load the
same input?* (killing firmware, card and selection). Both are questions about
what the FAILURE does, not about what the code says, and either could have
been asked on day one.

**A diagnostic you can only reach through the broken path is not a
diagnostic.** Landau printed the detected bank geometry on the menu's sub
display — and the menu was the thing that crashed. It sat there useless for
seven versions. Put the state that explains a failure somewhere reachable
*without* the feature that fails.

**Anchor version bumps; never sed globally.** `s/0.2.10/0.2.11/g` across the
Lua rewrote a comment about a DIFFERENT project's version. Match on
`VERSION :=`, `version =`, `local VERSION` — and grep afterwards for the new
number in places it has no business being.

**Assert on the anchor of every scripted edit.** A `str.replace` whose
anchor does not match changes nothing and reports nothing. In this codebase
that produced a full green test run for a suite that was never compiled in —
the edit had silently no-oped on a `printf` string that had since gained a
`(%d issues)`. Every scripted edit that carried an assert caught its own
mistakes; the one that did not, did not.

**Probe where the function varies.** Landau's external-addressing test first
compared the output at two different input values and found them equal —
because both landed on zeros of a sine. It "passed" a case that was in fact
broken, and the real bug (the top of the input range wrapping to the start of
the wave) only surfaced when the probe moved to a monotone waveform. A test
point where the thing under test is stationary proves nothing.

And then **measure**, because plausible DSP is often wrong:

> Landau's mip filter started at 9 taps. It looked reasonable. It cost 4 dB
> of 7–16 kHz at 50 Hz and made the 2048 setting quieter than 1024. 33 taps
> fixed both, and the test now prints the dB figure every run.

Tests that print a number you can argue with beat tests that print PASS.

## 7. `od::OptionControl` shows a **maximum of three choices**

Confirmed from the firmware source: each choice maps to a physical button
inside a box `(3 + descriptionWidth) * ply` wide. A fourth choice is invisible
*and* unpressable — the option silently becomes unusable on the device.

If you need more than three, it is a dial (Dirac's `Window`, 7 shapes) or a
set of Tasks (Landau's `Cut at`). Both are proven.

## 8. Display: one space, layered

Each unit gets one graphic area and it should answer more than one question at
once, layered by brightness rather than split into panels:

- Draw **post-everything** — after blending, after degradation. What you see
  must be what you hear, stairs and all.
- One idea bright (`WHITE`), context faint (`GRAY5`/`GRAY3`).
- Show **only what the user is currently touching.** Landau ghosts the
  neighbours of the axis being moved, not all three axes: three ghost pairs at
  once is noise on a 4-bit panel. Label which one it is.
- Adapt to width. A view may be built at any number of `ply` slots, so scale
  cells and drop optional furniture rather than overflowing.
- **Small glyphs read better crisp than glowing.** A stem glow was added to
  Dirac and removed again on sight.
- Only these `od::FrameBuffer` primitives exist: `clear` `blend` `pixel`
  `line` `hline` `vline` `box` `fill` `invert` `text` `vtext` `circle`
  `fillCircle` `arc8` `drawUpTriangle`. `hline`/`vline` take a `dotting`
  argument; `text(color, x, y, str, size = 12, align)`.
- Shift-tap to page the sub display is free on a display-only ViewControl —
  no gain/bias readouts means no collision with the firmware's shift bindings.
  Still distinguish a shift *tap* from shift-as-a-modifier (`shiftUsed`).

## 9. `const` is a build hazard

`od::Option::value()` is **non-const** in the device SDK. A `const` accessor
wrapping it compiles happily on the host and fails inside Docker.

Keep the host stubs signature-identical to the SDK, and when they disagree,
**change the stub to match the device** — never the reverse.

## 10. The host harness is the real test rig

Hardware round-trips are slow and hardware bugs are hard to see. Every unit
gets a `test/host/main.cpp` with named modes, a fake `od::` layer, and:

- a **stress/`asan` mode** that sweeps every control to its extremes while
  toggling every option, and counts non-finite and out-of-range samples;
- a build under `-fsanitize=address,undefined`;
- a **`nan` mode** (§5);
- a **`rebuild` mode** that times a block during every option transition and
  fails if any of them costs meaningfully more than a quiet block (§2);
- **bit-identity checks between versions** when a change is meant to be a
  refactor;
- a **`cpu` mode** that prints construction time and per-block cost and fails
  on regression. Print the numbers — a threshold nobody can see gets raised
  quietly until it means nothing.

Verify your own tests too. A zero-crossing harmonic counter was wrong twice
in Landau — it omitted the wrap from the last sample back to the first, so
harmonic *h* counted as *h−1*. It passed for months behind a ±1 tolerance
and was only exposed when a second test used it without one. **Tolerances
hide test bugs; assert exact values where the maths allows it.**

## 10a. Two features failing at once means suspect the BUILD

Independent bugs do not usually appear together. If two unrelated features
both misbehave on the device and both pass on the bench, the most likely
explanation is not two coincident bugs — it is that **the device is not
running what you are testing.**

> Landau 0.2.3: warp inaudible and a wavetable folder loading blank, both
> green in 16 host suites. An hour went into auditing the Lua plumbing, the
> inlet registration and the Makefile dependency graph — all correct. The
> cause was the firmware serving a **cached .so** (§11).

Ask "is the device running this binary?" *first*. It is one question, it is
cheap, and every other line of investigation is worthless until it is
answered. Bump the version, confirm the package installed, then debug.

## 10b. Ship exactly one package; verify the log describes YOUR build

The firmware scans its packages folder and will install an old build if one is
there. Two habits stop a week of phantom debugging:

**Prune the output directory.** `make pkg` must leave exactly the version it
just built. Landau's build directory accumulated sixteen installable `.pkg`
files (`clean` did not touch them), and the device was found running 0.1.5
while the source tree sat at 0.2.8 — every reported symptom was that old
build's known, already-fixed bugs. Make the cleanup failure-tolerant, so a
cleanup step can never fail a build.

**Fingerprint the log before believing it.** A crash log names a file and a
LINE NUMBER. If the function it blames does not live at that line in your
source, the log is describing a different binary — stop and establish which
one before reading another line of code:

```bash
md5sum crash.log                       # same file re-sent?
grep -oE 'pkg-[0-9]+\.[0-9]+\.[0-9]+' crash.log | sort -u   # which versions ran?
sed -n '123p' assets/Unit.lua          # is the blamed line even that call?
```

## 10c. The unit must state its own version, and be unable to lie about it

A unit that cannot say what it is cannot be supported at a distance. Print the
version somewhere reachable **without** opening the menu — the menu is often
what breaks — and gate it:

- `local VERSION = "x.y.z"` in the Lua, shown in a menu header AND a display
  page;
- `tools/check-version.sh` fails the build unless the Makefile, `toc.lua` and
  every Lua that states a version all agree.

A displayed version that can drift from the packaged one is worse than no
version at all.

**Know where packages actually live.** On the ER-301 the `.pkg` on the card is
only an installer; the installed copy sits in INTERNAL storage under a
firmware-version directory (`0:/v0.7/libs/<unit>/`). Deleting `.pkg` files
from the card uninstalls nothing, and a firmware update changes that path
while leaving the old tree behind. "Reinstall it" is not a diagnosis.

## 11. Version discipline

The firmware **caches the `.so` per version**. If you flash without bumping,
you are testing the old binary and will chase a ghost.

Bump `VERSION` in the `Makefile` **and** `version` in `assets/toc.lua` on
every single flash. Check the header comments in both — they drift.

## 12. Repo hygiene

- Apache-2.0 + `NOTICE`. §4(d) is the reason: it makes the attribution travel
  with binaries, which is the point.
- **No submodules.** Vendor the engine with a provenance document saying where
  it came from and at what version.
- One `memory.md` per repo: what was decided, why, and what it cost. This file
  is for rules that outlive any single unit; `memory.md` is for that unit's
  history.

---

## The failure index

Every rule above, indexed by the symptom that taught it — so a future symptom
can be looked up rather than rediscovered.

| Symptom on hardware | Cause | Rule |
|---|---|---|
| Unit fails to load, Lua nil-method error | Invented Lua API | §1 |
| Crash on opening a menu, nil-method error, C++ looks right | Method hidden inside `#ifndef SWIGLUA` | §1a |
| Freeze / audio dropout when changing a menu option | Rebuild on the audio thread | §2 |
| Crash when changing a table-size option | Reallocation under the reader | §3 |
| Unit goes silent and never recovers | Sticky NaN in an accumulator | §5 |
| A setting sounds duller or quieter than it should | Under-designed filter, never measured | §6 |
| A menu option's later choices cannot be selected | More than three `OptionControl` choices | §7 |
| Builds on the host, fails in Docker | Host stub `const` where the SDK is not | §9 |
| Changes have no effect after flashing | Version not bumped; firmware served the cached `.so` | §11 |
| Two unrelated features both broken on device, both green on the bench | Stale binary — suspect the build before the code | §10a |
| A crash log blames a line where that call no longer lives | The log is from a different build — check versions before code | §10b |
| Fixed bugs reappearing | An old `.pkg` still installable on the card | §10b |
| Reinstalling does not change behaviour | The installed copy lives in internal storage, not on the card | §10c |
| Device locks up hard and NO crash log appears | The fault is below Lua — suspect ownership/locking of an SDK type | §1c |
| A loaded bank goes silent with no error | Snapshotted a sample buffer that was empty | §3 |
| Feature works, but its "off" position colours the sound | Default is not an exact bypass | §4 |
| Slow to load, or high idle CPU | Integer `%` / `/` in a build or audio loop | §5a |
| High CPU that scales with nothing musical | Display recomputed every block | §5b |
| A control exists but cannot be patched | Branch with no view control | §1 |
| A test passes on a case that is visibly broken | Probe point where the function is stationary | §6 |

## §12 — Attaching a sample: what the firmware actually guarantees

Learned the hard way on Landau, over about fifteen wrong hypotheses. Anything
that reads an attached buffer rather than streaming it must obey all four.

**a. The sample you are handed is empty.** `Sample.Pool.load()` allocates the
buffer (`prepareForLoading` → `allocateBuffer`), queues the file, and returns.
The bytes arrive later on a background job queue driven by
`Timer.every(0.3, updateQueue)`. `mSampleLoadCount` is 0 when your `setSample`
runs, every single time. `mSampleCount` is what was ALLOCATED; only
`mSampleLoadCount` is what has ARRIVED. (`setMemoryOnly()` equates them, which
is why in-memory buffers and host tests never show this.)

**b. Wait by SIGNAL, not by polling.** The pool announces completion with
`Signal.emit("sampleStatusChanged", sample)`. Subscribe with
`Signal.weakRegister("sampleStatusChanged", self)` and define
`function Unit:sampleStatusChanged(sample)` — this is what
`Sample.Pool.Interface` does, it needs no unsubscribe, and it holds no
reference, so a deleted unit neither leaks nor gets called. Guard with
`sample:isPending()`.

**c. Attaching a Sample does NOT keep its audio alive.**
`Head::setSample` calls `pSample->attach()`, which is `ReferenceCounted` on
the OBJECT. `Sample::allocateBuffer()` begins with `freeBuffer()`, which hands
`mpData` back to the BigHeap. So a holder that is still validly attached can
find the audio freed or replaced by a shorter one. **Never cache a data
pointer or a length across calls.** Re-read `mpData` and re-derive the length
from `mSampleLoadCount` at every read, and use the smaller of that and
whatever your own validation decided.

**d. A graphic draws. It does not touch pool memory.**
Deferring expensive work to `draw()` looks attractive — it is off the audio
thread and outside the chooser callback. It is still wrong: `draw()` is a
render callback with no relationship to the sample's lifetime. Do the work
synchronously on the Lua thread, from a menu `Task` or a signal handler, the
way `setCut()` does. Building 512 waves there is fine; the units that work
are not avoiding the cost, they are avoiding the place.

`mSampleCount` counts FRAMES, not floats: `get(i,ch) = mpData[i*mChannelCount + ch]`.

### And a rule about diagnostics

When bisecting a device-only failure with a mode switch, **every mode must be
exactly what it claims**. Landau shipped a "build nothing" mode that still ran
the slice snapshot, and later a rung that would have built identically to
normal after a redesign. Each cost a full round of confident wrong
conclusions. Put the modes under test — the `inert` suite asserts the safe
mode touches no slices, reads no audio, and still makes sound, and that normal
mode does do the work.

## §13 — ReferenceCounted: borrowing vs holding

`od::ReferenceCounted::release()` **deletes the object when the count reaches
zero**. So this, which looks like careful bookkeeping, is a free:

```cpp
thing->attach();
...read from thing...
thing->release();      // count back to 0 -> delete this
```

Anything reachable from Lua may be sitting at a refcount of zero. Landau did
exactly the above around a three-line read of `od::Slices` and destroyed the
list it had just read, which froze every sample load — single file and folder
alike — for twenty versions, with no crash log, because a memory fault writes
none.

The SDK's own pattern (`od::SliceHead::setSample`) is to **hold**:

```cpp
Slices *p = mpSlices;
mpSlices = 0;              // publish nothing mid-swap
if (p) p->release();       // give back the PREVIOUS one
p = incoming;
if (p) { p->attach(); ...use it... }
mpSlices = p;              // hold until replaced
```
…and release in the destructor.

Decide explicitly which you are doing:

- **Borrowing** for the duration of one call, with something else guaranteed to
  hold a reference: don't attach at all.
- **Holding** beyond the call: attach once, release on replacement and in the
  destructor. Never both in the same function.

And model this in the host stubs. A stub whose `release()` is just `--mRefs`
cannot catch it — worse, the suite here asserted `refs() == 0` after an attach,
which encoded the bug as the expected result. Stubs must reproduce SDK
behaviour at the edges (zero, null, partial), not just the happy path.

### §13a — Two facts about samples and slices, verified in the tree

**Every Sample has a Slices.** `Sample:init()` does `self.slices = Slices()`.
There is no "this file has no slice list" case, and `SingleCycle:setSample`
passes `sample.slices.pSlices` unconditionally. Do the same. A defensive
branch here is a workaround for a problem that does not exist, and it will
send you looking in the wrong place when something else breaks.

**Lua-constructed ReferenceCounted objects sit at refCount 0.**
`od/glue/app.cpp.swig` includes `ReferenceCounted.h` with no `%newobject`,
`%delobject` or refcount typemap, and `attachLua()`/`releaseLua()` are never
called anywhere in the firmware. So `app.Slices()` is owned by Lua and its
`mRefCount` is zero — which is what makes a balanced `attach()`/`release()`
pair on it a `delete`.

**Mirror the SDK's arities.** `od::SliceHead::setSample(Sample*)` is
`setSample(sample, NULL)` and nothing else; the two-argument form does the
work. Inverting that — making the one-argument form the real one — means a
one-argument call silently keeps the previous slice list attached and leaks
its reference. If you override a pair of SDK arities, override them in the
same direction the SDK does.

## §14 — The pre-flash assumption audit (REQUIRED before every device build)

A flash-and-test cycle is the most expensive check available and the only one
that needs another person. Landau burned a dozen of them on questions that were
sitting in the SDK source the whole time. So before packaging, write down what
the change assumes and sort those assumptions into two piles.

**Ask, every time:**

1. **What does this change assume about the SDK?** Name each one. Then read the
   source — it is mounted. "SingleCycle calls it on the audio thread so it must
   be cheap" is a guess; `Slices.cpp` is an answer.
2. **Does the stub model what the SDK actually does — at the edges?** Zero,
   null, partial, replaced. A stub is a claim; put the SDK's real body in the
   comment above it. Do not flatten a class hierarchy in a stub.
3. **Anything new crossing Lua↔C++?** Is the method visible to SWIG (declared
   above `#ifndef SWIGLUA`)? If it is INHERITED, does `od/glue/mod.cpp.swig`
   `%import` that base? An unimported base makes SWIG drop every inherited
   method silently.
4. **New base class, port, option, or member?** Diff the names against the
   inherited set. Collisions and shadowing are free to check and silent to hit.
5. **New signal or callback?** Exact-name match (`Signal.emit` does
   `f[s](f, ...)` with no nil check), correct thread, and a lifetime that
   cannot outlive the object.
6. **What single on-device observation would falsify this?** Decide the test
   before building, not after the freeze.
7. **What is left that genuinely CANNOT be checked off-device?** List it. That
   list — and only that list — is what the flash cycle is for. If it is empty,
   you are flashing for confirmation, which is fine, but say so.

**Then record the answers with the version.** The audit is worth as much as the
fix: a wrong assumption that gets written down stops being repeatable. Several
of Landau's bugs were the same misunderstanding wearing different clothes.

The rule this all reduces to: **the device tells you THAT something is wrong.
The source tells you WHAT. Only ask the device what the source cannot answer.**

## §15 — How to read the ER-301's logs (and how to instrument for them)

Landau lost roughly ten debugging rounds to the belief that "no crash log was
written, so the fault is below Lua." The log was being written the whole time.
Everything below is from `xroot/Crash.lua`, `xroot/boot/logging.lua` and
`xroot/LogHistory.lua`.

### Where the report is, and why it looked empty

```lua
local f = io.open(app.roots.front .. "/crash.log", "a+")
```

- **`crash.log` at the ROOT of the FRONT card.**
- **Opened `"a+"` — APPEND.** Every crash is added to the END of the file.
  **Read it bottom-up.** Reading from the top shows the oldest report on the
  card, which is how a live log gets mistaken for a stale one.
- Reports are delimited by `---CRASH REPORT BEGIN` / `---CRASH REPORT END`.
- Written **only if `Card.mounted()`**.

### Confirming the entry is yours

Do not eyeball it — the header carries the answer:

```
Time Since Boot: <s>      Firmware Version: <v>
Boot Count: <n>           Mount Count: <n>
```

Compare **Boot Count** against the current session. That is the freshness test.

### What a report contains

Error message, Lua stack trace, and then — the useful part —

```
Recent Log Messages:
```

which dumps `LogHistory`: a **256-entry ring** fed by Signal from
`app.logInfo` / `app.logWarn` / `app.logError`.

**So `app.logInfo("...")` calls from your unit land in the crash report.** That
is a real instrumentation channel for a failure that produces no visible
output: sprinkle breadcrumbs, reproduce, read the tail of `crash.log`. Use it
before inventing a diagnostic mode — a menu of attach modes costs a flash cycle
per rung, while breadcrumbs cost one.

### Ordering guarantees that make it trustworthy

```lua
local function onError(msg, trace)
  local reportSaved = saveCrashReport(msg, trace)   -- 1. write the log
  showDialog(reportSaved)                           -- 2. then draw, then eject
end
```

The log is written **before** any display work and **before**
`Card.forceEject()`. So a report survives even when the crash screen never
renders and the card is ejected underneath you.

### Reading the symptom itself

`showDialog` ends in the crash event loop:

```lua
while true do
  app.Events_wait()
  ... EVENT_RELEASE_SUB1/SUB2/SUB3/ENTER -> app.reboot()
      EVENT_DISPLAY_READY                -> app.UIThread.updateDisplay()
end
```

**SUB1/2/3 and ENTER reboot ONLY inside this loop.** So:

- **Those buttons reboot the device ⇒ `app.onErrorHook` fired ⇒ it was a caught
  LUA ERROR, and a report exists.** Even if the screen is frozen or blank.
- Truly dead to those buttons ⇒ something below Lua.

That single observation distinguishes "Lua error" from "hard fault" without
guessing, and it is free.

### `app.logFatal` is a deliberate crash

```lua
function app.logFatal(...) error(string.format(...), 2) ... end
```

It raises, so it routes through `onErrorHook` and produces a full report with
the 256 preceding log lines. Useful as an assert when you want the state dump.

**Rule: never conclude anything from an absent log until you have checked the
END of the append-mode file and matched the Boot Count.**

### There is a SECOND log: `<card>/logs/<package>.log`

`xroot/Unit/Factory/init.lua` writes unit **construction** and **registration**
failures to `Path.join(FS.getRoot("logs"), library.name .. ".log")`, i.e.
**`<front>/logs/landau.log`**. Same format (`---ERROR REPORT BEGIN/END`), same
append mode, same LogHistory dump, same Boot Count header. A unit that fails to
instantiate never reaches `crash.log` — it lands here instead. Check both.

### Firmware 9.6.0+ (stolmine): real diagnostics, but OFF by default

`Admin > Settings > General` has two opt-in toggles, **both off**:

- **Enable crash diagnostics?** — trap capture, hang monitor, flight recorder.
- **Enable UI hang detection?** — separate, because a legitimately long
  main-thread operation (preset load, package install, graph recompile) trips it.

Reports appear under `Admin > Crash Reports`. What it adds over `crash.log`:
fault kind with pc/lr/registers and the faulting thread; a module map bounded by
loaded packages so an address inside YOUR package symbolizes; **hang detection
via an audio-thread heartbeat on the am335x watchdog, which an exception hook
can never see**; per-task and ISR stack high-water marks with canaries; heap
pressure and allocation-failure counters; and a flight-recorder ring.

**Arm both before reproducing any hardware-only failure.** Recording is
zero-cost while disarmed, so there is no reason to leave them off while
debugging. This is the tool for exactly the failures that leave no evidence.


## §16 — Lua has no compiler to catch you. Gate it.

A file-local used ABOVE its declaration is not an error in Lua. The name simply
resolves to a **global**, an unset global is **nil**, and you get
`attempt to call a nil value` at runtime — with no syntax error, no load error
and no warning.

Landau shipped this three times in one file:

    Landau:setSample  line 347  calls ok_channels(   declared line 440
    Landau:setSample  line 360  calls bankName(      declared line 445
    (v0.2.24)                   calls trace(         declared line 427

The `bankName` one raised on **every sample attach**, and it long predated the
version where it was finally found. Twenty device tests did not locate it. A
twenty-line shell script found all three in under a second.

`tools/check-lua-scope.sh` is now a hard gate on both `hosttest` and `pkg`.
Declare file-locals near the requires, above everything that uses them.

**The general point: for every class of bug that has cost more than one device
cycle, ask whether a static check could have caught it — and if so, write the
check instead of the fix.** That is how `check-swig-api.sh`, `check-version.sh`,
`prune-old-pkgs` and now `check-lua-scope.sh` came to exist. Each one encodes a
failure that was expensive exactly once.
