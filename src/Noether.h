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
    // shows nothing — the stock loopers' zeroBuffer (RecordHead::zeroBuffer,
    // od::Sample::zero from the Lua thread; the audio thread has already
    // left the buffer alone by the time the task runs).
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

#ifndef SWIGLUA
    void process() override;

    od::Inlet  mLeftIn   {"Left In"};
    od::Inlet  mRightIn  {"Right In"};
    od::Outlet mLeftOut  {"Left Out"};
    od::Outlet mRightOut {"Right Out"};

    od::Inlet  mRecIn    {"Rec"};      // trigger: the one-button pedal grammar
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
    float vizSpeedTarget() const { return mSpeedT; }  // detented, pre-glide

    static constexpr int   kSections    = 8;
    static constexpr int   kClkTimeout  = 4 * 48000;   // no edge for 4 s = no clock
    static constexpr int   kClkMinPeriod = 960;        // 20 ms: faster edges are noise
    static constexpr int   kSyncTol     = 64;          // head within this of the seam: no re-sync jump
    static constexpr int   kUndoMaxSecs = 60;     // bitmap sized once for the largest buffer
    static constexpr int   kRestorePerBlock = 2048; // frames swapped per block during undo
    static constexpr float kDetentPct   = 0.03f;  // +/-3 % magnetic window
    static constexpr float kDetentZero  = 0.03f;  // absolute window around 0

    static constexpr int   kSeam        = 128;   // seam blend (2.7 ms)
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

private:
    static constexpr int   kSampleRate  = 48000;
    static constexpr int   kMaxCh       = 2;

    int   mNc      = 1;        // channels of the attached buffer (validated)
    int   mCap     = 0;        // usable frames of the attached buffer (validated)
    int   mChannelCount;       // what the unit was built for (1 or 2)

    State mState   = EMPTY;
    bool  mClearRequested = false;
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
    // record level ramp (overdub in/out)
    float  mRecLvl = 0.0f, mRecGoal = 0.0f;
    float  mPrevRec = 0.0f;    // last Rec sample of the previous block
    float  mPrevUndo = 0.0f;
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
    int    mTailLeft = 0;         // frames of tail still to capture after a clocked close
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
    void onClockEdge();        // audio thread, at the edge sample
    void blendSeam(float *d, int L);
    void closeLoopExact();     // clocked close: exact length + deferred tail
    void beginPass();          // an overdub / extend starts: new undo point
    void beginUndo();          // the Undo edge: start swapping
    void restoreSome(float *d);// per block: swap up to kRestorePerBlock frames
    void clearUndo();
    // Smoothstep seam blend of the tail into the head; shortens the loop by kSeam.
    void closeLoop();
    // The phase-matched cut: the L in [hi - kSeamSearch, hi] whose following
    // kMatchWin samples best match the head (least squared difference).
    int  findSeam(const float *d, int hi) const;
    void beginJump(double toWindowPhase, double shadowAbs);
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
    static inline float clampf(float x, float lo, float hi)
    {
        return x < lo ? lo : (x > hi ? hi : x);
    }
#endif // SWIGLUA
};

} // namespace noether
