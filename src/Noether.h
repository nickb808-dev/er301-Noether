/* Noether.h — Seamless vari-speed loop recorder for the ER-301
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * A stereo loop recorder in the pedal-looper grammar (one REC button: empty
 * -> record -> close and play -> overdub in -> overdub out) whose playback is
 * a fractional-phase tape head: continuous speed from -2x to +2x, 0 = stopped,
 * negative = reverse, pitch follows speed. Clock input quantises recording and
 * re-syncs the loop; free-running otherwise. See LOOPER-PLAN.md.
 *
 * WHAT THE STOCK LOOPERS DO NOT: the stock Pedal Looper closes its loop on
 * 128-sample frame boundaries and only plays at 1x forward; Feedback / Dub
 * Looper are fixed-length punch-in buffers. Noether closes the loop at the
 * exact sample the REC edge landed on, blends the tail of the take into its
 * head so the wrap is continuous, and plays it back at any speed.
 *
 * BUFFER: an od::Sample created from Lua (a pool buffer, like the stock
 * Feedback Looper), so the waveform view, the HeadSubDisplay readout and
 * "save loop to card" all come from the SDK. The engine never allocates.
 * Every block re-reads mpData / mSampleLoadCount from the sample (standard
 * §12c) and validates them ONCE into mCap / mNc before anything reads audio.
 *
 * SEAM (why the loop is seamless):
 *   1. On close, the last kSeam samples of the take are crossfaded INTO the
 *      first kSeam samples (smoothstep), and the loop is shortened by kSeam. The
 *      sample after the loop's last sample is then literally the sample that
 *      was recorded next, so the wrap is continuous at any speed or direction.
 *   2. Every JUMP of the read head (a free-mode clock reset, a sync-mode
 *      downbeat re-sync) ramps the new position in over kJumpIn while a
 *      shadow head tails the old material out over kJumpOut.
 *   3. Record level ramps (5 ms) on overdub in/out.
 *
 * EXTEND: with the ext gate latched on, an overdub that reaches the end of
 * the loop keeps going — the loop plays underneath (one cycle back) while
 * new material is laid on top, and the next REC closes the longer loop.
 * The Echoplex "multiply" / the reference's time-lag accumulation.
 *
 * V/OCT: 1 V/oct into the speed (0.1 = one octave up on the ER-301 scale).
 *
 * PERSISTENCE: exportLoop / importLoop copy the loop to / from a pool sample
 * on the UI thread so Lua can save it to the card at serialize time and
 * bring it back on deserialize (the stock loopers lose their loops).
 *
 * STATUS: see memory.md. Slices / Start-Len windows were removed in 0.7.0;
 * speed is capped at +/-2x.
 *
 * Named for Emmy Noether: a loop that is the same under every transformation.
 * Sibling to Dirac, Planck, Bohr and Landau. */

#pragma once

#ifndef SWIGLUA
#include <od/objects/heads/Head.h>
#include <od/objects/Object.h>
#include <od/audio/Sample.h>
#include <od/config.h>
#include <cmath>
#include <cstdint>
#endif

// Forward declaration OUTSIDE the SWIGLUA guard so SWIG can see the pointer
// type in setSample below (it matches the SDK's own od::Sample wrapper).
namespace od { class Sample; }

namespace noether {

class Noether : public od::Head
{
public:
    Noether(int channelCount);
    virtual ~Noether();

    /* ── THE LUA-CALLABLE API ───────────────────────────────────────────
     * EVERYTHING Lua CALLS ON THE HEAD IS DECLARED HERE, ABOVE THE GUARD.
     * Declarations only, plain types only (standard §1a). */

    // Attach the loop buffer. Overrides od::Head::setSample so the state
    // machine resets when the buffer changes. Same single arity as the base.
    void setSample(od::Sample *sample);
    // Menu Task: drop the loop and return to EMPTY (audio-thread safe: sets a
    // flag that process() honours at the next block).
    void clearLoop();
    // Menu Task, after clearLoop(): silence the buffer so the waveform view
    // shows nothing. Since 0.8.5 this only REQUESTS it: the audio thread
    // zeroes kZeroPerBlock frames a block once the loop has faded out. Until
    // then the Lua thread did od::Sample::zero() — one 5.7 MB memset (30 s
    // stereo) while the audio ran, and the tester heard a click on clear in
    // a busy patch (75 % CPU) but not with Noether alone.
    void zeroBuffer();
    // 0 EMPTY · 1 RECORD · 2 PLAY · 3 OVERDUB · 4 STOP — for the display.
    int   getState();
    int   getLoopSamples();
    float getLoopSeconds();
    // Effective (glided) playback speed, for the display.
    float getSpeed();
    // 1 = when the loop closes, search kSeamSearch samples before the pressed
    // point for the cut that best matches the head of the loop (phase-matched
    // seam for periodic material). 0 = close exactly where pressed (clock sync
    // will set this, since the clock dictates the length). Default 1.
    void setSeamMatch(int on);
    int  getSeamMatch();
    // Loop persistence (UI thread only). exportLoop copies the loop [0, L)
    // into `dst` (a pool buffer of at least L frames); returns frames copied.
    // importLoop copies min(src frames, capacity) into our buffer, sets the
    // loop length and starts playing; returns the loop length, or a negative
    // reason code (see Noether.cpp).
    int  exportLoop(od::Sample *dst);
    int  importLoop(od::Sample *src);
    // UI thread (0.8.7): play the first `frames` of what is ALREADY in the
    // buffer as the loop — for a buffer the player saved to the card at
    // quicksave time and that came back from that file. Returns the loop
    // length, -2 no buffer, -4 too short, -5 the buffer is still loading.
    int  playBuffer(int frames);
    // 1 (default): never mark the loop buffer "unsaved" — Noether saves its
    // loop itself, so its own buffers need not make every quicksave ask to
    // save them (Nick, 2026-10-23). 0 for a buffer the player attached from
    // the pool: that one is theirs, and the normal prompt protects it.
    void setKeepClean(int on);
    // Bumps whenever the loop's content or length changes (close, overdub
    // out, extend, clear, import), so Lua saves only what changed.
    int  getLoopRev();
    // 1 while the effective speed sits on a musical ratio detent
    // (+/-2, +/-1, +/-1/2, +/-1/4, 0), else 0. For the display.
    int  getDetent();
    // Sections the display divides the loop into (8 until a clock exists).
    int  getSections();
    // Undo buffer: a second pool buffer of the SAME capacity as the loop
    // buffer (Lua creates both). Copy-on-write snapshots of every frame an
    // overdub / extend touches go here; undo swaps them back (so undo again
    // is redo). nil disables undo. Same claim / release rules as setSample.
    void setUndoSample(od::Sample *sample);
    // 1 while an undo / redo is being applied (spread over blocks).
    int  isRestoring();
    // 1 if there is something to undo or redo.
    int  canUndo();
    // Clock: the number of clock periods the current loop spans (0 = the loop
    // was not recorded against a clock). Persisted with the preset.
    void setSyncN(int n);
    int  getSyncN();
    // 1 while REC is waiting for the next clock edge (sync mode).
    int  isArmed();
    // 1 while a clock is present (an edge within the last 4 s).
    int  hasClock();
    // Bars: the current bar (1-based) and the bar count, for the display —
    // while recording under a bar count: bars taken so far / the count;
    // while playing a clocked loop: the edge phase / the loop's periods.
    // Both 0 when there is nothing to count.
    int  getBar();
    int  getBars();
    int  getBarsWhole();   // 1 when the bar readout counts bars, 0 when it counts pulses ("beat")
    // Pulses per bar (0.8.9, menu): the clock's resolution, so Bars counts
    // BARS — a counted take is bars x pulses-per-bar clock pulses. 1 (the
    // default) is the old behaviour exactly. Clamped 1..96. The bar readouts
    // divide by it; the downbeat re-sync keeps the exact pulse count.
    void setPulsesPerBar(int n);
    int  getPulsesPerBar();
    // The pulse within the current bar, 1-based, when pulses per bar > 1 and
    // a bar count is shown; else 0. For the display.
    int  getBeat();
    // Sync-mode overdub (0.8.10): 1 = armed to punch in at the next bar line,
    // 2 = armed to punch out at the next bar line, 0 = not armed.
    int  getDubArmed();
    // A counted overdub (Bars > 0): the bar it is in (1-based) and its length
    // in bars; both 0 when no counted overdub runs. For the display.
    int  getDubBar();
    int  getDubBars();

#ifndef SWIGLUA
    void process() override;

    od::Inlet  mLeftIn   {"Left In"};
    od::Inlet  mRightIn  {"Right In"};
    od::Outlet mLeftOut  {"Left Out"};
    od::Outlet mRightOut {"Right Out"};

    od::Inlet  mRecIn    {"Rec"};      // trigger: the one-button pedal grammar
    od::Inlet  mDubIn    {"Dub"};      // trigger (0.8.10): overdub in / out NOW, sync or not
    od::Inlet  mStopIn   {"Stop"};     // latched gate: high = stop (level fades, head holds)
    od::Inlet  mUndoIn   {"Undo"};     // trigger: swap the last pass out / back in
    od::Inlet  mClkIn    {"Clk"};      // trigger: free mode = restart the loop; sync mode = the clock
    od::Option mSyncOpt  {"Sync", 1};  // 1 = free, 2 = sync (never 0: CHOICE_UNKNOWN)
    od::Inlet  mSpeedIn  {"Speed"};    // -2..+2, rate multiplier, 0 = stopped
    od::Inlet  mVoctIn   {"V/Oct"};    // 1 V/oct into speed (ER-301: 1.0 = 10 oct)
    od::Inlet  mExtIn    {"Extend"};   // latched gate: overdub past the end grows the loop
    od::Inlet  mBarsIn   {"Bars"};     // sync mode: close the take by itself after this many pulses (0 = manual)
    od::Inlet  mSosIn    {"SOS"};      // crossfader: 0 replace .. 1 keep loop
    od::Inlet  mDryIn    {"Dry"};      // dry (live input) level 0..1
    od::Inlet  mLevelIn  {"Level"};    // loop level 0..1

    enum State { EMPTY = 0, RECORD = 1, PLAY = 2, OVERDUB = 3, STOP = 4, EXTEND = 5 };

    // display accessors (cheap scalars, read from the UI thread)
    int  vizWrite()   const { return (mState == RECORD || mState == EXTEND) ? mWrite : mVizPos; }
    bool vizWriting() const { return mState == RECORD || mState == OVERDUB || mState == EXTEND; }
    int  vizPos()     const { return mVizPos; }
    int  vizLoop0()   const { return mLoop0; }        // loop length an EXTEND started from
    int  vizCapacity() const { return mCap; }
    int  vizZeroed()   const { return mZeroPos; }        // zeroed up to here (rises during a clear)
    float seamRho()    const { return mSeamRho; }
    int  seamUsedPreRoll() const { return mSeamPre; }
    float vizSpeedTarget() const { return mSpeedT; }  // detented, pre-glide

    static constexpr int   kSections    = 8;
    static constexpr int   kBarsMax     = 16;   // the Bars dial's top, and CV's (0.8.13: was 64 / 999)
    static constexpr int   kClkTimeout  = 4 * 48000;   // no edge for 4 s = no clock
    static constexpr int   kClkMinPeriod = 960;        // 20 ms: faster edges are noise
    static constexpr int   kSyncTol     = 64;          // head within this of the seam: no re-sync jump
    static constexpr int   kUndoMaxSecs = 60;     // bitmap sized once for the largest buffer
    static constexpr int   kRestorePerBlock = 2048; // frames swapped per block during undo
    static constexpr int   kZeroPerBlock = 4096;    // frames zeroed per block after a clear (30 s: ~1 s)
    static constexpr float kDetentPct   = 0.03f;  // +/-3 % magnetic window
    static constexpr float kDetentZero  = 0.03f;  // absolute window around 0

    static constexpr int   kSeam        = 128;   // seam blend (2.7 ms): free closes, phase-matched
    // A clocked close cannot move its cut (the clock sets the length), so a
    // steady tone that does not fit the loop meets itself out of phase at the
    // wrap, and no cut point fixes that. Over 2.7 ms that mismatch is a pop on
    // every pass (tester, 0.8.4); over 21 ms it is a smooth transition.
    static constexpr int   kSeamClocked = 1024;
    // The handover when a first take closes: live input -> loop, and the
    // clear fade-out. A smoothstep over kJump (21 ms) from mXfade. 0.8.4 used
    // the 5 ms LINEAR transport ramp: shorter than a period of a low tone and
    // cornered at both ends — the tester's "pop when you stop recording",
    // free and clocked (host: 40x a smooth sum's curvature at 55 Hz).
    static constexpr int   kHandover    = 1024;
    // PRE-ROLL (0.8.6, after lucasowens/previously DESIGN §5.2): the input
    // is kept in a ring while EMPTY, so a clocked close can blend the loop's
    // END into the audio that preceded its start. The wrap then flows as the
    // recording did, and the downbeat is never touched — 0.8.5 blended the
    // tail INTO the head over 21 ms, softening a downbeat attack every pass.
    static constexpr int   kPreRoll     = 1024;   // power of two (mask, no %)
    static constexpr int   kPreMin      = 64;     // less pre-roll than this: the old head blend
    static constexpr int   kJump        = 1024;  // fade table resolution
    static constexpr int   kJumpIn      = 48;    // a jump's incoming ramp (1 ms): attacks survive
    static constexpr int   kJumpOut     = 384;   // the outgoing material's own tail (8 ms): no click
    static constexpr int   kSeamSearch  = 2048;  // seam match: how far back to look (43 ms: a full period down to 23 Hz)
    static constexpr int   kMatchWin    = 512;   // seam match: samples compared (10.7 ms)
    static constexpr int   kMatchDec    = 4;     // coarse pass compares every 4th frame of the window
    static constexpr float kSpeedMax    = 2.0f;   // +/-2x: an octave each way (Nick, 2026-09-25)
    static constexpr float kVoctOct     = 10.0f; // ER-301: 1.0 normalised = 10 octaves
    static constexpr float kSpeedStop   = 0.05f; // |speed| below = stopped
    static constexpr float kRecRampSec  = 0.005f;
    static constexpr float kGlideSec    = 0.040f; // speed glide (tape slide)
    static constexpr int   kHeadGap     = 3;      // overdub writes this many frames BEHIND the play head

private:
    static constexpr int   kSampleRate  = 48000;
    static constexpr int   kMaxCh       = 2;

    int   mNc      = 1;        // channels of the attached buffer (validated)
    int   mCap     = 0;        // usable frames of the attached buffer (validated)
    int   mChannelCount;       // what the unit was built for (1 or 2)

    State mState   = EMPTY;
    bool  mClearRequested = false;
    bool  mKeepClean = true;      // see setKeepClean
    inline void markDirty(od::Sample *s) { if (s && !mKeepClean) s->setDirty(); }
    bool  mClearing = false;      // the loop is fading out before EMPTY (kHandover)
    int   mClearK = 0;            // position in that fade (mXfade index)
    int   mHandover = 0;          // samples left in the live -> loop handover after a close
    bool  mZeroRequested = false; // zeroBuffer() asked; starts once EMPTY
    bool  mZeroing = false;
    int   mZeroPos = 0;           // frames zeroed so far in this pass
    bool  mSeamMatch = true;

    // record head (integer, 1x forward on the first take)
    int   mWrite   = 0;
    // loop
    int   mLoopLen = 0;
    int   mLoopRev = 0;
    int   mLoop0   = 0;        // loop length when an EXTEND began
    // read head: fractional phase in [0, mLoopLen)
    double mRead   = 0.0;
    int    mVizPos = 0;        // absolute loop position of the read head
    // shadow head for jump crossfades: ABSOLUTE loop position
    double mShadow = 0.0;
    int    mJumpCount = 0;     // samples the OUTGOING tail still has to run
    float  mInPh = 0.0f, mInStep = 0.0f;       // incoming ramp: table phase / step
    float  mOutPh = 0.0f, mOutStep = 0.0f;     // outgoing tail: table phase / step
    // fade table: mXfade[k] = 1 - smoothstep(k/kJump). A jump is NOT a
    // crossfade: the new position ramps in over kJumpIn (1 ms, so a drum's
    // attack survives) while the old material tails out on its own over
    // kJumpOut (8 ms, so tonal material does not click) — what a sampler
    // does on a retrigger, not what a crossfade does to a slice's edge.
    float  mXfade[kJump + 1];
    // speed glide state
    float  mSpeedZ = 1.0f;
    float  mSpeedT = 1.0f;     // detented target (this block)
    bool   mDetent = false;
    // anti-alias one-pole on the read (|speed| > 1 only; a = 1 is a bypass)
    float  mAaL = 0.0f, mAaR = 0.0f;
    // overdub write head (0.8.2): the loop position written up to on the
    // previous sample, the previous (filtered) input, and a two-pole input
    // low-pass used below 1x (a = 1 is a bypass)
    double mWPos = 0.0;
    bool   mWValid = false;
    float  mXpL = 0.0f, mXpR = 0.0f;
    float  mWl1 = 0.0f, mWl2 = 0.0f, mWr1 = 0.0f, mWr2 = 0.0f;
    // record level ramp (overdub in/out)
    float  mRecLvl = 0.0f, mRecGoal = 0.0f;
    float  mPrevRec = 0.0f;    // last Rec sample of the previous block
    float  mPrevUndo = 0.0f;
    float  mPrevDub = 0.0f;
    bool   mPrevStop = false;  // last block's Stop gate (a stop press ends a take, 0.8.14)
    // clock
    float  mPrevClk = 0.0f;
    int    mClkSince = 1 << 30;   // samples since the last edge (huge = none)
    int    mClkPeriod = 0;
    int    mClkCount = 0;         // edges seen, ever
    int    mClkAtStart = 0;       // edge count when the take started
    int    mSyncN = 0;            // clock periods per loop (0 = unknown)
    int    mEdgePhase = 0;        // edges since the last downbeat
    int    mArmed = 0;            // 0 none, 1 = start on next edge, 2 = stop on next edge
    int    mBars = 0;             // the Bars inlet, rounded, read once per block
    int    mPpb = 1;              // pulses per bar (menu), 1..96
    // sync-mode overdub / extend (0.8.10)
    int    mDubArm = 0;           // 1 punch in at the next bar line, 2 punch out there
    int    mDubLeft = 0;          // pulses left in a counted overdub (0 = open-ended)
    int    mDubTotal = 0;         // that overdub's length in pulses (display)
    int    mBeatInBar = 0;        // pulses since the last bar line of the loop (no %: a counter)
    int    mExtEdges = 0;         // pulses since this extension began
    int    mExtBeat = 0;          // pulses since its last bar line
    bool   mExtArm = false;       // REC pressed during a sync extension: close at the next bar line
    int    mTailLeft = 0;         // frames of tail still to capture after a clocked close
    int    mSeamLen = kSeam;      // that close's crossfade length (kSeamClocked, or half a short loop)
    float  mPre[kPreRoll * kMaxCh];   // the pre-roll ring (input while EMPTY), interleaved
    int    mPrePos = 0;           // next ring slot to write
    int    mPreCount = 0;         // valid frames in the ring (<= kPreRoll)
    float  mSeamRho = 1.0f;       // the last pre-roll seam's signed rho^2 (tests)
    int    mSeamPre = 0;          // the last clocked close used the pre-roll (1) or the head blend (0)
    bool   mFirstTake = false;    // that tail's blend is not an undo-able pass
    // transport: stop gate high -> level ramps to 0 and the head holds
    bool   mStopped = false;
    float  mPlayLvl = 1.0f;
    // undo: copy-on-write bitmap (one bit per frame, allocated once), the
    // spare buffer, the lengths before / after the pass, and the restore
    // cursor while an undo is being applied
    od::Sample *mpUndo = 0;
    uint32_t *mBits = 0;       // kUndoMaxSecs * 48000 bits, owned, never resized
    int    mBitsFrames = 0;
    bool   mUndoAvail = false; // marked frames or a length change exist
    int    mUndoLenA = 0, mUndoLenB = 0;   // loop length before the pass / after it
    bool   mRestoring = false;
    int    mRestPos = 0, mRestLeft = 0, mRestDir = 1;

    bool   mInit = false;
    float  mGlideA = 0.0f, mRecStep = 1.0f, mDryA = 1.0f, mLn2Voct = 6.93f;
    float  mDryZ = 1.0f;       // glided dry gain (see MONITORING in process)

    // undo helpers (audio thread)
    inline bool bitGet(int i) const { return (mBits[i >> 5] >> (i & 31)) & 1u; }
    inline void bitSet(int i)       { mBits[i >> 5] |= (1u << (i & 31)); }
    // copy-on-write: save frame i to the undo buffer once per pass
    inline void cow(const float *d, int i)
    {
        if (!mpUndo || mRestoring || mState == RECORD || mFirstTake || i < 0 || i >= mBitsFrames) return;   // a first take has no undo
        if (mpUndo->mChannelCount != (uint32_t)mNc) return;   // different layout: no undo, never an overrun
        if (bitGet(i)) return;
        float *u = mpUndo->mpData;
        if (!u || (uint32_t)i >= mpUndo->mSampleCount) return;
        const int nc = mNc;
        const float *a = d + (size_t)i * nc;
        float *b = u + (size_t)i * nc;
        b[0] = a[0];
        if (nc > 1) b[1] = a[1];
        bitSet(i);
        mUndoAvail = true;
    }
    // One overdub write into loop frame c: the SOS crossfade at record level g.
    inline void dubCell(float *d, int c, float xl, float xr, float g, float sos)
    {
        cow(d, c);
        float *w = d + (size_t)c * mNc;
        const float nl = sos * w[0] + (1.0f - sos) * xl;
        w[0] = flushTiny(softLimit(w[0] + g * (nl - w[0])));
        if (mNc > 1) {
            const float nr = sos * w[1] + (1.0f - sos) * xr;
            w[1] = flushTiny(softLimit(w[1] + g * (nr - w[1])));
        }
    }
    void dubWrite(float *d, double pos, int L, float xl, float xr, float sos);
    void onClockEdge();        // audio thread, at the edge sample
    void blendSeam(float *d, int L, int len = kSeam);
    void closeLoopExact(bool firstTake = true);   // clocked close: exact length (pre-roll seam on a first take)
    int  readoutPulse();       // UI: the pulse (0..N-1) the bar readout shows, -1 none
    void closeExtendExact();   // sync mode: an extension closes on the clock, N grows by its pulses
    void preRollSeam(float *d, int L, int F);   // blend buf[L-F, L) toward the pre-roll
    void beginPass();          // an overdub / extend starts: new undo point
    void beginUndo();          // the Undo edge: start swapping
    void restoreSome(float *d);// per block: swap up to kRestorePerBlock frames
    void clearUndo();
    // Smoothstep seam blend of the tail into the head; shortens the loop by kSeam.
    // preRoll: a FIRST take (0.8.6) — the seam goes into the pre-roll, the
    // head untouched; an extend keeps the head blend (its head has been
    // overdubbed since, so the pre-roll no longer precedes it)
    void closeLoop(bool preRoll = false);
    // The phase-matched cut: the L in [hi - kSeamSearch, hi] whose following
    // kMatchWin samples best match the head (least squared difference).
    int  findSeam(const float *d, int hi) const;
    // findSeam's coarse errors. A MEMBER, never a local: the audio thread's
    // stack on the ER-301 is 2 KB (stolmine crash diagnostics, 2026-10-23:
    // "STACK BLOWN", a 2048-byte task stack 100 % used) and this array is
    // 1 KB — it overflowed the stack the moment a take closed (0.8.10), and
    // the corruption distorted ALL audio until a reboot. tools/check-stack.sh
    // now fails the build on any engine frame over the limit.
    mutable float mSeamErr[kSeamSearch / 8 + 1];
    // inFrames: the new position's ramp (kJumpIn for clock jumps: attacks
    // survive; kJumpOut at an extend close: a symmetric crossfade)
    void beginJump(double toWindowPhase, double shadowAbs, int inFrames = kJumpIn);
    void closeExtend();
    inline float readHermite(const float *d, double pos, int ch) const;
    void enterEmpty();
    void enterRecord();
    void enterPlay();

    // sanitise: bit-test the exponent; -ffast-math folds `x == x` (standard §5)
    static inline float sanitize(float x)
    {
        union { float f; uint32_t u; } v; v.f = x;
        return ((v.u & 0x7f800000u) == 0x7f800000u) ? 0.0f : x;
    }
    // Identity below full scale (an overdub at SOS=1 must leave the loop
    // bit-identical), then a soft knee that never exceeds +/-2 (slope 1 at
    // the knee, so layering into the red is gentle rather than a wall).
    static inline float softLimit(float x)
    {
        if (x >  1.0f) return  2.0f - 1.0f / x;
        if (x < -1.0f) return -2.0f - 1.0f / x;
        return x;
    }
    // Magnetic detents: within kDetentPct of a musical ratio the target speed
    // is exactly that ratio (the "LED goes solid" moment on the hardware).
    inline float detent(float s, bool &locked) const
    {
        static const float kRatios[4] = { 0.25f, 0.5f, 1.0f, 2.0f };
        const float a = s < 0.0f ? -s : s;
        if (a < kDetentZero) { locked = true; return 0.0f; }
        for (int i = 0; i < 4; ++i) {
            const float r = kRatios[i];
            const float lo = r * (1.0f - kDetentPct), hi = r * (1.0f + kDetentPct);
            if (a >= lo && a <= hi) { locked = true; return s < 0.0f ? -r : r; }
        }
        locked = false;
        return s;
    }
    // Denormals: an overdub at SOS < 1 over digital silence decays the loop
    // by SOS every pass, into the denormal range after ~126 passes at 0.5
    // (host soak: every frame of a 2 s loop denormal at 4.5 min), and the
    // A8's VFP handles denormals slowly. Anything under 1e-30 (-600 dB) is 0.
    static inline float flushTiny(float x)
    {
        return (x < 1.0e-30f && x > -1.0e-30f) ? 0.0f : x;
    }
    static inline float clampf(float x, float lo, float hi)
    {
        return x < lo ? lo : (x > hi ? hi : x);
    }
#endif // SWIGLUA
};

} // namespace noether
