/* Noether.cpp — Seamless vari-speed loop recorder for the ER-301
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * Audio-thread rules (standard §2, §5, §5a): no allocation, no integer
 * division or modulo, no trig (sinf/cosf resolved from a package .so have
 * miscomputed on am335x — the seam uses a smoothstep, not a raised cosine),
 * every inlet sanitised at the read, positive-test guards on the phase
 * accumulators so a NaN can never stick. */

#include "Noether.h"
#include <cstring>

namespace noether {

Noether::Noether(int channelCount)
    : mChannelCount(channelCount < 2 ? 1 : 2)
{
    addInput(mLeftIn);
    addInput(mRightIn);
    addOutput(mLeftOut);
    addOutput(mRightOut);
    addInput(mRecIn);
    addInput(mSpeedIn);
    addInput(mVoctIn);
    addInput(mStartIn);
    addInput(mLenIn);
    addInput(mExtIn);
    addInput(mSosIn);
    addInput(mDryIn);
    addInput(mLevelIn);
    // block constants that need libm, computed once here rather than in
    // process() (standard §5c; and no libm calls on the audio thread)
    mGlideA  = 1.0f - expf(-1.0f / ((float)kSampleRate * kGlideSec));
    mRecStep = 1.0f / ((float)kSampleRate * kRecRampSec);
    mDryA    = 1.0f - expf(-1.0f / ((float)kSampleRate * kRecRampSec));
    mLn2Voct = 0.69314718f * kVoctOct;
}

Noether::~Noether() {}

/* ── Lua-callable API ──────────────────────────────────────────────────── */

void Noether::setSample(od::Sample *sample)
{
    // Publish "no loop" BEFORE the base swaps the buffer, so a block that
    // lands mid-swap reads nothing.
    mState = EMPTY;
    mLoopLen = 0;
    mWrite = 0;
    mRead = 0.0;
    mJumpCount = 0;
    mCap = 0;
    od::Head::setSample(sample);
}

void Noether::clearLoop()          { mClearRequested = true; }
int   Noether::getState()          { return (int)mState; }
int   Noether::getLoopSamples()    { return mLoopLen; }
float Noether::getLoopSeconds()    { return (float)mLoopLen * (1.0f / (float)kSampleRate); }
float Noether::getSpeed()          { return mSpeedZ; }
void  Noether::setSeamMatch(int on){ mSeamMatch = (on != 0); }
int   Noether::getSeamMatch()      { return mSeamMatch ? 1 : 0; }
int   Noether::getLoopRev()        { return mLoopRev; }
int   Noether::getDetent()         { return mDetent ? 1 : 0; }
int   Noether::getSections()       { return kSections; }
int   Noether::getWindowStart()    { return mWinStart; }
int   Noether::getWindowLength()   { return mWinLen > 0 ? mWinLen : mLoopLen; }

// UI thread. Copies the loop [0, L) into dst; dst was allocated by Lua with
// at least L frames (standard §3: never resize here). Channel-mapped.
int Noether::exportLoop(od::Sample *dst)
{
    od::Sample *src = mpSample;
    const int L = mLoopLen;
    if (!dst || !dst->mpData || !src || !src->mpData || L <= 0) return 0;
    const int nd = (int)dst->mChannelCount, ns = mNc;
    int n = L;
    if ((uint32_t)n > dst->mSampleCount) n = (int)dst->mSampleCount;
    if (n > mCap) n = mCap;
    if (nd < 1 || nd > kMaxCh || ns < 1) return 0;
    for (int i = 0; i < n; ++i) {
        const float *a = src->mpData + (size_t)i * ns;
        float *b = dst->mpData + (size_t)i * nd;
        b[0] = a[0];
        if (nd > 1) b[1] = (ns > 1) ? a[1] : a[0];
    }
    dst->mSampleLoadCount = dst->mSampleCount;
    dst->setDirty();
    return n;
}

// UI thread. The audio thread is told EMPTY first (its EMPTY path never
// touches the buffer), the copy runs, then the loop is published.
int Noether::importLoop(od::Sample *src)
{
    od::Sample *dst = mpSample;
    if (!src || !src->mpData || !dst || !dst->mpData) return 0;
    uint32_t avail = src->mSampleLoadCount < src->mSampleCount ? src->mSampleLoadCount : src->mSampleCount;
    const int ns = (int)src->mChannelCount, nd = (int)dst->mChannelCount;
    if (ns < 1 || ns > kMaxCh || nd < 1 || nd > kMaxCh) return 0;
    int n = (int)avail;
    uint32_t cap = dst->mSampleLoadCount < dst->mSampleCount ? dst->mSampleLoadCount : dst->mSampleCount;
    if ((uint32_t)n > cap) n = (int)cap;
    if (n < 4) return 0;

    enterEmpty();                       // audio thread now ignores the buffer
    for (int i = 0; i < n; ++i) {
        const float *a = src->mpData + (size_t)i * ns;
        float *b = dst->mpData + (size_t)i * nd;
        b[0] = a[0];
        if (nd > 1) b[1] = (ns > 1) ? a[1] : a[0];
    }
    mNc = nd;
    mCap = (int)cap;
    mLoopLen = n;
    mWinStart = 0; mWinLen = 0;
    mRead = 0.0; mJumpCount = 0;
    ++mLoopRev;
    dst->setDirty();
    mState = PLAY;
    return n;
}

/* ── state transitions (audio thread) ──────────────────────────────────── */

void Noether::enterEmpty()
{
    mState = EMPTY;
    if (mLoopLen > 0) ++mLoopRev;
    mLoopLen = 0;
    mWinStart = 0; mWinLen = 0;
    mWrite = 0;
    mRead = 0.0;
    mJumpCount = 0;
    mRecLvl = mRecGoal = 0.0f;
}

void Noether::enterRecord()
{
    mState = RECORD;
    mWrite = 0;
    mLoopLen = 0;
    mWinStart = 0; mWinLen = 0;
    mRead = 0.0;
    mJumpCount = 0;
    mRecLvl = mRecGoal = 1.0f;   // first take: full level, no ramp — the seam
                                 // blend takes care of both ends
}

void Noether::enterPlay()
{
    mState = PLAY;
    mRecGoal = 0.0f;
}

// Blend the tail of the take into its head and shorten the loop by kSeam.
// After this, buf[L] (the sample recorded right after the loop's last
// sample) IS buf[0] at the seam, so the wrap is continuous.
void Noether::closeLoop()
{
    od::Sample *s = mpSample;
    if (!s || !s->mpData || mCap <= 0) { enterEmpty(); return; }
    float *d = s->mpData;
    const int nc = mNc;
    int len = mWrite;
    if (len > mCap) len = mCap;
    if (len < 4) { enterEmpty(); return; }

#ifdef NOETHER_NO_SEAM
    // TEST ONLY: the host seam test is verified against this deliberately
    // broken build (it must report CLICK). Never defined in a device build.
    if (false) {
        const int L = len - kSeam;
#else
    if (len > 2 * kSeam) {
        // The cut: where REC was pressed (less the crossfade tail), or the
        // phase-matched point up to kSeamSearch samples earlier.
        int L = len - kSeam;
        if (mSeamMatch && len > 2 * (kSeamSearch + kMatchWin)) {
            L = findSeam(d, len - (kSeam > kMatchWin ? kSeam : kMatchWin));
        }
#endif
        const float inv = 1.0f / (float)kSeam;
        for (int k = 0; k < kSeam; ++k) {
            const float t = ((float)k + 0.5f) * inv;
            const float w = t * t * (3.0f - 2.0f * t);   // head fade-in
            float *h = d + (size_t)k * nc;
            const float *tl = d + (size_t)(L + k) * nc;
            for (int c = 0; c < nc; ++c)
                h[c] = h[c] * w + tl[c] * (1.0f - w);
        }
        mLoopLen = L;
    } else {
        mLoopLen = len;
    }
    // Playback starts where the seam left off: at the head, which now carries
    // the tail — i.e. exactly where the recording would have continued.
    mRead = 0.0;
    mJumpCount = 0;
    mWinStart = 0; mWinLen = 0;
    ++mLoopRev;
    s->setDirty();
}

// Least-squared-difference match of buf[L..L+kMatchWin) against the head
// buf[0..kMatchWin), L in [hi - kSeamSearch, hi]. Coarse pass at stride 8,
// fine pass +/-8, both on the FULL window: a half-window coarse pass was
// tried first and tied on the flat top of a pulse wave (host `pulse` test),
// choosing a cut 107 samples off the period. Ties (silence, DC) keep the
// LATEST L, so an unmatched cut is as close as possible to where REC was
// pressed. One-shot at loop close: ~80k multiply-adds stereo, no divides,
// no allocation. The window bounds what can be matched: a period longer
// than kMatchWin (below ~94 Hz for a pulse) may tie on its flat regions.
int Noether::findSeam(const float *d, int hi) const
{
    const int nc = mNc;
    const int lo = hi - kSeamSearch;
    if (lo < kMatchWin) return hi;
    const float *head = d;
    const int n = kMatchWin * nc;

    int   bestL = hi;
    float bestE = 3.0e38f;
    for (int L = hi; L >= lo; L -= 8) {
        const float *t = d + (size_t)L * nc;
        float e = 0.0f;
        for (int k = 0; k < n; ++k) { const float df = t[k] - head[k]; e += df * df; }
        if (e < bestE) { bestE = e; bestL = L; }
    }
    const int c = bestL;
    bestE = 3.0e38f;
    for (int L = c + 8; L >= c - 8; --L) {
        if (L > hi || L < lo) continue;
        const float *t = d + (size_t)L * nc;
        float e = 0.0f;
        for (int k = 0; k < n; ++k) { const float df = t[k] - head[k]; e += df * df; }
        if (e < bestE) { bestE = e; bestL = L; }
    }
    return bestL;
}

// Jump the read head to a window phase while a shadow head (absolute loop
// position) keeps playing from where it was and fades out over kSeam.
void Noether::beginJump(double toWindowPhase, double shadowAbs)
{
    mShadow = shadowAbs;
    mRead = toWindowPhase;
    mJumpCount = kSeam;
}

// Window targets -> latched window. startS / lenS are loop-sample counts
// derived from the Start / Len inlets. Returns true if the latched window
// is anything but the whole loop.
bool Noether::latchWindow(int startS, int lenS)
{
    const int L = mLoopLen;
    if (L <= 0) { mWinStart = 0; mWinLen = 0; return false; }
    if (lenS >= L && startS <= 0) { mWinStart = 0; mWinLen = 0; return false; }
    if (lenS < kMinWindow) lenS = kMinWindow;
    if (lenS > L) lenS = L;
    if (startS < 0) startS = 0;
    if (startS >= L) startS = L - 1;
    mWinStart = startS;
    mWinLen = lenS;
    return true;
}

// End of an EXTEND: the write head is the new end of the loop. Same seam
// treatment as a first take (match + blend into the head).
void Noether::closeExtend()
{
    closeLoop();
    if (mState == EXTEND) { mState = PLAY; mRecGoal = 0.0f; mRecLvl = 0.0f; }
}

/* ── reads ─────────────────────────────────────────────────────────────── */

// 4-point Hermite (Catmull-Rom) read of channel `ch` at fractional loop
// position `pos` in [0, L). Neighbour indices wrap by conditional subtract.
inline float Noether::readHermite(const float *d, double pos, int ch) const
{
    const int L = mLoopLen;
    int i1 = (int)pos;
    if (i1 >= L) i1 -= L;
    if (i1 < 0)  i1 = 0;
    const float t = (float)(pos - (double)i1);
    int i0 = i1 - 1; if (i0 < 0)  i0 += L;
    int i2 = i1 + 1; if (i2 >= L) i2 -= L;
    int i3 = i2 + 1; if (i3 >= L) i3 -= L;
    const int nc = mNc;
    const float y0 = d[(size_t)i0 * nc + ch];
    const float y1 = d[(size_t)i1 * nc + ch];
    const float y2 = d[(size_t)i2 * nc + ch];
    const float y3 = d[(size_t)i3 * nc + ch];
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/* ── the block ─────────────────────────────────────────────────────────── */

void Noether::process()
{
    const int N = FRAMELENGTH;
    const float *inL = mLeftIn.buffer();
    const float *inR = (mChannelCount > 1) ? mRightIn.buffer() : mLeftIn.buffer();
    float *outL = mLeftOut.buffer();
    float *outR = mRightOut.buffer();

    // ── validate the buffer ONCE per block (standard §3a / §12c) ──
    od::Sample *s = mpSample;
    float *d = nullptr;
    if (s && s->mpData && s->mChannelCount >= 1 && s->mChannelCount <= (uint32_t)kMaxCh) {
        uint32_t n = s->mSampleLoadCount < s->mSampleCount ? s->mSampleLoadCount : s->mSampleCount;
        if (n > 4) {
            d = s->mpData;
            mNc = (int)s->mChannelCount;
            mCap = (int)n;
        }
    }
    if (!d) {
        mCap = 0;
        if (mState != EMPTY) enterEmpty();
        // no buffer: the unit is a wire
        for (int i = 0; i < N; ++i) { outL[i] = sanitize(inL[i]); outR[i] = sanitize(inR[i]); }
        mCurrentIndex = 0;
        return;
    }
    if (mClearRequested) { mClearRequested = false; enterEmpty(); }
    if (mLoopLen > mCap) mLoopLen = mCap;   // buffer shrank under us

    // ── block constants (standard §5c) ──
    const float speedKnob = clampf(sanitize(mSpeedIn.buffer()[0]), -kSpeedMax, kSpeedMax);
    const float voct   = clampf(sanitize(mVoctIn.buffer()[0]), -1.0f, 1.0f);
    // 1 V/oct: speed * 2^(octaves). expf once per block (Dirac does the same
    // in its process(); only sinf/cosf are the known am335x hazard).
    const float speedRaw = clampf(speedKnob * expf(voct * mLn2Voct), -kSpeedMax, kSpeedMax);
    bool locked = false;
    const float speedT = detent(speedRaw, locked);
    mSpeedT = speedT; mDetent = locked;
    // anti-alias: a one-pole LP on the read output when reading faster than
    // 1x (cutoff 0.5/|speed| of Nyquist); coefficient exactly 1 = bypass at
    // and below 1x, so the default is bit-identical to no filter.
    float aAa = 1.0f;
    {
        const float sp = mSpeedZ < 0.0f ? -mSpeedZ : mSpeedZ;
        if (sp > 1.0f) aAa = 1.0f - expf(-3.14159265f / sp);   // 2*pi*fc, fc = 0.5/sp cycles/sample
    }
    const float sos    = clampf(sanitize(mSosIn.buffer()[0]), 0.0f, 1.0f);
    const float dry    = clampf(sanitize(mDryIn.buffer()[0]), 0.0f, 1.0f);
    const float level  = clampf(sanitize(mLevelIn.buffer()[0]), 0.0f, 1.0f);
    const float startN = clampf(sanitize(mStartIn.buffer()[0]), 0.0f, 1.0f);
    const float lenN   = clampf(sanitize(mLenIn.buffer()[0]), 0.0f, 1.0f);
    const bool  extend = sanitize(mExtIn.buffer()[0]) > 0.5f;
    const float aGlide = mGlideA;
    const float recStep = mRecStep;
    const float aDry    = mDryA;
    const float *rec = mRecIn.buffer();
    const float jumpInv = 1.0f / (float)kSeam;
    // window targets in loop samples (latched at the next wrap)
    const int   winStartT = (int)(startN * (float)mLoopLen + 0.5f);
    const int   winLenT   = (lenN >= 0.999f) ? mLoopLen : (int)(lenN * (float)mLoopLen + 0.5f);

    if (!mInit) { mSpeedZ = speedT; mInit = true; }
    // sticky-NaN guards (positive tests, standard §5)
    if (!(mSpeedZ >= -kSpeedMax && mSpeedZ <= kSpeedMax)) mSpeedZ = speedT;
    {
        const int wl = mWinLen > 0 ? mWinLen : (mLoopLen > 0 ? mLoopLen : 1);
        if (!(mRead >= 0.0 && mRead < (double)wl)) mRead = 0.0;
        if (!(mShadow >= 0.0 && mShadow < (double)(mLoopLen > 0 ? mLoopLen : 1))) { mShadow = 0.0; mJumpCount = 0; }
    }
    if (!(mRecLvl >= 0.0f && mRecLvl <= 1.0f)) mRecLvl = mRecGoal;
    if (!(mDryZ >= 0.0f && mDryZ <= 1.0f)) mDryZ = dry;
    if (!(mAaL > -8.0f && mAaL < 8.0f)) mAaL = 0.0f;
    if (!(mAaR > -8.0f && mAaR < 8.0f)) mAaR = 0.0f;

    float prevRec = mPrevRec;

    for (int i = 0; i < N; ++i) {
        const float xl = sanitize(inL[i]);
        const float xr = sanitize(inR[i]);

        // ── REC edge: the whole pedal grammar on one button ──
        const float r = rec[i];
        if (r > 0.5f && prevRec <= 0.5f) {
            switch (mState) {
            case EMPTY:   enterRecord(); break;
            case RECORD:  mWrite = (mWrite < 1) ? 1 : mWrite;  // at least one frame
                          closeLoop();
                          if (mState == RECORD) enterPlay();
                          break;
            case PLAY:    mState = OVERDUB; mRecGoal = 1.0f; break;
            case OVERDUB: mRecGoal = 0.0f; break;   // ramp out, then PLAY
            case EXTEND:  closeExtend(); break;
            case STOP:    break;
            }
        }
        prevRec = r;

        // ── record level ramp ──
        if (mRecLvl < mRecGoal) { mRecLvl += recStep; if (mRecLvl > mRecGoal) mRecLvl = mRecGoal; }
        else if (mRecLvl > mRecGoal) { mRecLvl -= recStep; if (mRecLvl < mRecGoal) mRecLvl = mRecGoal; }
        if (mState == OVERDUB && mRecGoal == 0.0f && mRecLvl == 0.0f) { mState = PLAY; ++mLoopRev; }

        // ── speed glide ──
        mSpeedZ += aGlide * (speedT - mSpeedZ);

        float yl = 0.0f, yr = 0.0f;   // loop output
        switch (mState) {
        case EMPTY:
            mDryZ += aDry * (1.0f - mDryZ);
            outL[i] = xl * mDryZ; outR[i] = xr * mDryZ;
            continue;

        case RECORD: {
            float *w = d + (size_t)mWrite * mNc;
            w[0] = xl;
            if (mNc > 1) w[1] = xr;
            ++mWrite;
            if (mWrite >= mCap) { closeLoop(); if (mState == RECORD) enterPlay(); }
            mDryZ += aDry * (1.0f - mDryZ);
            outL[i] = xl * mDryZ; outR[i] = xr * mDryZ;   // monitor the take
            continue;
        }

        case EXTEND: {
            // The loop keeps playing one cycle back while the input is laid
            // on top of that copy, at 1x, pedal-multiply style.
            const int L0 = mLoop0;
            const float *src = d + (size_t)(mWrite - L0) * mNc;
            float *w = d + (size_t)mWrite * mNc;
            const float sl = src[0];
            const float sr = (mNc > 1) ? src[1] : sl;
            w[0] = softLimit(sos * sl + (1.0f - sos) * xl);
            if (mNc > 1) w[1] = softLimit(sos * sr + (1.0f - sos) * xr);
            ++mWrite;
            mVizPos = mWrite;
            if (mWrite >= mCap) closeExtend();
            yl = sl; yr = sr;
            mDryZ += aDry * (1.0f - mDryZ);
            yl *= level; yr *= level;
            outL[i] = xl * mDryZ + yl;
            outR[i] = xr * mDryZ + yr;
            continue;
        }

        case PLAY:
        case OVERDUB: {
            const int L = mLoopLen;
            if (L <= 0) { outL[i] = xl; outR[i] = xr; continue; }
            const bool windowed = (mWinLen > 0);
            const int  wl = windowed ? mWinLen : L;

            // absolute loop position of the read head
            double pos = mRead + (double)mWinStart;
            if (pos >= (double)L) pos -= (double)L;

            // overdub write at the read head (nearest sample), blended by the
            // record ramp so punching in/out never steps the existing audio
            if (mState == OVERDUB && mRecLvl > 0.0f &&
                (mSpeedZ > kSpeedStop || mSpeedZ < -kSpeedStop)) {
                int wi = (int)(pos + 0.5);
                if (wi >= L) wi -= L;
                float *w = d + (size_t)wi * mNc;
                const float g = mRecLvl;
                float nl = sos * w[0] + (1.0f - sos) * xl;
                w[0] = softLimit(w[0] + g * (nl - w[0]));
                if (mNc > 1) {
                    float nr = sos * w[1] + (1.0f - sos) * xr;
                    w[1] = softLimit(w[1] + g * (nr - w[1]));
                }
            }

            yl = readHermite(d, pos, 0);
            yr = (mNc > 1) ? readHermite(d, pos, 1) : yl;
            if (mJumpCount > 0) {
                const float wj = (float)mJumpCount * jumpInv;   // shadow weight
                float sl = readHermite(d, mShadow, 0);
                float sr = (mNc > 1) ? readHermite(d, mShadow, 1) : sl;
                yl += (sl - yl) * wj;
                yr += (sr - yr) * wj;
                mShadow += (double)mSpeedZ;
                if (mShadow >= (double)L) mShadow -= (double)L;
                if (mShadow < 0.0)        mShadow += (double)L;
                --mJumpCount;
            }
            mVizPos = (int)pos;
            if (aAa < 1.0f) {
                mAaL += aAa * (yl - mAaL); yl = mAaL;
                mAaR += aAa * (yr - mAaR); yr = mAaR;
            } else { mAaL = yl; mAaR = yr; }

            // advance, and handle the wrap
            mRead += (double)mSpeedZ;
            if (mRead >= (double)wl || mRead < 0.0) {
                const bool fwd = mRead >= (double)wl;
                // EXTEND: an overdub with the ext gate on runs off the end of
                // the whole loop at forward speed and keeps recording
                if (fwd && mState == OVERDUB && extend && !windowed && mRecGoal > 0.0f) {
                    mLoop0 = L;
                    mWrite = L;
                    if (mWrite < mCap) { mState = EXTEND; ++mLoopRev; break; }
                }
                // where the head would have continued (for the shadow)
                double cont = pos + (double)mSpeedZ;
                if (cont >= (double)L) cont -= (double)L;
                if (cont < 0.0)        cont += (double)L;
                // latch the window targets, then place the head
                const bool wasWindowed = windowed;
                const bool nowWindowed = latchWindow(winStartT, winLenT);
                const int  nwl = nowWindowed ? mWinLen : L;
                double ph;
                if (fwd) { ph = mRead - (double)wl; if (ph >= (double)nwl || ph < 0.0) ph = 0.0; }
                else     { ph = mRead + (double)nwl; if (ph < 0.0 || ph >= (double)nwl) ph = (double)nwl - 1.0; }
                if (wasWindowed || nowWindowed) beginJump(ph, cont);   // a window edge is a jump
                else mRead = ph;                                       // the whole loop: the natural seam
            }
            break;
        }

        case STOP:
            break;
        }

        // MONITORING: while recording or overdubbing the live input always
        // passes at full level (you must hear what you are laying down);
        // otherwise Dry sets the live level and the loop is ADDED at Level,
        // pedal-looper style. The dry gain glides so a state change never
        // steps the dry signal.
        const float dryT = (mState == OVERDUB) ? 1.0f : dry;
        mDryZ += aDry * (dryT - mDryZ);
        yl *= level; yr *= level;
        outL[i] = xl * mDryZ + yl;
        outR[i] = xr * mDryZ + yr;
    }
    mPrevRec = prevRec;

    if (mState == RECORD || mState == OVERDUB || mState == EXTEND) s->setDirty();
    mCurrentIndex = (mState == RECORD || mState == EXTEND) ? mWrite : mVizPos;
}

} // namespace noether
