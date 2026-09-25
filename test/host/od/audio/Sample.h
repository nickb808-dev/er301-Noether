// Host-test stub of od/audio/Sample.h
#pragma once
#include <cstddef>
#include <cstdint>
namespace od {
struct Sample {
    // od::Sample IS ReferenceCounted on the device — Head::setSample attaches
    // and releases it. The stub models that (and, like Slices, records death
    // at zero) so a host test can see a lifetime mistake instead of assuming
    // one cannot happen.
    int  mRefs = 0;
    bool mDestroyed = false;
    void attach()  { ++mRefs; }
    void release() { --mRefs; if (mRefs == 0) mDestroyed = true; }
    int  refs() const { return mRefs; }
    bool destroyed() const { return mDestroyed; }

    // Mirrors the device SDK. mSampleCount is what the buffer was ALLOCATED
    // for; mSampleLoadCount is how much has actually been LOADED. They differ
    // while a file is still coming off the card — which is exactly the window
    // an attach lands in.
    uint32_t mSampleCount     = 0;
    uint32_t mSampleLoadCount = 0;
    uint32_t mChannelCount    = 1;   // uint32_t on the device SDK
    float   *mpData           = nullptr;
    float    mSampleRate      = 48000.0f;
    bool     mDirty           = false;
    void setDirty() { mDirty = true; }
    void zero() { if (mpData) for (uint32_t i = 0; i < mSampleCount * mChannelCount; ++i) mpData[i] = 0.0f; mDirty = true; }

    // Test helper: mark the buffer fully loaded (the SDK's setMemoryOnly()).
    void setMemoryOnly() { mSampleLoadCount = mSampleCount; }
};
} // namespace od
