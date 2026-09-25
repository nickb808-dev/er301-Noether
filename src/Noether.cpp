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
    addInput(mStopIn);
    addInput(mUndoIn);
    addInput(mClkIn);
    addOption(mSyncOpt);
    addInput(mSpeedIn);
    addInput(mVoctIn);
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
    // the fade table (see the header): a falling smoothstep
    for (int k = 0; k <= kJump; ++k) {
        const float t = (float)k / (float)kJump;
        mXfade[k] = 1.0f - t * t * (3.0f - 2.0f * t);
    }
    // the undo bitmap: one bit per frame for the largest buffer the menu
    // offers, allocated ONCE here (standard §3: never resize under the
    // audio thread; 60 s = 360 kB)
    mBitsFrames = kUndoMaxSecs * kSampleRate;
    const size_t words = (size_t)(mBitsFrames / 32) + 1;
    // plain operator new / delete (NOT new[]): those two symbols are already
    // what the SWIG wrapper needs from the firmware; operator new[] is not
    // in the firmware's export list and would fail the load silently
    mBits = (uint32_t *)::operator new(words * sizeof(uint32_t));
    memset(mBits, 0, words * sizeof(uint32_t));
}

Noether::~Noether()
{
    if (mpUndo) mpUndo->release();
    ::operator delete(mBits);
}

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
void Noether::zeroBuffer()         { if (mpSample) mpSample->zero(); }
int   Noether::isRestoring()       { return mRestoring ? 1 : 0; }
int   Noether::canUndo()           { return (mUndoAvail && mpUndo) ? 1 : 0; }

// UI thread, the od::Head::setSample order: publish nothing mid-swap.
void Noether::setUndoSample(od::Sample *sample)
{
    od::Sample *p = mpUndo;
    mpUndo = 0;
    mRestoring = false;
    clearUndo();
    if (p) p->release();
    p = sample;
    if (p) p->attach();
    mpUndo = p;
}

void Noether::clearUndo()
{
    if (mBits && mBitsFrames > 0) memset(mBits, 0, ((size_t)(mBitsFrames / 32) + 1) * sizeof(uint32_t));
    mUndoAvail = false;
    mUndoLenA = mUndoLenB = 0;
}

// A new pass (overdub in, extend): the previous pass can no longer be undone.
void Noether::beginPass()
{
    if (mRestoring) return;
    clearUndo();
    mUndoLenA = mLoopLen;
    mUndoLenB = mLoopLen;
}

// The Undo edge. Swaps loop <-> undo for every marked frame, spread over
// the following blocks starting at the head and moving with it; swaps the
// two lengths; a second edge swaps everything back (redo).
void Noether::beginUndo()
{
    if (!mpUndo || !mUndoAvail || mRestoring || mLoopLen <= 0) return;
    if (mState == OVERDUB) { mRecGoal = 0.0f; mRecLvl = 0.0f; mState = PLAY; }
    mRestoring = true;
    // the frames to visit: the longer of the two lengths (an extend's new
    // material sits above the shorter one)
    int span = mUndoLenA > mUndoLenB ? mUndoLenA : mUndoLenB;
    if (span > mBitsFrames) span = mBitsFrames;
    if (span > mCap) span = mCap;
    mRestLeft = span;
    mRestDir = (mSpeedZ < 0.0f) ? -1 : 1;
    int p = mVizPos; if (p < 0) p = 0; if (p >= span) p = 0;
    mRestPos = p;
    // the length swap happens now; the window re-latches at the next wrap
    const int newLen = (mLoopLen == mUndoLenA) ? mUndoLenB : mUndoLenA;
    if (newLen > 0 && newLen <= mCap) {
        mLoopLen = newLen;
            if (mRead >= (double)newLen) mRead = 0.0;
        mJumpCount = 0;
    }
    ++mLoopRev;
}

void Noether::restoreSome(float *d)
{
    if (!mRestoring) return;
    float *u = mpUndo ? mpUndo->mpData : 0;
    if (!u) { mRestoring = false; return; }
    const int span = mUndoLenA > mUndoLenB ? mUndoLenA : mUndoLenB;
    const int nc = mNc;
    int n = kRestorePerBlock;
    while (n > 0 && mRestLeft > 0) {
        const int i = mRestPos;
        if (i >= 0 && i < mBitsFrames && (uint32_t)i < mpUndo->mSampleCount && i < mCap && bitGet(i)) {
            float *a = d + (size_t)i * nc;
            float *b = u + (size_t)i * nc;
            float t = a[0]; a[0] = b[0]; b[0] = t;
            if (nc > 1) { t = a[1]; a[1] = b[1]; b[1] = t; }
        }
        mRestPos += mRestDir;
        if (mRestPos >= span) mRestPos = 0;
        if (mRestPos < 0) mRestPos = span - 1;
        --mRestLeft; --n;
    }
    if (mRestLeft <= 0) { mRestoring = false; ++mLoopRev; if (mpSample) mpSample->setDirty(); }
}
int   Noether::getState()          { return (mState == PLAY && mStopped && mPlayLvl <= 0.0f) ? (int)STOP : (int)mState; }
int   Noether::getLoopSamples()    { return mLoopLen; }
float Noether::getLoopSeconds()    { return (float)mLoopLen * (1.0f / (float)kSampleRate); }
float Noether::getSpeed()          { return mSpeedZ; }
void  Noether::setSeamMatch(int on){ mSeamMatch = (on != 0); }
int   Noether::getSeamMatch()      { return mSeamMatch ? 1 : 0; }
int   Noether::getLoopRev()        { return mLoopRev; }
int   Noether::getDetent()         { return mDetent ? 1 : 0; }
int   Noether::getSections()       { return (mSyncN > 0 && mSyncOpt.value() == 2) ? mSyncN : kSections; }
void  Noether::setSyncN(int n)     { mSyncN = (n > 0 && n < 1024) ? n : 0; mEdgePhase = 0; }
int   Noether::getSyncN()          { return mSyncN; }
int   Noether::isArmed()           { return mArmed; }
int   Noether::hasClock()          { return mClkSince < kClkTimeout ? 1 : 0; }

// A clock edge, at its sample. Free mode: restart the loop (a reset
// trigger). Sync mode: start / stop an armed take, and every N-th edge pull
// the head back to the seam so the loop stays on the downbeat at any speed.
void Noether::onClockEdge()
{
    const bool sync = mSyncOpt.value() == 2;
    if (mClkSince >= kClkMinPeriod && mClkSince < kClkTimeout) mClkPeriod = mClkSince;
    mClkSince = 0;
    ++mClkCount;

    if (!sync) {
        // a reset: restart the loop from the seam — unless the head is
        // already within kSyncTol of it, so a loop in time with its clock
        // never retriggers itself
        if (mState == PLAY || mState == OVERDUB) {
            const int L = mLoopLen;
            if (L > 0 && !(mRead < (double)kSyncTol || mRead > (double)(L - kSyncTol)))
                beginJump(0.0, mRead);
        }
        return;
    }
    if (mArmed == 1 && mState == EMPTY) {
        enterRecord();
        mClkAtStart = mClkCount;
        mArmed = 0;
        return;
    }
    if (mArmed == 2 && mState == RECORD) {
        mSyncN = mClkCount - mClkAtStart;
        if (mSyncN < 1) mSyncN = 1;
        mWrite = (mWrite < 1) ? 1 : mWrite;
        closeLoopExact();                    // the clock dictates the length
        if (mState == RECORD) enterPlay();
        mArmed = 0;
        mEdgePhase = 0;                      // this edge IS the downbeat
        return;
    }
    if ((mState == PLAY || mState == OVERDUB) && mSyncN > 0) {
        ++mEdgePhase;
        if (mEdgePhase >= mSyncN) {          // the N-th edge: the downbeat
            mEdgePhase = 0;
            const int L = mLoopLen;
            if (L > 0 && !(mRead < (double)kSyncTol || mRead > (double)(L - kSyncTol)))
                beginJump(0.0, mRead);
        }
    }
}

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
// Returns the frames imported, or a NEGATIVE reason (shown on the menu's
// status line): -1 no source / no source data, -2 no loop buffer,
// -3 channel count, -4 fewer than 4 frames available. Lua only calls this
// once the pool has reported the file complete, so when the file's
// "loaded" counter is 0 but its frame count is not (a completed card load
// that never touched mSampleLoadCount) the frame count is trusted.
int Noether::importLoop(od::Sample *src)
{
    od::Sample *dst = mpSample;
    if (!src || !src->mpData) return -1;
    if (!dst || !dst->mpData) return -2;
    uint32_t avail = src->mSampleLoadCount < src->mSampleCount ? src->mSampleLoadCount : src->mSampleCount;
    if (avail == 0) avail = src->mSampleCount;
    const int ns = (int)src->mChannelCount, nd = (int)dst->mChannelCount;
    if (ns < 1 || ns > kMaxCh || nd < 1 || nd > kMaxCh) return -3;
    int n = (int)avail;
    uint32_t cap = dst->mSampleLoadCount < dst->mSampleCount ? dst->mSampleLoadCount : dst->mSampleCount;
    if ((uint32_t)n > cap) n = (int)cap;
    if (n < 4) return -4;

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
    mRead = 0.0; mJumpCount = 0;
    ++mLoopRev;
    dst->setDirty();
    mState = PLAY;
    return n;
}

/* ── state transitions (audio thread) ──────────────────────────────────── */

void Noether::enterEmpty()
{
    mArmed = 0;
    clearUndo();
    mRestoring = false;
    mTailLeft = 0; mFirstTake = false;
    mState = EMPTY;
    if (mLoopLen > 0) ++mLoopRev;
    mLoopLen = 0;
    mWrite = 0;
    mRead = 0.0;
    mJumpCount = 0;
    mRecLvl = mRecGoal = 0.0f;
}

void Noether::enterRecord()
{
    clearUndo();
    mTailLeft = 0; mFirstTake = false;
    mState = RECORD;
    mWrite = 0;
    mLoopLen = 0;
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
        if (mSeamMatch && len > 2 * kMatchWin + kSeam + 8) {
            L = findSeam(d, len - (kSeam > kMatchWin ? kSeam : kMatchWin));
        }
#endif
        blendSeam(d, L);
        mLoopLen = L;
    } else {
        mLoopLen = len;
    }
    // Playback starts where the seam left off: at the head, which now carries
    // the tail — i.e. exactly where the recording would have continued.
    mRead = 0.0;
    mJumpCount = 0;
    ++mLoopRev;
    s->setDirty();
}

// The seam itself: crossfade buf[L..L+kSeam) INTO buf[0..kSeam), so the
// sample after the loop's last one is what was recorded next.
void Noether::blendSeam(float *d, int L)
{
    const int nc = mNc;
    const float inv = 1.0f / (float)kSeam;
    for (int k = 0; k < kSeam; ++k) {
        const float t = ((float)k + 0.5f) * inv;
        const float w = t * t * (3.0f - 2.0f * t);   // head fade-in
        cow(d, k);
        float *h = d + (size_t)k * nc;
        const float *tl = d + (size_t)(L + k) * nc;
        for (int c = 0; c < nc; ++c)
            h[c] = h[c] * w + tl[c] * (1.0f - w);
    }
}

// A clocked close: the loop is EXACTLY mWrite frames (the clock's length),
// playback starts now, and the next kSeam input frames are still captured
// as the tail (mTailLeft) so the seam can be blended without shortening.
void Noether::closeLoopExact()
{
    od::Sample *s = mpSample;
    if (!s || !s->mpData || mCap <= 0) { enterEmpty(); return; }
    int len = mWrite;
    if (len > mCap - kSeam - 1) len = mCap - kSeam - 1;
    if (len < 4) { enterEmpty(); return; }
    mLoopLen = len;
    mTailLeft = kSeam;
    mFirstTake = true;
    mRead = 0.0;
    mJumpCount = 0;
    ++mLoopRev;
    s->setDirty();
}

// Least-squared-difference match of buf[L..L+kMatchWin) against the head
// buf[0..kMatchWin), L in [hi - kSeamSearch, hi]. Coarse pass at stride 8
// comparing every kMatchDec-th frame of the FULL window (a half-window pass
// was tried first and tied on the flat top of a pulse wave — host `pulse`
// test; decimation keeps the window's extent so an edge always lands in
// it), then a fine pass +/-8 on every frame. The search reaches back 43 ms
// so one full period of anything down to 23 Hz fits: 0.7.1, after Nick heard
// a bass triangle kink at the seam with the old 10.7 ms reach. Among the
// coarse candidates the LATEST one within a small margin of the best error
// wins, so a periodic signal is cut one period back, not four — the loop
// stays as close as possible to where REC was pressed; ties (silence, DC)
// likewise keep the latest. One-shot at loop close: ~170k multiply-adds
// stereo, no divides, no allocation.
int Noether::findSeam(const float *d, int hi) const
{
    const int nc = mNc;
    // The reach is clamped so the tail window never overlaps the head window
    // (short loops still match, over what room there is).
    int lo = hi - kSeamSearch;
    if (lo < kMatchWin) lo = kMatchWin;
    if (lo > hi) return hi;
    const float *head = d;
    const int n  = kMatchWin * nc;
    const int dn = kMatchDec * nc;

    // coarse errors, index i <-> L = hi - 8 i
    constexpr int kMaxN = kSeamSearch / 8 + 1;
    float ce[kMaxN];
    const int kN = (hi - lo) / 8 + 1;
    int   bestI = 0;
    float bestE = 3.0e38f;
    for (int i = 0; i < kN; ++i) {
        const float *t = d + (size_t)(hi - 8 * i) * nc;
        float e = 0.0f;
        for (int k = 0; k < n; k += dn)
            for (int c = 0; c < nc; ++c) { const float df = t[k + c] - head[k + c]; e += df * df; }
        ce[i] = e;
        if (e < bestE) { bestE = e; bestI = i; }
    }

    // Fine pass (+/-8, every frame) around a coarse candidate.
    auto fine = [&](int c0, float &eOut) {
        int   bL = c0;
        float bE = 3.0e38f;
        for (int L = c0 + 8; L >= c0 - 8; --L) {
            if (L > hi || L < lo) continue;
            const float *t = d + (size_t)L * nc;
            float e = 0.0f;
            for (int k = 0; k < n; ++k) { const float df = t[k] - head[k]; e += df * df; }
            if (e < bE) { bE = e; bL = L; }
        }
        eOut = bE;
        return bL;
    };

    float eBest;
    int   LBest = fine(hi - 8 * bestI, eBest);
    // "About as good": within 25 % of the best, or a residual 25 dB below
    // the head's energy (a non-integer period puts the integer-sample best
    // several periods back; the nearest period is a hair worse and just as
    // inaudible after the blend).
    float energy = 0.0f;
    for (int k = 0; k < n; ++k) energy += head[k] * head[k];
    float margin = eBest * 1.25f + 1.0e-9f;
    const float floorE = energy * 0.003f;
    if (floorE > margin) margin = floorE;

    // The latest coarse LOCAL minimum that fine-tunes to within the margin
    // wins (at most a few fine passes, bounded for noise-like material).
    int passes = 0;
    for (int i = 0; i < bestI && passes < 4; ++i) {
        const bool localMin = (i == 0 || ce[i] <= ce[i - 1]) && (i + 1 >= kN || ce[i] <= ce[i + 1]);
        if (!localMin) continue;
        float e;
        const int L = fine(hi - 8 * i, e);
        ++passes;
        if (e <= margin) return L;
    }
    return LBest;
}

// Jump the read head to a window phase while a shadow head (absolute loop
// position) keeps playing from where it was and fades out over kSeam.
// A jump of the read head: the new position ramps in over kJumpIn while a
// shadow head keeps playing the old material and tails out over kJumpOut
// (capped at half the window so a short slice never hears its neighbour).
void Noether::beginJump(double toWindowPhase, double shadowAbs)
{
    mShadow = shadowAbs;
    mRead = toWindowPhase;
    int out = kJumpOut;
    if (mLoopLen > 0 && out > mLoopLen / 2) out = mLoopLen / 2;
    if (out < kJumpIn) out = kJumpIn;
    int in = kJumpIn;
    if (in > out) in = out;
    mJumpCount = out;
    mOutPh = 0.0f; mOutStep = (float)kJump / (float)out;
    mInPh  = 0.0f; mInStep  = (float)kJump / (float)in;
}

// End of an EXTEND: the write head is the new end of the loop. Same seam
// treatment as a first take (match + blend into the head).
void Noether::closeExtend()
{
    closeLoop();
    if (mState == EXTEND) { mState = PLAY; mRecGoal = 0.0f; mRecLvl = 0.0f; }
    mUndoLenB = mLoopLen;
    mUndoAvail = true;
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
    const bool  extend = sanitize(mExtIn.buffer()[0]) > 0.5f;
    const bool  stopGate = sanitize(mStopIn.buffer()[0]) > 0.5f;
    const float *undo = mUndoIn.buffer();
    const float *clk  = mClkIn.buffer();
    const bool  syncMode = mSyncOpt.value() == 2;
    const bool  clockPresent = mClkSince < kClkTimeout;
    if (!clockPresent) mArmed = 0;    // the clock went away: never a dead REC button
    const float playStep = mRecStep;   // 5 ms, same ramp as record
    const float aGlide = mGlideA;
    const float recStep = mRecStep;
    const float aDry    = mDryA;
    const float *rec = mRecIn.buffer();
    // window targets in loop samples (latched at the next wrap)

    if (!mInit) { mSpeedZ = speedT; mInit = true; }
    // sticky-NaN guards (positive tests, standard §5)
    if (!(mSpeedZ >= -kSpeedMax && mSpeedZ <= kSpeedMax)) mSpeedZ = speedT;
    if (!(mRead >= 0.0 && mRead < (double)(mLoopLen > 0 ? mLoopLen : 1))) mRead = 0.0;
    if (!(mShadow >= 0.0 && mShadow < (double)(mLoopLen > 0 ? mLoopLen : 1))) { mShadow = 0.0; mJumpCount = 0; }
    if (!(mRecLvl >= 0.0f && mRecLvl <= 1.0f)) mRecLvl = mRecGoal;
    if (!(mDryZ >= 0.0f && mDryZ <= 1.0f)) mDryZ = dry;
    if (!(mAaL > -8.0f && mAaL < 8.0f)) mAaL = 0.0f;
    if (!(mAaR > -8.0f && mAaR < 8.0f)) mAaR = 0.0f;

    float prevRec = mPrevRec, prevUndo = mPrevUndo, prevClk = mPrevClk;
    mStopped = stopGate;
    if (mRestoring) restoreSome(d);

    for (int i = 0; i < N; ++i) {
        const float xl = sanitize(inL[i]);
        const float xr = sanitize(inR[i]);

        // ── REC edge: the whole pedal grammar on one button ──
        const float r = rec[i];
        if (r > 0.5f && prevRec <= 0.5f) {
            switch (mState) {
            case EMPTY:   if (syncMode && clockPresent) { mArmed = 1; mClkAtStart = mClkCount; }
                          else { mArmed = 0; enterRecord(); }
                          break;
            case RECORD:  if (syncMode && clockPresent) { mArmed = 2; break; }   // stop on the next edge
                          mArmed = 0;
                          mWrite = (mWrite < 1) ? 1 : mWrite;  // at least one frame
                          mSyncN = 0;
                          closeLoop();
                          if (mState == RECORD) enterPlay();
                          break;
            case PLAY:    if (!mRestoring && !(mStopped && mPlayLvl <= 0.0f)) { beginPass(); mState = OVERDUB; mRecGoal = 1.0f; } break;
            case OVERDUB: mRecGoal = 0.0f; break;   // ramp out, then PLAY
            case EXTEND:  closeExtend(); break;
            case STOP:    break;
            }
        }
        prevRec = r;
        // ── CLOCK edge ──
        const float ce = clk[i];
        if (ce > 0.5f && prevClk <= 0.5f) onClockEdge();
        prevClk = ce;
        if (mClkSince < (1 << 30)) ++mClkSince;
        // ── UNDO edge ──
        const float ue = undo[i];
        if (ue > 0.5f && prevUndo <= 0.5f) {
            if (mState == EXTEND) closeExtend();
            if (mState == PLAY || mState == OVERDUB) beginUndo();
        }
        prevUndo = ue;
        // ── transport ramp ──
        if (mStopped) { if (mPlayLvl > 0.0f) { mPlayLvl -= playStep; if (mPlayLvl < 0.0f) mPlayLvl = 0.0f; } }
        else          { if (mPlayLvl < 1.0f) { mPlayLvl += playStep; if (mPlayLvl > 1.0f) mPlayLvl = 1.0f; } }
        const bool held = mStopped && mPlayLvl <= 0.0f;

        // ── record level ramp ──
        if (mRecLvl < mRecGoal) { mRecLvl += recStep; if (mRecLvl > mRecGoal) mRecLvl = mRecGoal; }
        else if (mRecLvl > mRecGoal) { mRecLvl -= recStep; if (mRecLvl < mRecGoal) mRecLvl = mRecGoal; }
        if (mState == OVERDUB && mRecGoal == 0.0f && mRecLvl == 0.0f) { mState = PLAY; ++mLoopRev; mUndoLenB = mLoopLen; }

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
            cow(d, mWrite);
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
            // a clocked close still captures kSeam frames of tail, then blends
            if (mTailLeft > 0 && mWrite < mCap) {
                float *w = d + (size_t)mWrite * mNc;
                w[0] = xl;
                if (mNc > 1) w[1] = xr;
                ++mWrite;
                if (--mTailLeft == 0) { blendSeam(d, L); mFirstTake = false; ++mLoopRev; }
            }
            // absolute loop position of the read head
            double pos = mRead;
            if (pos >= (double)L) pos -= (double)L;

            // overdub write at the read head (nearest sample), blended by the
            // record ramp so punching in/out never steps the existing audio
            if (mState == OVERDUB && mRecLvl > 0.0f && !held &&
                (mSpeedZ > kSpeedStop || mSpeedZ < -kSpeedStop)) {
                int wi = (int)(pos + 0.5);
                if (wi >= L) wi -= L;
                cow(d, wi);
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
                int ko = (int)mOutPh; if (ko > kJump) ko = kJump; if (ko < 0) ko = 0;
                int ki = (int)mInPh;  if (ki > kJump) ki = kJump; if (ki < 0) ki = 0;
                mOutPh += mOutStep; mInPh += mInStep;
                const float wOut = mXfade[ko];                 // old material, tailing out
                const float wIn  = 1.0f - mXfade[ki];          // new position, ramping in (1 ms)
                float sl = readHermite(d, mShadow, 0);
                float sr = (mNc > 1) ? readHermite(d, mShadow, 1) : sl;
                yl = yl * wIn + sl * wOut;
                yr = yr * wIn + sr * wOut;
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

            // advance (unless the transport holds), and handle the wrap
            if (held) break;
            mRead += (double)mSpeedZ;
            if (mRead >= (double)L || mRead < 0.0) {
                const bool fwd = mRead >= (double)L;
                // EXTEND: an overdub with the ext gate on runs off the end
                // at forward speed and keeps recording
                if (fwd && mState == OVERDUB && extend && mRecGoal > 0.0f) {
                    mLoop0 = L;
                    mWrite = L;
                    if (mWrite < mCap) { mState = EXTEND; ++mLoopRev; break; }
                }
                // the natural seam: buf[L] is buf[0] after the blend
                if (fwd) { mRead -= (double)L; if (mRead >= (double)L || mRead < 0.0) mRead = 0.0; }
                else     { mRead += (double)L; if (mRead < 0.0 || mRead >= (double)L) mRead = (double)L - 1.0; }
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
        yl *= level * mPlayLvl; yr *= level * mPlayLvl;
        outL[i] = xl * mDryZ + yl;
        outR[i] = xr * mDryZ + yr;
    }
    mPrevRec = prevRec; mPrevUndo = prevUndo; mPrevClk = prevClk;

    if (mState == RECORD || mState == OVERDUB || mState == EXTEND) s->setDirty();
    mCurrentIndex = (mState == RECORD || mState == EXTEND) ? mWrite : mVizPos;
}

} // namespace noether
