// Host-test stub of od/objects/heads/Head.h
//
// FAITHFUL TO THE REAL Head::setSample, including the order:
//
//     Sample *p = mpSample;
//     mpSample = 0;               // "Prevent audio code from accessing"
//     if (p) p->release();
//     p = sample;
//     if (p) { p->attach(); mEndIndex = p->mSampleCount - 2; }
//     mpSample = p;               // publish last
//
// The old stub was `mpSample = s;` and nothing else, which meant no host test
// could ever observe a reference-counting mistake. That is how the Slices
// attach()/release() bug survived eighteen green suites.
#pragma once
#include <od/objects/Object.h>
#include <od/audio/Sample.h>
namespace od {
struct Head : Object {
    Sample *mpSample      = nullptr;
    int     mCurrentIndex = 0;
    int     mEndIndex     = 0;

    virtual void setSample(Sample *sample)
    {
        Sample *p = mpSample;
        mpSample = nullptr;                 // publish nothing mid-swap
        if (p) p->release();
        p = sample;
        if (p) {
            p->attach();
            mEndIndex = int(p->mSampleCount) - 2;
        } else {
            mEndIndex = 0;
        }
        mCurrentIndex = 0;
        mpSample = p;
    }
    virtual ~Head() { if (mpSample) mpSample->release(); }

    Sample *getSample() { return mpSample; }
    int getPosition() { return mCurrentIndex; }
    void attach() {}
    void release() {}
};
} // namespace od
