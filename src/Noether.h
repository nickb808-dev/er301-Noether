/* Noether.h — Seamless vari-speed loop recorder for the ER-301
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * A stereo loop recorder in the pedal-looper grammar (one REC button: empty
 * -> record -> close and play -> overdub in -> overdub out) whose playback is
 * a fractional-phase tape head: continuous speed from -4x to +4x, 0 = stopped,
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
 *   2. Every JUMP of the read head (a reset, a clock re-sync, a stop/start)
 *      fades a shadow head out over kSeam samples while the new position fades
 *      in — the FeedbackLooper jumpTo() idea.
 *   3. Record level ramps (5 ms) on overdub in/out.
 *
 * WINDOW (Start / Len): playback runs inside a window of the loop — a start
 * point and a length, both fractions of the loop, both CV-able. The window
 * is LATCHED at each wrap so a moving Start does not scrub the head; the
 * wrap at a window edge is a shadow-head crossfade. Start 0 / Len 1 is
 * exactly the whole loop and takes the natural (phase-matched) seam.
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
 * PHASE STATUS: 0.2.0 — phases 1 + 3 (less undo). Detents / scale graphic,
 * undo, clock and the end-of-loop output follow per the plan.
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
    // loop length and starts playing; returns the loop length (0 = refused).
    int  exportLoop(od::Sample *dst);
    int  importLoop(od::Sample *src);
    // Bumps whenever the loop's content or length changes (close, overdub
    // out, extend, clear, import), so Lua saves only what changed.
    int  getLoopRev();
    // Window as latched (loop-sample units), for the display.
    int  getWindowStart();
    int  getWindowLength();
    // 1 while the effective speed sits on a musical ratio detent
    // (+/-4, +/-2, +/-1, +/-1/2, +/-1/4, 0), else 0. For the display.
    int  getDetent();
    // Sections the display divides the loop into (8 until a clock exists).
    int  getSections();

#ifndef SWIGLUA
    void process() override;

    od::Inlet  mLeftIn   {"Left In"};
    od::Inlet  mRightIn  {"Right In"};
    od::Outlet mLeftOut  {"Left Out"};
    od::Outlet mRightOut {"Right Out"};

    od::Inlet  mRecIn    {"Rec"};      // trigger: the one-button pedal grammar
    od::Inlet  mSpeedIn  {"Speed"};    // -4..+4, rate multiplier, 0 = stopped
    od::Inlet  mVoctIn   {"V/Oct"};    // 1 V/oct into speed (ER-301: 1.0 = 10 oct)
    od::Inlet  mStartIn  {"Start"};    // window start, fraction of the loop [0,1)
    od::Inlet  mLenIn    {"Len"};      // window length, fraction of the loop (0,1]
    od::Inlet  mExtIn    {"Extend"};   // latched gate: overdub past the end grows the loop
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
    static constexpr float kDetentPct   = 0.03f;  // +/-3 % magnetic window
    static constexpr float kDetentZero  = 0.03f;  // absolute window around 0

    static constexpr int   kSeam        = 128;   // seam / jump crossfade (2.7 ms)
    static constexpr int   kSeamSearch  = 512;   // seam match: how far back to look (10.7 ms)
    static constexpr int   kMatchWin    = 512;   // seam match: samples compared (10.7 ms)
    static constexpr float kSpeedMax    = 4.0f;
    static constexpr int   kMinWindow   = 256;   // shortest window (5.3 ms)
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
    // window (latched at each wrap), in loop samples
    int   mWinStart = 0;
    int   mWinLen   = 0;       // 0 = whole loop
    // read head: fractional phase in [0, window length), window-relative
    double mRead   = 0.0;
    int    mVizPos = 0;        // absolute loop position of the read head
    // shadow head for jump crossfades: ABSOLUTE loop position
    double mShadow = 0.0;
    int    mJumpCount = 0;     // samples remaining in the jump fade
    // speed glide state
    float  mSpeedZ = 1.0f;
    float  mSpeedT = 1.0f;     // detented target (this block)
    bool   mDetent = false;
    // anti-alias one-pole on the read (|speed| > 1 only; a = 1 is a bypass)
    float  mAaL = 0.0f, mAaR = 0.0f;
    // record level ramp (overdub in/out)
    float  mRecLvl = 0.0f, mRecGoal = 0.0f;
    float  mPrevRec = 0.0f;    // last Rec sample of the previous block

    bool   mInit = false;
    float  mGlideA = 0.0f, mRecStep = 1.0f, mDryA = 1.0f, mLn2Voct = 6.93f;
    float  mDryZ = 1.0f;       // glided dry gain (see MONITORING in process)

    // Smoothstep seam blend of the tail into the head; shortens the loop by kSeam.
    void closeLoop();
    // The phase-matched cut: the L in [hi - kSeamSearch, hi] whose following
    // kMatchWin samples best match the head (least squared difference).
    int  findSeam(const float *d, int hi) const;
    void beginJump(double toWindowPhase, double shadowAbs);
    // latch the window from the Start / Len targets; returns true if windowed
    bool latchWindow(int startS, int lenS);
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
        static const float kRatios[5] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
        const float a = s < 0.0f ? -s : s;
        if (a < kDetentZero) { locked = true; return 0.0f; }
        for (int i = 0; i < 5; ++i) {
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
