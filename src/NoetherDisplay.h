/* NoetherDisplay.h — waveform + loop region + heads for Noether.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * Subclass of od::HeadDisplay (waveform + head marker for free), plus:
 *   - the freshly written region is INVALIDATED every draw so the cached
 *     waveform follows the recording (SampleView only redraws invalidated
 *     intervals; setDirty() alone is not enough — the stock
 *     RecordHeadDisplay does exactly this);
 *   - the loop [0, L) is the SampleView's marked region, so the part of the
 *     buffer that is the loop reads bright and the unused capacity faint;
 *   - a state word (REC / PLAY / DUB) in the corner.
 *
 * Every virtual is defined INLINE here: no out-of-line virtuals on a
 * framework subclass (habitat memory: non-COMDAT vtables hard-fault on
 * insert across firmware rebuilds). */

#pragma once

#include <od/graphics/sampling/HeadDisplay.h>
#include <Noether.h>

namespace noether {

class NoetherDisplay : public od::HeadDisplay
{
public:
    NoetherDisplay(Noether *head, int left, int bottom, int width, int height)
        : od::HeadDisplay(head, left, bottom, width, height)
    {
        mSampleView.setMarkColor(GRAY7);
    }
    virtual ~NoetherDisplay() {}

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
        Noether *h = (Noether *)mpHead;
        if (h == 0) { od::HeadDisplay::draw(fb); return; }
        od::Sample *pSample = h->getSample();
        if (pSample == 0) { od::HeadDisplay::draw(fb); return; }

        // Follow the write head: invalidate what was written since last draw.
        const int w = h->vizWrite();
        if (h->vizWriting()) {
            if (mLastWrite < w) {
                mSampleView.invalidateInterval(mLastWrite, w + 1);
            } else if (mLastWrite > w) {
                mSampleView.invalidateInterval(mLastWrite, (int)pSample->mSampleCount);
                mSampleView.invalidateInterval(0, w + 1);
            }
        }
        mLastWrite = w;

        // the loop, or the window inside it when one is latched
        const int L = h->getLoopSamples();
        const int ws = h->getWindowStart(), wl = h->getWindowLength();
        int me = ws + wl; if (me > L) me = L;
        mSampleView.setMarkedRegion(L > 0 ? ws : 0, L > 0 ? me : 0);

        od::HeadDisplay::draw(fb);

        const char *word = 0;
        switch (h->getState()) {
        case Noether::RECORD:  word = "REC";  break;
        case Noether::PLAY:    word = "PLAY"; break;
        case Noether::OVERDUB: word = "DUB";  break;
        case Noether::STOP:    word = "STOP"; break;
        case Noether::EXTEND:  word = "EXT";  break;
        default: break;
        }
        if (word) fb.text(WHITE, mWorldLeft + 2, mWorldBottom + mHeight - 8, word, 10);
    }

    int mLastWrite = 0;
#endif
};

} // namespace noether
