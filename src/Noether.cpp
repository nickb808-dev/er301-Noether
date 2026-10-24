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
    addInput(mDubIn);
    addInput(mStopIn);
    addInput(mUndoIn);
    addInput(mClkIn);
    addOption(mSyncOpt);
    addInput(mSpeedIn);
    addInput(mVoctIn);
    addInput(mExtIn);
    addInput(mBarsIn);
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
void Noether::setKeepClean(int on) { mKeepClean = (on != 0); }
void Noether::zeroBuffer()         { mZeroRequested = true; }   // done by the audio thread, see process()
int   Noether::isRestoring()       { return mRestoring ? 1 : 0; }
int   Noether::canUndo()           { return (mUndoAvail && mpUndo && mpUndo->mChannelCount == (uint32_t)mNc) ? 1 : 0; }

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
    mWValid = false;
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
    // the length swap happens now
    const int newLen = (mLoopLen == mUndoLenA) ? mUndoLenB : mUndoLenA;
    if (newLen > 0 && newLen <= mCap) {
        mLoopLen = newLen;
        mWValid = false;
        if (mRead >= (double)newLen) mRead = 0.0;
        mJumpCount = 0;
    }
    ++mLoopRev;
}

void Noether::restoreSome(float *d)
{
    if (!mRestoring) return;
    float *u = mpUndo ? mpUndo->mpData : 0;
    // same frame layout or nothing: indexing the undo buffer with the loop's
    // channel count would walk off its end (a mono undo under a stereo loop)
    if (!u || mpUndo->mChannelCount != (uint32_t)mNc) { mRestoring = false; return; }
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
    if (mRestLeft <= 0) { mRestoring = false; ++mLoopRev; markDirty(mpSample); }
}
int   Noether::getState()          { return (mState == PLAY && mStopped && mPlayLvl <= 0.0f) ? (int)STOP : (int)mState; }
int   Noether::getLoopSamples()    { return mLoopLen; }
float Noether::getLoopSeconds()    { return (float)mLoopLen * (1.0f / (float)kSampleRate); }
float Noether::getSpeed()          { return mSpeedZ; }
void  Noether::setSeamMatch(int on){ mSeamMatch = (on != 0); }
int   Noether::getSeamMatch()      { return mSeamMatch ? 1 : 0; }
int   Noether::getLoopRev()        { return mLoopRev; }
int   Noether::getDetent()         { return mDetent ? 1 : 0; }
int   Noether::getSections()
{
    // a clocked loop's ticks mark BARS when it is a whole number of them
    // (0.8.9: 64 pulse ticks for 4 bars of 16ths would just be a grey ring)
    if (!(mSyncN > 0 && mSyncOpt.value() == 2)) return kSections;
    const int ppb = mPpb > 0 ? mPpb : 1;
    return (ppb > 1 && mSyncN % ppb == 0) ? mSyncN / ppb : mSyncN;
}
void  Noether::setSyncN(int n)     { mSyncN = (n > 0 && n < 1024) ? n : 0; mEdgePhase = 0; }
int   Noether::getSyncN()          { return mSyncN; }
int   Noether::isArmed()           { return mArmed; }
int   Noether::hasClock()          { return mClkSince < kClkTimeout ? 1 : 0; }
// The bar readouts (UI thread: the divides here never run on the audio
// thread). While a counted take records: bars taken / the count. While a
// clocked loop plays: the bar the PLAY HEAD is in / the loop's bars — in
// pulses when the loop is not a whole number of bars (a manual take can
// close on any pulse; the display then says "beat"). While held: where the
// clock will bring it back in. While extending: the bar being added.
// Until 0.8.14 the playing readout counted clock pulses (mEdgePhase): right
// at 1x only — at 0.5x it showed bar 2 while the downbeat re-sync kept the
// head in bar 1, it ran on while stopped, and it froze during an extension.
void  Noether::setPulsesPerBar(int n) { mPpb = n < 1 ? 1 : (n > 96 ? 96 : n); }
int   Noether::getPulsesPerBar()      { return mPpb; }
// the pulse (0 .. N-1) a readout shows, or -1 when there is none
int   Noether::readoutPulse()
{
    if (!(mSyncN > 0 && mSyncOpt.value() == 2 && mLoopLen > 0)) return -1;
    if (mState == EXTEND) return mSyncN + mExtEdges;
    if (mState != PLAY && mState != OVERDUB) return -1;
    if (mStopped && mPlayLvl <= 0.0f) return mEdgePhase < mSyncN ? mEdgePhase : mSyncN - 1;
    long p = (long)mVizPos * mSyncN / mLoopLen;
    if (p < 0) p = 0;
    if (p >= mSyncN) p = mSyncN - 1;
    return (int)p;
}
int   Noether::getBar()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    if (mState == RECORD && mBars > 0 && mSyncOpt.value() == 2) {
        const int b = (mClkCount - mClkAtStart) / ppb + 1;
        return b > mBars ? mBars : b;
    }
    const int p = readoutPulse();
    if (p < 0) return 0;
    if (mState == EXTEND) return p / ppb + 1;
    return (mSyncN % ppb == 0) ? p / ppb + 1 : p + 1;
}
int   Noether::getBars()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    if (mState == RECORD) return (mBars > 0 && mSyncOpt.value() == 2) ? mBars : 0;
    if (!(mSyncN > 0 && mSyncOpt.value() == 2 && mLoopLen > 0)) return 0;
    if (mState == EXTEND) {
        // counted: what it will be; manual: what it is so far (whole bars)
        const int total = mSyncN + (mBars > 0 ? mBars * ppb : mExtEdges);
        const int b = (total + ppb - 1) / ppb;
        return b > 0 ? b : 1;
    }
    return (mSyncN % ppb == 0) ? mSyncN / ppb : mSyncN;
}
int   Noether::getBarsWhole()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    if (mState == RECORD || mState == EXTEND) return 1;
    return (ppb < 2 || mSyncN % ppb == 0) ? 1 : 0;
}
int   Noether::getDubArmed() { return mDubArm; }
int   Noether::getDubBars()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    return (mState == OVERDUB && mDubTotal > 0) ? mDubTotal / ppb : 0;
}
int   Noether::getDubBar()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    if (!(mState == OVERDUB && mDubTotal > 0)) return 0;
    const int b = (mDubTotal - mDubLeft) / ppb + 1;
    const int n = mDubTotal / ppb;
    return b > n ? n : b;
}
int   Noether::getBeat()
{
    const int ppb = mPpb > 0 ? mPpb : 1;
    if (ppb < 2 || mSyncOpt.value() != 2) return 0;
    if (mState == RECORD) return mBars > 0 ? (mClkCount - mClkAtStart) % ppb + 1 : 0;
    const int p = readoutPulse();
    if (p < 0) return 0;
    if (mState == EXTEND) return p % ppb + 1;
    return (mSyncN % ppb == 0) ? p % ppb + 1 : 0;
}

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
    // A counted take closes itself on the edge that completes the count;
    // a second REC press (mArmed == 2) still closes early on the next edge.
    if (mState == RECORD && mArmed != 2 && mBars > 0 && mClkCount - mClkAtStart >= mBars * mPpb)
        mArmed = 2;
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
    // EXTEND under the clock (0.8.10): count its pulses and bar lines; it
    // closes ON the clock — at the next bar line after a REC press, or by
    // itself after Bars bars — and the loop's pulse count grows by them
    if (mState == EXTEND) {
        // the downbeat AT the extension's start is its boundary, not its
        // first pulse: at 1x the head runs off the end one sample before
        // that edge (it advances after the read), so the edge lands just
        // inside EXTEND — counted, Bars 1 added 3 pulses, not 4 (host)
        if (mExtEdges == 0 && mWrite - mLoop0 < kSyncTol) return;
        ++mExtEdges;
        if (++mExtBeat >= mPpb) mExtBeat = 0;
        if ((mExtArm && mExtBeat == 0) || (mBars > 0 && mExtEdges >= mBars * mPpb))
            closeExtendExact();
        return;
    }
    if ((mState == PLAY || mState == OVERDUB) && mSyncN > 0) {
        ++mEdgePhase;
        if (mEdgePhase >= mSyncN) {          // the N-th edge: the downbeat
            mEdgePhase = 0;
            const int L = mLoopLen;
            // held (stop): the head stays put; the release puts it where the
            // clock is (process, 0.8.14) — until then this moved a silent head
            const bool held = mStopped && mPlayLvl <= 0.0f;
            if (!held && L > 0 && !(mRead < (double)kSyncTol || mRead > (double)(L - kSyncTol)))
                beginJump(0.0, mRead);
        }
        // bar lines of the loop, counted from its downbeat (a counter, no %)
        if (mEdgePhase == 0) mBeatInBar = 0;
        else if (++mBeatInBar >= mPpb) mBeatInBar = 0;
        const bool barLine = mBeatInBar == 0;
        // OVERDUB AS A CLOCKED TAKE (0.8.10): a counted overdub punches out
        // by itself after Bars bars; an armed one punches in / out on a bar
        if (mState == OVERDUB && mDubLeft > 0 && --mDubLeft == 0) {
            mRecGoal = 0.0f; mDubArm = 0;
        }
        if (mDubArm == 1 && mState == PLAY && barLine) {
            beginPass(); mState = OVERDUB; mRecGoal = 1.0f; mDubArm = 0;
            mDubTotal = mDubLeft = (mBars > 0) ? mBars * mPpb : 0;
        } else if (mDubArm == 2 && mState == OVERDUB && barLine) {
            mRecGoal = 0.0f; mDubArm = 0; mDubLeft = 0;
        }
    }
}

// Sync mode: an extension closes ON the clock (0.8.10). Until 0.8.9 it
// closed the free way (a phase-matched cut anywhere) and KEPT the old pulse
// count, so every old-N pulses the downbeat re-sync yanked the longer loop
// back to its start. Now: the exact length at this edge, N grows by the
// extension's pulses, this edge is the new downbeat. No pre-roll seam (the
// head has been overdubbed since the take): the clocked head blend.
void Noether::closeExtendExact()
{
    const double from = (double)(mWrite - mLoop0);
    mSyncN += mExtEdges;
    if (mSyncN > 1023) mSyncN = 1023;
    closeLoopExact(false);
    if (mState == EXTEND && mLoopLen > 0 && from >= 0.0 && from < (double)mLoopLen)
        beginJump(0.0, from, kJumpOut);
    if (mState == EXTEND) { mState = PLAY; mRecGoal = 0.0f; mRecLvl = 0.0f; }
    mUndoLenB = mLoopLen;
    mUndoAvail = true;
    mEdgePhase = 0; mBeatInBar = 0;
    mExtArm = false; mExtEdges = 0; mExtBeat = 0;
    mDubArm = 0; mDubLeft = 0; mDubTotal = 0;
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
    // 0.8.7: never into a buffer that is still LOADING (a buffer the player
    // saved at quicksave time comes back from its file after a reboot): the
    // pool's loader would overwrite the copy, or the engine would see the
    // unloaded buffer and drop to EMPTY — the loop then "visible, not playing"
    if (dst->mSampleLoadCount < dst->mSampleCount) return -5;
    uint32_t cap = dst->mSampleCount;
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
    markDirty(dst);
    mHandover = kHandover;              // fade in, like a closed take (enterPlay)
    mState = PLAY;
    return n;
}

int Noether::playBuffer(int frames)
{
    od::Sample *dst = mpSample;
    if (!dst || !dst->mpData) return -2;
    if (dst->mSampleLoadCount < dst->mSampleCount) return -5;
    const int nd = (int)dst->mChannelCount;
    if (nd < 1 || nd > kMaxCh) return -3;
    int n = frames;
    if ((uint32_t)n > dst->mSampleCount) n = (int)dst->mSampleCount;
    if (n < 4) return -4;
    enterEmpty();                       // the audio thread lets go, then the loop is published
    mNc = nd;
    mCap = (int)dst->mSampleCount;
    mLoopLen = n;
    mRead = 0.0; mJumpCount = 0;
    ++mLoopRev;
    mHandover = kHandover;              // fade in, like any loop that starts
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
    mWValid = false;
    mPreCount = 0;          // the pre-roll restarts: nothing before EMPTY precedes the next take
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
    // a new take is counted from THIS edge count, and has no periods yet: a
    // take started with no clock present must not inherit an old count
    // (bars would close it on the first edge) or an old loop's N
    mClkAtStart = mClkCount;
    mSyncN = 0;
    mEdgePhase = 0;
    mWrite = 0;
    mLoopLen = 0;
    mRead = 0.0;
    mJumpCount = 0;
    mRecLvl = mRecGoal = 1.0f;   // first take: full level, no ramp — the seam
                                 // blend takes care of both ends
}

// Only ever called when a FIRST TAKE closes (manual, clocked, buffer full).
// While recording the loop is silent and only the input is monitored; at the
// close the loop starts from its head, whose first sample is whatever was
// recorded there — generally out of phase with the live input. 0.8.3 started
// it at full level (a step); 0.8.4 faded it in over the 5 ms linear ramp,
// still a thump on low tones (shorter than their period, cornered ends).
// Now a smoothstep handover over kHandover (21 ms), dry level included.
void Noether::enterPlay()
{
    mState = PLAY;
    mRecGoal = 0.0f;
    mHandover = kHandover;   // 0.8.5: a 21 ms smoothstep, not the 5 ms linear ramp
}

// Blend the tail of the take into its head and shorten the loop by kSeam.
// After this, buf[L] (the sample recorded right after the loop's last
// sample) IS buf[0] at the seam, so the wrap is continuous.
void Noether::closeLoop(bool preRoll)
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
        int F = kSeamClocked;
        if (F > (L >> 1)) F = L >> 1;
        if (F > mPreCount) F = mPreCount;
        if (preRoll && F >= kPreMin) { preRollSeam(d, L, F); mSeamPre = 1; }   // 0.8.6: the downbeat untouched
        else { blendSeam(d, L); mSeamPre = 0; }
        mLoopLen = L;
    } else {
        mLoopLen = len;
    }
    // Playback starts where the seam left off: at the head, which now carries
    // the tail — i.e. exactly where the recording would have continued.
    mRead = 0.0;
    mJumpCount = 0;
    ++mLoopRev;
    markDirty(s);
}

// The seam itself: crossfade buf[L..L+kSeam) INTO buf[0..kSeam), so the
// sample after the loop's last one is what was recorded next.
void Noether::blendSeam(float *d, int L, int len)
{
    const int nc = mNc;
    const float inv = 1.0f / (float)len;
    for (int k = 0; k < len; ++k) {
        const float t = ((float)k + 0.5f) * inv;
        const float w = t * t * (3.0f - 2.0f * t);   // head fade-in
        cow(d, k);
        float *h = d + (size_t)k * nc;
        const float *tl = d + (size_t)(L + k) * nc;
        for (int c = 0; c < nc; ++c)
            h[c] = h[c] * w + tl[c] * (1.0f - w);
    }
}

// Equal-power gains without libm (no trig on the audio thread, standard and
// habitat memory): sin(pi/2 u) by its odd Taylor series to x^9, scaled so
// u = 1 gives exactly 1; cos(pi/2 u) = sin(pi/2 (1 - u)). To x^7 only, the
// series reaches 0.99985 at u = 1 — the fade never arrived at the pre-roll
// and the residue was a corner at the wrap (host: 4x the sine's curvature).
static inline float sinHalfPiRaw(float u)
{
    const float x = 1.57079633f * u, x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f - x2 * (1.0f / 362880.0f)))));
}
static inline float sinHalfPi(float u)
{
    // 1 / sinHalfPiRaw(1) as a literal (float arithmetic, computed once off
    // the device): a function-local static initialised by a call needs
    // __cxa_guard_acquire / release at runtime — two new firmware imports
    // for one constant, caught by the nm diff against 0.8.5
    const float kNorm = 0.999996424f;
    return u >= 1.0f ? 1.0f : sinHalfPiRaw(u) * kNorm;
}

// The pre-roll seam: buf[L-F+k] fades from the loop's own end toward the
// pre-roll frame that preceded the take's start by F-k, so buf[L-1] is the
// sample recorded just before buf[0] and the wrap is the recording's own
// continuity. The head (the downbeat) is not written. The SHAPE follows the
// correlation of the two signals over the window, both channels together
// (previously DESIGN §5.2): correlated -> equal gain (equal power would peak
// +3 dB: Noether 0.6.5 measured that on re-syncs), uncorrelated -> equal
// power (equal gain dips 3 dB), a blend between. Compared as rho |rho|
// against squared thresholds: no sqrt. Zero energy counts as correlated.
void Noether::preRollSeam(float *d, int L, int F)
{
    const int nc = mNc;
    const int oldest = (mPrePos - F) & (kPreRoll - 1);   // pre-roll frame for k = 0
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int k = 0; k < F; ++k) {
        const float *a = d + (size_t)(L - F + k) * nc;
        const float *b = mPre + (size_t)((oldest + k) & (kPreRoll - 1)) * kMaxCh;
        for (int c = 0; c < nc; ++c) { ab += (double)a[c] * b[c]; aa += (double)a[c] * a[c]; bb += (double)b[c] * b[c]; }
    }
    // rho |rho| = ab |ab| / (aa bb); equal gain at rho >= 0.7, equal power at
    // rho <= 0.2, linear in rho^2 between
    float m = 1.0f;                                 // 1 = equal gain
    if (aa > 1.0e-12 && bb > 1.0e-12) {
        const double r2 = ab * (ab < 0.0 ? -ab : ab) / (aa * bb);   // signed rho^2
        const double lo = 0.04, hi = 0.49;
        m = (float)(r2 <= lo ? 0.0 : (r2 >= hi ? 1.0 : (r2 - lo) / (hi - lo)));
        mSeamRho = (float)r2;                       // signed rho^2
    } else {
        mSeamRho = 1.0f;
    }
    const float inv = 1.0f / (float)F;
    for (int k = 0; k < F; ++k) {
        // t reaches exactly 1 on the LAST frame: buf[L-1] must be the pre-roll
        // frame itself, or the residue is a corner at the wrap (measured: 4x)
        const float t = ((float)k + 1.0f) * inv;
        const float sm = t * t * (3.0f - 2.0f * t);                  // smoothstep
        const float gIn  = m * sm + (1.0f - m) * sinHalfPi(sm);          // toward the pre-roll
        const float gOut = m * (1.0f - sm) + (1.0f - m) * sinHalfPi(1.0f - sm);
        float *a = d + (size_t)(L - F + k) * nc;
        const float *b = mPre + (size_t)((oldest + k) & (kPreRoll - 1)) * kMaxCh;
        for (int c = 0; c < nc; ++c) a[c] = a[c] * gOut + b[c] * gIn;
    }
}

// A clocked close: the loop is EXACTLY mWrite frames (the clock's length),
// playback starts now, and the next kSeam input frames are still captured
// as the tail (mTailLeft) so the seam can be blended without shortening.
void Noether::closeLoopExact(bool firstTake)
{
    od::Sample *s = mpSample;
    if (!s || !s->mpData || mCap <= 0) { enterEmpty(); return; }
    // 0.8.6: with enough pre-roll the seam lives at the END of the loop and
    // needs nothing recorded after it — exact length, no tail, no clamp
    // (a first take only: an extension's head has been overdubbed since)
    if (firstTake) {
        int len = mWrite;
        if (len > mCap) len = mCap;
        int F = kSeamClocked;
        if (F > (len >> 1)) F = len >> 1;
        if (F > mPreCount) F = mPreCount;
        if (len >= 4 && F >= kPreMin) {
            mLoopLen = len;
            mSeamLen = F;
            preRollSeam(s->mpData, len, F);
            mSeamPre = 1;
            mTailLeft = 0;
            mFirstTake = false;
            mRead = 0.0;
            mJumpCount = 0;
            ++mLoopRev;
            markDirty(s);
            return;
        }
    }
    mSeamPre = 0;
    // no pre-roll (a take started within kPreMin of the buffer appearing):
    // the 0.8.5 head blend with a deferred tail
    int len = mWrite;
    if (len > mCap - kSeamClocked - 1) len = mCap - kSeamClocked - 1;
    if (len < 4) { enterEmpty(); return; }
    // the clocked crossfade, at most half the loop (short loops at fast clocks)
    mSeamLen = kSeamClocked;
    if (mSeamLen > (len >> 1)) mSeamLen = len >> 1;
    mLoopLen = len;
    // The tail is buf[len .. len+kSeam). Normally none of it exists yet
    // (mWrite == len). If the clamp above shortened the loop, part or all of
    // it was already recorded: capture only the rest, contiguously, or blend
    // now — a tail that can never finish would leave the seam unblended and
    // mFirstTake (no undo) set forever.
    int tail = len + mSeamLen - mWrite;
    if (tail > mSeamLen) tail = mSeamLen;
    mFirstTake = firstTake;              // an extension's seam blend is undoable (cow)
    if (tail <= 0) {
        blendSeam(s->mpData, len, mSeamLen);
        mFirstTake = false;
        mTailLeft = 0;
    } else {
        mTailLeft = tail;
    }
    mRead = 0.0;
    mJumpCount = 0;
    ++mLoopRev;
    markDirty(s);
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
    float *ce = mSeamErr;              // a member: 1 KB off the 2 KB audio stack
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

// The overdub write head (0.8.2). Until 0.8.1 an overdub wrote the input to
// the NEAREST frame at the read head: exact at 1x, but below 1x several
// input samples landed on one frame (SOS applied two, four ... times per
// pass, the input decimated with no filter) and above 1x frames were
// skipped (never decayed). Heard as "sample rate mismatch" that grows with
// every pass. Now every frame the head CROSSES between the previous sample
// and this one is written exactly once, with the input interpolated to the
// moment of the crossing — a tape head, not a sample-and-hold. At exactly
// 1x the crossing is this sample (tau = 1), bit-identical to the old write.
// No floor(): the positions are >= 0, so an int cast is the floor.
void Noether::dubWrite(float *d, double pos, int L, float xl, float xr, float sos)
{
    const float g = mRecLvl;
    double dp = pos - mWPos;
    if (dp > 0.5 * (double)L) dp -= (double)L;
    else if (dp < -0.5 * (double)L) dp += (double)L;
    if (!mWValid || dp > 4.0 || dp < -4.0) {
        // first sample of a pass, or after a jump: the frame under the head
        int wi = (int)(pos + 0.5);
        if (wi >= L) wi -= L;
        dubCell(d, wi, xl, xr, g, sos);
    } else if (dp > 0.0) {
        const double inv = 1.0 / dp;
        for (int c = (int)mWPos + 1, k = 0; k < 4; ++c, ++k) {
            const double reach = (double)c - mWPos;          // (0, dp]
            if (reach > dp) break;
            const float tau = (float)(reach * inv);
            const int ci = c >= L ? c - L : c;
            dubCell(d, ci, mXpL * (1.0f - tau) + xl * tau, mXpR * (1.0f - tau) + xr * tau, g, sos);
        }
    } else if (dp < 0.0) {
        const double inv = -1.0 / dp;
        int c = (int)mWPos;
        if ((double)c == mWPos) --c;                          // the largest frame BELOW
        for (int k = 0; k < 4; --c, ++k) {
            const double reach = mWPos - (double)c;          // (0, -dp]
            if (reach > -dp) break;
            const float tau = (float)(reach * inv);
            const int ci = c < 0 ? c + L : c;
            dubCell(d, ci, mXpL * (1.0f - tau) + xl * tau, mXpR * (1.0f - tau) + xr * tau, g, sos);
        }
    }
    mWPos = pos;
    mXpL = xl; mXpR = xr;
    mWValid = true;
}

// A jump of the read head: the new position ramps in over kJumpIn while a
// shadow head (absolute loop position) keeps playing the old material and
// tails out over kJumpOut (capped at half the loop for very short loops).
void Noether::beginJump(double toWindowPhase, double shadowAbs, int inFrames)
{
    mWValid = false;
    mShadow = shadowAbs;
    mRead = toWindowPhase;
    int out = kJumpOut;
    if (mLoopLen > 0 && out > mLoopLen / 2) out = mLoopLen / 2;
    if (out < kJumpIn) out = kJumpIn;
    int in = inFrames < 1 ? 1 : inFrames;
    if (in > out) in = out;
    mJumpCount = out;
    mOutPh = 0.0f; mOutStep = (float)kJump / (float)out;
    mInPh  = 0.0f; mInStep  = (float)kJump / (float)in;
}

// End of an EXTEND: the write head is the new end of the loop. Same seam
// treatment as a first take (match + blend into the head).
// The head moves at this close: during EXTEND the loop is heard one cycle
// back (frame mWrite - L0), after it playback starts at the new seam. Until
// 0.8.4 that was a hard cut (host `close`: 316x the curvature a smooth sum
// can have); now it is the jump machinery with a symmetric 8 ms crossfade
// from where the head was to the new seam.
void Noether::closeExtend()
{
    const bool wasExtend = mState == EXTEND;
    const double from = (double)(mWrite - mLoop0);
    closeLoop();
    if (wasExtend && mState == EXTEND && mLoopLen > 0 && from >= 0.0 && from < (double)mLoopLen)
        beginJump(0.0, from, kJumpOut);   // nothing to keep sharp here: a plain 8 ms crossfade
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
    // CLEAR: a loop that can be heard fades out (5 ms) before EMPTY; until
    // 0.8.5 it stopped dead — a click whenever it was not silent.
    if (mClearRequested) {
        mClearRequested = false;
        if (mState == PLAY || mState == OVERDUB || mState == EXTEND) { mClearing = true; mClearK = 0; }
        else enterEmpty();
    }
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
    // overdub input anti-alias: below 1x the buffer is COARSER than the input
    // (1/|speed| input samples per frame), so the input is low-passed at the
    // buffer's Nyquist (0.5 |speed| cycles/sample) by two one-poles; exactly
    // 1 (a bypass) at and above 1x, where the write interpolates instead
    float aW = 1.0f;
    {
        const float sp = mSpeedZ < 0.0f ? -mSpeedZ : mSpeedZ;
        if (sp < 1.0f) aW = 1.0f - expf(-3.14159265f * sp);
    }
    const float sos    = clampf(sanitize(mSosIn.buffer()[0]), 0.0f, 1.0f);
    const float dry    = clampf(sanitize(mDryIn.buffer()[0]), 0.0f, 1.0f);
    const float level  = clampf(sanitize(mLevelIn.buffer()[0]), 0.0f, 1.0f);
    const bool  extend = sanitize(mExtIn.buffer()[0]) > 0.5f;
    {
        const float b = clampf(sanitize(mBarsIn.buffer()[0]), 0.0f, (float)kBarsMax);
        mBars = (int)(b + 0.5f);
    }
    const bool  stopGate = sanitize(mStopIn.buffer()[0]) > 0.5f;
    const float *undo = mUndoIn.buffer();
    const float *clk  = mClkIn.buffer();
    const bool  syncMode = mSyncOpt.value() == 2;
    const bool  clockPresent = mClkSince < kClkTimeout;
    if (!clockPresent) mArmed = 0;    // the clock went away: never a dead REC button
    // and the sync overdub / extend arms (0.8.10) resolve at once the same way
    const bool dubSync = syncMode && clockPresent && mSyncN > 0;
    if (!dubSync) {
        if (mDubArm == 1 && mState == PLAY) { beginPass(); mState = OVERDUB; mRecGoal = 1.0f; }
        else if (mDubArm == 2 && mState == OVERDUB) mRecGoal = 0.0f;
        mDubArm = 0; mDubLeft = 0; mDubTotal = 0;
        if (mExtArm && mState == EXTEND) closeExtend();
        mExtArm = false;
    }
    const float playStep = mRecStep;   // 5 ms, same ramp as record
    const float aGlide = mGlideA;
    const float recStep = mRecStep;
    const float aDry    = mDryA;
    const float *rec = mRecIn.buffer();

    if (!mInit) { mSpeedZ = speedT; mInit = true; }
    // sticky-NaN guards (positive tests, standard §5)
    if (!(mSpeedZ >= -kSpeedMax && mSpeedZ <= kSpeedMax)) mSpeedZ = speedT;
    if (!(mRead >= 0.0 && mRead < (double)(mLoopLen > 0 ? mLoopLen : 1))) mRead = 0.0;
    if (!(mShadow >= 0.0 && mShadow < (double)(mLoopLen > 0 ? mLoopLen : 1))) { mShadow = 0.0; mJumpCount = 0; }
    if (!(mRecLvl >= 0.0f && mRecLvl <= 1.0f)) mRecLvl = mRecGoal;
    if (!(mDryZ >= 0.0f && mDryZ <= 1.0f)) mDryZ = dry;
    if (!(mAaL > -8.0f && mAaL < 8.0f)) mAaL = 0.0f;
    if (!(mAaR > -8.0f && mAaR < 8.0f)) mAaR = 0.0f;
    if (!(mWl1 > -8.0f && mWl1 < 8.0f && mWl2 > -8.0f && mWl2 < 8.0f)) { mWl1 = mWl2 = 0.0f; }
    if (!(mWr1 > -8.0f && mWr1 < 8.0f && mWr2 > -8.0f && mWr2 < 8.0f)) { mWr1 = mWr2 = 0.0f; }
    if (!(mXpL > -8.0f && mXpL < 8.0f)) mXpL = 0.0f;
    if (!(mXpR > -8.0f && mXpR < 8.0f)) mXpR = 0.0f;
    // denormal guard on every recursive state (see flushTiny)
    mAaL = flushTiny(mAaL); mAaR = flushTiny(mAaR);
    mWl1 = flushTiny(mWl1); mWl2 = flushTiny(mWl2);
    mWr1 = flushTiny(mWr1); mWr2 = flushTiny(mWr2);
    mDryZ = flushTiny(mDryZ); mRecLvl = flushTiny(mRecLvl);
    if (!(mWPos >= 0.0 && mWPos < (double)(mLoopLen > 0 ? mLoopLen : 1))) mWValid = false;

    float prevRec = mPrevRec, prevUndo = mPrevUndo, prevClk = mPrevClk, prevDub = mPrevDub;
    const float *dubIn = mDubIn.buffer();
    // STOP during a take (0.8.14): it ends the take the way a REC press
    // would — free at once, sync on the next edge, so the loop keeps a whole
    // number of pulses — and the loop lands held, ready to start from the
    // stop button. Until 0.8.13 a stop while recording did nothing to the
    // take: it kept recording, and the loop it later closed into was silent
    // and frozen (Nick: "it kind of holds"). A waiting take or dub is
    // cancelled; a running dub punches out.
    if (stopGate && !mPrevStop && !mClearing) {
        switch (mState) {
        case EMPTY:   mArmed = 0; break;
        case RECORD:
            if (syncMode && clockPresent) { mArmed = 2; break; }
            mArmed = 0;
            mWrite = (mWrite < 1) ? 1 : mWrite;
            mSyncN = 0;
            closeLoop(true);
            if (mState == RECORD) enterPlay();
            break;
        case PLAY:    mDubArm = 0; mDubLeft = 0; mDubTotal = 0; break;
        case OVERDUB: mRecGoal = 0.0f; mDubArm = 0; mDubLeft = 0; mDubTotal = 0; break;
        case EXTEND:
            if (syncMode && clockPresent) mExtArm = true;
            else closeExtend();
            break;
        default: break;
        }
    }
    // STOP RELEASED in sync (0.8.14): the loop comes back in where the
    // clock is — the head the downbeat re-sync would have carried there —
    // not where it stopped. It used to resume in place, out of time, until
    // the next downbeat jumped it. It is silent at this moment (held), so
    // no crossfade: the 5 ms transport ramp brings it in.
    if (!stopGate && mPrevStop && syncMode && clockPresent && mSyncN > 0 && mLoopLen > 0 &&
        (mState == PLAY || mState == OVERDUB) && mPlayLvl <= 0.0f) {
        const double L = (double)mLoopLen;
        double frac = mClkPeriod > 0 ? (double)mClkSince / (double)mClkPeriod : 0.0;
        if (frac > 1.0) frac = 1.0;
        double pos = (double)mSpeedZ * ((double)mEdgePhase + frac) * L / (double)mSyncN;
        // |speed| <= 2 and phase <= N + 1 pulses: within a few loops, so a
        // bounded wrap (no fmod — libm on the audio thread)
        for (int k = 0; k < 4 && pos >= L; ++k) pos -= L;
        for (int k = 0; k < 4 && pos < 0.0; ++k) pos += L;
        if (!(pos >= 0.0 && pos < L)) pos = 0.0;
        mRead = pos;
        mJumpCount = 0;
        mWValid = false;
    }
    mPrevStop = stopGate;
    mStopped = stopGate;
    if (mRestoring) restoreSome(d);

    for (int i = 0; i < N; ++i) {
        const float xl = sanitize(inL[i]);
        const float xr = sanitize(inR[i]);

        // ── REC edge: the whole pedal grammar on one button ──
        const float r = rec[i];
        if (r > 0.5f && prevRec <= 0.5f) {
            // a REC press during the clear's 21 ms fade is a press on an EMPTY
            // loop (finish the clear now) — never an overdub the clear discards
            if (mClearing) { mClearing = false; enterEmpty(); }
            switch (mState) {
            case EMPTY:   if (syncMode && clockPresent) { mArmed = 1; mClkAtStart = mClkCount; }
                          else { mArmed = 0; enterRecord(); }
                          break;
            case RECORD:  if (syncMode && clockPresent) { mArmed = 2; break; }   // stop on the next edge
                          mArmed = 0;
                          mWrite = (mWrite < 1) ? 1 : mWrite;  // at least one frame
                          mSyncN = 0;
                          closeLoop(true);
                          if (mState == RECORD) enterPlay();
                          break;
            case PLAY:
                if (mRestoring || (mStopped && mPlayLvl <= 0.0f)) break;
                // sync mode (0.8.10): an overdub is a clocked take — armed
                // here, in on the next bar line; pressing again while armed
                // cancels. Free mode: in at once.
                if (dubSync) mDubArm = (mDubArm == 1) ? 0 : 1;
                else { beginPass(); mState = OVERDUB; mRecGoal = 1.0f; }
                break;
            case OVERDUB:
                if (dubSync) mDubArm = 2;                  // out on the next bar line
                else mRecGoal = 0.0f;                      // ramp out, then PLAY
                break;
            case EXTEND:
                if (syncMode && clockPresent) mExtArm = true;   // close on the next bar line
                else closeExtend();
                break;
            case STOP:    break;
            }
        }
        prevRec = r;
        // ── DUB edge (0.8.10): the unsynced overdub — in / out at THIS sample
        //    whatever the sync mode; a waiting bar-line overdub starts now, a
        //    counted one stops now; with no loop there is nothing to dub ──
        {
            const float g = sanitize(dubIn[i]);
            if (g > 0.5f && prevDub <= 0.5f && !mClearing) {
                switch (mState) {
                case PLAY:
                    if (mRestoring || (mStopped && mPlayLvl <= 0.0f)) break;
                    beginPass(); mState = OVERDUB; mRecGoal = 1.0f;
                    mDubArm = 0; mDubLeft = 0; mDubTotal = 0;
                    break;
                case OVERDUB:
                    mRecGoal = 0.0f; mDubArm = 0; mDubLeft = 0; mDubTotal = 0;
                    break;
                case EXTEND:                              // closes as rec does
                    if (syncMode && clockPresent) mExtArm = true;
                    else closeExtend();
                    break;
                default: break;                           // EMPTY / RECORD / STOP
                }
            }
            prevDub = g;
        }
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
        float clearGain = 1.0f;
        if (mClearing) {
            if (mClearK >= kHandover) { mClearing = false; enterEmpty(); }
            else clearGain = mXfade[mClearK++];   // falling smoothstep, 21 ms
        }

        // ── record level ramp ──
        if (mRecLvl < mRecGoal) { mRecLvl += recStep; if (mRecLvl > mRecGoal) mRecLvl = mRecGoal; }
        else if (mRecLvl > mRecGoal) { mRecLvl -= recStep; if (mRecLvl < mRecGoal) mRecLvl = mRecGoal; }
        if (mState == OVERDUB && mRecGoal == 0.0f && mRecLvl == 0.0f) { mState = PLAY; ++mLoopRev; mUndoLenB = mLoopLen; }

        // ── speed glide ──
        // A float one-pole never ARRIVES: once a * gap is under half an ulp
        // of the speed, it stalls ~1e-4 short (a "1x" of 0.99989 — measured),
        // and the head then drifts across frames forever. Snap the last 2e-4
        // (0.35 cents) so a detent is exactly its ratio again.
        {
            const float gap = speedT - mSpeedZ;
            if (gap < 2.0e-4f && gap > -2.0e-4f) mSpeedZ = speedT;
            else mSpeedZ += aGlide * gap;
        }

        float yl = 0.0f, yr = 0.0f;   // loop output
        switch (mState) {
        case EMPTY: {
            // the pre-roll ring: what preceded the next take (0.8.6)
            float *pr = mPre + (size_t)mPrePos * kMaxCh;
            pr[0] = xl; pr[1] = xr;
            mPrePos = (mPrePos + 1) & (kPreRoll - 1);
            if (mPreCount < kPreRoll) ++mPreCount;
            mDryZ += aDry * (1.0f - mDryZ);
            outL[i] = xl * mDryZ; outR[i] = xr * mDryZ;
            continue;
        }

        case RECORD: {
            float *w = d + (size_t)mWrite * mNc;
            w[0] = xl;
            if (mNc > 1) w[1] = xr;
            ++mWrite;
            if (mWrite >= mCap) {
                // the buffer is full: a free close, whatever the clock was
                // doing — not a whole number of periods, so no N and no arm
                mArmed = 0; mSyncN = 0;
                closeLoop(true); if (mState == RECORD) enterPlay();
            }
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
            // the stop fade applies here too: a sync extension stopped mid-bar
            // records on to the bar line, and the loop must not step to
            // silence when it closes held
            yl *= level * mPlayLvl * clearGain; yr *= level * mPlayLvl * clearGain;
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
                if (--mTailLeft == 0) { blendSeam(d, L, mSeamLen); mFirstTake = false; ++mLoopRev; }
            }
            // absolute loop position of the read head
            double pos = mRead;
            if (pos >= (double)L) pos -= (double)L;

            // overdub write at the read head (nearest sample), blended by the
            // record ramp so punching in/out never steps the existing audio
            if (mState == OVERDUB && mRecLvl > 0.0f && !held &&
                (mSpeedZ > kSpeedStop || mSpeedZ < -kSpeedStop)) {
                float il = xl, ir = xr;
                if (aW < 1.0f) {
                    mWl1 += aW * (il - mWl1); mWl2 += aW * (mWl1 - mWl2); il = mWl2;
                    mWr1 += aW * (ir - mWr1); mWr2 += aW * (mWr1 - mWr2); ir = mWr2;
                } else { mWl1 = mWl2 = il; mWr1 = mWr2 = ir; }
                // HEAD GAP: the write head trails the play head by kHeadGap
                // frames (ahead of it in reverse), so every frame the Hermite
                // read touches (-1 .. +2) still holds the loop as it was
                // before this pass. Until 0.8.2 the write landed AT the play
                // head and was read straight back: what you heard while
                // dubbing carried a second, decimated copy of the input.
                double wpos = mSpeedZ > 0.0f ? pos - (double)kHeadGap : pos + (double)kHeadGap;
                if (wpos < 0.0) wpos += (double)L;
                else if (wpos >= (double)L) wpos -= (double)L;
                if (!(wpos >= 0.0 && wpos < (double)L)) wpos = 0.0;   // loops shorter than the gap
                dubWrite(d, wpos, L, il, ir, sos);
            } else {
                mWValid = false;
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
                    mExtEdges = 0; mExtBeat = 0; mExtArm = false;
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
        float handGain = 1.0f;
        if (mHandover > 0) {
            // the close's handover: the loop rises and the dry level (forced
            // to 1 while recording) settles to Dry along the SAME smoothstep
            const float xf = mXfade[kHandover - mHandover];   // 1 -> 0
            handGain = 1.0f - xf;
            mDryZ = dryT + (mDryZ > dryT ? (1.0f - dryT) : 0.0f) * xf;
            if (!(mDryZ >= 0.0f && mDryZ <= 1.0f)) mDryZ = dryT;
            --mHandover;
        } else {
            mDryZ += aDry * (dryT - mDryZ);
        }
        yl *= level * mPlayLvl * clearGain * handGain; yr *= level * mPlayLvl * clearGain * handGain;
        outL[i] = xl * mDryZ + yl;
        outR[i] = xr * mDryZ + yr;
    }
    mPrevRec = prevRec; mPrevUndo = prevUndo; mPrevClk = prevClk; mPrevDub = prevDub;

    // ZERO after a clear, a slice per block, never under the write head: in
    // EMPTY from the start, in RECORD (a new take began) only ahead of it;
    // anything else (the take already closed) ends the pass.
    if (mZeroRequested && !mClearing && (mState == EMPTY || mState == RECORD)) {
        mZeroRequested = false; mZeroing = true; mZeroPos = 0;
    }
    if (mZeroing) {
        if (mState == EMPTY || mState == RECORD) {
            int from = mZeroPos;
            if (mState == RECORD && from < mWrite + 1) from = mWrite + 1;
            int to = from + kZeroPerBlock;
            if (to > mCap) to = mCap;
            if (from < to) memset(d + (size_t)from * mNc, 0, (size_t)(to - from) * mNc * sizeof(float));
            mZeroPos = to > from ? to : from;
            if (mZeroPos >= mCap) mZeroing = false;
            markDirty(s);
        } else {
            mZeroing = false;
        }
    }
    if (mState == RECORD || mState == OVERDUB || mState == EXTEND) markDirty(s);
    mCurrentIndex = (mState == RECORD || mState == EXTEND) ? mWrite : mVizPos;
}

} // namespace noether
