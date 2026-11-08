#include "MixScore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mix {

namespace {

// Share of the overall score per area (compare.AREA_WEIGHTS).
const std::map<std::string, double> kAreaWeights = {
    {"tone", 0.30}, {"loudness", 0.20}, {"dynamics", 0.20}, {"low_end", 0.15}, {"stereo", 0.15}};
constexpr double kFalloff = 1.25;
constexpr double kMinImpact = 0.5;

struct Side {
    const char* kind;
    std::string text;
};

struct Metric {
    std::string key, area, label, unit;
    double weight;
    Side low, high;
    int decimals;
};

const char* kBandHz[kNumBands] = {"20-60 Hz",  "60-120 Hz", "120-250 Hz", "250-500 Hz", "500 Hz-1 kHz",
                                  "1-2 kHz",   "2-4 kHz",   "4-8 kHz",    "8-12 kHz",   "12-20 kHz"};
const char* kBandNames[kNumBands] = {"sub",  "bass",       "low mids", "lower mids", "mids",
                                     "upper mids", "presence", "highs",    "air",        "top end"};

std::vector<Metric> makeMetrics() {
    std::vector<Metric> m = {
        {"loudness.integrated_lufs", "loudness", "Loudness", "LUFS", 1.0,
         {"adjust", "Quieter than the genre. Raise the limiter's gain by about {gap}."},
         {"adjust", "Louder than the genre. Back the limiter off by about {gap} to get punch back."}, 1},
        {"loudness.max_short_term_lufs", "loudness", "Loudest section", "LUFS", 0.4,
         {"missing", "The loudest part doesn't get as loud as the genre's drops."},
         {"adjust", "The loudest part is pushed harder than the genre's drops."}, 1},
        {"dynamics.crest_db", "dynamics", "Punch (crest factor)", "dB", 1.0,
         {"adjust", "Squashed compared with the genre. Use less limiting or bus compression so the kick and snare punch through."},
         {"adjust", "Peaks stick out more than in the genre. A little more bus compression or limiting will glue it."}, 1},
        {"dynamics.plr_db", "dynamics", "Peak to loudness", "dB", 0.5,
         {"adjust", "Little headroom between peaks and loudness, which points to heavy limiting."},
         {"adjust", "Lots of room between peaks and loudness. The master could take more limiting."}, 1},
        {"dynamics.lra_lu", "dynamics", "Loudness range", "LU", 0.5,
         {"missing", "The track stays at one level more than the genre does. Breakdowns could drop further."},
         {"adjust", "Level swings more than the genre. Even out quiet and loud parts."}, 1},
        {"dynamics.section_contrast_lu", "dynamics", "Drop impact", "LU", 0.8,
         {"missing", "The drop doesn't lift above the breakdown as much as in the genre. Thin out the breakdown or add energy to the drop."},
         {"adjust", "The gap between breakdown and drop is bigger than the genre's. The quiet parts may lose the floor."}, 1},
        {"tonal.tilt_db_per_oct", "tone", "Brightness (tilt)", "dB/oct", 1.0,
         {"adjust", "Darker overall than the genre. A gentle high shelf from about 3 kHz brings it in line."},
         {"adjust", "Brighter overall than the genre. Ease the top end with a gentle high shelf."}, 2},
        {"tonal.centroid_hz", "tone", "Brightness (centroid)", "Hz", 0.4,
         {"adjust", "The balance sits lower than the genre's."},
         {"adjust", "The balance sits higher than the genre's."}, 0},
        {"stereo.width", "stereo", "Stereo width", "", 1.0,
         {"missing", "Narrower than the genre. Widen pads, synths and effects, not the low end."},
         {"adjust", "Wider than the genre. Check how it sounds in mono."}, 2},
        {"stereo.correlation", "stereo", "Mono compatibility", "", 0.6,
         {"adjust", "Left and right are less alike than in the genre. Check wideners and chorus in mono."},
         {"adjust", "Left and right are more alike than in the genre, so it may sound flat."}, 2},
        {"low_end.sub_to_bass_db", "low_end", "Sub vs bass", "dB", 1.0,
         {"missing", "Less sub (20-60 Hz) under the bass than the genre. Bring up the sub or the kick's tail."},
         {"adjust", "The sub outweighs the 60-120 Hz punch. Lift the kick and bass body around 80-100 Hz."}, 1},
        {"low_end.low_width", "low_end", "Bass width", "", 0.6,
         {"adjust", "The low end is more mono than the genre."},
         {"adjust", "The low end is wider than the genre. Keep everything below 120 Hz in mono."}, 3},
        {"stereo.low_correlation", "low_end", "Bass phase", "", 0.6,
         {"adjust", "Left and right disagree more below 120 Hz than in the genre. Mono the bass."},
         {"adjust", "The bass is more mono than the genre."}, 2},
    };
    for (int b = 0; b < kNumBands; ++b) {
        std::string hz = kBandHz[b], name = kBandNames[b];
        m.push_back({std::string("tonal.bands_db.") + kBandIds[b], "tone", hz + " (" + name + ")", "dB", 0.6,
                     {"missing", "Less " + name + " than the genre. Boost " + hz + " by about {gap}."},
                     {"adjust", "Too much " + name + " for the genre. Cut " + hz + " by about {gap}."}, 1});
    }
    for (int b = 4; b <= 8; ++b) {
        std::string hz = kBandHz[b];
        m.push_back({std::string("stereo.width_by_band.") + kBandIds[b], "stereo", "Width at " + hz, "", 0.25,
                     {"missing", "Narrower than the genre at " + hz + "."},
                     {"adjust", "Wider than the genre at " + hz + "."}, 2});
    }
    return m;
}

const std::vector<Metric>& metrics() {
    static const std::vector<Metric> m = makeMetrics();
    return m;
}

const std::vector<std::string> kMatchMetrics = {
    "loudness.integrated_lufs", "tonal.tilt_db_per_oct", "low_end.sub_to_bass_db", "stereo.width",
    "dynamics.crest_db",        "tonal.bands_db.sub",    "tonal.bands_db.bass",    "tonal.bands_db.low_mid",
    "tonal.bands_db.mid_low",   "tonal.bands_db.mid",    "tonal.bands_db.upper_mid", "tonal.bands_db.presence",
    "tonal.bands_db.high",      "tonal.bands_db.air",    "tonal.bands_db.top"};

double halfRange(const Stat& s) { return std::max((s.p90 - s.p10) / 2, 1e-6); }

double metricScore(double v, const Stat& s, double& outside) {
    double half = halfRange(s), d = (v - s.median) / half;
    outside = 0;
    if (v >= s.p10 && v <= s.p90) return 100 - 10 * std::pow(std::min(std::abs(d), 1.0), 2);
    outside = (v > s.p90 ? v - s.p90 : v - s.p10) / half;
    return 90 * std::exp(-0.5 * std::pow(outside / kFalloff, 2));
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string gapText(double gap, const std::string& unit) {
    double g = std::round(std::abs(gap) * 2) / 2;
    std::string s = fmt("%g", g);
    return unit.empty() ? s : s + " " + unit;
}

std::string replaceGap(std::string text, const std::string& gap) {
    auto at = text.find("{gap}");
    if (at != std::string::npos) text.replace(at, 5, gap);
    return text;
}

struct RawFinding {
    Finding f;
    double score, weight;
};

std::vector<Finding> technicalChecks(const Snapshot& s) {
    std::vector<Finding> out;
    auto add = [&](const char* metric, const char* label, std::string text, double penalty) {
        Finding f;
        f.kind = "problem"; f.metric = metric; f.label = label; f.text = std::move(text); f.impact = penalty;
        out.push_back(f);
    };
    if (s.has("loudness.true_peak_dbtp")) {
        double tp = s.get("loudness.true_peak_dbtp");
        if (tp > 0.0)
            add("loudness.true_peak_dbtp", "Clipping",
                "True peak is " + fmt("%+.1f", tp) + " dBTP, so it clips. Set the limiter's ceiling to -1 dBTP.", 8);
        else if (tp > -1.0)
            add("loudness.true_peak_dbtp", "True peak",
                "True peak is " + fmt("%.1f", tp) +
                    " dBTP. Streaming services ask for -1 dBTP or lower, or the encode can distort. "
                    "Lower the limiter's ceiling.", 3);
    }
    if (s.stereo) {
        if (s.has("stereo.correlation") && s.get("stereo.correlation") < 0)
            add("stereo.correlation", "Phase",
                "Left and right are out of phase overall. Parts will vanish in mono. "
                "Look for a polarity flip or an extreme widener.", 10);
        if (s.has("stereo.low_correlation") && s.get("stereo.low_correlation") < 0.5)
            add("stereo.low_correlation", "Bass phase",
                "The bass is out of phase between left and right (correlation " +
                    fmt("%.2f", s.get("stereo.low_correlation")) +
                    "). It will thin out on club systems. Mono everything below 120 Hz.", 6);
        if (s.has("stereo.balance_db") && std::abs(s.get("stereo.balance_db")) > 1.5) {
            double bal = s.get("stereo.balance_db");
            add("stereo.balance_db", "Balance",
                "The mix leans " + fmt("%.1f", std::abs(bal)) + " dB to the " + (bal > 0 ? "left" : "right") +
                    ". Check panning.", 3);
        }
    }
    return out;
}

std::vector<Finding> mergeBandWidth(std::vector<Finding> findings) {
    std::vector<Finding> out;
    std::map<std::string, std::vector<Finding>> groups;
    for (auto& f : findings) {
        if (f.metric.rfind("stereo.width_by_band.", 0) == 0) groups[f.status].push_back(f);
        else out.push_back(f);
    }
    for (auto& [status, fs] : groups) {
        if (fs.size() == 1) { out.push_back(fs[0]); continue; }
        std::string where;
        double impact = 0;
        for (size_t i = 0; i < fs.size(); ++i) {
            std::string band = fs[i].metric.substr(fs[i].metric.rfind('.') + 1);
            std::string hz;
            for (int b = 0; b < kNumBands; ++b)
                if (band == kBandIds[b]) hz = kBandHz[b];
            where += (i == 0 ? "" : (i + 1 == fs.size() ? " and " : ", ")) + hz;
            impact += fs[i].impact;
        }
        Finding m = fs[0];
        m.metric = "stereo.width_by_band";
        m.label = "Width by band";
        m.text = std::string(status == "low" ? "Narrower" : "Wider") + " than the genre at " + where + ".";
        m.impact = std::round(impact * 10) / 10;
        out.push_back(m);
    }
    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.impact > b.impact; });
    return out;
}

}  // namespace

const std::vector<std::string> kAreaOrder = {"tone", "loudness", "dynamics", "low_end", "stereo"};

std::string areaLabel(const std::string& a) {
    if (a == "loudness") return "Loudness";
    if (a == "tone") return "Tonal balance";
    if (a == "dynamics") return "Dynamics";
    if (a == "stereo") return "Stereo image";
    if (a == "low_end") return "Low end";
    return a;
}

std::string verdictFor(int score) {
    if (score >= 85) return "Release ready";
    if (score >= 70) return "Almost there";
    if (score >= 50) return "Needs work";
    return "Early mix";
}

double distanceTo(const Snapshot& s, const Profile& p, int* compared) {
    double sum = 0;
    int n = 0;
    for (const auto& key : kMatchMetrics) {
        auto it = p.metrics.find(key);
        if (it == p.metrics.end() || !s.has(key)) continue;
        double d = (s.get(key) - it->second.median) / halfRange(it->second);
        sum += d * d;
        ++n;
    }
    if (compared) *compared = n;
    return n ? std::sqrt(sum / n) : INFINITY;
}

Report compare(const Snapshot& s, const Profile& p) {
    Report r;
    r.genreId = p.id;
    r.genreName = p.name;

    std::vector<RawFinding> raw;
    std::map<std::string, std::vector<std::pair<double, double>>> parts;
    for (const auto& m : metrics()) {
        auto it = p.metrics.find(m.key);
        if (it == p.metrics.end() || !s.has(m.key)) continue;
        const Stat& st = it->second;
        double v = s.get(m.key), outside;
        double score = metricScore(v, st, outside);
        std::string status = outside == 0 ? "ok" : (outside < 0 ? "low" : "high");
        MetricRow row{m.key, m.area, m.label, m.unit, m.decimals, v, st.median, st.p10, st.p90, score, status};
        r.metrics.push_back(row);
        parts[m.area].push_back({score, m.weight});
        if (status != "ok") {
            const Side& side = status == "low" ? m.low : m.high;
            Finding f;
            f.kind = side.kind; f.metric = m.key; f.area = m.area; f.label = m.label; f.status = status;
            f.text = replaceGap(side.text, gapText(v - st.median, m.unit));
            raw.push_back({f, score, m.weight});
        }
    }

    std::map<std::string, double> areas, areaW;
    for (auto& [a, ps] : parts) {
        double tw = 0, sum = 0;
        for (auto& [sc, w] : ps) { tw += w; sum += sc * w; }
        if (tw > 0) { areas[a] = sum / tw; areaW[a] = tw; }
    }
    if (areas.empty()) {
        r.error = "Play some of the song so there's something to compare.";
        return r;
    }

    r.problems = technicalChecks(s);
    for (const auto& pr : r.problems) {
        std::string area = "loudness";
        for (const auto& m : metrics())
            if (m.key == pr.metric) area = m.area;
        double cur = areas.count(area) ? areas[area] : 100.0;
        areas[area] = std::max(0.0, cur - pr.impact * 2);
    }

    double weightSum = 0, overall = 0;
    for (auto& [a, v] : areas) {
        weightSum += kAreaWeights.at(a);
        overall += v * kAreaWeights.at(a);
    }
    overall /= weightSum;

    std::vector<Finding> findings;
    for (auto& rf : raw) {
        double w = areaW.count(rf.f.area) ? areaW[rf.f.area] : 1.0;
        rf.f.impact = std::round((100 - rf.score) * rf.weight / w * kAreaWeights.at(rf.f.area) / weightSum * 10) / 10;
        findings.push_back(rf.f);
    }
    std::vector<Finding> kept;
    for (auto& f : mergeBandWidth(findings))
        if (f.impact >= kMinImpact) kept.push_back(f);
    auto byImpact = [](const Finding& a, const Finding& b) { return a.impact > b.impact; };
    std::stable_sort(kept.begin(), kept.end(), byImpact);
    std::stable_sort(r.problems.begin(), r.problems.end(), byImpact);

    for (auto& f : r.problems) if (r.nextSteps.size() < 3) r.nextSteps.push_back(f.text);
    for (auto& f : kept) if (r.nextSteps.size() < 3) r.nextSteps.push_back(f.text);
    for (auto& f : kept) (f.kind == "missing" ? r.missing : r.adjust).push_back(f);

    r.overall = (int)std::lround(overall);
    r.verdict = verdictFor(r.overall);
    for (auto& [a, v] : areas) r.areas[a] = (int)std::lround(v);

    // The reference songs it sounds closest to (profiles.nearest_tracks).
    std::vector<std::pair<double, std::string>> near;
    for (const auto& t : p.tracks) {
        double sum = 0;
        int n = 0;
        for (const auto& [k, v] : t.match) {
            auto it = p.metrics.find(k);
            if (it == p.metrics.end() || !s.has(k)) continue;
            double d = (s.get(k) - v) / halfRange(it->second);
            sum += d * d;
            ++n;
        }
        if (n) near.push_back({std::sqrt(sum / n), t.label});
    }
    std::sort(near.begin(), near.end());
    for (size_t i = 0; i < near.size() && i < 3; ++i) r.soundsLike.push_back(near[i].second);

    r.valid = true;
    return r;
}

}  // namespace mix
