/* Host verification harness for Noether (no hardware).
 * Build: g++ -std=c++11 -O2 -ffast-math -Itest/host -Isrc \
 *          src/Noether.cpp test/host/main.cpp -o test/t
 * Modes: ident · length · seam · pulse · tri · bars · speed · sos · extend · voct · persist ·
 *        detent · aa · viz · undo · stop · clock · nan · cpu · asan
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
#include <algorithm>
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
                         &d.mVoctIn, &d.mExtIn, &d.mBarsIn,
                         &d.mStopIn, &d.mUndoIn, &d.mClkIn, &d.mDubIn };
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
               {"V/Oct", 0.0f}, {"Extend", 0.0f}, {"Bars", 0.0f},
               {"Stop", 0.0f}, {"Undo", 0.0f}, {"Clk", 0.0f}, {"Dub", 0.0f}};
    setInputs(d, c, 12);
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

static int kRecRampFrames() { return (int)(kSR * Noether::kRecRampSec + 0.5f); }

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
        d.zeroBuffer();                      // the menu task: clear, then silence the buffer
        // 0.8.5: the Lua-thread call only REQUESTS the zeroing (no 5.7 MB
        // memset beside the audio); the loop fades out, then the audio
        // thread zeroes kZeroPerBlock frames a block
        double peak0 = 0; for (int i = 0; i < 10007; ++i) if (fabs(b.s.mpData[i]) > peak0) peak0 = fabs(b.s.mpData[i]);
        silentBlocks(d, 2 * kSR / Noether::kZeroPerBlock + 4, out);
        { double peak = 0; for (int i = 0; i < 2 * kSR; ++i) if (fabs(b.s.mpData[i]) > peak) peak = fabs(b.s.mpData[i]);
          printf("length: clear + zeroBuffer -> untouched by the call (peak %.3f), then state %d, buffer peak %.3f\n", peak0, d.getState(), peak);
          if (!(peak0 > 0.0 && d.getState() == 0 && peak == 0.0)) ++fails; }
        silentBlocks(d, 1, out);
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
    const float speeds[] = { 1.0f, 2.0f, 0.5f, 0.25f, -1.0f, -2.0f, -0.5f };
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
// A bass triangle across the seam (0.7.1, Nick heard a kink): at 1x the
// largest step in playback must be the triangle's own slope (within 5 %),
// down to 30 Hz, and the cut must sit within one period + the match window
// of where REC was pressed. Noise (worst case for the candidate walk) must
// still close and stay bounded.
static float triWave(double ph) { ph -= floor(ph); return (float)(ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph); }
static int t_tri()
{
    int fails = 0;
    const double freqs[] = { 30.0, 55.0, 110.0, 440.0 };
    for (double f : freqs) for (int nc = 1; nc <= 2; ++nc) {
        Noether d(nc);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(nc, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        const int edge0 = 7, frames = kSR + 1234;
        const int total = edge0 + frames, blocks = (total + FRAMELENGTH) / FRAMELENGTH + 1;
        std::vector<float> src(FRAMELENGTH);
        long t = 0;
        for (int bl = 0; bl < blocks; ++bl) {
            for (int i = 0; i < FRAMELENGTH; ++i, ++t) src[i] = 0.8f * triWave(f * (double)(t - edge0) / kSR);
            int edge = (bl == 0) ? edge0 : -1;
            int cs = total - bl * FRAMELENGTH;
            if (cs >= 0 && cs < FRAMELENGTH && bl > 0) edge = cs;
            block(d, src.data(), FRAMELENGTH, edge, out);
        }
        out.clear();
        silentBlocks(d, 3 * kSR / FRAMELENGTH, out);
        const double slope = 0.8 * 4.0 * f / kSR;
        // from after the close's handover: the loop fades in there on
        // purpose (0.8.4: 5 ms; 0.8.5: kHandover, 21 ms); the wraps this
        // test guards come a second later
        const double step = maxAbsDiff(out, Noether::kHandover + 64, out.size());
        const int back = frames - d.getLoopSamples();
        const int maxBack = (int)(kSR / f) + Noether::kMatchWin + 8;
        const bool ok = step <= slope * 1.05 && back <= maxBack && back >= 0;
        printf("tri: %5.0f Hz nc%d  max step %.2fx slope  cut %d back (<= %d)  %s\n",
               f, nc, step / slope, back, maxBack, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    {   // noise: the candidate walk is bounded; the wrap is no worse than the interior
        Noether d(2);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(2, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        const int edge0 = 7, frames = kSR + 1234;
        const int total = edge0 + frames, blocks = (total + FRAMELENGTH) / FRAMELENGTH + 1;
        std::vector<float> src(FRAMELENGTH);
        unsigned r = 12345u;
        for (int bl = 0; bl < blocks; ++bl) {
            for (int i = 0; i < FRAMELENGTH; ++i) { r = r * 1664525u + 1013904223u; src[i] = ((float)(r >> 8) / 8388608.0f - 1.0f) * 0.5f; }
            int edge = (bl == 0) ? edge0 : -1;
            int cs = total - bl * FRAMELENGTH;
            if (cs >= 0 && cs < FRAMELENGTH && bl > 0) edge = cs;
            block(d, src.data(), FRAMELENGTH, edge, out);
        }
        out.clear();
        silentBlocks(d, 2 * kSR / FRAMELENGTH, out);
        const int back = frames - d.getLoopSamples();
        const bool ok = d.getState() == 2 && back >= Noether::kMatchWin && back <= Noether::kSeamSearch + Noether::kMatchWin && allFinite(out);
        printf("tri: noise -> state %d, cut %d back, finite %d  %s\n", d.getState(), back, (int)allFinite(out), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

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
        double expect = 200.0 * fmin(2.0, pow(2.0, v * 10.0));   // clamped at 2x
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
    // a completed load whose "loaded" counter was never set: frame count trusted
    Buffer zl(1, 700); zl.s.mSampleLoadCount = 0;
    int kz = c.importLoop(&zl.s);
    // and the refusal codes
    Buffer tiny(1, 2); int kt = c.importLoop(&tiny.s);
    int kn = c.importLoop(nullptr);
    bool ok = n == L && m == L && diff == 0 && pdiff == 0 && c.getState() == 2 && k == 500 && kz == 700 && kt == -4 && kn == -1;
    printf("persist: exported %d, imported %d (loop %d), %d buffer samples differ, %d output samples differ, state %d, mono-500 import -> %d, zero-loadcount-700 -> %d, tiny -> %d, null -> %d  %s\n",
           n, m, L, diff, pdiff, c.getState(), k, kz, kt, kn, ok ? "ok" : "FAIL");
    a.setSample(nullptr); c.setSample(nullptr);
    // ── 0.8.7: the loop buffer itself coming back from a SAVED file (the
    //    player saved it at the quicksave prompt): while it is still loading
    //    an import is refused (-5) and nothing changes; once it is in, the
    //    import works; and playBuffer plays what the buffer already holds.
    int f = 0;
    {
        Noether d(2);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer saved(2, kSR);
        for (int i = 0; i < kSR; ++i) { const float v = (float)(0.6 * sin(2 * kPi * 220.0 * i / kSR)); saved.data[(size_t)i * 2] = v; saved.data[(size_t)i * 2 + 1] = -v; }
        saved.s.mSampleLoadCount = kSR / 3;                // the pool is still filling it
        d.setSample(&saved.s);
        const int busy = d.importLoop(&ex.s), busy2 = d.playBuffer(L);
        silentBlocks(d, 4, out);
        const int stBusy = d.getState();
        saved.s.mSampleLoadCount = kSR;                    // the load completes
        const int pb = d.playBuffer(L);
        std::vector<float> o1; silentBlocks(d, 40, o1);
        const int stPlay = d.getState();
        // what it plays is the buffer's own start, as an import of the same
        // frames would: compare with a twin unit that imported them
        Noether e(2); base(e, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer be(2, kSR); e.setSample(&be.s);
        Buffer src(2, L); for (int i = 0; i < L * 2; ++i) src.data[(size_t)i] = saved.data[(size_t)i];
        e.importLoop(&src.s);
        std::vector<float> o2; silentBlocks(e, 40, o2);
        int same = 0; for (size_t i = 0; i < o1.size(); ++i) if (o1[i] != o2[i]) ++same;
        const bool ok2 = busy == -5 && busy2 == -5 && stBusy == 0 && pb == L && stPlay == 2 && d.getLoopSamples() == L && same == 0;
        printf("persist: buffer still loading -> import %d, playBuffer %d (both -5), state %d; loaded -> playBuffer %d (loop %d), state %d, output vs an import of the same frames: %d differ  %s\n",
               busy, busy2, stBusy, pb, d.getLoopSamples(), stPlay, same, ok2 ? "ok" : "FAIL");
        if (!ok2) ++f;
        d.setSample(nullptr); e.setSample(nullptr);
    }
    return (ok && f == 0) ? 0 : 1;
}

// Detents: within 3 % of a ratio the target is exactly that ratio; outside
// it is the raw value; the display flag follows.
static int t_detent()
{
    struct Case { float in; float expect; int locked; };
    const Case cases[] = {
        { 1.0f, 1.0f, 1 }, { 1.02f, 1.0f, 1 }, { 0.98f, 1.0f, 1 }, { 1.05f, 1.05f, 0 },
        { 2.03f, 2.0f, 1 }, { 0.51f, 0.5f, 1 }, { 0.255f, 0.25f, 1 }, { 0.26f, 0.26f, 0 }, { 3.9f, 2.0f, 1 },
        { -1.01f, -1.0f, 1 }, { -1.5f, -1.5f, 0 }, { 0.02f, 0.0f, 1 }, { 0.1f, 0.1f, 0 },
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
        silentBlocks(d, Noether::kHandover / FRAMELENGTH + 2, out);   // past the close's handover (0.8.5: 21 ms)
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
            float sp = pass ? 2.0f : 1.0f;
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
        bool ok = db < -2.0;
        printf("aa: 8 kHz tone at 2x vs 1x: %.1f dB (anti-alias LP at 2x, expect a few dB down)  %s\n", db, ok ? "ok" : "FAIL");
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
    // like the SDK: returns the x AFTER the last glyph (6 px advance here)
    int  text(od::Color, int x, int y, const char *s, int, int) override { touch(x, y); ++texts; int n = 0; while (s[n]) ++n; return x + 6 * n; }
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
    float p1 = noether::NoetherScale::place(1.0f), p2 = noether::NoetherScale::place(2.0f), ph = noether::NoetherScale::place(0.5f), pq = noether::NoetherScale::place(0.25f);
    bool okPlace = fabs(p1 - 0.75f) < 0.01f && fabs(p2 - 1.0f) < 0.01f && fabs(ph - 0.5f) < 0.01f && fabs(pq - 0.25f) < 0.01f;
    printf("viz: scale placement 1/4 %.2f  1/2 %.2f  1 %.2f  2 %.2f  %s\n", pq, ph, p1, p2, okPlace ? "ok" : "FAIL");
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

static void undoEdge(Noether &d, std::vector<float> &out)
{
    float *u = d.mUndoIn.buffer();
    for (int i = 0; i < FRAMELENGTH; ++i) u[i] = 1.0f;
    std::vector<float> z(FRAMELENGTH, 0.0f);
    block(d, z.data(), FRAMELENGTH, -1, out);
    for (int i = 0; i < FRAMELENGTH; ++i) u[i] = 0.0f;
    block(d, z.data(), FRAMELENGTH, -1, out);
}

// Undo: an overdub pass is swapped back out (bit-identical to the take),
// undo again brings it back (redo); an extend's length is undone too; undo
// with nothing to undo changes nothing.
static int t_undo()
{
    int fails = 0;
    Noether d(2);
    base(d, 1.0f, 0.0f, 0.0f, 1.0f);    // SOS 0: the overdub REPLACES, easy to see
    Buffer b(2, kSR), ub(2, kSR);
    d.setSample(&b.s);
    d.setUndoSample(&ub.s);
    std::vector<float> out;
    recordSine(d, 300.0, 9600, 3, out, 0.5);
    const int L = d.getLoopSamples();
    std::vector<float> take(b.data.begin(), b.data.begin() + (size_t)L * 2);
    // nothing to undo yet
    undoEdge(d, out);
    silentBlocks(d, 4, out);
    int diff0 = 0; for (size_t i = 0; i < take.size(); ++i) if (b.data[i] != take[i]) ++diff0;
    printf("undo: before any overdub -> canUndo %d, %d samples changed by an undo edge\n", d.canUndo(), diff0);
    if (diff0 || d.canUndo()) ++fails;
    // overdub a full pass of DC 0.4
    std::vector<float> dc(FRAMELENGTH, 0.4f);
    block(d, dc.data(), FRAMELENGTH, 0, out);
    for (int k = 0; k < L / FRAMELENGTH + 2; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
    block(d, dc.data(), FRAMELENGTH, 0, out);
    silentBlocks(d, 4, out);
    std::vector<float> dubbed(b.data.begin(), b.data.begin() + (size_t)L * 2);
    int changed = 0; for (size_t i = 0; i < take.size(); ++i) if (dubbed[i] != take[i]) ++changed;
    // undo
    undoEdge(d, out);
    int blocks = 0; while (d.isRestoring() && blocks < 1000) { silentBlocks(d, 1, out); ++blocks; }
    int back = 0; for (size_t i = 0; i < take.size(); ++i) if (b.data[i] != take[i]) ++back;
    printf("undo: overdub changed %d of %d samples; undo restored in %d blocks, %d samples still differ from the take, canUndo %d\n",
           changed, (int)take.size(), blocks, back, d.canUndo());
    if (!(changed > take.size() / 2 && back == 0 && d.canUndo() == 1)) ++fails;
    // redo
    undoEdge(d, out);
    blocks = 0; while (d.isRestoring() && blocks < 1000) { silentBlocks(d, 1, out); ++blocks; }
    int redo = 0; for (size_t i = 0; i < take.size(); ++i) if (b.data[i] != dubbed[i]) ++redo;
    printf("undo: redo -> %d samples differ from the overdubbed loop\n", redo);
    if (redo) ++fails;
    // an extend, then undo: length and content come back
    Ctl ce[] = {{"Extend", 1.0f}}; setInputs(d, ce, 1);
    block(d, dc.data(), FRAMELENGTH, 0, out);
    for (int k = 0; k < L / FRAMELENGTH + 30; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
    block(d, dc.data(), FRAMELENGTH, 0, out);
    silentBlocks(d, 2, out);
    const int L1 = d.getLoopSamples();
    undoEdge(d, out);
    blocks = 0; while (d.isRestoring() && blocks < 2000) { silentBlocks(d, 1, out); ++blocks; }
    int backX = 0; for (size_t i = 0; i < (size_t)L * 2; ++i) if (b.data[i] != dubbed[i]) ++backX;
    printf("undo: extend %d -> %d, undo -> %d, %d samples differ from the pre-extend loop\n", L, L1, d.getLoopSamples(), backX);
    if (!(L1 > L && d.getLoopSamples() == L && backX == 0)) ++fails;
    d.setUndoSample(nullptr);
    d.setSample(nullptr);
    return fails ? 1 : 0;
}

// Stop gate: high fades the loop out within 10 ms and holds the head; low
// resumes from the same position; the state reads STOP while held.
static int t_stop()
{
    int fails = 0;
    Noether d(1);
    base(d, 1.0f, 0.5f, 0.0f, 1.0f);
    Buffer b(1, kSR);
    d.setSample(&b.s);
    std::vector<float> out;
    recordSine(d, 300.0, 9600, 3, out, 0.5);
    Ctl c0[] = {{"Stop", 1.0f}}; setInputs(d, c0, 1);
    out.clear(); silentBlocks(d, 8, out);                 // 21 ms
    double tail = 0; for (size_t i = out.size() - FRAMELENGTH; i < out.size(); ++i) tail = fmax(tail, fabs(out[i]));
    int p1 = d.vizPos();
    silentBlocks(d, 40, out);
    int p2 = d.vizPos();
    int st = d.getState();
    Ctl c1[] = {{"Stop", 0.0f}}; setInputs(d, c1, 1);
    out.clear(); silentBlocks(d, 8, out);
    double head = 0; for (size_t i = out.size() - FRAMELENGTH; i < out.size(); ++i) head = fmax(head, fabs(out[i]));
    int p3 = d.vizPos();
    bool ok = tail == 0.0 && p1 == p2 && st == 4 && head > 0.3 && p3 > p2 && p3 - p2 < 9 * FRAMELENGTH && d.getState() == 2;
    printf("stop: after gate low: last block peak %.4f, head %d -> %d (held), state %d; after gate high: peak %.3f, head %d, state %d  %s\n",
           tail, p1, p2, st, head, p3, d.getState(), ok ? "ok" : "FAIL");
    if (!ok) ++fails;
    d.setSample(nullptr);
    return fails ? 1 : 0;
}

// Run `blocks` blocks with a clock of `period` samples on Clk (edge at
// absolute sample multiples of period, phase-tracked across calls via t),
// input `src` value, optional REC edge at absolute sample recAt.
struct ClockRig {
    Noether &d; std::vector<float> &out; int period; long t = 0;
    ClockRig(Noether &dd, std::vector<float> &o, int p) : d(dd), out(o), period(p) {}
    void run(int blocks, float inVal = 0.0f, long recAt = -1)
    {
        float *L = d.mLeftIn.buffer(), *R = d.mRightIn.buffer(), *rec = d.mRecIn.buffer(), *clk = d.mClkIn.buffer();
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
                L[i] = R[i] = inVal;
                clk[i] = ((t % period) < 4) ? 1.0f : 0.0f;
                rec[i] = (recAt >= 0 && t >= recAt && t < recAt + 4) ? 1.0f : 0.0f;
            }
            d.process();
            const float *o = d.mLeftOut.buffer();
            for (int i = 0; i < FRAMELENGTH; ++i) out.push_back(o[i]);
        }
        for (int i = 0; i < FRAMELENGTH; ++i) { rec[i] = 0.0f; clk[i] = 0.0f; }
    }
};

// Clock. Free: an edge restarts the loop. Sync: REC arms and the take starts
// / stops on the next edge, so the loop is exactly N periods; every N-th edge
// re-syncs the head even at 2x; no clock for 4 s -> REC acts immediately.
static int t_clock()
{
    int fails = 0;
    const int P = 12000;   // 0.25 s at 48 kHz
    // ── free mode: reset ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordRamp(d, 9600, 7, out);
        ClockRig rig(d, out, P);
        rig.run(2);                                   // an edge lands in here (t=0)
        int p0 = d.vizPos();
        // the head was reset near the seam: position small (a couple of blocks in)
        bool ok = p0 >= 0 && p0 < 3 * FRAMELENGTH;
        printf("clock: free mode, edge -> head at %d (expect < %d)  %s\n", p0, 3 * FRAMELENGTH, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── sync mode: armed take of exactly 4 periods ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);                            // a few edges so the clock is "present"
        long now = rig.t;
        const long startEdge = ((now + 50) / P + 1) * P;   // the first edge after the arm
        rig.run(1, 0.3f, now + 50);                   // REC mid-period: arms
        int armed = d.isArmed(), st = d.getState();
        rig.run(P / FRAMELENGTH + 2, 0.3f);           // the next edge starts the take
        int st2 = d.getState();
        long recStart = rig.t;
        rig.run(3 * P / FRAMELENGTH, 0.3f);           // ~3 periods in
        rig.run(1, 0.3f, rig.t + 30);                 // REC: arm the stop
        int armed2 = d.isArmed();
        rig.run(2 * P / FRAMELENGTH + 2, 0.3f);       // the 4th edge closes
        int L = d.getLoopSamples(), N = d.getSyncN(), st3 = d.getState();
        bool ok = armed == 1 && st == 0 && st2 == 1 && armed2 == 2 && st3 == 2 &&
                  N == 4 && (L == 4 * P);
        printf("clock: sync armed %d (state %d) -> recording %d -> stop armed %d -> loop %d = %d periods (expect exactly %d, N=4), state %d  %s\n",
               armed, st, st2, armed2, L, N, 4 * P, st3, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        // ── the clocked seam is click-free: a 100 Hz pulse (period 480, P = 25 periods) ──
        {
            Noether e(1);
            base(e, 1.0f, 0.5f, 0.0f, 1.0f);
            e.mSyncOpt.set(2);
            Buffer be(1, kSR * 4);
            e.setSample(&be.s);
            std::vector<float> oe;
            // a pulse source in phase with the clock
            float *Lb = e.mLeftIn.buffer(), *Rb = e.mRightIn.buffer(), *rb = e.mRecIn.buffer(), *cb = e.mClkIn.buffer();
            long t = 0; long recAt1 = 20 * FRAMELENGTH + 50, recAt2 = -1; int Lp = 0;
            for (int blk = 0; blk < 700; ++blk) {
                for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
                    Lb[i] = Rb[i] = ((t % 480) < 240) ? 0.8f : -0.8f;
                    cb[i] = ((t % P) < 4) ? 1.0f : 0.0f;
                    rb[i] = ((t >= recAt1 && t < recAt1 + 4) || (recAt2 >= 0 && t >= recAt2 && t < recAt2 + 4)) ? 1.0f : 0.0f;
                }
                e.process();
                if (e.getState() == 1 && recAt2 < 0 && t > recAt1 + 2 * P) recAt2 = t + 10;   // arm the stop ~2 periods in
                if (e.getState() == 2 && Lp == 0) Lp = e.getLoopSamples();
                for (int i = 0; i < FRAMELENGTH; ++i) { rb[i] = 0.0f; cb[i] = 0.0f; }
            }
            for (int i = 0; i < FRAMELENGTH; ++i) { Lb[i] = Rb[i] = 0.0f; }
            oe.clear(); silentBlocks(e, 20 * Lp / FRAMELENGTH, oe);
            int last = -1, irregular = 0, count = 0;
            for (size_t i = 1; i < oe.size(); ++i)
                if (oe[i] > 0.0f && oe[i - 1] <= 0.0f) { if (last >= 0) { int iv = (int)i - last; ++count; if (iv < 479 || iv > 481) ++irregular; } last = (int)i; }
            bool okp = Lp > 0 && (Lp % 480) == 0 && irregular == 0 && count > 100;
            printf("clock: clocked pulse loop %d (= %d periods of 480, %d clock periods), 20 wraps: %d periods measured, %d irregular  %s\n",
                   Lp, Lp / 480, Lp / P, count, irregular, okp ? "ok" : "FAIL");
            if (!okp) ++fails;
            e.setSample(nullptr);
        }
        // ── downbeat re-sync at 2x: after each 4 edges the head is back near the seam ──
        Ctl c2[] = {{"Speed", 2.0f}}; setInputs(d, c2, 1);
        rig.run(24000 / FRAMELENGTH, 0.0f);           // glide settles
        int bad = 0, checks = 0;
        for (int rep = 0; rep < 6; ++rep) {
            // step to just after the next downbeat: run until t is a multiple of 4P plus a block
            long target = startEdge + (((rig.t - startEdge) / (4L * P)) + 1) * 4L * P;   // the next downbeat
            rig.run((int)((target - rig.t) / FRAMELENGTH) + 1, 0.0f);
            int pos = d.vizPos();
            ++checks;
            if (!(pos < 4 * FRAMELENGTH || pos > L - 4 * FRAMELENGTH)) ++bad;
        }
        printf("clock: 2x, head at the downbeat within a few blocks of the seam: %d of %d checks failed  %s\n", bad, checks, bad ? "FAIL" : "ok");
        if (bad) ++fails;
        (void)recStart;
        // ── clock loss: after 4 s without edges, REC acts immediately ──
        d.clearLoop();
        silentBlocks(d, 5 * kSR / FRAMELENGTH, out);   // 5 s of nothing
        int hc = d.hasClock();
        std::vector<float> dc(FRAMELENGTH, 0.3f);
        block(d, dc.data(), FRAMELENGTH, 0, out);
        int stL = d.getState(), armL = d.isArmed();
        bool okL = hc == 0 && stL == 1 && armL == 0;
        printf("clock: after 5 s silence hasClock %d, REC -> state %d armed %d  %s\n", hc, stL, armL, okL ? "ok" : "FAIL");
        if (!okL) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Poison every inlet with NaN / inf; output stays finite; unit recovers.
// Bars (0.8.0): in sync mode with bars = N, one REC press arms, the take
// starts on the next edge and closes itself after N edges (loop = N periods,
// X/Y readouts count up); a second press still closes early; bars = 0 is the
// manual two-press take; in free mode bars is ignored.
static int t_bars()
{
    int fails = 0;
    const int P = 12000;
    auto setBars = [](Noether &d, float n) { Ctl c[] = {{"Bars", n}}; setInputs(d, c, 1); };
    // ── counted take: 4 bars, one press ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 4.2f);                              // rounds to 4
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        long now = rig.t;
        rig.run(1, 0.3f, now + 50);                    // REC: arms
        int armed = d.isArmed();
        rig.run(P / FRAMELENGTH + 2, 0.3f);            // edge: take starts
        int st1 = d.getState(), bar1 = d.getBar(), bars1 = d.getBars();
        rig.run(2 * P / FRAMELENGTH, 0.3f);            // two more edges
        int bar3 = d.getBar();
        rig.run(2 * P / FRAMELENGTH + 2, 0.3f);        // the 4th edge closes it by itself
        int st2 = d.getState(), L = d.getLoopSamples(), N = d.getSyncN();
        rig.run(P / FRAMELENGTH, 0.3f);                // one period into playback
        int barP = d.getBar(), barsP = d.getBars();
        bool ok = armed == 1 && st1 == 1 && bar1 == 1 && bars1 == 4 && bar3 == 3 &&
                  st2 == 2 && L == 4 * P && N == 4 && barsP == 4 && (barP == 1 || barP == 2);
        printf("bars: 4 bars, one press -> armed %d, recording %d (bar %d/%d), bar %d at 3rd edge, closed itself: state %d loop %d (expect %d) N %d, playing bar %d/%d  %s\n",
               armed, st1, bar1, bars1, bar3, st2, L, 4 * P, N, barP, barsP, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── early close: 8 bars set, second press after 2 -> 3 periods ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 8.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(P / FRAMELENGTH + 2, 0.3f);            // starts
        rig.run(2 * P / FRAMELENGTH, 0.3f);            // 2 edges in
        rig.run(1, 0.3f, rig.t + 30);                  // REC again: close on the next edge
        int armed2 = d.isArmed();
        rig.run(P / FRAMELENGTH + 2, 0.3f);
        int st = d.getState(), L = d.getLoopSamples(), N = d.getSyncN();
        bool ok = armed2 == 2 && st == 2 && L == 3 * P && N == 3;
        printf("bars: 8 set, second press after 2 -> stop armed %d, loop %d = %d periods (expect 3), state %d  %s\n",
               armed2, L, N, st, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── bars = 0: still recording after 6 edges (manual) ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 0.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(P / FRAMELENGTH + 2, 0.3f);
        rig.run(6 * P / FRAMELENGTH, 0.3f);
        bool ok = d.getState() == 1 && d.getBars() == 0;
        printf("bars: 0 -> still recording after 6 edges (state %d), no count shown (%d)  %s\n", d.getState(), d.getBars(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── free mode: bars ignored, REC is immediate, the take runs on ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        setBars(d, 2.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        int st0 = d.getState();
        rig.run(4 * P / FRAMELENGTH, 0.3f);
        bool ok = st0 == 1 && d.getState() == 1 && d.getBars() == 0;
        printf("bars: free mode, bars 2 -> immediate record (%d), still recording after 4 edges (%d)  %s\n", st0, d.getState(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.1: a take started with NO clock, then the clock arrives ──
    // Before the fix the take inherited the edge count from an older clock,
    // so bars closed it on the first edge with a nonsense N.
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 4.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        { ClockRig rig(d, out, P); rig.run(20 * P / FRAMELENGTH, 0.3f); }   // 20 edges of history
        std::vector<float> z(FRAMELENGTH, 0.3f);
        for (int k = 0; k < 5 * kSR / FRAMELENGTH; ++k) block(d, z.data(), FRAMELENGTH, -1, out);  // 5 s: clock gone
        block(d, z.data(), FRAMELENGTH, 10, out);      // REC with no clock: records at once
        int st0 = d.getState(), bar0 = d.getBar();
        ClockRig rig(d, out, P);                       // the clock comes back (edge at once)
        rig.run(2, 0.3f);
        int st1 = d.getState();
        rig.run(3 * P / FRAMELENGTH + 2, 0.3f);        // three more edges: the 4th closes it
        int st2 = d.getState(), N = d.getSyncN();
        bool ok = st0 == 1 && bar0 <= 1 && st1 == 1 && st2 == 2 && N == 4;
        printf("bars: take started with no clock, clock returns -> recording %d (bar %d), still recording after 1st edge %d, closed after 4th: state %d N %d  %s\n",
               st0, bar0, st1, st2, N, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.1: a counted take that fills the buffer closes free, N = 0 ──
    // Before the fix the previous loop's N survived and re-synced the new,
    // unrelated loop every N edges.
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 4.0f);
        Buffer b(1, kSR * 2);                          // 8 periods of room
        d.setSample(&b.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(5 * P / FRAMELENGTH + 4, 0.3f);        // a 4-bar loop: N = 4
        int N0 = d.getSyncN();
        d.clearLoop();
        rig.run(2, 0.3f);
        setBars(d, 16.0f);                             // longer than the buffer
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(10 * P / FRAMELENGTH, 0.3f);           // fills at 8 periods
        int st = d.getState(), N1 = d.getSyncN(), armed = d.isArmed();
        bool ok = N0 == 4 && st == 2 && N1 == 0 && armed == 0;
        printf("bars: 16 bars into an 8-period buffer after a 4-bar loop -> state %d, N %d (was %d), armed %d  %s\n",
               st, N1, N0, armed, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.1: a clocked close within kSeam of the buffer's end ──
    // Before the fix the tail could never finish: seam unblended and every
    // later overdub un-undoable (mFirstTake stuck).
    {
        Noether d(1);
        base(d, 1.0f, 0.0f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        setBars(d, 4.0f);
        Buffer b(1, 4 * P + 50), ub(1, 4 * P + 50);   // 4 periods + 50 frames of room
        d.setSample(&b.s);
        d.setUndoSample(&ub.s);
        std::vector<float> out;
        ClockRig rig(d, out, P);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(5 * P / FRAMELENGTH + 4, 0.3f);        // closes at 4P, clamped
        int st = d.getState(), L = d.getLoopSamples();
        rig.run(4, 0.3f);
        rig.run(1, 0.7f, rig.t + 10);                  // overdub: armed (sync mode, 0.8.10)
        rig.run(P / FRAMELENGTH + 2, 0.7f);            // in on the next bar line
        rig.run(1, 0.7f, rig.t + 10);                  // out: armed
        rig.run(P / FRAMELENGTH + 10, 0.7f);           // out on the next bar line
        bool ok = st == 2 && L == 4 * P && d.canUndo() == 1;   // 0.8.6: the pre-roll seam needs no tail room — exactly 4 periods
        printf("bars: clocked close at the buffer's end -> state %d loop %d, overdub then canUndo %d  %s\n",
               st, L, d.canUndo(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setUndoSample(nullptr);
        d.setSample(nullptr);
    }
    // ── 0.8.9: pulses per bar. 4 pulses a bar, Bars 2 -> the take is 8
    //    pulses; the readouts count bars (and the beat within one); a
    //    clocked loop's reel ticks are its bars. A manual clocked take that
    //    is not whole bars shows pulses.
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        d.setPulsesPerBar(4);
        setBars(d, 2.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        const int Q = 3000;                               // a 16 Hz pulse: 4 a bar
        ClockRig rig(d, out, Q);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);                     // arm
        rig.run(Q / FRAMELENGTH + 2, 0.3f);               // starts on the next pulse
        int bar1 = d.getBar(), beat1 = d.getBeat(), bars1 = d.getBars();
        rig.run(5 * Q / FRAMELENGTH, 0.3f);               // 5 more pulses: bar 2, beat 2
        int bar2 = d.getBar(), beat2 = d.getBeat(), st2 = d.getState();
        rig.run(3 * Q / FRAMELENGTH + 2, 0.3f);           // the 8th pulse closes it
        int st = d.getState(), L = d.getLoopSamples(), N = d.getSyncN();
        rig.run(Q / FRAMELENGTH, 0.3f);
        int barsP = d.getBars(), secs = d.getSections();
        bool ok = bar1 == 1 && beat1 == 1 && bars1 == 2 && bar2 == 2 && beat2 == 2 && st2 == 1 &&
                  st == 2 && L == 8 * Q && N == 8 && barsP == 2 && secs == 2;
        printf("bars: 4 pulses a bar, bars 2 -> bar %d.%d of %d, then %d.%d (state %d), closed itself: loop %d (expect %d = 8 pulses), N %d, playing shows %d bars, reel %d sections  %s\n",
               bar1, beat1, bars1, bar2, beat2, st2, L, 8 * Q, N, barsP, secs, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    {   // a manual clocked take of 3 pulses at 4 a bar: not whole bars -> pulses
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        d.setPulsesPerBar(4);
        setBars(d, 0.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        const int Q = 3000;
        ClockRig rig(d, out, Q);
        rig.run(20, 0.3f);
        rig.run(1, 0.3f, rig.t + 50);
        rig.run(Q / FRAMELENGTH + 2, 0.3f);               // starts
        rig.run(2 * Q / FRAMELENGTH, 0.3f);
        rig.run(1, 0.3f, rig.t + 30);                     // REC: close on the next pulse
        rig.run(Q / FRAMELENGTH + 2, 0.3f);
        int L = d.getLoopSamples(), barsP = d.getBars(), beat = d.getBeat();
        bool ok = L == 3 * Q && barsP == 3 && beat == 0;
        printf("bars: 4 a bar, manual take of 3 pulses -> loop %d (expect %d), shows %d (pulses: not whole bars), beat %d  %s\n",
               L, 3 * Q, barsP, beat, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Overdub at any speed (0.8.2). Until 0.8.1 an overdub wrote the NEAREST
// frame: below 1x one frame took several input samples (SOS applied two,
// four ... times a pass, input decimated), above 1x frames were skipped —
// "sample rate mismatch" that grew with every pass. And the speed glide
// stalled ~1e-4 short of a detent, so "1x" drifted across frames.
static double sineSnrDb(const std::vector<float> &x, size_t from, size_t to, double f)
{
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (size_t i = from; i < to; ++i) {
        const double w = 2 * kPi * f * (double)i / kSR, sn = sin(w), cs = cos(w);
        ss += sn * sn; cc += cs * cs; sc += sn * cs; xs += x[i] * sn; xc += x[i] * cs;
    }
    const double det = ss * cc - sc * sc, a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double sig = 0, res = 0;
    for (size_t i = from; i < to; ++i) {
        const double w = 2 * kPi * f * (double)i / kSR, y = a * sin(w) + b * cos(w), e = x[i] - y;
        sig += y * y; res += e * e;
    }
    return 10 * log10(sig / (res + 1e-30));
}

static int t_dub()
{
    int fails = 0;
    // ── 1. the glide arrives: 1 -> 0.5 -> 1 ends at EXACTLY 1 ──
    {
        Noether d(1);
        base(d, 1.0f, 0.5f, 1.0f, 1.0f);
        Buffer b(1, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 300.0, 9600, 3, out);
        Ctl h[] = {{"Speed", 0.5f}}; setInputs(d, h, 1); silentBlocks(d, 200, out);
        const float s05 = d.getSpeed();
        Ctl o[] = {{"Speed", 1.0f}}; setInputs(d, o, 1); silentBlocks(d, 200, out);
        const float s1 = d.getSpeed();
        const bool ok = s05 == 0.5f && s1 == 1.0f;
        printf("dub: glide 1 -> 0.5 -> 1 settles at %.9f / %.9f (must be exact)  %s\n", (double)s05, (double)s1, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 2. SOS is applied ONCE per pass at any speed: a DC 0.5 loop,
    //       silence overdubbed at SOS 0.5 for part of a pass -> every frame
    //       the head crossed is 0.25 (never 0.125 = twice), with no holes ──
    for (float sp : {0.5f, 0.25f, 0.7f, 1.5f, 2.0f, -0.5f, -2.0f}) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 1.0f, 1.0f);
        Buffer b(1, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        std::vector<float> dc(FRAMELENGTH, 0.5f), z(FRAMELENGTH, 0.0f);
        block(d, dc.data(), FRAMELENGTH, 0, out);
        for (int k = 0; k < 150; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
        block(d, dc.data(), FRAMELENGTH, 0, out);                  // a DC 0.5 loop
        const int L = d.getLoopSamples();
        Ctl c[] = {{"Speed", sp}}; setInputs(d, c, 1);
        silentBlocks(d, 100, out);
        block(d, z.data(), FRAMELENGTH, -1, out);
        block(d, z.data(), FRAMELENGTH, 0, out);                   // overdub silence in
        const int blocks = (int)(0.4 * L / fabs(sp)) / FRAMELENGTH; // ~40 % of a pass
        for (int k = 0; k < blocks; ++k) block(d, z.data(), FRAMELENGTH, -1, out);
        block(d, z.data(), FRAMELENGTH, 0, out);                   // out
        silentBlocks(d, 4, out);
        int quarter = 0, twice = 0, other = 0, holes = 0;
        for (int i = 0; i < L; ++i) {
            const float v = b.data[i];
            if (fabs(v - 0.25f) < 1e-6f) ++quarter;
            else if (fabs(v - 0.125f) < 1e-3f) ++twice;
            else if (fabs(v - 0.5f) > 1e-6f) ++other;     // the 5 ms ramps at the punch points
            if (i > 0 && i + 1 < L && fabs(v - 0.5f) < 1e-6f &&
                fabs(b.data[i - 1] - 0.25f) < 1e-6f && fabs(b.data[i + 1] - 0.25f) < 1e-6f) ++holes;
        }
        const int ramp = (int)(2 * kRecRampFrames() * fabs(sp)) + 8;
        const bool ok = twice == 0 && holes == 0 && quarter > (int)(0.3 * L) && other <= ramp;
        printf("dub: SOS once per pass at %5.2fx -> %d frames at 0.25, %d at 0.125 (twice), %d holes, %d in the ramps (<= %d)  %s\n",
               (double)sp, quarter, twice, holes, other, ramp, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 3. purity: a tone overdubbed at speed s (SOS 0, several passes)
    //       plays back at s as a clean tone ──
    for (float sp : {1.0f, 0.5f, 0.25f, 0.7f, 1.5f, 2.0f, -1.0f, -0.5f}) {
        Noether d(1);
        base(d, 1.0f, 0.0f, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 0.0, 24000, 3, out, 0.0);
        const int L = d.getLoopSamples();
        const double as = fabs((double)sp);
        const double f = floor(700.0 / as * L / kSR + 0.5) * kSR * as / L;   // whole cycles per loop in the buffer
        Ctl c[] = {{"Speed", sp}}; setInputs(d, c, 1);
        silentBlocks(d, 200, out);
        std::vector<float> src(FRAMELENGTH); long t = 0;
        auto feed = [&](int edge) {
            for (int i = 0; i < FRAMELENGTH; ++i, ++t) src[i] = (float)(0.5 * sin(2 * kPi * f * (double)t / kSR));
            block(d, src.data(), FRAMELENGTH, edge, out);
        };
        const int blocks = (int)(3.0 * L / as) / FRAMELENGTH;       // three passes
        feed(0);
        for (int k = 0; k < blocks; ++k) feed(-1);
        feed(0);
        for (int k = 0; k < 20; ++k) feed(-1);                       // through the punch-out ramp
        std::vector<float> pb;
        silentBlocks(d, 2 * kSR / FRAMELENGTH, pb);
        const double snr = sineSnrDb(pb, 4800, pb.size(), f);
        const double need = (sp == 1.0f || sp == -1.0f) ? 100.0 : 55.0;
        const bool ok = snr >= need;
        printf("dub: tone overdubbed and played at %5.2fx -> %6.1f dB SNR (>= %.0f)  %s\n", (double)sp, snr, need, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 4. AUDITION while dubbing (0.8.2, the device report "only when DUB
    //       is active ... sound records properly"): onto a SILENT loop, the
    //       first pass must sound like the dry input and nothing else. With
    //       the write AT the play head it carried a second, decimated copy
    //       of the input read straight back (-4 .. -6 dB below 1x; 0 dB,
    //       i.e. the input doubled, at 1x). The head gap fixes it. ──
    for (float sp : {1.0f, 0.5f, 0.25f, 2.0f, -1.0f, -0.5f}) for (int glide = 0; glide < 2; ++glide) {
        Noether d(1);
        base(d, 1.0f, 0.0f, 1.0f, 1.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 0.0, 3 * kSR, 3, out, 0.0);
        if (glide) { Ctl g[] = {{"Speed", 0.6f}}; setInputs(d, g, 1); silentBlocks(d, 5, out); }   // a fractional head
        Ctl c[] = {{"Speed", sp}}; setInputs(d, c, 1);
        silentBlocks(d, 200, out);
        std::vector<float> src(FRAMELENGTH), o; long t = 0; double e = 0, x2 = 0;
        for (int k = 0; k < 300; ++k) {
            for (int i = 0; i < FRAMELENGTH; ++i, ++t) src[i] = (float)(0.5 * sin(2 * kPi * 700.0 * (double)t / kSR));
            o.clear();
            block(d, src.data(), FRAMELENGTH, k == 0 ? 0 : -1, o);
            if (k > 20) for (int i = 0; i < FRAMELENGTH; ++i) { const double r = o[i] - src[i]; e += r * r; x2 += (double)src[i] * src[i]; }
        }
        const double db = 10 * log10(e / x2 + 1e-30);
        const bool ok = db < -120.0;
        printf("dub: audition at %5.2fx%s -> loop output while dubbing onto silence %7.1f dB re the input (< -120)  %s\n",
               (double)sp, glide ? " after a glide" : "              ", db, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 5. no denormals: SOS 0.5 over digital silence for 200 passes of a
    //       short loop (the device ran ~5 min before "it did the same") ──
    {
        Noether d(2);
        base(d, 1.0f, 0.5f, 1.0f, 1.0f);
        Buffer b(2, kSR);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 220.0, 4800, 3, out, 0.5);                     // 0.1 s loop
        const int L = d.getLoopSamples();
        std::vector<float> z(FRAMELENGTH, 0.0f);
        block(d, z.data(), FRAMELENGTH, 0, out);                      // overdub silence, for good
        for (int k = 0; k < 200 * L / FRAMELENGTH; ++k) { out.clear(); block(d, z.data(), FRAMELENGTH, -1, out); }
        int den = 0;
        for (int i = 0; i < 2 * L; ++i) {
            const float v = b.data[i];
            if (v != 0.0f && v < 1.17549435e-38f && v > -1.17549435e-38f) ++den;
        }
        const bool ok = den == 0 && d.getState() == 3;
        printf("dub: 200 passes of SOS 0.5 over silence -> %d denormal frames (state %d)  %s\n", den, d.getState(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Closing a take (0.8.4). Device report: "during the initial recording, if
// the input is not completely silent at the moment of turning the record
// off, there's still a click". While recording only the input is heard; at
// the close the loop started from its head at FULL level — a step of up to
// the signal's amplitude. The loop now fades in over 5 ms. Judged as the
// largest sample-to-sample step anywhere in the run against the signal's
// own largest step (dry + loop can at most double it).
static int t_close()
{
    int fails = 0;
    for (double f : {123.0, 1234.0}) for (float dry : {1.0f, 0.0f}) for (int clocked = 0; clocked < 2; ++clocked) {
        Noether d(1);
        base(d, 1.0f, 0.5f, dry, 1.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out, src(FRAMELENGTH);
        long t = 0;
        auto x = [&](long n) { return (float)(0.8 * sin(2 * kPi * f * (double)n / kSR)); };
        float *clk = d.mClkIn.buffer();
        const int P = 12000;
        if (clocked) d.mSyncOpt.set(2);
        for (int k = 0; k < 600; ++k) {
            for (int i = 0; i < FRAMELENGTH; ++i) {
                src[i] = x(t + i);
                clk[i] = (clocked && ((t + i) % P) < 4) ? 1.0f : 0.0f;
            }
            int edge = -1;
            if (!clocked) { if (k == 1) edge = 5; if (k == 375) edge = 77; }
            else if (k == 30 || k == 300) edge = 5;
            block(d, src.data(), FRAMELENGTH, edge, out); t += FRAMELENGTH;
        }
        for (int i = 0; i < FRAMELENGTH; ++i) clk[i] = 0.0f;
        const double own = 2 * kPi * f * 0.8 / kSR * (dry > 0.5f ? 2.0 : 1.0);
        double worst = 0;
        for (size_t i = 1000; i < out.size(); ++i) { const double st = fabs(out[i] - out[i - 1]); if (st > worst) worst = st; }
        const bool ok = d.getState() == 2 && d.getLoopSamples() > 0 && worst / own <= 1.5;
        printf("close: %6.0f Hz dry %.0f %s -> state %d, worst step %.2fx the signal's own (<= 1.5)  %s\n",
               f, (double)dry, clocked ? "clocked" : "manual ", d.getState(), worst / own, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ending an EXTEND: the loop was already playing, so no fade — but the
    // head moves from one cycle back to the new seam; judge that step too
    for (double f : {123.0, 1234.0}) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 1.0f, 1.0f);
        Buffer b(1, kSR * 6);
        d.setSample(&b.s);
        std::vector<float> out, src(FRAMELENGTH);
        long t = 0;
        auto x = [&](long n) { return (float)(0.8 * sin(2 * kPi * f * (double)n / kSR)); };
        auto run = [&](int blocks, int edge) {
            for (int k = 0; k < blocks; ++k) {
                for (int i = 0; i < FRAMELENGTH; ++i) src[i] = x(t + i);
                block(d, src.data(), FRAMELENGTH, k == 0 ? edge : -1, out); t += FRAMELENGTH;
            }
        };
        run(2, 5); run(300, -1); run(1, 40);                    // a ~0.8 s take
        run(20, -1);
        Ctl e[] = {{"Extend", 1.0f}}; setInputs(d, e, 1);
        run(1, 10); run(450, -1);                               // overdub, runs off the end: EXTEND
        const int stX = d.getState();
        const size_t from = out.size();
        run(1, 60); run(100, -1);                               // close the extend
        // A click is a step out of line with its neighbours: the output's
        // CURVATURE (second difference) at the worst point against the
        // largest curvature a smooth sum of these sines can have. (Level is
        // the wrong test here: the jump overlaps two correlated positions
        // for 8 ms by design, so the slope can briefly exceed 3 layers'.)
        // Only the close: from the REC edge through the 8 ms crossfade and a
        // margin. (Later in the run, the overdub's punch-in point comes round
        // — the end of a LINEAR 5 ms record ramp is a slope corner, about
        // 2x this bound at 123 Hz and inaudible; not what this test is for.)
        double worst = 0; size_t at = from;
        const size_t until = from + 60 + Noether::kJumpOut + 256;
        for (size_t i = from + 1; i + 1 < out.size() && i < until; ++i) {
            const double c2 = fabs(out[i + 1] - 2.0 * out[i] + out[i - 1]);
            if (c2 > worst) { worst = c2; at = i; }
        }
        const double w = 2 * kPi * f / kSR, own = 0.8 * w * w * 4.0;   // four layers at most
        const bool ok = stX == 5 && d.getState() == 2 && worst / own <= 1.0;
        printf("close: extend at %6.0f Hz -> was state %d, now %d, worst curvature %.2fx a smooth sum's (<= 1) at +%d  %s\n",
               f, stX, d.getState(), worst / own, (int)(at - from), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.5: a CLOCKED loop of a tone that does not fit it (55 Hz: 13.75
    //    cycles a period; 110 Hz: half a cycle off) wraps without a kink.
    //    Tester: "click ... in the playback each time it loops" in sync
    //    mode — the 2.7 ms blend between two phases the clock cannot align.
    for (double f : {55.0, 110.0}) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        d.mSyncOpt.set(2);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        const int P = 12000;
        float *clk = d.mClkIn.buffer(), *rec = d.mRecIn.buffer(), *inL = d.mLeftIn.buffer();
        long t = 0;
        auto run = [&](int blocks, long recAt) {
            for (int k = 0; k < blocks; ++k) {
                for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
                    inL[i] = (float)(0.8 * sin(2 * kPi * f * (double)t / kSR));
                    clk[i] = (t % P) < 4 ? 1.0f : 0.0f;
                    rec[i] = (recAt >= 0 && t >= recAt && t < recAt + 4) ? 1.0f : 0.0f;
                }
                d.process();
                for (int i = 0; i < FRAMELENGTH; ++i) out.push_back(d.mLeftOut.buffer()[i]);
            }
        };
        run(20, -1); run(1, t + 50); run(100, -1);
        run(1, t + 50); run(2 * P / FRAMELENGTH, -1);
        const int Lp = d.getLoopSamples();
        out.clear();
        run(5 * Lp / FRAMELENGTH, -1);                     // five wraps, dry 0
        const double w = 2 * kPi * f / kSR, own = 0.8 * w * w;
        double worst = 0;
        for (size_t i = 1; i + 1 < out.size(); ++i) { const double c2 = fabs(out[i + 1] - 2 * out[i] + out[i - 1]); if (c2 > worst) worst = c2; }
        const bool ok = Lp == P && worst / own <= 2.0;
        printf("close: clocked loop of %3.0f Hz (does not fit) -> loop %d, worst curvature over 5 wraps %.1fx the sine's (<= 2)  %s\n",
               f, Lp, worst / own, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.5: CLEAR while the loop plays fades it out (was a hard stop) ──
    for (double f : {123.0, 1234.0}) {
        Noether d(1);
        base(d, 1.0f, 0.5f, 0.0f, 1.0f);
        Buffer b(1, kSR * 2);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, f, 30000, 3, out, 0.8);
        silentBlocks(d, 40, out);
        out.erase(out.begin(), out.end() - 1);   // keep the last sample BEFORE the clear: the step is between it and the next
        d.clearLoop(); d.zeroBuffer();
        silentBlocks(d, 2 * kSR / Noether::kZeroPerBlock + 4, out);
        const double own = 2 * kPi * f * 0.8 / kSR;
        const double worst = maxAbsDiff(out, 0, out.size());
        double peak = 0; for (int i = 0; i < 2 * kSR; ++i) if (fabs(b.data[i]) > peak) peak = fabs(b.data[i]);
        const bool ok = worst / own <= 1.5 && d.getState() == 0 && peak == 0.0;
        printf("close: clear while playing %6.0f Hz -> worst step %.2fx the sine's own (<= 1.5), state %d, buffer peak %.3f  %s\n",
               f, worst / own, d.getState(), peak, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    // ── 0.8.5: the HANDOVER at a close, judged by CURVATURE. Tester on
    //    0.8.4: "a short pop only when you stop recording", free and
    //    clocked. 0.8.4's 5 ms linear fade passed the step checks above
    //    (small steps) but is a thump on low tones: shorter than their
    //    period, cornered at both ends — 40x a smooth sum's curvature at
    //    55 Hz. The step metric was the wrong instrument (standard §6).
    for (double f : {55.0, 123.0}) for (float dry : {1.0f, 0.0f}) for (int clocked = 0; clocked < 2; ++clocked) {
        Noether d(1);
        base(d, 1.0f, 0.5f, dry, 1.0f);
        Buffer b(1, kSR * 4);
        d.setSample(&b.s);
        std::vector<float> out;
        float *clk = d.mClkIn.buffer(), *rec = d.mRecIn.buffer(), *inL = d.mLeftIn.buffer();
        const int P = 12000; long t = 0; long closeAt = -1;
        if (clocked) d.mSyncOpt.set(2);
        auto run = [&](int blocks, long recAt) {
            for (int k = 0; k < blocks; ++k) {
                for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
                    inL[i] = (float)(0.8 * sin(2 * kPi * f * (double)t / kSR));
                    clk[i] = (clocked && (t % P) < 4) ? 1.0f : 0.0f;
                    rec[i] = (recAt >= 0 && t >= recAt && t < recAt + 4) ? 1.0f : 0.0f;
                }
                const int st0 = d.getState();
                d.process();
                if (st0 == 1 && d.getState() == 2 && closeAt < 0) closeAt = (long)out.size();
                for (int i = 0; i < FRAMELENGTH; ++i) out.push_back(d.mLeftOut.buffer()[i]);
            }
        };
        if (clocked) { run(20, -1); run(1, t + 50); run(100, -1); run(1, t + 50); run(200, -1); }
        else         { run(2, 5); run(300, -1); run(1, t + 77); run(200, -1); }
        const double w = 2 * kPi * f / kSR, own = 0.8 * w * w * 2.0;   // dry + loop
        double worst = 0;
        for (long i = closeAt - 128; closeAt > 0 && i < closeAt + 2880 && i + 1 < (long)out.size(); ++i) {
            const double c2 = fabs(out[i + 1] - 2 * out[i] + out[i - 1]); if (c2 > worst) worst = c2; }
        const bool ok = closeAt > 0 && worst / own <= 1.5;
        printf("close: handover %3.0f Hz dry %.0f %s -> closed %d, worst curvature %.1fx a smooth sum's (<= 1.5)  %s\n",
               f, (double)dry, clocked ? "clocked" : "free   ", closeAt > 0, worst / own, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Seam quality, by the metrics of lucasowens/previously DESIGN §5.6 (0.8.6):
// play the loop ACROSS the wrap from the buffer (what playback at 1x reads)
// and compare the seam to the loop's own body —
//   click metric: high-passed (2nd difference) energy in the seam window /
//                 the mean of same-length body windows; <= 1.1
//   envelope:     5 ms RMS through the seam within the body's own 5 ms RMS
//                 range +/- 1 dB
// on their signals: a sine that does not fit the loop, 40 Hz bass, DC
// offset, white noise, a kick on the downbeat, an anti-phase stereo pair —
// for clocked closes (pre-roll seam) and free closes (phase-matched blend).
// A clocked close must also leave the downbeat bit-exact.
struct SeamQ { double click, envLo, envHi; int headChanged; bool closed; int L; };

static SeamQ seamQuality(int which, bool clocked)
{
    const int nc = (which == 5) ? 2 : 1;
    Noether d(nc);
    base(d, 1.0f, 0.5f, 0.0f, 1.0f);
    if (clocked) d.mSyncOpt.set(2);
    Buffer b(nc, kSR * 4);
    d.setSample(&b.s);
    const int P = 12000;
    uint32_t rng = 0x12345u;
    auto sig = [&](long t, int ch) -> float {
        const double tt = (double)t / kSR;
        switch (which) {
        case 0: return (float)(0.8 * sin(2 * kPi * 54.5 * tt));                       // does not fit: 1/4 cycle off a 2-period loop
        case 1: return (float)(0.8 * sin(2 * kPi * 40.0 * tt));                       // 40 Hz bass
        case 2: return (float)(0.4 + 0.3 * sin(2 * kPi * 97.0 * tt));                 // DC offset
        case 3: { rng = rng * 1664525u + 1013904223u; return ((float)(rng >> 8) / 8388608.0f - 1.0f) * 0.5f; }  // noise
        case 4: { const long ph = t % P;                                              // a kick on every clock edge, alone
                  return (float)(ph < 9600 ? 0.8 * exp(-(double)ph / 2400.0) * sin(2 * kPi * 60.0 * ph / kSR) : 0.0); }
        case 6: return (float)(0.8 * sin(2 * kPi * 55.0 * tt));                       // EXACTLY half a cycle off (report only)
        default: { const float v = (float)(0.7 * sin(2 * kPi * 54.5 * tt)); return ch ? -v : v; }   // anti-phase pair
        }
    };
    float *clk = d.mClkIn.buffer(), *rec = d.mRecIn.buffer(), *inL = d.mLeftIn.buffer(), *inR = d.mRightIn.buffer();
    long t = 0, startT = -1;
    std::vector<float> histL, histR;                                   // the input, to check the downbeat
    auto run = [&](int blocks, long recAt) {
        for (int k = 0; k < blocks; ++k) {
            for (int i = 0; i < FRAMELENGTH; ++i, ++t) {
                inL[i] = sig(t, 0); inR[i] = nc > 1 ? sig(t, 1) : inL[i];
                histL.push_back(inL[i]); histR.push_back(inR[i]);
                clk[i] = (clocked && (t % P) < 4) ? 1.0f : 0.0f;
                rec[i] = (recAt >= 0 && t >= recAt && t < recAt + 4) ? 1.0f : 0.0f;
            }
            const int st0 = d.getState();
            d.process();
            if (st0 != 1 && d.getState() == 1 && startT < 0) {
                // the take began in this block: find its first frame in the history
                for (long c = t - FRAMELENGTH; c < t; ++c)
                    if (b.data[0] == histL[c] && (nc < 2 || b.data[1] == histR[c]) && b.data[(size_t)nc] == (c + 1 < t ? histL[c + 1] : b.data[(size_t)nc])) { startT = c; break; }
            }
        }
    };
    if (clocked) { run(20, -1); run(1, t + 50); run(200, -1); run(1, t + 50); run(2 * P / FRAMELENGTH, -1); }   // a 2-period take
    else         { run(20, -1); run(1, t + 40); run(320, -1); run(1, t + 77); run(40, -1); }
    SeamQ q = { 0, 0, 0, -1, d.getState() == 2, d.getLoopSamples() };
    // W spans several periods of the lowest test tone (100 ms, 4 of 40 Hz):
    // a 1536-frame window was 1.28 periods and its energy followed the
    // phase it started at (free 40 Hz read 1.12 with an exact seam)
    const int L = q.L, W = 4800, N = 4096;
    if (!q.closed || L < 3 * N) { d.setSample(nullptr); return q; }
    // the played sequence: buf[L-N .. L) then buf[0 .. N), channel by channel
    auto played = [&](int c, int i) -> float { const int pos = i < N ? L - N + i : i - N; return b.data[(size_t)pos * nc + c]; };
    auto hpE = [&](int c, int from, int len, bool acrossWrap) {
        double e = 0;
        for (int i = from + 1; i < from + len - 1; ++i) {
            float a, m, z;
            if (acrossWrap) { a = played(c, i - 1); m = played(c, i); z = played(c, i + 1); }
            else { a = b.data[(size_t)(i - 1) * nc + c]; m = b.data[(size_t)i * nc + c]; z = b.data[(size_t)(i + 1) * nc + c]; }
            const double h = z - 2.0 * m + a; e += h * h;
        }
        return e;
    };
    auto rms = [&](int c, int from, int len, bool acrossWrap) {
        double e = 0;
        for (int i = from; i < from + len; ++i) { const float v = acrossWrap ? played(c, i) : b.data[(size_t)i * nc + c]; e += (double)v * v; }
        return sqrt(e / len);
    };
    double clickWorst = 0, lo = 0, hi = 0;
    for (int c = 0; c < nc; ++c) {
        // seam window: the pre-roll crossfade before the wrap, +/- margins.
        // Reference: a clocked 2-period loop is compared with the SAME
        // place half a loop away (also a clock edge: a kick's attack there
        // is content, not a click); a free loop with the body's mean.
        const int seamFrom = N - Noether::kSeamClocked / 2 - W / 2;   // centred on the crossfade
        const double eSeam = hpE(c, seamFrom, W, true);
        double eBody = 0; int nb = 0;
        if (clocked) { eBody = hpE(c, L / 2 - (N - seamFrom), W, false); nb = 1; }
        else for (int at = L / 4; at + W < 3 * L / 4; at += W) { eBody += hpE(c, at, W, false); ++nb; }
        eBody /= nb;
        const double click = eBody > 1e-20 ? eSeam / eBody : (eSeam > 1e-20 ? 1e9 : 1.0);
        if (click > clickWorst) clickWorst = click;
        // envelope: 25 ms RMS (longer than a 40 Hz period — a 5 ms window
        // measures a low tone's phase, not its level) through the seam vs the
        // body's own 25 ms RMS range
        const int R = 1200;
        double bMin = 1e9, bMax = 0;
        for (int at = L / 4; at + R < 3 * L / 4; at += 120) { const double r = rms(c, at, R, false); if (r < bMin) bMin = r; if (r > bMax) bMax = r; }
        for (int at = seamFrom - R / 2; at + R < seamFrom + W + R / 2 && at + R < 2 * N; at += 120) {
            const double r = rms(c, at, R, true);
            const double dLo = 20 * log10((r + 1e-12) / (bMin + 1e-12)), dHi = 20 * log10((r + 1e-12) / (bMax + 1e-12));
            if (dLo < lo) lo = dLo;
            if (dHi > hi) hi = dHi;
        }
    }
    q.click = clickWorst; q.envLo = lo; q.envHi = hi;
    if (startT >= 0) {                                   // 0.8.6: free closes too keep the downbeat
        q.headChanged = 0;
        for (int k = 0; k < 2048; ++k) for (int c = 0; c < nc; ++c)
            if (b.data[(size_t)k * nc + c] != (c ? histR[startT + k] : histL[startT + k])) ++q.headChanged;
    }
    d.setSample(nullptr);
    return q;
}

static int t_seamq()
{
    int fails = 0;
    const char *names[7] = { "54.5 Hz, 1/4 cycle off", "40 Hz bass", "DC offset", "white noise", "kick on the downbeat", "anti-phase stereo", "55 Hz, 1/2 cycle off" };
    for (int clocked = 1; clocked >= 0; --clocked) for (int w = 0; w < 6; ++w) {
        const SeamQ q = seamQuality(w, clocked != 0);
        const bool envOk = q.envLo >= -1.0 && q.envHi <= 1.0;
        const bool headOk = q.headChanged == 0;
        const bool ok = q.closed && q.click <= 1.1 && envOk && headOk;
        printf("seamq: %s %-22s -> loop %6d, click %.2f (<= 1.1), envelope %+.2f / %+.2f dB (within 1), downbeat frames changed %d  %s\n",
               clocked ? "clocked" : "free   ", names[w], q.L, q.click, q.envLo, q.envHi, q.headChanged, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    // the known limit, reported not judged: a steady tone EXACTLY half a
    // cycle off a clocked loop meets its own inverse — any crossfade between
    // a signal and its inverse passes through silence. Track the number.
    {
        const SeamQ q = seamQuality(6, true);
        printf("seamq: clocked %-22s -> envelope dip %+.1f dB (known limit: a crossfade between a tone and its inverse), downbeat frames changed %d\n",
               names[6], q.envLo, q.headChanged);
    }
    return fails ? 1 : 0;
}

// Keep clean (0.8.8): Noether saves its loop itself, so its own buffer must
// not be marked "unsaved" — any dirty pool sample makes every quicksave ask
// to save buffers (Persist/init.lua handleUnsavedBuffers). A buffer the
// player attached (setKeepClean(0)) keeps the SDK's behaviour.
static int t_clean()
{
    int fails = 0;
    for (int keep = 1; keep >= 0; --keep) {
        Noether d(2);
        base(d, 1.0f, 0.5f, 1.0f, 1.0f);
        Buffer b(2, kSR * 2);
        const bool bornDirty = b.s.mDirty;              // setMemoryOnly marks a new buffer dirty
        b.s.setClean();                                 // what Noether.lua does on attach
        d.setKeepClean(keep);
        d.setSample(&b.s);
        std::vector<float> out;
        recordSine(d, 220.0, 20000, 3, out);            // a take, closed
        const bool afterTake = b.s.mDirty;
        std::vector<float> dc(FRAMELENGTH, 0.3f);
        block(d, dc.data(), FRAMELENGTH, 0, out);       // overdub in
        for (int k = 0; k < 40; ++k) block(d, dc.data(), FRAMELENGTH, -1, out);
        block(d, dc.data(), FRAMELENGTH, 0, out);       // out
        silentBlocks(d, 4, out);
        const bool afterDub = b.s.mDirty;
        d.clearLoop(); d.zeroBuffer();
        silentBlocks(d, 2 * kSR / Noether::kZeroPerBlock + 12, out);
        const bool afterClear = b.s.mDirty;
        const bool ok = bornDirty && (keep ? (!afterTake && !afterDub && !afterClear) : (afterTake && afterDub));
        printf("clean: keep %d -> new buffer dirty %d; after a take %d, an overdub %d, a clear %d (keep 1: all 0; keep 0: marked)  %s\n",
               keep, bornDirty, afterTake, afterDub, afterClear, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}

// Sync-mode overdub and extend (0.8.10). Nick: "I want overdub to do
// another looped take at that bar count when in sync mode. Extend should
// update the bar count so re-sync and readouts follow new length."
// Rig: 4 pulses a bar, a pulse every Q; a 2-bar loop (8 pulses).
struct SyncRig {
    Noether d;
    Buffer b;
    std::vector<float> out;
    long t = 0;
    const int Q = 3000;
    std::vector<int> stateAt;                // per block
    std::vector<long> edges;                 // clock edge times
    SyncRig(bool sync) : d(1), b(1, kSR * 6)
    {
        base(d, 1.0f, 0.0f, 0.0f, 1.0f);
        if (sync) d.mSyncOpt.set(2);
        d.setPulsesPerBar(4);
        d.setSample(&b.s);
    }
    void bars(float n) { Ctl c[] = {{"Bars", n}}; setInputs(d, c, 1); }
    void ext(float g)  { Ctl c[] = {{"Extend", g}}; setInputs(d, c, 1); }
    // run whole blocks; REC pulse at absolute sample recAt (if >= 0)
    void run(int blocks, float in, long recAt = -1, long dubAt = -1)
    {
        float *L = d.mLeftIn.buffer(), *R = d.mRightIn.buffer(), *rec = d.mRecIn.buffer(), *clk = d.mClkIn.buffer(), *dub = d.mDubIn.buffer();
        for (int k = 0; k < blocks; ++k) {
            for (int i = 0; i < FRAMELENGTH; ++i) {
                const long tt = t + i;
                L[i] = R[i] = in;
                clk[i] = (tt % Q) < 4 ? 1.0f : 0.0f;
                if ((tt % Q) == 0) edges.push_back(tt);
                rec[i] = (recAt >= 0 && tt >= recAt && tt < recAt + 4) ? 1.0f : 0.0f;
                dub[i] = (dubAt >= 0 && tt >= dubAt && tt < dubAt + 4) ? 1.0f : 0.0f;
            }
            d.process();
            for (int i = 0; i < FRAMELENGTH; ++i) out.push_back(d.mLeftOut.buffer()[i]);
            stateAt.push_back(d.getState());
            t += FRAMELENGTH;
        }
    }
    void blocksTo(long until, float in) { while (t < until) run(1, in); }
    // a REC press at sample `at`: run up to the block that CONTAINS it, then
    // that block with the press (the first draft stopped 200 samples short
    // and ran one block — which often ended before the press: no press)
    void pressAt(long at, float in)
    {
        while (t + FRAMELENGTH <= at) run(1, in);
        run(1, in, at);
    }
    void dubAt(long at, float in)                     // the DUB button (0.8.10)
    {
        while (t + FRAMELENGTH <= at) run(1, in);
        run(1, in, -1, at);
    }
    // the first block index whose state is s, at or after block `from`
    long firstBlock(int s, long from) const { for (size_t k = (size_t)from; k < stateAt.size(); ++k) if (stateAt[k] == s) return (long)k; return -1; }
    // a 2-bar take (Bars 2 = 8 pulses): returns the close edge time
    long take()
    {
        bars(2.0f);
        run(20, 0.3f);
        run(1, 0.3f, t + 50);                         // arm
        blocksTo(t + 10 * Q, 0.3f);                   // starts, 8 pulses, closes
        for (size_t k = 1; k < stateAt.size(); ++k) if (stateAt[k - 1] == 1 && stateAt[k] == 2) return (long)k * FRAMELENGTH;
        return -1;
    }
};

static int t_syncdub()
{
    int fails = 0;
    {   // ── a counted overdub: armed mid-bar, in on the bar line, out by
        //    itself after exactly 2 bars (8 pulses), on a bar line
        SyncRig r(true);
        const long tc = r.take();
        const int L = r.d.getLoopSamples();
        r.bars(2.0f);
        const long press = r.t + r.Q + 700;           // mid-bar
        const long pressBlock = press / FRAMELENGTH;
        r.pressAt(press, 0.6f);
        r.blocksTo(r.t + 14 * r.Q, 0.6f);
        const int armedThen = 0;
        const long in = r.firstBlock(3, pressBlock) * FRAMELENGTH;
        const long outB = r.firstBlock(2, in / FRAMELENGTH) * FRAMELENGTH;
        // bar lines: every 4 pulses from the loop's downbeat (the close edge)
        const long inPulses = (in - tc + r.Q / 2) / r.Q, outPulses = (outB - tc + r.Q / 2) / r.Q;
        const bool onBarIn = labs(in - (tc + inPulses * r.Q)) < 2 * FRAMELENGTH && inPulses % 4 == 0;
        const bool notAtPress = in > press + r.Q / 2;
        const long span = outB - in;                  // 8 pulses + the 5 ms ramp out
        const bool spanOk = labs(span - 8 * r.Q) < 240 + 2 * FRAMELENGTH;
        const bool ok = L == 8 * r.Q && onBarIn && notAtPress && spanOk && r.d.getDubArmed() == 0;
        printf("syncdub: counted (Bars 2, 4 a bar) -> press mid-bar, in %ld samples later on pulse %ld from the downbeat (a bar line %d), lasted %ld (expect %d + ramp) %s\n",
               in - press, inPulses, (int)onBarIn, span, 8 * r.Q, ok ? "ok" : "FAIL");
        (void)armedThen; (void)outPulses;
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── manual (Bars 0): in and out both on bar lines
        SyncRig r(true);
        const long tc = r.take();
        r.bars(0.0f);
        const long p1 = r.t + r.Q + 900;
        r.pressAt(p1, 0.6f);
        r.blocksTo(r.t + 6 * r.Q, 0.6f);
        const long p2 = r.t + 1300;
        r.pressAt(p2, 0.6f);
        r.blocksTo(r.t + 6 * r.Q, 0.6f);
        const long in = r.firstBlock(3, p1 / FRAMELENGTH) * FRAMELENGTH;
        const long outB = r.firstBlock(2, in / FRAMELENGTH) * FRAMELENGTH;
        const long ip = (in - tc + r.Q / 2) / r.Q, op = (outB - 240 - tc + r.Q / 2) / r.Q;
        const bool ok = ip % 4 == 0 && op % 4 == 0 && outB > p2;
        printf("syncdub: manual (Bars 0) -> in on pulse %ld, out on pulse %ld from the downbeat (both bar lines)  %s\n", ip, op, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── pressing again while armed cancels: no overdub, the loop untouched
        SyncRig r(true);
        r.take();
        std::vector<float> before(r.b.data.begin(), r.b.data.begin() + r.d.getLoopSamples());
        const long p1 = r.t + r.Q + 500;
        r.pressAt(p1, 0.9f);
        r.pressAt(r.t + 200, 0.9f);                   // again, well before the bar line
        r.blocksTo(r.t + 10 * r.Q, 0.9f);
        int changed = 0; for (size_t i = 0; i < before.size(); ++i) if (r.b.data[i] != before[i]) ++changed;
        const bool ok = r.firstBlock(3, 0) < 0 && changed == 0 && r.d.getDubArmed() == 0;
        printf("syncdub: armed then pressed again -> overdub never started %d, %d loop samples changed  %s\n", r.firstBlock(3, 0) < 0, changed, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── free mode: the overdub starts on the press, as before
        SyncRig r(false);
        recordSine(r.d, 220.0, 20000, 3, r.out);
        const long press = r.t + 700;
        r.pressAt(press, 0.5f);
        const bool ok = r.stateAt.back() == 3;
        printf("syncdub: free mode -> overdub in the block of the press %d  %s\n", (int)ok, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── the DUB button (0.8.10), sync mode: in and out NOW, not on bars
        SyncRig r(true);
        r.take();
        r.bars(0.0f);
        const long d1 = r.t + r.Q + 900;               // mid-bar
        r.dubAt(d1, 0.6f);
        const int inNow = r.stateAt.back();             // the press's own block
        const long d2 = r.t + 2 * r.Q + 400;            // mid-bar again
        r.dubAt(d2, 0.6f);
        r.run(3, 0.6f);                                 // the 5 ms ramp out
        const int outSoon = r.stateAt.back();
        // dub while a rec overdub is WAITING for the bar line: starts now
        const long p = r.t + r.Q + 700;
        r.pressAt(p, 0.6f);
        const int armed = r.d.getDubArmed();
        r.dubAt(r.t + 300, 0.6f);
        const int nowIn = r.stateAt.back(), armedAfter = r.d.getDubArmed();
        r.dubAt(r.t + 300, 0.6f); r.run(3, 0.6f);
        // dub during a COUNTED (rec) overdub: stops now, not after its bars
        r.bars(2.0f);
        r.pressAt(r.t + r.Q + 500, 0.6f);
        // wait for it to start on its bar line (bar lines come every 4
        // pulses here; the first draft waited 2 pulses and saw nothing)
        for (int k = 0; k < 6 * r.Q / FRAMELENGTH && r.stateAt.back() != 3; ++k) r.run(1, 0.6f);
        r.run(8, 0.6f);
        const int counting = r.stateAt.back(), dubBars = r.d.getDubBars();
        r.dubAt(r.t + 300, 0.6f); r.run(3, 0.6f);
        const int stopped = r.stateAt.back();
        const bool ok = inNow == 3 && outSoon == 2 && armed == 1 && nowIn == 3 && armedAfter == 0 &&
                        counting == 3 && dubBars == 2 && stopped == 2;
        printf("syncdub: dub button -> in at the press %d, out within the ramp %d; while a rec dub waits: armed %d -> in now %d (armed %d); during a counted dub (%d bars): out now %d  %s\n",
               inNow == 3, outSoon == 2, armed, nowIn == 3, armedAfter, dubBars, stopped == 2, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── dub with no loop: nothing to overdub, nothing happens
        SyncRig r(true);
        r.run(20, 0.3f);
        r.dubAt(r.t + 300, 0.6f);
        r.run(4, 0.6f);
        const bool ok = r.stateAt.back() == 0 && r.d.getLoopSamples() == 0;
        printf("syncdub: dub with no loop -> state %d (stays empty)  %s\n", r.stateAt.back(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    {   // ── extend in sync, Bars 1: adds exactly one bar; N grows 8 -> 12; the
        //    loop then stays on the downbeat (no re-sync yanking it back)
        SyncRig r(true);
        const long tc = r.take();
        (void)tc;
        r.bars(1.0f);
        r.ext(1.0f);
        const long press = r.t + 600;
        r.pressAt(press, 0.5f);                       // armed; in at the next bar line, runs off the end, extends
        r.blocksTo(r.t + 14 * r.Q, 0.5f);
        r.ext(0.0f);
        const int N = r.d.getSyncN(), L = r.d.getLoopSamples(), bars = r.d.getBars();
        long close = -1; for (size_t k = 1; k < r.stateAt.size(); ++k) if (r.stateAt[k - 1] == 5 && r.stateAt[k] != 5) close = (long)k * FRAMELENGTH;   // stateAt[k] = after block k: it closed IN block k
        // the close edge: the clock edge inside the block that left EXTEND
        long ce = -1; for (long e : r.edges) if (close >= 0 && e >= close && e < close + FRAMELENGTH) ce = e;
        // over the next 3 loops the head is at the loop's start on every
        // downbeat (every 12 pulses); with the old pulse count (8) the
        // re-sync would yank it back every 8 and it would be 4 pulses in
        int worst = 0;
        for (int k = 1; k <= 3 && ce >= 0; ++k) {
            r.blocksTo(ce + (long)k * 12 * r.Q + 2 * FRAMELENGTH, 0.0f);
            const int v = r.d.vizPos();
            const int dist = v < L / 2 ? v : L - v;
            if (dist > worst) worst = dist;
        }
        const bool ok = N == 12 && L == 12 * r.Q && bars == 3 && ce >= 0 && worst <= 3 * FRAMELENGTH;
        printf("syncdub: extend in sync, Bars 1 -> loop %d (expect %d = 12 pulses), N %d (expect 12), shows %d bars; close edge found %d; on the next 3 downbeats the head is %d samples from the start (<= %d)  %s\n",
               L, 12 * r.Q, N, bars, (int)(ce >= 0), worst, 3 * FRAMELENGTH, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
        r.d.setSample(nullptr);
    }
    return fails ? 1 : 0;
}


// Stop during a take (0.8.14). Nick: "interesting behavior when you try to
// hit 'stop' when it is recording. it kind of holds" — until 0.8.13 the take
// ignored stop and kept recording. Now: free, the take closes at the press;
// sync, on the next pulse (a whole number of pulses); the loop lands held
// (STOP) and plays when stop is released.
// The bar readout (0.8.14). Nick: "check that sub-screen bar count is
// accurate ... maybe the graphic is not representing reality". Until 0.8.14
// it counted clock pulses: right at 1x, wrong at other speeds (0.5x showed
// bar 2 while the downbeat re-sync kept the head in bar 1), running while
// stopped, frozen through an extension, and a loop off the bar lines read
// its pulses as "bar". Now it reads the play head; and a sync stop releases
// in phase with the clock (it used to resume in place, out of time).
static int t_barview()
{
    int fails = 0;
    auto headBar = [](SyncRig &r) { const int L = r.d.getLoopSamples(), b = r.d.getBars();
                                    return (L > 0 && b > 0) ? (int)((long)r.d.vizPos() * b / L) + 1 : 0; };
    const float speeds[] = { 1.0f, 0.5f, 2.0f, -1.0f };
    for (float sp : speeds) {
        SyncRig r(true); r.take();
        { Ctl c[] = {{"Speed", sp}}; setInputs(r.d, c, 1); }
        r.run(40, 0.0f);                              // glide settles
        int bad = 0, n = 0;
        for (int k = 0; k < 400; ++k) { r.run(1, 0.0f); ++n; if (r.d.getBar() != headBar(r)) ++bad; }
        const bool ok = bad == 0;
        printf("barview: speed %+.1f -> readout != head bar in %d of %d blocks  %s\n", sp, bad, n, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    {   // stop / release in sync: back where an unstopped twin is
        SyncRig a(true), b(true); a.take(); b.take();
        a.run(30, 0.0f); b.run(30, 0.0f);
        { Ctl c[] = {{"Stop", 1.0f}}; setInputs(a.d, c, 1); }
        a.run(100, 0.0f); b.run(100, 0.0f);           // across a downbeat
        const int heldBar = a.d.getBar(), twinBar = b.d.getBar();
        { Ctl c[] = {{"Stop", 0.0f}}; setInputs(a.d, c, 1); }
        a.run(1, 0.0f); b.run(1, 0.0f);
        const int L = a.d.getLoopSamples();
        int dpos = a.d.vizPos() - b.d.vizPos(); if (dpos > L / 2) dpos -= L; if (dpos < -L / 2) dpos += L;
        a.run(200, 0.0f); b.run(200, 0.0f);
        int dlater = a.d.vizPos() - b.d.vizPos(); if (dlater > L / 2) dlater -= L; if (dlater < -L / 2) dlater += L;
        const bool ok = heldBar == twinBar && abs(dpos) <= FRAMELENGTH && abs(dlater) <= FRAMELENGTH && a.d.getState() == 2;
        printf("barview: sync stop -> held readout bar %d (twin %d); released head %+d samples from the twin, %+d later  %s\n",
               heldBar, twinBar, dpos, dlater, ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    {   // an extension counts up; a manual 7-pulse loop reads in beats
        SyncRig r(true); r.take(); r.bars(1.0f); r.ext(1.0f); r.run(2, 0.3f); r.pressAt(r.t + 300, 0.3f);
        int lo = 99, hi = 0, seen = 0;
        for (int k = 0; k < 400; ++k) { r.run(1, 0.3f); if (r.d.getState() == 5) { ++seen; lo = std::min(lo, r.d.getBar()); hi = std::max(hi, r.d.getBar()); } }
        SyncRig m(true); m.bars(0.0f); m.run(20, 0.3f); m.run(1, 0.3f, m.t + 50); m.blocksTo(m.t + 6 * m.Q + 500, 0.3f); m.run(1, 0.3f, m.t + 50); m.blocksTo(m.t + m.Q, 0.3f);
        const bool ok = seen > 0 && lo == 3 && hi == 3 && r.d.getBars() == 3 && m.d.getSyncN() == 7 && m.d.getBarsWhole() == 0 && r.d.getBarsWhole() == 1;
        printf("barview: extending 2 -> 3 bars reads bar %d..%d (expect 3), after: %d bars; manual 7-pulse loop whole-bars %d (expect 0)  %s\n",
               lo, hi, r.d.getBars(), m.d.getBarsWhole(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    return fails ? 1 : 0;
}

static int t_stoprec()
{
    int fails = 0;
    auto stopGate = [](SyncRig &r, float g) { Ctl c[] = {{"Stop", g}}; setInputs(r.d, c, 1); };
    {   // free
        SyncRig r(false);
        r.run(20, 0.3f);
        r.run(1, 0.3f, r.t + 50);                     // REC: recording
        r.run(40, 0.3f);
        const int s0 = r.stateAt.back();
        stopGate(r, 1.0f); r.run(3, 0.3f);           // closed at once; held once the 5 ms fade is out
        const int s1 = r.stateAt.back(), L = r.d.getLoopSamples();
        r.run(20, 0.0f);
        const int s2 = r.stateAt.back();
        double held = 0; for (size_t i = r.out.size() - 10 * FRAMELENGTH; i < r.out.size(); ++i) held = fmax(held, fabs(r.out[i]));
        stopGate(r, 0.0f); r.run(10, 0.0f);
        double play = 0; for (size_t i = r.out.size() - 5 * FRAMELENGTH; i < r.out.size(); ++i) play = fmax(play, fabs(r.out[i]));
        const bool ok = s0 == 1 && s1 == 4 && L > 30 * FRAMELENGTH && L < 42 * FRAMELENGTH && s2 == 4 && held == 0.0 && play > 0.25 && r.stateAt.back() == 2;
        printf("stoprec: free -> state %d, stop: state %d (STOP=4) loop %d, held peak %.4f, released: peak %.3f state %d  %s\n",
               s0, s1, L, held, play, r.stateAt.back(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    {   // sync, Bars 0 (manual): closes on the next pulse, a whole number of pulses
        SyncRig r(true);
        r.bars(0.0f);
        r.run(20, 0.3f);
        r.run(1, 0.3f, r.t + 50);                     // armed; starts on the next pulse
        r.blocksTo(r.t + 5 * r.Q + r.Q / 3, 0.3f);    // mid-pulse
        const int s0 = r.stateAt.back();
        stopGate(r, 1.0f); r.run(1, 0.3f);
        const int s1 = r.stateAt.back();             // still recording to the pulse
        r.blocksTo(r.t + r.Q, 0.3f);
        const int L = r.d.getLoopSamples(), s2 = r.stateAt.back();
        const bool whole = L > 0 && L % r.Q == 0;
        stopGate(r, 0.0f); r.run(10, 0.0f);
        const bool ok = s0 == 1 && s1 == 1 && s2 == 4 && whole && r.stateAt.back() == 2;
        printf("stoprec: sync -> stop mid-pulse: state %d, then %d (STOP=4), loop %d = %d pulses (whole %d), released: state %d  %s\n",
               s1, s2, L, L / r.Q, (int)whole, r.stateAt.back(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    {   // a running dub punches out
        SyncRig r(false);
        r.run(20, 0.3f); r.run(1, 0.3f, r.t + 50); r.run(40, 0.3f); r.run(1, 0.3f, r.t + 50);   // record, close
        r.run(10, 0.3f); r.run(1, 0.3f, r.t + 50);   // dub
        r.run(5, 0.3f);
        const int s0 = r.stateAt.back();
        stopGate(r, 1.0f); r.run(10, 0.3f);
        const int s1 = r.stateAt.back();
        stopGate(r, 0.0f); r.run(10, 0.3f);
        const bool ok = s0 == 3 && s1 == 4 && r.stateAt.back() == 2;
        printf("stoprec: dub -> state %d, stop: %d, released: %d (play, not dub)  %s\n", s0, s1, r.stateAt.back(), ok ? "ok" : "FAIL");
        if (!ok) ++fails;
    }
    return fails ? 1 : 0;
}

static int t_nan()
{
    const float poisons[] = { NAN, INFINITY, -INFINITY };
    const char *ports[] = { "Left In", "Right In", "Rec", "Speed", "SOS", "Dry", "Level",
                            "V/Oct", "Extend", "Stop", "Undo", "Clk", "Dub", "Bars" };
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
    const float speeds[] = { -2.0f, -1.0f, -0.01f, 0.0f, 0.01f, 1.0f, 2.0f, 9.0f, -9.0f };
    for (int ch = 1; ch <= 2; ++ch) for (float sp : speeds) for (float sos : {0.0f, 1.0f}) for (int gridDummy = 0; gridDummy < 2; ++gridDummy) {
        Noether d(ch);
        base(d, sp, sos, 1.0f, 1.0f);
        Ctl wc[] = {{"Extend", 1.0f}, {"V/Oct", 0.05f}}; setInputs(d, wc, 2);
        Buffer ub(ch, kSR / 4);
        d.setUndoSample(&ub.s);
        d.mSyncOpt.set((gridDummy == 1) ? 2 : 1);
        { float *ck = d.mClkIn.buffer(); for (int i = 0; i < FRAMELENGTH; ++i) ck[i] = (i < 2) ? 1.0f : 0.0f; }   // an edge every block
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
        undoEdge(d, out); for (int k = 0; k < 30; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        undoEdge(d, out); undoEdge(d, out);                              // redo, and an undo mid-restore
        { Ctl pg[] = {{"Stop", 1.0f}}; setInputs(d, pg, 1); }
        for (int k = 0; k < 10; ++k) block(d, src.data(), FRAMELENGTH, 3, out);   // rec edges while stopped
        { Ctl pg[] = {{"Stop", 0.0f}}; setInputs(d, pg, 1); }
        d.clearLoop();
        for (int k = 0; k < 5; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        block(d, src.data(), FRAMELENGTH, 127, out);                     // edge on the last sample
        block(d, src.data(), FRAMELENGTH, 0, out);                       // 1-sample take
        for (int k = 0; k < 20; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        // bound: the loop is softLimited to +/-2, a jump overlap can sum two of
        // those for 8 ms, and the dry input here is 1.5 -> 5.5 is legitimate
        for (float v : out) if (!std::isfinite(v) || fabs(v) > 6.0f) ++bad;
        d.setUndoSample(nullptr);
        d.setSample(nullptr);
        // and with no buffer at all, mid-flight
        block(d, src.data(), FRAMELENGTH, 0, out);
        undoEdge(d, out);
    }
    // 0.8.1: a mono unit holding a STEREO loop buffer and a MONO undo buffer
    // (Attach pool buffer... in a mono chain). Undo must switch itself off,
    // never index the mono buffer with the loop's two channels.
    {
        Noether d(1);
        base(d, 1.0f, 0.0f, 1.0f, 1.0f);
        Buffer b(2, kSR / 2), ub(1, kSR / 2);
        d.setSample(&b.s);
        d.setUndoSample(&ub.s);
        std::vector<float> out, src(FRAMELENGTH, 0.4f);
        // the take must span MORE than half the buffer, so frame i * 2 of a
        // stereo index lands past the end of the mono undo buffer
        block(d, src.data(), FRAMELENGTH, 0, out);
        for (int k = 0; k < 150; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        block(d, src.data(), FRAMELENGTH, 0, out);            // close
        block(d, src.data(), FRAMELENGTH, -1, out);           // REC low: the next press is a new edge
        block(d, src.data(), FRAMELENGTH, 0, out);            // overdub in
        for (int k = 0; k < 170; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        const int dubState = d.getState();
        block(d, src.data(), FRAMELENGTH, 0, out);            // overdub out
        for (int k = 0; k < 10; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        d.mUndoIn.buffer()[0] = 1.0f; block(d, src.data(), FRAMELENGTH, -1, out); d.mUndoIn.buffer()[0] = 0.0f;
        for (int k = 0; k < 10; ++k) block(d, src.data(), FRAMELENGTH, -1, out);
        if (dubState != 3) { printf("asan: mismatch scenario never overdubbed (state %d)\n", dubState); ++bad; }
        if (d.canUndo() != 0) { printf("asan: channel-mismatched undo buffer still reports canUndo\n"); ++bad; }
        d.setUndoSample(nullptr);
        d.setSample(nullptr);
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
    else if (!strcmp(m, "tri"))    r = t_tri();
    else if (!strcmp(m, "bars"))   r = t_bars();
    else if (!strcmp(m, "dub"))    r = t_dub();
    else if (!strcmp(m, "close"))  r = t_close();
    else if (!strcmp(m, "seamq"))  r = t_seamq();
    else if (!strcmp(m, "clean"))  r = t_clean();
    else if (!strcmp(m, "syncdub")) r = t_syncdub();
    else if (!strcmp(m, "stoprec")) r = t_stoprec();
    else if (!strcmp(m, "barview")) r = t_barview();
    else if (!strcmp(m, "extend")) r = t_extend();
    else if (!strcmp(m, "voct"))   r = t_voct();
    else if (!strcmp(m, "persist")) r = t_persist();
    else if (!strcmp(m, "detent")) r = t_detent();
    else if (!strcmp(m, "aa"))     r = t_aa();
    else if (!strcmp(m, "viz"))    r = t_viz();
    else if (!strcmp(m, "undo"))   r = t_undo();
    else if (!strcmp(m, "stop"))   r = t_stop();
    else if (!strcmp(m, "clock"))  r = t_clock();
    else if (!strcmp(m, "speed"))  r = t_speed();
    else if (!strcmp(m, "sos"))    r = t_sos();
    else if (!strcmp(m, "nan"))    r = t_nan();
    else if (!strcmp(m, "cpu"))    r = t_cpu();
    else if (!strcmp(m, "asan"))   r = t_asan();
    else { fprintf(stderr, "unknown mode %s\n", m); return 2; }
    printf("%s: %s\n", m, r == 0 ? "PASS" : "FAIL");
    return r;
}
