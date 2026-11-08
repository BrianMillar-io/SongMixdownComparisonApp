// Live version of analyzer_app/analyze.py: the same measurements, made on a
// stream of audio instead of a whole file.
//
// The audio thread only copies samples into a lock-free ring (push). A worker
// thread does the measuring (process) and builds a Snapshot that the screen
// reads. Plain C++17, no JUCE, so the self test can run it on any machine.
#pragma once

#include <array>
#include <atomic>
#include <complex>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mix {

constexpr int kNumBands = 10;
extern const char* const kBandIds[kNumBands];
extern const double kBandEdges[kNumBands][2];
extern const std::vector<double> kThirdOctaveHz;

// Biquad in direct form II transposed, run per channel.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    double run(double x, int ch) {
        double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
    void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};

// Everything measured, keyed with the metric names the genre profiles use
// (e.g. "tonal.bands_db.air"). A metric that can't be measured yet is absent.
struct Snapshot {
    std::map<std::string, double> values;
    double seconds = 0;          // audio measured (silence not counted)
    bool stereo = false;
    bool recent = false;         // true: last ~10 s, false: since reset
    double momentaryLufs = -100; // right now, for the meter
    double shortTermLufs = -100;
    double truePeakNow = -100;   // the last second
    std::vector<double> thirdOctaveDb; // same freqs as kThirdOctaveHz; NaN if empty
    bool has(const std::string& k) const { return values.count(k) > 0; }
    double get(const std::string& k, double fallback = 0) const {
        auto it = values.find(k);
        return it == values.end() ? fallback : it->second;
    }
};

class LiveAnalyzer {
public:
    LiveAnalyzer();

    // Message thread, while audio is stopped (prepareToPlay).
    void prepare(double sampleRate);

    // Audio thread. Never blocks or allocates. right may equal left (mono).
    void push(const float* left, const float* right, int numSamples);

    // Ask for a fresh start. Safe from any thread; the worker applies it.
    void requestReset() { resetRequested.store(true); }

    // Worker thread: measure everything pushed so far, then rebuild the
    // snapshots. Returns true if anything new was measured.
    bool process();

    // Any non-audio thread. whole = since reset, recent = last ~10 s.
    Snapshot snapshot(bool recent) const;

    double sampleRate() const { return fs; }

private:
    struct Block {  // one 100 ms block of measurements
        double kw = 0;      // K-weighted mean square, L + R
        double sumSq = 0;   // raw sum of squares, both channels
        double n = 0;       // samples
        double peak = 0, truePeak = 0;
        double l = 0, r = 0, ll = 0, rr = 0, lr = 0;
        double bl = 0, br = 0, bll = 0, brr = 0, blr = 0;  // below 120 Hz
        double mid2 = 0, side2 = 0;
        bool loud = false;  // above the -70 LUFS silence gate
        void add(const Block& o);
    };

    void resetState();
    void consume(double l, double r);
    void endBlock();
    void runFft();
    Snapshot build(bool recent) const;
    void fillTonal(Snapshot& s, const std::vector<double>& pm,
                   const std::vector<double>& ps) const;

    double fs = 44100;
    int blockLen = 4410;

    // Lock-free single producer / single consumer ring.
    std::vector<float> ring;  // interleaved L R
    int ringFrames = 0;
    std::atomic<int> writePos{0}, readPos{0};
    std::atomic<bool> resetRequested{false};

    Biquad kShelf, kHighPass, low1, low2;
    std::vector<double> tpTaps;   // 4 phases x tpLen taps
    int tpLen = 0;
    std::vector<double> tpHist[2];
    int tpPos = 0;

    Block cur;
    int curCount = 0;
    Block total;
    std::vector<Block> recentBlocks;  // ring of the last 100 blocks
    int recentPos = 0;
    std::vector<double> kwRing;       // last 30 block energies, for the 400 ms / 3 s windows
    int kwCount = 0;
    std::vector<double> momentary;    // 400 ms window energies every 100 ms, above -70 LUFS
    std::vector<double> shortTerm;    // 3 s loudness every 1 s, in LUFS, above -70
    std::vector<double> tpSecond;     // last 10 block true peaks
    int tpSecondPos = 0;
    double momentaryNow = -100, shortTermNow = -100;
    int blocksSeen = 0;

    // Spectrum (Welch: Hann window, 50% overlap).
    int fftN = 16384;
    std::vector<double> window;
    std::vector<double> fftMid, fftSide;  // collected samples
    int fftFill = 0;
    std::vector<std::complex<double>> fftBuf;
    std::vector<double> psdMidTotal, psdSideTotal, psdMidRecent, psdSideRecent;
    double fftFrames = 0, recentDecay = 0.5;

    std::mutex workMutex;
    mutable std::mutex snapMutex;
    Snapshot snapWhole, snapRecent;
};

// In-place radix-2 FFT, size a power of two.
void fft(std::vector<std::complex<double>>& a);

}  // namespace mix
