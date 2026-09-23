// Host-test stub of od/graphics/FrameBuffer.h: the primitive set the standard
// lists (§8), abstract, so a test can record every call.
#pragma once
namespace od {
typedef int Color;
struct FrameBuffer {
    virtual ~FrameBuffer() {}
    virtual void pixel(Color color, int x, int y) = 0;
    virtual void line(Color color, int x0, int y0, int x1, int y1) = 0;
    virtual void hline(Color color, int x, int x2, int y, int dotting = 0) = 0;
    virtual void vline(Color color, int x, int y, int y2, int dotting = 0) = 0;
    virtual int  text(Color color, int x, int y, const char *text, int size = 12, int alignment = 0) = 0;
    virtual void circle(Color color, int x, int y, int radius) = 0;
    virtual void fillCircle(Color color, int x, int y, int radius) = 0;
};
} // namespace od
