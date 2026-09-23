/* Host verification harness for Noether (no hardware).
 * Build: g++ -std=c++11 -O2 -ffast-math -Itest/host -Isrc \
 *          src/Noether.cpp test/host/main.cpp -o test/t
 * Modes: ident · length · seam · pulse · speed · sos · window · extend · voct · persist ·
 *        detent · aa · viz · nan · cpu · asan
 *
 * Every test prints the number it judged, not just PASS (standard §6/§10). */
#include "Noether.h"
#include "NoetherReel.h"
#include "NoetherScale.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <ctime>

using noether::Noether;

static const double kPi = 3.14159265358979;
static const int    kSR = 48000;

/* ── a pool buffer stand-in ────────────────────────────────────────────── */
struct Buffer {
    od::Sample s;
    std::vector<float> data;
    Buffer(int ch, int frames) {
        data.assign((size_t)ch * frames, 0.0f);
        s.mpData = data.data();
        s.mChannelCount = ch;
        s.mSampleCount = frames;
        s.setMemoryOnly();
    }
};

struct Ctl { const char *port; float v; };

static void setInputs(Noether &d, const Ctl *c, int n)
{
    od::Inlet *ins[] = { &d.mLeftIn, &d.mRightIn, &d.mRecIn, &d.mSpeedIn,
                         &d.mSosIn, &d.mDryIn, &d.mLevelIn,
                         &d.mVoctIn, &d.mStartIn, &d.mLenIn, &d.mExtIn };
    for (int i = 0; i < n; ++i) {
        bool matched = false;
        for (auto *p : ins)
            if (!strcmp(p->mName, c[i].port)) {
                matched = true;
                for (int s = 0; s < FRAMELENGTH; ++s) p->buffer()[s] = c[i].v;
            }
        if (!matched) { fprintf(stderr, "setInputs: UNKNOWN PORT '%s'\n", c[i].port); abort(); }
    }
}

static void base(Noether &d, float speed = 1.0f, float sos = 0.5f, float dry = 1.0f, float level = 1.0f)
{
    Ctl c[] = {{"Speed", speed}, {"SOS", sos}, {"Dry", dry}, {"Level", level}, {"Rec", 0.0f},
               {"V/Oct", 0.0f}, {"Start", 0.0f}, {"Len", 1.0f}, {"Extend", 0.0f}};
    setInputs(d, c, 9);
}

// Feed `src` (mono, may be shorter than the block) through one block; a REC
// edge at sample `edgeAt` (< 0 = none). Returns the left output.
static void block(Noether &d, const float *src, int n, int edgeAt, std::vector<float> &outL)
{
    float *L = d.mLeftIn.buffer(), *R = d.mRightIn.buffer(), *rec = d.mRecIn.buffer();
    for (int i = 0; i < FRAMELENGTH; ++i) {
        float x = (i < n) ? src[i] : 0.0f;
        L[i] = x; R[i] = x;
        rec[i] = (edgeAt >= 0 && i >= edgeAt) ? 1.0f : 0.0f;
    }
    d.process();
    const float *o = d.mLeftOut.buffer();
    for (int i = 0; i < FRAMELENGTH; ++i) outL.push_back(o[i]);
    // the Comparator emits a one-block-ish pulse; drop the gate afterwards
    for (int i = 0; i < FRAMELENGTH; ++i) rec[i] = 0.0f;
}

static void silentBlocks(Noether &d, int blocks, std::vector<float> &outL)
{
    std::vector<float> z(FRAMELENGTH, 0.0f);
    for (int b = 0; b < blocks; ++b) block(d, z.data(), FRAMELENGTH, -1, outL);
}

// Record a sine of `freq` for `frames` samples: REC edge at sample `edge0`
// of block 0, close edge `frames` samples later (sample-accurate).
static void recordSine(Noether &d, double freq, int frames, int edge0, std::vector<float> &outL, double amp = 0.8)
{
    int total = edge0 + frames;
    int blocks = (total + FRAMELENGTH) / FRAMELENGTH + 1;
    std::vector<float> src(FRAMELENGTH);
    long t = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < FRAMELENGTH; ++i, ++t)
            src[i] = (float)(amp * sin(2.0 * kPi * freq * (double)(t - edge0) / kSR));
        int edge = -1;
        if (b == 0) edge = edge0;
        int closeSample = total - b * FRAMELENGTH;
        if (closeSample >= 0 && closeSample < FRAMELENGTH && b > 0) edge = closeSample;
        if (b == 0 && closeSample >= 0 && closeSample < FRAMELENGTH) {
            fprintf(stderr, "recordSine: take too short for the harness\n"); abort();
        }
        block(d, src.data(), FRAMELENGTH, edge, outL);
    }
}

static double maxAbsDiff(const std::vector<float> &x, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from + 1; i < to && i < x.size(); ++i) {
        double dd = fabs((double)x[i] - x[i - 1]);
        if (dd > m) m = dd;
    }
    return m;
}

static int zeroCrossings(const std::vector<float> &x, size_t from, size_t to)
{
    int n = 0;
    for (size_t i = from + 1; i < to && i < x.size(); ++i)
        if ((x[i] >= 0.0f) != (x[i - 1] >= 0.0f)) ++n;
    return n;
}

static bool allFinite(const std::vector<float> &x)
{
    for (float v : x) if (!std::isfinite(v)) return false;
    return true;
}

/* ── modes ─────────────────────────────────────────────────────────────── */

// EMPTY passes the input bit-identically; no buffer = a wire.
static int t_ident()
{
    Noether d(2);
    base(d);
    std::vector<float> out;
    std::vector<float> src(FRAMELENGTH);
    for (int i = 0; i < FRAMELENGTH; ++i) src[i] = (float)sin(i * 0.1);
    block(d, src.data(), FRAMELENGTH, -1, out);            // no buffer
    Buffer b(2, kSR);
    d.setSample(&b.s);
    block(d, src.data(), FRAMELENGTH, -1, out);            // EMPTY with buffer
    int bad = 0;
    for (int i = 0; i < FRAMELENGTH; ++i) {
        if (out[i] != src[i]) ++bad;
        if (out[FRAMELENGTH + i] != src[i]) ++bad;
    }
    printf("ident: %d of %d samples differ from input (EMPTY / no-buffer)\n", bad, 2 * FRAMELENGTH);
    d.setSample(nullptr);
    return bad == 0 ? 0 : 1;
}

// Record `frames` samples of a 100 Hz pulse (period 480), edges mid-block.
static void recordPulse(Noether &d, int frames, int edge0, std::vector<float> &outL, int period = 480)
{
    int total = edge0 + frames;
    int blocks = (total + FRAMELENGTH) / FRAMELENGTH + 1;
    std::vector<float> src(FRAMELENGTH);
    long t = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
            long ph = (t - edge0) % period; if (ph < 0) ph += period;
            src[i] = (ph < period / 2) ? 0.8f : -0.8f;
        }
        int edge = -1;
        if (b == 0) edge = edge0;
        int closeSample = total - b * FRAMELENGTH;
        if (closeSample >= 0 && closeSample < FRAMELENGTH && b > 0) edge = closeSample;
        block(d, src.data(), FRAMELENGTH, edge, outL);
    }
}

// Loop length: exactly (frames - kSeam) when the REC edges land mid-block on
// material with nothing to match (silence: every cut ties, the latest wins),
// and a whole number of periods on a periodic tone (the phase-matched cut).
static int t_length()
{
    int fails = 0;
    {
        Noether d(1);
        base(d);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        const int frames = 10007;   // prime: never a frame multiple
        recordSine(d, 440.0, frames, 37, out, 0.0);   // silence
        int L = d.getLoopSamples();
        int st = d.getState();
        // with matching on, the latest cut the match can consider is
        // frames - kMatchWin (the window must exist after the cut)
        int expect = frames - Noether::kMatchWin;
        printf("length: silence, %d frames (edge at 37) -> loop %d, expected %d, state %d\n",
               frames, L, expect, st);
        if (!(L == expect && st == 2)) ++fails;
        d.setSample(nullptr);
    }
    {
        Noether d(1);
        base(d);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        const int frames = 10007;
        recordPulse(d, frames, 37, out);
        int L = d.getLoopSamples();
        int hi = frames - Noether::kMatchWin, lo = hi - Noether::kSeamSearch;
        printf("length: 100 Hz pulse, %d frames -> loop %d = %d periods + %d  (search window %d..%d)\n",
               frames, L, L / 480, L % 480, lo, hi);
        if (!(L % 480 == 0 && L >= lo && L <= hi)) ++fails;
        // and with matching OFF the cut is exactly where REC was pressed
        d.setSeamMatch(0);
        d.clearLoop();
        silentBlocks(d, 2, out);
        recordPulse(d, frames, 37, out);
        int L2 = d.getLoopSamples();
        printf("length: same, seam match off -> loop %d, expected %d\n", L2, frames - Noether::kSeam);
        if (L2 != frames - Noether::kSeam) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// The pulse-wave seam: after the phase-matched cut, 20 wraps of a 100 Hz
// pulse loop must keep every period at exactly 480 samples at 1x. The same
// with matching off (the negative control) must NOT.
static int t_pulse()
{
    int fails = 0;
    for (int match = 1; match >= 0; --match) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.setSeamMatch(match);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordPulse(d, 4801, 11, out);
        out.clear();
        silentBlocks(d, 20 * 4801 / FRAMELENGTH + 2, out);
        // intervals between rising edges
        int last = -1, irregular = 0, count = 0;
        for (size_t i = 1; i < out.size(); ++i) {
            if (out[i] > 0.0f && out[i - 1] <= 0.0f) {
                if (last >= 0) { int iv = (int)i - last; ++count; if (iv < 479 || iv > 481) ++irregular; }
                last = (int)i;
            }
        }
        bool ok = match ? (irregular == 0) : (irregular > 0);
        printf("pulse: seam match %s  loop %d  periods measured %d  irregular %d  %s\n",
               match ? "on " : "off", d.getLoopSamples(), count, irregular, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// A sine that does not fit the loop: the wrap must be no rougher than the
// interior, at every speed and direction.
static int t_seam()
{
    const float speeds[] = { 1.0f, 2.0f, 0.5f, 0.25f, -1.0f, -2.0f, -0.5f, 4.0f, -4.0f };
    int fails = 0;
    for (float sp : speeds) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);     // dry off: judge the loop alone
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        const int frames = 4801;              // ~100 ms, not a cycle multiple of 333 Hz
        recordSine(d, 333.0, frames, 11, out);
        base(d, sp, 0.5f, 0.0f, 1.0f);
        out.clear();
        silentBlocks(d, 24000 / FRAMELENGTH + 2, out);  // let the glide settle (>10 tau)
        out.clear();
        silentBlocks(d, (int)(20.0 * frames / fabs(sp) / FRAMELENGTH) + 2, out);   // 20 wraps
        // interior step vs any step: the seam is somewhere inside; if it
        // clicked, the global max first-difference exceeds the sine's own.
        double stepAll = maxAbsDiff(out, 0, out.size());
        // expected max step of a 333 Hz sine at |sp|x, amplitude 0.8
        double stepSine = 0.8 * 2.0 * kPi * 333.0 * fabs(sp) / kSR;
        double ratio = stepAll / stepSine;
        // Verified against a build with the seam blend removed
        // (-DNOETHER_NO_SEAM): that reports ratios of 3-40 here.
        bool ok = ratio < 1.15;
        printf("seam: speed %+5.2f  max step %.5f  sine step %.5f  ratio %.3f  %s\n",
               sp, stepAll, stepSine, ratio, ok ? "ok" : "CLICK");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Pitch follows speed: zero crossings scale with |speed|; reverse keeps pitch.
static int t_speed()
{
    const float speeds[] = { 1.0f, 2.0f, 0.5f, 0.25f, -1.0f, -2.0f };
    int fails = 0;
    for (float sp : speeds) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 200.0, 24000, 5, out);   // 0.5 s of 200 Hz
        base(d, sp, 0.5f, 0.0f, 1.0f);
        out.clear();
        silentBlocks(d, 4000 / FRAMELENGTH + 2, out);
        out.clear();
        silentBlocks(d, kSR / FRAMELENGTH, out);           // 1 s
        int zc = zeroCrossings(out, 0, out.size());
        double f = zc / 2.0;                                // crossings per second / 2
        double expect = 200.0 * fabs(sp);
        bool ok = fabs(f - expect) / expect < 0.02;
        printf("speed: %+5.2f  measured %.1f Hz  expected %.1f Hz  glided speed %.4f  %s\n",
               sp, f, expect, d.getSpeed(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// SOS is a crossfader: 1 leaves the loop bit-identical, 0 replaces it,
// 0.5 blends; the first take ignores SOS.
static int t_sos()
{
    int fails = 0;
    const float soss[] = { 1.0f, 0.0f, 0.5f };
    for (float sos : soss) {
        Noether d(1);
        base(d, 1.0f, sos, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 300.0, 9600, 3, out, 0.5);          // first take, amp 0.5
        std::vector<float> take(b.data.begin(), b.data.begin() + d.getLoopSamples());
        double takePeak = 0; for (float v : take) takePeak = fmax(takePeak, fabs(v));
        // overdub one full pass of silence-with-DC 0.4 (easy to see in the buffer)
        int L = d.getLoopSamples();
        std::vector<float> dc(FRAMELENGTH, 0.4f);
        out.clear();
        block(d, dc.data(), FRAMELENGTH, 0, out);          // REC edge -> OVERDUB
        for (int k = 0; k < L / FRAMELENGTH + 1; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
        block(d, dc.data(), FRAMELENGTH, 0, out);          // REC edge -> ramp out
        silentBlocks(d, 4, out);
        // judge the middle of the loop (away from the punch ramps)
        double maxDiff = 0, meanNew = 0; int n = 0;
        for (int i = L / 4; i < 3 * L / 4; ++i) {
            double old = take[(size_t)i], nw = b.data[(size_t)i];
            double expect = sos * old + (1.0 - sos) * 0.4;
            maxDiff = fmax(maxDiff, fabs(nw - expect));
            meanNew += nw; ++n;
        }
        meanNew /= n;
        int st = d.getState();
        bool ok = maxDiff < 1e-3 && st == 2 && takePeak > 0.45;
        printf("sos: %.1f  first-take peak %.3f  max |buf - (sos*old + (1-sos)*in)| %.6f  mean %.3f  state %d  %s\n",
               sos, takePeak, maxDiff, meanNew, st, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Record a ramp 0..1 over the loop (value = position / length), so any
// output sample says where in the loop it was read from.
static void recordRamp(Noether &d, int frames, int edge0, std::vector<float> &outL)
{
    int total = edge0 + frames;
    int blocks = (total + FRAMELENGTH) / FRAMELENGTH + 1;
    std::vector<float> src(FRAMELENGTH);
    long t = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < FRAMELENGTH; ++i, ++t)
            src[i] = (float)((double)(t - edge0) / frames);
        int edge = -1;
        if (b == 0) edge = edge0;
        int closeSample = total - b * FRAMELENGTH;
        if (closeSample >= 0 && closeSample < FRAMELENGTH && b > 0) edge = closeSample;
        block(d, src.data(), FRAMELENGTH, edge, outL);
    }
}

// Start / Len: the head reads only inside the window; Start 0 / Len 1 is
// bit-identical to no window; a window edge is a crossfade, not a step.
static int t_window()
{
    int fails = 0;
    // (a) whole loop with Start 0 / Len 1 == the same loop played before
    //     the controls existed: compare two units, one with Len 1 explicitly
    //     and one with Len slightly under the threshold (0.9995 rounds to L).
    {
        Noether a(1), b(1);
        base(a, 1.0f, 0.5f, 0.0f, 1.0f); base(b, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer ba(1, kSR), bb(1, kSR);
        a.setSample(&ba.s); b.setSample(&bb.s);
        std::vector<float> oa, ob;
        recordRamp(a, 9600, 7, oa); recordRamp(b, 9600, 7, ob);
        Ctl c[] = {{"Len", 1.0f}, {"Start", 0.0f}}; setInputs(a, c, 2);
        Ctl c2[] = {{"Len", 1.0f}, {"Start", 0.0f}}; setInputs(b, c2, 2);
        oa.clear(); ob.clear();
        silentBlocks(a, 300, oa); silentBlocks(b, 300, ob);
        int diff = 0; for (size_t i = 0; i < oa.size(); ++i) if (oa[i] != ob[i]) ++diff;
        printf("window: whole loop, two identical units differ in %d samples\n", diff);
        if (diff) ++fails;
        a.setSample(nullptr); b.setSample(nullptr);
    }
    // (b) Start 0.5 / Len 0.25: every output sample (after the latch and
    //     away from the edge crossfades) reads from [0.5, 0.75] of the loop
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordRamp(d, 9600, 7, out);
        const int L = d.getLoopSamples();
        Ctl c[] = {{"Start", 0.5f}, {"Len", 0.25f}}; setInputs(d, c, 2);
        out.clear();
        silentBlocks(d, 2 * L / FRAMELENGTH + 2, out);   // latches at the first wrap
        out.clear();
        silentBlocks(d, 4 * L / FRAMELENGTH, out);
        // the ramp value at loop position p is p / 9600 (the recorded length)
        const float wLo = (float)(L / 2) / 9600.0f, wHi = (float)(3 * L / 4) / 9600.0f;
        int outside = 0; double lo = 9, hi = -9;
        for (float v : out) { if (v < lo) lo = v; if (v > hi) hi = v; if (v < wLo - 0.002f || v > wHi + 0.002f) ++outside; }
        // edge crossfade: the max step must be far below a hard jump (0.25)
        double step = maxAbsDiff(out, 0, out.size());
        int ws = d.getWindowStart(), wl = d.getWindowLength();
        bool ok = outside == 0 && step < 0.25 * 0.5 && ws == L / 2 && (wl == L / 4 || wl == L / 4 + 1);
        printf("window: start 0.5 len 0.25 -> latched [%d, +%d) of %d; output range %.3f..%.3f (window %.3f..%.3f), %d outside, max step %.4f (hard jump would be 0.25)  %s\n",
               ws, wl, L, lo, hi, wLo, wHi, outside, step, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        // (c) reverse through the window stays inside it too
        Ctl c3[] = {{"Speed", -1.0f}}; setInputs(d, c3, 1);
        out.clear(); silentBlocks(d, 4000 / FRAMELENGTH + 2, out);
        out.clear(); silentBlocks(d, 4 * L / FRAMELENGTH, out);
        outside = 0; for (float v : out) if (v < wLo - 0.002f || v > wHi + 0.002f) ++outside;
        printf("window: reverse, %d samples outside the window  %s\n", outside, outside ? "FAIL" : "ok");
        if (outside) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Extend: overdub with the ext gate on runs off the end and grows the loop;
// the new part is the loop copy blended with the input by SOS.
static int t_extend()
{
    Noether d(1);
    base(d, 1.0f, 0.5f, 0.0f, 1.0f);
    Buffer b(1, kSR);
    d.setSample(&b.s);
    std::vector<float> out;
    recordPulse(d, 4801, 11, out);
    const int L0 = d.getLoopSamples();
    Ctl c[] = {{"Extend", 1.0f}}; setInputs(d, c, 1);
    std::vector<float> dc(FRAMELENGTH, 0.3f);
    block(d, dc.data(), FRAMELENGTH, 0, out);                      // overdub in
    // run past the end of the loop into EXTEND, then 3000 more samples
    int blocksToEnd = L0 / FRAMELENGTH + 2;
    for (int k = 0; k < blocksToEnd; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
    int stMid = d.getState();
    for (int k = 0; k < 3000 / FRAMELENGTH; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
    block(d, dc.data(), FRAMELENGTH, 0, out);                      // rec -> close
    silentBlocks(d, 2, out);
    const int L1 = d.getLoopSamples();
    // content check: sample L0 + 1000 should be 0.5*loop[1000] + 0.5*0.3
    int i = L0 + 1000;
    double expect = 0.5 * b.data[(size_t)1000] + 0.5 * 0.3;
    double got = b.data[(size_t)i];
    bool ok = stMid == 5 && L1 > L0 + 2000 && L1 < L0 + 5000 && fabs(got - expect) < 1e-3 && d.getState() == 2;
    printf("extend: L0 %d -> state during %d (5 = EXTEND) -> L1 %d; buf[L0+1000] %.4f expected %.4f  %s\n",
           L0, stMid, L1, got, expect, ok ? "ok" : "FAIL");
    d.setSample(nullptr);
    return ok ? 0 : 1;
}

// V/Oct: 0.1 (1 V) doubles the speed, -0.1 halves it, with the knob at 1.
static int t_voct()
{
    const float volts[] = { 0.1f, -0.1f, 0.2f };
    int fails = 0;
    for (float v : volts) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 200.0, 24000, 5, out);
        Ctl c[] = {{"V/Oct", v}}; setInputs(d, c, 1);
        out.clear(); silentBlocks(d, 24000 / FRAMELENGTH + 2, out);
        out.clear(); silentBlocks(d, kSR / FRAMELENGTH, out);
        double f = zeroCrossings(out, 0, out.size()) / 2.0;
        double expect = 200.0 * pow(2.0, v * 10.0);
        bool ok = fabs(f - expect) / expect < 0.02;
        printf("voct: %+.1f -> %.1f Hz, expected %.1f  %s\n", v, f, expect, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Persistence: export the loop to a pool sample, import it into a fresh
// unit: same length, bit-identical audio, playing.
static int t_persist()
{
    Noether a(2);
    base(a, 1.0f, 0.5f, 0.0f, 1.0f);
    Buffer ba(2, kSR);
    a.setSample(&ba.s);
    std::vector<float> out;
    recordSine(a, 333.0, 7000, 13, out);
    const int L = a.getLoopSamples();
    Buffer ex(2, L);
    int n = a.exportLoop(&ex.s);
    Noether c(2);
    base(c, 1.0f, 0.5f, 0.0f, 1.0f);
    Buffer bc(2, kSR);
    c.setSample(&bc.s);
    int m = c.importLoop(&ex.s);
    a.importLoop(&ex.s);            // re-seat a at the loop start too
    int diff = 0;
    for (int i = 0; i < L * 2; ++i) if (ba.data[(size_t)i] != bc.data[(size_t)i]) ++diff;
    // and both units now play the same thing
    std::vector<float> oa, oc;
    silentBlocks(a, 200, oa); silentBlocks(c, 200, oc);
    int pdiff = 0; for (size_t i = 0; i < oa.size(); ++i) if (oa[i] != oc[i]) ++pdiff;
    // a mono file into a stereo unit, and a short buffer, must not fault
    Buffer mono(1, 1000); mono.s.mSampleLoadCount = 500;
    int k = c.importLoop(&mono.s);
    bool ok = n == L && m == L && diff == 0 && pdiff == 0 && c.getState() == 2 && k == 500;
    printf("persist: exported %d, imported %d (loop %d), %d buffer samples differ, %d output samples differ, state %d, mono-500 import -> %d  %s\n",
           n, m, L, diff, pdiff, c.getState(), k, ok ? "ok" : "FAIL");
    a.setSample(nullptr); c.setSample(nullptr);
    return ok ? 0 : 1;
}

// Detents: within 3 % of a ratio the target is exactly that ratio; outside
// it is the raw value; the display flag follows.
static int t_detent()
{
    struct Case { float in; float expect; int locked; };
    const Case cases[] = {
        { 1.0f, 1.0f, 1 }, { 1.02f, 1.0f, 1 }, { 0.98f, 1.0f, 1 }, { 1.05f, 1.05f, 0 },
        { 2.03f, 2.0f, 1 }, { 0.51f, 0.5f, 1 }, { 0.255f, 0.25f, 1 }, { 0.26f, 0.26f, 0 }, { 3.9f, 4.0f, 1 },
        { -1.01f, -1.0f, 1 }, { -2.5f, -2.5f, 0 }, { 0.02f, 0.0f, 1 }, { 0.1f, 0.1f, 0 },
        { 1.5f, 1.5f, 0 },
    };
    int fails = 0;
    for (const Case &c : cases) {
        Noether d(1);
        base(d, c.in, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 300.0, 4800, 3, out);
        silentBlocks(d, 2, out);
        float t = d.vizSpeedTarget();
        int lk = d.getDetent();
        bool ok = t == c.expect && lk == c.locked;
        if (!ok) printf("detent: in %+.3f -> target %+.4f locked %d, expected %+.4f / %d  FAIL\n", c.in, t, lk, c.expect, c.locked);
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    printf("detent: %d failures over %d cases\n", fails, (int)(sizeof(cases) / sizeof(cases[0])));
    return fails ? 1 : 0;
}

// Anti-alias: at 1x the filter is an exact bypass (output == the recorded
// samples); at 4x an 8 kHz loop tone (which would read at 32 kHz) is cut hard.
static int t_aa()
{
    int fails = 0;
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 300.0, 4800, 0, out);
        const int L = d.getLoopSamples();
        int pos0 = d.vizPos() + 1;          // the next sample the head will read
        out.clear();
        silentBlocks(d, 4, out);
        int diff = 0;
        for (int i = 0; i < 4 * FRAMELENGTH; ++i) {
            int p = pos0 + i; while (p >= L) p -= L;
            if (out[(size_t)i] != b.data[(size_t)p]) ++diff;
        }
        printf("aa: 1x -> %d of %d samples differ from the buffer (must be 0: exact bypass)\n", diff, 4 * FRAMELENGTH);
        if (diff) ++fails;
        d.setSample(nullptr);
    }
    {
        double rms1 = 0, rms4 = 0;
        for (int pass = 0; pass < 2; ++pass) {
            float sp = pass ? 4.0f : 1.0f;
            Noether d(1);
            base(d, 1.0f, 0.5f, 0.0f, 1.0f);
            Buffer b(1, kSR);
            d.setSample(&b.s);
            std::vector<float> out;
            recordSine(d, 8000.0, 9600, 0, out);
            base(d, sp, 0.5f, 0.0f, 1.0f);
            out.clear(); silentBlocks(d, 24000 / FRAMELENGTH, out);
            out.clear(); silentBlocks(d, 100, out);
            double acc = 0; for (float v : out) acc += (double)v * v;
            double r = sqrt(acc / out.size());
            if (pass) rms4 = r; else rms1 = r;
            d.setSample(nullptr);
        }
        double db = 20.0 * log10(rms4 / (rms1 > 1e-9 ? rms1 : 1e-9));
        bool ok = db < -6.0;
        printf("aa: 8 kHz tone at 4x vs 1x: %.1f dB (anti-alias LP, expect well below -6)  %s\n", db, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    return fails ? 1 : 0;
}

// A FrameBuffer that records every primitive so the reel can be judged.
struct RecFB : od::FrameBuffer {
    int minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30);
    int pixels = 0, lines = 0, circles = 0, fills = 0, texts = 0, calls = 0;
    int lastCircleR = -1;
    void touch(int x, int y) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; ++calls; }
    void pixel(od::Color, int x, int y) override { touch(x, y); ++pixels; }
    void line(od::Color, int x0, int y0, int x1, int y1) override { touch(x0, y0); touch(x1, y1); ++lines; }
    void hline(od::Color, int x, int x2, int y, int) override { touch(x, y); touch(x2, y); ++lines; }
    void vline(od::Color, int x, int y, int y2, int) override { touch(x, y); touch(x, y2); ++lines; }
    int  text(od::Color, int x, int y, const char *, int, int) override { touch(x, y); ++texts; return 20; }
    void circle(od::Color, int x, int y, int r) override { touch(x - r, y - r); touch(x + r, y + r); ++circles; lastCircleR = r; }
    void fillCircle(od::Color, int x, int y, int r) override { touch(x - r, y - r); touch(x + r, y + r); ++fills; }
    void reset() { minx = miny = 1 << 30; maxx = maxy = -(1 << 30); pixels = lines = circles = fills = texts = calls = 0; lastCircleR = -1; }
};

// The reel and the scale: everything drawn stays inside the graphic, the
// envelope fills within a few frames, the head is hollow on a detent and
// solid off it, the comet tail grows with distance from 1x, EXTEND and
// RECORD draw their winding, and the scale places 1x and 2x where it says.
static int t_viz()
{
    int fails = 0;
    Noether d(2);
    base(d, 1.0f, 0.5f, 0.0f, 1.0f);
    Buffer b(2, kSR * 2);
    d.setSample(&b.s);
    noether::NoetherReel reel(0, 0, 84, 64);
    reel.follow(&d);
    noether::NoetherScale scale(0, 0, 128, 64);
    scale.follow(&d);
    RecFB fb;

    // empty
    reel.draw(fb);
    bool inside = fb.minx >= 0 && fb.maxx < 84 && fb.miny >= 0 && fb.maxy < 64;
    printf("viz: empty -> %d calls, bbox x %d..%d y %d..%d  %s\n", fb.calls, fb.minx, fb.maxx, fb.miny, fb.maxy, inside ? "ok" : "OUT OF BOUNDS");
    if (!inside) ++fails;

    // recording: the winding arc
    std::vector<float> out;
    std::vector<float> src(FRAMELENGTH, 0.5f);
    block(d, src.data(), FRAMELENGTH, 0, out);
    for (int k = 0; k < 300; k++) block(d, src.data(), FRAMELENGTH, -1, out);   // ~0.8 s: a fifth of a turn
    fb.reset(); reel.draw(fb);
    inside = fb.minx >= 0 && fb.maxx < 84 && fb.miny >= 0 && fb.maxy < 64;
    printf("viz: recording -> %d lines (arc), bbox x %d..%d y %d..%d  %s\n", fb.lines, fb.minx, fb.maxx, fb.miny, fb.maxy, (inside && fb.lines > 15) ? "ok" : "FAIL");
    if (!(inside && fb.lines > 15)) ++fails;
    block(d, src.data(), FRAMELENGTH, 5, out);   // close

    // playing at 1x (detent): hollow head, no tail, envelope fills
    for (int k = 0; k < 12; ++k) { fb.reset(); reel.draw(fb); }
    inside = fb.minx >= 0 && fb.maxx < 84 && fb.miny >= 0 && fb.maxy < 64;
    int filled = 0; for (int i = 0; i < noether::NoetherReel::kBins; ++i) if (reel.mEnv[i] > 0.3f) ++filled;
    bool okPlay = inside && fb.circles == 1 && fb.lastCircleR == 3 && fb.fills == 0 && fb.pixels == 1 && filled == noether::NoetherReel::kBins;
    printf("viz: playing 1x -> %d ring lines, %d/%d envelope bins filled (DC 0.5 take), head circle r=%d, fills %d, tail px %d  %s\n",
           fb.lines, filled, noether::NoetherReel::kBins, fb.lastCircleR, fb.fills, fb.pixels - 1, okPlay ? "ok" : "FAIL");
    if (!okPlay) ++fails;

    // 2x: solid-on-detent still hollow; tail 4 px. 1.5x: solid head, tail 2 px
    Ctl c2[] = {{"Speed", 2.0f}}; setInputs(d, c2, 1);
    silentBlocks(d, 24000 / FRAMELENGTH, out);
    fb.reset(); reel.draw(fb);
    bool ok2 = fb.circles == 1 && fb.pixels == 1 + 4;
    printf("viz: 2x -> hollow head %d, tail px %d (expect 4)  %s\n", fb.circles, fb.pixels - 1, ok2 ? "ok" : "FAIL");
    if (!ok2) ++fails;
    Ctl c15[] = {{"Speed", -1.5f}}; setInputs(d, c15, 1);
    silentBlocks(d, 24000 / FRAMELENGTH, out);
    fb.reset(); reel.draw(fb);
    bool ok15 = fb.circles == 0 && fb.fills == 1 && fb.pixels == 2;
    printf("viz: -1.5x -> solid head %d, tail px %d (expect 2)  %s\n", fb.fills, fb.pixels, ok15 ? "ok" : "FAIL");
    if (!ok15) ++fails;

    // the scale: 1x marker sits at 60 % of the half width, 2x at 80 %
    Ctl c1[] = {{"Speed", 1.0f}}; setInputs(d, c1, 1);
    silentBlocks(d, 24000 / FRAMELENGTH, out);
    fb.reset(); scale.draw(fb);
    bool okScale = fb.circles == 1 && fb.texts >= 3 && fb.minx >= 0 && fb.maxx < 128 && fb.miny >= 0 && fb.maxy < 64;
    printf("viz: scale at 1x -> hollow marker %d, texts %d, bbox x %d..%d y %d..%d  %s\n", fb.circles, fb.texts, fb.minx, fb.maxx, fb.miny, fb.maxy, okScale ? "ok" : "FAIL");
    if (!okScale) ++fails;
    float p1 = noether::NoetherScale::place(1.0f), p2 = noether::NoetherScale::place(2.0f), p4 = noether::NoetherScale::place(4.0f), pq = noether::NoetherScale::place(0.25f);
    bool okPlace = fabs(p1 - 0.6f) < 0.01f && fabs(p2 - 0.8f) < 0.01f && fabs(p4 - 1.0f) < 0.01f && fabs(pq - 0.2f) < 0.01f;
    printf("viz: scale placement 1/4 %.2f  1 %.2f  2 %.2f  4 %.2f  %s\n", pq, p1, p2, p4, okPlace ? "ok" : "FAIL");
    if (!okPlace) ++fails;

    // extend: the outer winding
    Ctl ce[] = {{"Extend", 1.0f}}; setInputs(d, ce, 1);
    block(d, src.data(), FRAMELENGTH, 0, out);                                   // overdub in
    for (int k = 0; k < d.getLoopSamples() / FRAMELENGTH + 40; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
    fb.reset(); reel.draw(fb);
    inside = fb.minx >= 0 && fb.maxx < 84 && fb.miny >= 0 && fb.maxy < 64;
    bool okExt = d.getState() == 5 && inside && fb.fills == 1;
    printf("viz: extend -> state %d, bbox x %d..%d y %d..%d, end dot %d  %s\n", d.getState(), fb.minx, fb.maxx, fb.miny, fb.maxy, fb.fills, okExt ? "ok" : "FAIL");
    if (!okExt) ++fails;

    d.setSample(nullptr);
    fb.reset(); reel.draw(fb); scale.draw(fb);     // no buffer: must not fault
    return fails ? 1 : 0;
}

// Poison every inlet with NaN / inf; output stays finite; unit recovers.
static int t_nan()
{
    const float poisons[] = { NAN, INFINITY, -INFINITY };
    const char *ports[] = { "Left In", "Right In", "Rec", "Speed", "SOS", "Dry", "Level",
                            "V/Oct", "Start", "Len", "Extend" };
    int fails = 0;
    for (const char *port : ports) for (float p : poisons) {
        Noether d(2);
        base(d);
        Buffer b(2, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 220.0, 6000, 9, out);
        Ctl c[] = {{port, p}};
        setInputs(d, c, 1);
        out.clear();
        for (int k = 0; k < 40; ++k) { d.process(); for (int i = 0; i < FRAMELENGTH; ++i) out.push_back(d.mLeftOut.buffer()[i]); }
        bool fin = allFinite(out);
        base(d);
        // a fresh, clean input must come through again
        out.clear();
        std::vector<float> src(FRAMELENGTH, 0.3f);
        for (int k = 0; k < 40; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        bool rec = allFinite(out) && fabs(out.back()) > 0.0f;
        if (!fin || !rec) { ++fails; printf("nan: port %-8s %s -> finite=%d recovered=%d FAIL\n", port, std::isnan(p) ? "nan" : (p > 0 ? "+inf" : "-inf"), fin, rec); }
        d.setSample(nullptr);
    }
    printf("nan: %d failures over %d cases\n", fails, (int)(sizeof(ports) / sizeof(ports[0]) * 3));
    return fails ? 1 : 0;
}

static int t_cpu()
{
    Noether d(2);
    base(d, 1.37f, 0.5f, 1.0f, 1.0f);
    Buffer b(2, kSR * 30);
    d.setSample(&b.s);
    std::vector<float> out;
    recordSine(d, 220.0, 48000, 9, out);
    clock_t t0 = clock();
    const int blocks = 20000;
    for (int k = 0; k < blocks; ++k) d.process();
    double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / blocks;
    printf("cpu: %.2f us per stereo block on this host (budget on device: 2667 us)\n", us);
    d.setSample(nullptr);
    return us < 200.0 ? 0 : 1;
}

// Sweep every control to its extremes across states; count non-finite /
// out-of-range samples. Built with -fsanitize=address,undefined.
static int t_asan()
{
    int bad = 0;
    const float speeds[] = { -4.0f, -1.0f, -0.01f, 0.0f, 0.01f, 1.0f, 4.0f, 9.0f, -9.0f };
    const float wins[][2] = { {0.0f, 1.0f}, {0.5f, 0.25f}, {0.99f, 0.001f}, {0.3f, 0.999f} };
    for (int ch = 1; ch <= 2; ++ch) for (float sp : speeds) for (float sos : {0.0f, 1.0f}) for (auto &wv : wins) {
        Noether d(ch);
        base(d, sp, sos, 1.0f, 1.0f);
        Ctl wc[] = {{"Start", wv[0]}, {"Len", wv[1]}, {"Extend", 1.0f}, {"V/Oct", 0.05f}}; setInputs(d, wc, 4);
        Buffer b(ch, kSR / 4);     // small buffer: force the buffer-full close
        d.setSample(&b.s);
        std::vector<float> out;
        std::vector<float> src(FRAMELENGTH);
        for (int i = 0; i < FRAMELENGTH; ++i) src[i] = (float)sin(i * 0.3) * 1.5f;
        block(d, src.data(), FRAMELENGTH, 0, out);                       // record
        for (int k = 0; k < kSR / 4 / FRAMELENGTH + 3; ++k) block(d, src.data(), FRAMELENGTH, -1, out); // fills, auto-closes
        block(d, src.data(), FRAMELENGTH, 5, out);                       // overdub in
        for (int k = 0; k < 50; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        block(d, src.data(), FRAMELENGTH, 5, out);                       // overdub out
        for (int k = 0; k < 50; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        d.clearLoop();
        for (int k = 0; k < 5; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        block(d, src.data(), FRAMELENGTH, 127, out);                     // edge on the last sample
        block(d, src.data(), FRAMELENGTH, 0, out);                       // 1-sample take
        for (int k = 0; k < 20; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        for (float v : out) if (!std::isfinite(v) || fabs(v) > 4.0f) ++bad;
        d.setSample(nullptr);
        // and with no buffer at all, mid-flight
        block(d, src.data(), FRAMELENGTH, 0, out);
    }
    printf("asan: %d non-finite / out-of-range samples\n", bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: t <mode>\n"); return 2; }
    const char *m = argv[1];
    int r = 2;
    if      (!strcmp(m, "ident"))  r = t_ident();
    else if (!strcmp(m, "length")) r = t_length();
    else if (!strcmp(m, "seam"))   r = t_seam();
    else if (!strcmp(m, "pulse"))  r = t_pulse();
    else if (!strcmp(m, "window")) r = t_window();
    else if (!strcmp(m, "extend")) r = t_extend();
    else if (!strcmp(m, "voct"))   r = t_voct();
    else if (!strcmp(m, "persist")) r = t_persist();
    else if (!strcmp(m, "detent")) r = t_detent();
    else if (!strcmp(m, "aa"))     r = t_aa();
    else if (!strcmp(m, "viz"))    r = t_viz();
    else if (!strcmp(m, "speed"))  r = t_speed();
    else if (!strcmp(m, "sos"))    r = t_sos();
    else if (!strcmp(m, "nan"))    r = t_nan();
    else if (!strcmp(m, "cpu"))    r = t_cpu();
    else if (!strcmp(m, "asan"))   r = t_asan();
    else { fprintf(stderr, "unknown mode %s\n", m); return 2; }
    printf("%s: %s\n", m, r == 0 ? "PASS" : "FAIL");
    return r;
}
