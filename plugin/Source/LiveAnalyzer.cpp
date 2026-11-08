#include "LiveAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace mix {

const char* const kBandIds[kNumBands] = {"sub", "bass", "low_mid", "mid_low", "mid",
                                         "upper_mid", "presence", "high", "air", "top"};
const double kBandEdges[kNumBands][2] = {{20, 60},     {60, 120},    {120, 250},
                                         {250, 500},   {500, 1000},  {1000, 2000},
                                         {2000, 4000}, {4000, 8000}, {8000, 12000},
                                         {12000, 20000}};
const std::vector<double> kThirdOctaveHz = {
    20,  25,  31.5, 40,   50,   63,   80,   100,  125,  160,   200,   250,   315,   400,   500,  630,
    800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSilenceLufs = -70.0;
constexpr int kRecentBlocks = 100;   // 10 s
constexpr int kTpPhases = 4;
// The analyzer measures at 44.1 kHz, so nothing above 22.05 kHz counts.
constexpr double kAnalysisNyquist = 22050.0;

double lufs(double meanSquare) { return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1e-12)); }
double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-12)); }

// pyloudnorm's two K-weighting stages, designed for any sample rate.
Biquad highShelf(double fs, double gainDb, double q, double fc) {
    double A = std::pow(10.0, gainDb / 40.0), w0 = 2 * kPi * fc / fs;
    double alpha = std::sin(w0) / (2 * q), c = std::cos(w0), sa = 2 * std::sqrt(A) * alpha;
    double b0 = A * ((A + 1) + (A - 1) * c + sa), b1 = -2 * A * ((A - 1) + (A + 1) * c),
           b2 = A * ((A + 1) + (A - 1) * c - sa), a0 = (A + 1) - (A - 1) * c + sa,
           a1 = 2 * ((A - 1) - (A + 1) * c), a2 = (A + 1) - (A - 1) * c - sa;
    Biquad f;
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
    return f;
}

Biquad highPass(double fs, double q, double fc) {
    double w0 = 2 * kPi * fc / fs, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    double a0 = 1 + alpha;
    Biquad f;
    f.b0 = (1 + c) / 2 / a0; f.b1 = -(1 + c) / a0; f.b2 = (1 + c) / 2 / a0;
    f.a1 = -2 * c / a0; f.a2 = (1 - alpha) / a0;
    return f;
}

Biquad lowPass(double fs, double q, double fc) {
    double w0 = 2 * kPi * fc / fs, alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    double a0 = 1 + alpha;
    Biquad f;
    f.b0 = (1 - c) / 2 / a0; f.b1 = (1 - c) / a0; f.b2 = (1 - c) / 2 / a0;
    f.a1 = -2 * c / a0; f.a2 = (1 - alpha) / a0;
    return f;
}

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

// BS.1770 gating over 400 ms block energies that already passed -70 LUFS.
double gatedLoudness(const double* e, size_t n) {
    if (n == 0) return NAN;
    double mean = 0;
    for (size_t i = 0; i < n; ++i) mean += e[i];
    double gate = lufs(mean / n) - 10.0, sum = 0;
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i)
        if (lufs(e[i]) > gate) { sum += e[i]; ++kept; }
    return kept ? lufs(sum / kept) : NAN;
}

double percentile(std::vector<double> v, double p) {  // numpy's linear method
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (v.size() - 1);
    size_t lo = (size_t)std::floor(pos), hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - lo);
}

double corr(double n, double a, double b, double aa, double bb, double ab, bool& ok) {
    double va = aa / n - (a / n) * (a / n), vb = bb / n - (b / n) * (b / n);
    ok = va > 1e-18 && vb > 1e-18;
    return ok ? (ab / n - (a / n) * (b / n)) / std::sqrt(va * vb) : 0;
}

}  // namespace

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -2 * kPi / (double)len;
        std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; ++j) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void LiveAnalyzer::Block::add(const Block& o) {
    kw += o.kw * o.n; sumSq += o.sumSq; n += o.n;
    peak = std::max(peak, o.peak); truePeak = std::max(truePeak, o.truePeak);
    l += o.l; r += o.r; ll += o.ll; rr += o.rr; lr += o.lr;
    bl += o.bl; br += o.br; bll += o.bll; brr += o.brr; blr += o.blr;
    mid2 += o.mid2; side2 += o.side2;
}

LiveAnalyzer::LiveAnalyzer() { prepare(44100); }

void LiveAnalyzer::prepare(double sampleRate) {
    std::lock_guard<std::mutex> g(workMutex);
    fs = sampleRate > 0 ? sampleRate : 44100;
    blockLen = std::max(1, (int)std::lround(0.1 * fs));

    ringFrames = 1;
    while (ringFrames < (int)(fs * 8)) ringFrames <<= 1;
    ring.assign((size_t)ringFrames * 2, 0.f);

    kShelf = highShelf(fs, 4.0, 1.0 / std::sqrt(2.0), 1500.0);
    kHighPass = highPass(fs, 0.5, 38.0);
    // 4th-order Butterworth at 120 Hz as two biquads (scipy's butter + sosfilt).
    low1 = lowPass(fs, 0.54119610, 120.0);
    low2 = lowPass(fs, 1.30656296, 120.0);

    // 4x oversampling for true peak: Kaiser-windowed sinc, like resample_poly.
    tpLen = 12;
    int taps = tpLen * kTpPhases;
    tpTaps.assign((size_t)taps, 0);
    double centre = (taps - 1) / 2.0, beta = 5.0;
    for (int i = 0; i < taps; ++i) {
        double x = (i - centre) / kTpPhases;
        double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        double t = (i - centre) / centre;
        tpTaps[(size_t)i] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - t * t))) / besselI0(beta);
    }

    fftN = fs <= 50000 ? 16384 : fs <= 100000 ? 32768 : 65536;
    window.resize((size_t)fftN);
    for (int i = 0; i < fftN; ++i) window[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * i / fftN);  // scipy "hann" (periodic)
    fftBuf.resize((size_t)fftN);
    recentDecay = std::exp(-(fftN / 2.0) / fs / 10.0);

    resetState();
}

void LiveAnalyzer::resetState() {
    readPos.store(writePos.load());
    kShelf.clear(); kHighPass.clear(); low1.clear(); low2.clear();
    for (auto& h : tpHist) h.assign((size_t)tpLen, 0);
    tpPos = 0;
    cur = Block(); curCount = 0;
    total = Block();
    recentBlocks.assign(kRecentBlocks, Block());
    recentPos = 0;
    kwRing.assign(30, 0); kwCount = 0;
    momentary.clear(); shortTerm.clear();
    tpSecond.assign(10, 0); tpSecondPos = 0;
    blocksSeen = 0;
    fftMid.assign((size_t)fftN, 0); fftSide.assign((size_t)fftN, 0); fftFill = 0;
    size_t bins = (size_t)fftN / 2 + 1;
    psdMidTotal.assign(bins, 0); psdSideTotal.assign(bins, 0);
    psdMidRecent.assign(bins, 0); psdSideRecent.assign(bins, 0);
    fftFrames = 0;
}

void LiveAnalyzer::push(const float* left, const float* right, int numSamples) {
    if (ringFrames == 0) return;
    int w = writePos.load(std::memory_order_relaxed);
    int r = readPos.load(std::memory_order_acquire);
    int free = ringFrames - 1 - ((w - r) & (ringFrames - 1));
    int n = std::min(numSamples, free);  // the worker fell behind: drop the rest
    for (int i = 0; i < n; ++i) {
        size_t at = (size_t)((w + i) & (ringFrames - 1)) * 2;
        ring[at] = left[i];
        ring[at + 1] = right[i];
    }
    writePos.store((w + n) & (ringFrames - 1), std::memory_order_release);
}

bool LiveAnalyzer::process() {
    bool any = false;
    {
        std::lock_guard<std::mutex> g(workMutex);
        if (resetRequested.exchange(false)) resetState();
        int r = readPos.load(std::memory_order_relaxed);
        int w = writePos.load(std::memory_order_acquire);
        while (r != w) {
            size_t at = (size_t)r * 2;
            consume(ring[at], ring[at + 1]);
            r = (r + 1) & (ringFrames - 1);
            any = true;
        }
        readPos.store(r, std::memory_order_release);
        if (!any) return false;
        Snapshot whole = build(false), recent = build(true);
        std::lock_guard<std::mutex> s(snapMutex);
        snapWhole = std::move(whole);
        snapRecent = std::move(recent);
    }
    return true;
}

Snapshot LiveAnalyzer::snapshot(bool recent) const {
    std::lock_guard<std::mutex> s(snapMutex);
    return recent ? snapRecent : snapWhole;
}

void LiveAnalyzer::consume(double l, double r) {
    double kl = kHighPass.run(kShelf.run(l, 0), 0);
    double kr = kHighPass.run(kShelf.run(r, 1), 1);
    cur.kw += kl * kl + kr * kr;
    cur.sumSq += l * l + r * r;
    cur.peak = std::max(cur.peak, std::max(std::abs(l), std::abs(r)));

    double in[2] = {l, r};
    for (int ch = 0; ch < 2; ++ch) {
        auto& h = tpHist[ch];
        h[(size_t)tpPos] = in[ch];
        for (int p = 0; p < kTpPhases; ++p) {
            double y = 0;
            for (int k = 0; k < tpLen; ++k)
                y += tpTaps[(size_t)(p + kTpPhases * k)] * h[(size_t)((tpPos - k + tpLen) % tpLen)];
            cur.truePeak = std::max(cur.truePeak, std::abs(y));
        }
    }
    tpPos = (tpPos + 1) % tpLen;

    cur.l += l; cur.r += r; cur.ll += l * l; cur.rr += r * r; cur.lr += l * r;
    double bl = low2.run(low1.run(l, 0), 0), br = low2.run(low1.run(r, 1), 1);
    cur.bl += bl; cur.br += br; cur.bll += bl * bl; cur.brr += br * br; cur.blr += bl * br;

    double m = (l + r) / 2, s = (l - r) / 2;
    cur.mid2 += m * m;
    cur.side2 += s * s;
    fftMid[(size_t)fftFill] = m;
    fftSide[(size_t)fftFill] = s;
    if (++fftFill == fftN) runFft();

    if (++curCount == blockLen) endBlock();
}

void LiveAnalyzer::endBlock() {
    cur.n = blockLen;
    cur.kw /= blockLen;
    kwRing[(size_t)(blocksSeen % 30)] = cur.kw;
    kwCount = std::min(kwCount + 1, 30);
    ++blocksSeen;

    auto meanLast = [&](int count) {
        double sum = 0;
        int c = std::min(count, kwCount);
        for (int i = 1; i <= c; ++i) sum += kwRing[(size_t)((blocksSeen - i) % 30)];
        return c ? sum / c : 0.0;
    };
    double m400 = meanLast(4);
    // Integrated loudness: 400 ms windows every 100 ms (75 % overlap).
    if (blocksSeen >= 4 && lufs(m400) > kSilenceLufs) momentary.push_back(m400);
    // Short-term curve: 3 s windows every 1 s, as in the file analyzer.
    if (blocksSeen >= 30 && (blocksSeen - 30) % 10 == 0) {
        double st = lufs(meanLast(30));
        if (st > kSilenceLufs) shortTerm.push_back(st);
    }

    cur.loud = lufs(m400) > kSilenceLufs;
    tpSecond[(size_t)tpSecondPos] = cur.truePeak;
    tpSecondPos = (tpSecondPos + 1) % 10;
    if (cur.loud) {
        total.add(cur);
        recentBlocks[(size_t)recentPos] = cur;
        recentPos = (recentPos + 1) % kRecentBlocks;
    }
    momentaryNow = lufs(m400);
    shortTermNow = lufs(meanLast(30));
    cur = Block();
    curCount = 0;
}

void LiveAnalyzer::runFft() {
    double energy = 0;
    for (int i = 0; i < fftN; ++i) {
        double m = fftMid[(size_t)i] * window[(size_t)i], s = fftSide[(size_t)i] * window[(size_t)i];
        fftBuf[(size_t)i] = {m, s};
        energy += m * m + s * s;
    }
    if (energy > 1e-10 * fftN) {
        fft(fftBuf);
        // Two real spectra from one complex FFT.
        for (int k = 0; k <= fftN / 2; ++k) {
            auto x = fftBuf[(size_t)k], y = std::conj(fftBuf[(size_t)((fftN - k) % fftN)]);
            auto mk = (x + y) * 0.5;
            auto sk = (x - y) * std::complex<double>(0, -0.5);
            double pm = std::norm(mk), ps = std::norm(sk);
            psdMidTotal[(size_t)k] += pm;
            psdSideTotal[(size_t)k] += ps;
            psdMidRecent[(size_t)k] = psdMidRecent[(size_t)k] * recentDecay + pm;
            psdSideRecent[(size_t)k] = psdSideRecent[(size_t)k] * recentDecay + ps;
        }
        fftFrames += 1;
    }
    int half = fftN / 2;
    std::copy(fftMid.begin() + half, fftMid.end(), fftMid.begin());
    std::copy(fftSide.begin() + half, fftSide.end(), fftSide.begin());
    fftFill = half;
}

void LiveAnalyzer::fillTonal(Snapshot& s, const std::vector<double>& pm,
                             const std::vector<double>& ps) const {
    const double binHz = fs / fftN;
    const size_t bins = pm.size();
    auto bandSum = [&](const std::vector<double>& p, double lo, double hi) {
        size_t a = (size_t)std::ceil(lo / binHz), b = (size_t)std::ceil(hi / binHz);
        double sum = 0;
        for (size_t k = a; k < std::min(b, bins); ++k) sum += p[k];
        return sum;
    };
    double totalMid = bandSum(pm, 20, 20000);
    if (totalMid <= 0) return;

    double bandDb[kNumBands];
    for (int b = 0; b < kNumBands; ++b) {
        bandDb[b] = db10(bandSum(pm, kBandEdges[b][0], kBandEdges[b][1]) / totalMid);
        s.values[std::string("tonal.bands_db.") + kBandIds[b]] = bandDb[b];
    }
    s.values["low_end.sub_to_bass_db"] = bandDb[0] - bandDb[1];

    // 1/3-octave curve and the tilt of the spectral density, 31.5 Hz - 16 kHz.
    s.thirdOctaveDb.assign(kThirdOctaveHz.size(), NAN);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int np = 0;
    for (size_t i = 0; i < kThirdOctaveHz.size(); ++i) {
        double fc = kThirdOctaveHz[i];
        double lo = fc * std::pow(2.0, -1.0 / 6), hi = std::min(fc * std::pow(2.0, 1.0 / 6), kAnalysisNyquist);
        s.thirdOctaveDb[i] = db10(bandSum(pm, lo, hi) / totalMid);
        size_t a = (size_t)std::ceil(lo / binHz), b = std::min((size_t)std::ceil(hi / binHz), bins);
        if (b > a && fc >= 31.5 && fc <= 16000) {
            double x = std::log2(fc), y = db10(bandSum(pm, lo, hi) / (double)(b - a));
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++np;
        }
    }
    if (np >= 2) s.values["tonal.tilt_db_per_oct"] = (np * sxy - sx * sy) / (np * sxx - sx * sx);

    double num = 0, den = 0;
    for (size_t k = (size_t)std::ceil(20 / binHz); k < bins && k * binHz <= 20000; ++k) {
        num += k * binHz * pm[k];
        den += pm[k];
    }
    if (den > 0) s.values["tonal.centroid_hz"] = num / den;

    if (s.stereo) {
        for (int b = 0; b < kNumBands; ++b) {
            double m = bandSum(pm, kBandEdges[b][0], kBandEdges[b][1]);
            if (m > 0)
                s.values[std::string("stereo.width_by_band.") + kBandIds[b]] =
                    bandSum(ps, kBandEdges[b][0], kBandEdges[b][1]) / m;
        }
        double mLow = bandSum(pm, 20, 120);
        if (mLow > 0) s.values["low_end.low_width"] = bandSum(ps, 20, 120) / mLow;
    }
}

Snapshot LiveAnalyzer::build(bool recent) const {
    Snapshot s;
    s.recent = recent;
    s.momentaryLufs = momentaryNow;
    s.shortTermLufs = shortTermNow;
    double tpNow = 0;
    for (double v : tpSecond) tpNow = std::max(tpNow, v);
    s.truePeakNow = 20 * std::log10(std::max(tpNow, 1e-6));

    Block a;
    if (recent) {
        for (const auto& b : recentBlocks)
            if (b.n > 0) a.add(b);
    } else {
        a = total;
    }
    s.seconds = a.n / fs;
    if (a.n < fs * 1.0) return s;  // need a second of sound
    a.kw /= a.n;

    size_t nm = recent ? std::min<size_t>(momentary.size(), kRecentBlocks) : momentary.size();
    double integrated = gatedLoudness(momentary.data() + momentary.size() - nm, nm);
    if (!std::isnan(integrated)) s.values["loudness.integrated_lufs"] = integrated;

    size_t ns = recent ? std::min<size_t>(shortTerm.size(), 10) : shortTerm.size();
    if (ns > 0) {
        std::vector<double> st(shortTerm.end() - (long)ns, shortTerm.end());
        s.values["loudness.max_short_term_lufs"] = *std::max_element(st.begin(), st.end());
        if (!recent && st.size() >= 10) {
            s.values["dynamics.section_contrast_lu"] = percentile(st, 95) - percentile(st, 10);
            // EBU Tech 3342 loudness range.
            double mean = 0;
            for (double v : st) mean += std::pow(10.0, v / 10);
            double gate = 10 * std::log10(mean / st.size()) - 20;
            std::vector<double> kept;
            for (double v : st)
                if (v > gate) kept.push_back(v);
            if (kept.size() >= 2) s.values["dynamics.lra_lu"] = percentile(kept, 95) - percentile(kept, 10);
        }
    }

    double tp = 20 * std::log10(std::max(std::max(a.truePeak, a.peak), 1e-12));
    s.values["loudness.true_peak_dbtp"] = tp;
    double peakDb = 20 * std::log10(std::max(a.peak, 1e-12));
    s.values["dynamics.crest_db"] = peakDb - 10 * std::log10(std::max(a.sumSq / (2 * a.n), 1e-24));
    if (!std::isnan(integrated)) s.values["dynamics.plr_db"] = tp - integrated;

    s.stereo = a.mid2 > 0 && a.side2 / a.mid2 > 1e-6;
    if (s.stereo) {
        s.values["stereo.width"] = a.side2 / a.mid2;
        bool ok;
        double c = corr(a.n, a.l, a.r, a.ll, a.rr, a.lr, ok);
        if (ok) s.values["stereo.correlation"] = c;
        c = corr(a.n, a.bl, a.br, a.bll, a.brr, a.blr, ok);
        if (ok) s.values["stereo.low_correlation"] = c;
        s.values["stereo.balance_db"] = db10(a.ll / a.n) - db10(a.rr / a.n);
    }

    if (fftFrames > 0) fillTonal(s, recent ? psdMidRecent : psdMidTotal, recent ? psdSideRecent : psdSideTotal);
    return s;
}

}  // namespace mix
