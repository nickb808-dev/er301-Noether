// Host-test stub of od/objects/Object.h
#pragma once
#include <od/config.h>
namespace od {
struct Port {
    const char *mName;
    float mBuf[FRAMELENGTH];
    explicit Port(const char *n) : mName(n) { for (int i = 0; i < FRAMELENGTH; ++i) mBuf[i] = 0.0f; }
    float *buffer() { return mBuf; }
};
using Inlet  = Port;
using Outlet = Port;
// Menu option (preset-serialised on device; 1-based, per the OptionControl
// convention). Stub mirrors the reseq/pullstretch harnesses.
struct Option {
    const char *mName;
    int mValue;
    Option(const char *n, int v = 0) : mName(n), mValue(v) {}
    int  value() { return mValue; }  // non-const, matching the device SDK
    void set(int v) { mValue = v; }
};
struct Object {
    virtual ~Object() {}
    virtual void process() {}
    void addInput(Port &) {}
    void addOutput(Port &) {}
    void addOption(Option &) {}
};
} // namespace od
