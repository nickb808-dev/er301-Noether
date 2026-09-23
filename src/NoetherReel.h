/* NoetherReel.h — the loop as a reel.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * ONE glyph answers position, direction, speed, window, sections, state and
 * content (standard §8: one space, layered):
 *   - the loop's amplitude envelope drawn in POLAR form: the ring's thickness
 *     at each angle is the loudness there; 12 o'clock is the seam, clockwise
 *     is forward;
 *   - the Start/Len window is the bright arc, the rest of the ring faint;
 *   - kSections radial ticks divide the loop; the section the head is in is
 *     drawn brightest;
 *   - the read head is a dot on the ring. Its COMET TAIL is the distance from
 *     normal pitch: none at 1x, longer per octave away, pointing opposite to
 *     travel (so it doubles as a direction cue). On a speed detent the dot is
 *     drawn as a small hollow ring — the "LED goes solid" moment;
 *   - recording WINDS the ring on from 12 o'clock (one turn per 4 s, further
 *     turns one radius step out — tape onto a reel); Extend continues the
 *     winding outside the finished loop; overdub thickens the ring live.
 *
 * Costs are all off the audio thread: a kBins peak envelope of the loop is
 * computed here, incrementally (kBinsPerDraw bins per frame, strided reads,
 * only the bins the write head touched while overdubbing), and the ring is
 * kBins short radial lines from a compile-time cos/sin table (NoetherLut.h —
 * package sinf/cosf miscompute on am335x).
 *
 * Every virtual is defined INLINE (no out-of-line virtuals on a framework
 * subclass). Reads pool memory in draw() the way SampleView does; the buffer
 * is the head's own memory-only pool buffer, held while attached. */

#pragma once

#include <od/graphics/Graphic.h>
#include <od/graphics/constants.h>
#include "Noether.h"
#include "NoetherLut.h"

namespace noether {

class NoetherReel : public od::Graphic
{
public:
    NoetherReel(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpHead(0)
    {
        for (int i = 0; i < kBins; ++i) mEnv[i] = 0.0f;
    }
    virtual ~NoetherReel()
    {
        if (mpHead) mpHead->release();
    }

    void follow(Noether *p)
    {
        if (mpHead) mpHead->release();
        mpHead = p;
        if (mpHead) mpHead->attach();
        mEnvRev = -1;
    }

#ifndef SWIGLUA
    static const int kBins = 96;          // envelope resolution around the ring
    static const int kBinsPerDraw = 12;   // incremental envelope work per frame
    static const int kMaxStride = 512;    // reads per bin, at most
    static const int kWindSamples = 4 * 48000;   // one turn of winding while recording

    virtual void draw(od::FrameBuffer &fb)
    {
        Noether *h = mpHead;
        const int cx = mWorldLeft + mWidth / 2;
        const int cy = mWorldBottom + mHeight / 2;
        int R = (mWidth < mHeight ? mWidth : mHeight) / 2 - 13;
        if (R < 12) R = 12;
        if (!h) { fb.circle(GRAY3, cx, cy, R); return; }

        const int state = h->getState();
        od::Sample *pSample = h->getSample();
        const int L = h->getLoopSamples();

        if (state == Noether::EMPTY || !pSample || !pSample->mpData) {
            fb.circle(GRAY3, cx, cy, R);
            fb.text(GRAY7, cx - 9, cy - 4, "rec", 10);
            mEnvRev = -1;
            return;
        }

        if (state == Noether::RECORD) {
            // winding on: an arc from 12 o'clock, one turn per kWindSamples,
            // each further turn one step outward
            drawWinding(fb, cx, cy, R, h->vizWrite(), kWindSamples, WHITE);
            fb.text(WHITE, cx - 9, cy - 4, "REC", 10);
            mEnvRev = -1;
            return;
        }

        // ── the ring: envelope, window, sections ──
        const int L0 = (state == Noether::EXTEND) ? h->vizLoop0() : L;
        if (L0 <= 0) { fb.circle(GRAY3, cx, cy, R); return; }
        updateEnvelope(h, pSample, L0);

        const int ws = h->getWindowStart();
        int wl = h->getWindowLength(); if (wl <= 0 || wl > L0) wl = L0;
        const bool windowed = (ws != 0 || wl != L0);
        const int sections = h->getSections() > 0 ? h->getSections() : 1;
        const int pos = h->vizPos();
        const int curSection = (int)((long)pos * sections / (L0 > 0 ? L0 : 1));

        for (int i = 0; i < kBins; ++i) {
            const int a = (i * kLutN) / kBins;
            const int s0 = (int)((long)i * L0 / kBins);
            bool inWin = true;
            if (windowed) {
                int rel = s0 - ws; if (rel < 0) rel += L0;
                inWin = rel < wl;
            }
            const int sec = (int)((long)i * sections / kBins);
            int color;
            if (!inWin) color = GRAY3;
            else if (sec == curSection && state != Noether::EXTEND) color = WHITE;
            else color = GRAY9;
            const float e = mEnv[i];
            const int r0 = R - 1;
            const int r1 = R + 1 + (int)(e * 5.0f + 0.5f);
            radial(fb, color, cx, cy, a, r0, r1);
        }
        // section ticks, just outside the ring
        for (int k = 0; k < sections; ++k) {
            const int a = (k * kLutN) / sections;
            radial(fb, GRAY6, cx, cy, a, R + 8, R + 10);
        }

        if (state == Noether::EXTEND) {
            // the new material winds on outside the finished loop
            const int w = h->vizWrite() - L0;
            drawWinding(fb, cx, cy, R + 6, w, L0, WHITE);
            fb.text(WHITE, cx - 9, cy - 4, "EXT", 10);
            return;
        }

        // ── the head and its comet tail ──
        const int headA = (int)(((long)pos * kLutN) / L0) & (kLutN - 1);
        const float sp = h->getSpeed();
        const bool locked = h->getDetent() != 0;
        {
            // tail length: 4 px per octave from 1x (either way), cap 12
            float ratio = sp < 0.0f ? -sp : sp;
            if (ratio < 0.0625f) ratio = 0.0625f;
            if (ratio < 1.0f) ratio = 1.0f / ratio;
            int oct4 = 0;   // quarter-octaves from 1x: 2x -> 4, 4x -> 8
            while (ratio > 1.0905f && oct4 < 12) { ratio *= 0.8409f; ++oct4; }   // 2^(1/4) steps, 2^(1/8) threshold
            const int n = oct4;
            const int dir = sp >= 0.0f ? -1 : 1;    // tail trails behind travel
            for (int t = 1; t <= n; ++t) {
                const int a = (headA + dir * t) & (kLutN - 1);
                const int c = 11 - (t * 9) / (n > 0 ? n : 1);
                const int px = cx + (int)(kLutSin[a] * (float)R + 0.5f);
                const int py = cy + (int)(kLutCos[a] * (float)R + 0.5f);
                fb.pixel(c < GRAY3 ? GRAY3 : c, px, py);
            }
        }
        const int hx = cx + (int)(kLutSin[headA] * (float)R + 0.5f);
        const int hy = cy + (int)(kLutCos[headA] * (float)R + 0.5f);
        if (locked) { fb.circle(WHITE, hx, hy, 3); fb.pixel(WHITE, hx, hy); }
        else        { fb.fillCircle(WHITE, hx, hy, 2); }

        if (state == Noether::OVERDUB) fb.text(WHITE, cx - 9, cy - 4, "DUB", 10);
        else if (state == Noether::STOP) fb.text(GRAY9, cx - 11, cy - 4, "STOP", 10);
    }

    // ── envelope: kBins peak bins over [0, L0), incremental ──
    void updateEnvelope(Noether *h, od::Sample *s, int L0)
    {
        const int rev = h->getLoopRev();
        if (rev != mEnvRev || L0 != mEnvLen) {
            mEnvRev = rev; mEnvLen = L0; mCursor = 0; mPending = kBins;
            for (int i = 0; i < kBins; ++i) mDirty[i] = 1;
            mLastWrite = h->vizPos();
        }
        // overdub: the bins the write head crossed since last frame are stale
        if (h->vizWriting() && h->getState() == Noether::OVERDUB) {
            const int w = h->vizPos();
            int b0 = (int)((long)mLastWrite * kBins / L0), b1 = (int)((long)w * kBins / L0);
            if (b0 < 0) b0 = 0;
            if (b0 >= kBins) b0 = kBins - 1;
            if (b1 < 0) b1 = 0;
            if (b1 >= kBins) b1 = kBins - 1;
            int n = b1 - b0; if (n < 0) n += kBins;
            if (n > kBins / 2) n = kBins / 2;                 // a long jump: bound the work
            for (int k = 0; k <= n; ++k) { int b = b0 + k; if (b >= kBins) b -= kBins; if (!mDirty[b]) { mDirty[b] = 1; ++mPending; } }
            mLastWrite = w;
        }
        if (mPending <= 0) return;
        const float *d = s->mpData;
        const int nc = (int)s->mChannelCount;
        int budget = kBinsPerDraw;
        for (int k = 0; k < kBins && budget > 0; ++k) {
            int b = mCursor + k; if (b >= kBins) b -= kBins;
            if (!mDirty[b]) continue;
            const int s0 = (int)((long)b * L0 / kBins);
            const int s1 = (int)((long)(b + 1) * L0 / kBins);
            int span = s1 - s0; if (span < 1) span = 1;
            int stride = span / kMaxStride; if (stride < 1) stride = 1;
            float peak = 0.0f;
            for (int i = s0; i < s1; i += stride) {
                const float *f = d + (size_t)i * nc;
                float v = f[0] < 0.0f ? -f[0] : f[0];
                if (nc > 1) { const float g = f[1] < 0.0f ? -f[1] : f[1]; if (g > v) v = g; }
                if (v > peak) peak = v;
            }
            mEnv[b] = peak / (peak + 0.25f);       // soft curve into [0, 1), no libm
            mDirty[b] = 0; --mPending; --budget;
            mCursor = (b + 1 >= kBins) ? 0 : b + 1;
        }
    }

    // a radial line at LUT angle `a` (0 = 12 o'clock, clockwise) from r0 to r1
    static inline void radial(od::FrameBuffer &fb, int color, int cx, int cy, int a, int r0, int r1)
    {
        a &= (kLutN - 1);
        const float sx = kLutSin[a], cyy = kLutCos[a];
        const int x0 = cx + (int)(sx * (float)r0 + 0.5f), y0 = cy + (int)(cyy * (float)r0 + 0.5f);
        const int x1 = cx + (int)(sx * (float)r1 + 0.5f), y1 = cy + (int)(cyy * (float)r1 + 0.5f);
        fb.line(color, x0, y0, x1, y1);
    }

    // an arc from 12 o'clock, `w` samples long at `perTurn` samples per turn,
    // each full turn one radius step further out; a dot at its end
    static inline void drawWinding(od::FrameBuffer &fb, int cx, int cy, int R, int w, int perTurn, int color)
    {
        if (perTurn < 1) perTurn = 1;
        if (w < 0) w = 0;
        const int turns = w / perTurn;
        const int rem = w - turns * perTurn;
        const int maxTurns = 2;
        for (int t = 0; t < turns && t < maxTurns; ++t)
            fb.circle(GRAY7, cx, cy, R + 3 * t);
        const int r = R + 3 * (turns < maxTurns ? turns : maxTurns);
        const int endA = (int)(((long)rem * kLutN) / perTurn);
        int px = cx, py = cy + r;
        for (int a = 1; a <= endA; ++a) {
            const int x = cx + (int)(kLutSin[a & (kLutN - 1)] * (float)r + 0.5f);
            const int y = cy + (int)(kLutCos[a & (kLutN - 1)] * (float)r + 0.5f);
            fb.line(color, px, py, x, y);
            px = x; py = y;
        }
        fb.fillCircle(WHITE, px, py, 2);
    }

    Noether *mpHead;
    float mEnv[kBins];
    unsigned char mDirty[kBins] = {0};
    int mEnvRev = -1, mEnvLen = 0, mCursor = 0, mPending = 0, mLastWrite = 0;
#endif
};

} // namespace noether
