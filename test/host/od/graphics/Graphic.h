// Host-test stub of od/graphics/Graphic.h: geometry members as the SDK names
// them (mWorldLeft / mWorldBottom / mWidth / mHeight), a virtual draw.
#pragma once
#include <od/graphics/FrameBuffer.h>
namespace od {
struct Graphic {
    Graphic(int left, int bottom, int width, int height)
        : mLeft(left), mBottom(bottom), mWidth(width), mHeight(height),
          mWorldLeft(left), mWorldBottom(bottom) {}
    virtual ~Graphic() {}
    virtual void draw(FrameBuffer &fb) {}
    int mLeft, mBottom, mWidth, mHeight;
    int mWorldLeft, mWorldBottom;
};
} // namespace od
