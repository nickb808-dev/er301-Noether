/* NoetherScale.h — sub display: the speed scale and the numbers.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * A horizontal scale, 0 at the centre, forward to the right and reverse to
 * the left, one octave per step: 1/4 1/2 1 2. The marker is the effective
 * (glided) speed; on a detent it is hollow, like the head on the reel. Under
 * it: the ratio and semitone offset, the loop length, and the section the
 * head is in. logf is used for the semitone readout (UI thread; only
 * sinf / cosf are the am335x hazard). Inline virtuals only. */

#pragma once

#include <od/graphics/Graphic.h>
#include <od/graphics/constants.h>
#include "Noether.h"
#include <cmath>
#include <cstdio>

namespace noether {

class NoetherScale : public od::Graphic
{
public:
    NoetherScale(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpHead(0) {}
    virtual ~NoetherScale()
    {
        if (mpHead) mpHead->release();
    }
    void follow(Noether *p)
    {
        if (mpHead) mpHead->release();
        mpHead = p;
        if (mpHead) mpHead->attach();
    }

#ifndef SWIGLUA
    // |s| -> 0..1 along one half of the scale: 0 at 0, then one step per
    // octave from 1/4 (0.25) to 2 (1.0); linear below 1/4.
    static inline float place(float a)
    {
        if (a <= 0.25f) return 0.25f * (a / 0.25f);
        if (a > 2.0f) a = 2.0f;
        float u = 0.25f;
        while (a > 0.25f * 1.0001f && u < 1.0f) { a *= 0.5f; u += 0.25f; }
        const float frac = (a - 0.125f) / 0.125f;   // linear within the octave: close enough for 13 px
        return u - 0.25f + 0.25f * frac;
    }

    virtual void draw(od::FrameBuffer &fb)
    {
        Noether *h = mpHead;
        const int x0 = mWorldLeft, y0 = mWorldBottom;
        const int cx = x0 + mWidth / 2;
        const int half = mWidth / 2 - 10;
        const int yAxis = y0 + mHeight - 22;

        // the axis and its octave ticks
        fb.hline(GRAY5, cx - half, cx + half, yAxis);
        static const char *kLabels[4] = { "1/4", "1/2", "1", "2" };
        for (int i = 0; i < 4; ++i) {
            const float u = 0.25f * (float)(i + 1);
            const int dx = (int)(u * (float)half + 0.5f);
            fb.vline(GRAY7, cx + dx, yAxis - 2, yAxis + 2);
            fb.vline(GRAY7, cx - dx, yAxis - 2, yAxis + 2);
            const int lw = (i >= 2) ? 3 : 9;
            fb.text(GRAY7, cx + dx - lw / 2, yAxis + 5, kLabels[i], 8);
            fb.text(GRAY7, cx - dx - lw / 2, yAxis + 5, kLabels[i], 8);
        }
        fb.vline(GRAY9, cx, yAxis - 3, yAxis + 3);
        fb.text(GRAY7, cx - 2, yAxis + 5, "0", 8);
        fb.text(GRAY5, cx - half, yAxis + 13, "rev", 8);
        fb.text(GRAY5, cx + half - 14, yAxis + 13, "fwd", 8);

        if (!h) return;
        const float sp = h->getSpeed();
        const float a = sp < 0.0f ? -sp : sp;
        const int dx = (int)(place(a) * (float)half + 0.5f);
        const int mx = sp < 0.0f ? cx - dx : cx + dx;
        if (h->getDetent()) { fb.circle(WHITE, mx, yAxis, 3); fb.pixel(WHITE, mx, yAxis); }
        else                { fb.fillCircle(WHITE, mx, yAxis, 2); }

        // the numbers
        char buf[48];
        if (a < 0.001f) {
            fb.text(WHITE, x0 + 4, y0 + 16, "stopped", 10);
        } else {
            const float st = 12.0f * logf(a) * 1.4426950f;   // 12 * log2(|s|)
            const char *rev = sp < 0.0f ? "rev " : "";
            if (h->getDetent()) {
                const char *lab = a >= 1.5f ? "2x" : a >= 0.75f ? "1x" : a >= 0.375f ? "1/2" : "1/4";
                snprintf(buf, sizeof buf, "%s%s  %+.0f st", rev, lab, (double)st);
            } else {
                snprintf(buf, sizeof buf, "%s%.2fx  %+.1f st", rev, (double)a, (double)st);
            }
            fb.text(WHITE, x0 + 4, y0 + 16, buf, 10);
        }
        const int L = h->getLoopSamples();
        if (L > 0) {
            const int sec = h->getSections() > 0 ? h->getSections() : 1;
            const int cur = (int)((long)h->vizPos() * sec / L) + 1;
            // clocked: bar X / Y (the edge phase over the loop's periods);
            // otherwise the section the head is in
            const int bars = h->getBars();
            if (bars > 0 && h->hasClock()) snprintf(buf, sizeof buf, "%.2f s  bar %d/%d", (double)h->getLoopSeconds(), h->getBar(), bars);
            else                           snprintf(buf, sizeof buf, "%.2f s   %d/%d%s", (double)h->getLoopSeconds(), cur, sec, h->hasClock() ? "  clk" : "");
            fb.text(GRAY9, x0 + 4, y0 + 4, buf, 10);
        } else {
            if (h->getBars() > 0) {
                snprintf(buf, sizeof buf, "rec  bar %d/%d", h->getBar(), h->getBars());
                fb.text(WHITE, x0 + 4, y0 + 4, buf, 10);
            } else if (h->getState() == Noether::RECORD) {
                fb.text(GRAY7, x0 + 4, y0 + 4, h->isArmed() ? "rec  (closes on clk)" : "rec", 10);
            } else {
                fb.text(GRAY7, x0 + 4, y0 + 4, h->isArmed() ? "armed: waiting for clk" : (h->hasClock() ? "no loop   clk" : "no loop"), 10);
            }
        }
    }

    Noether *mpHead;
#endif
};

} // namespace noether
