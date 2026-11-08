// The score and advice from analyzer_app/compare.py, for a live Snapshot.
// Plain C++17 so the self test can check it against the Python version.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "LiveAnalyzer.h"

namespace mix {

struct Stat {
    double median = 0, p10 = 0, p90 = 0;
};

struct RefTrack {
    std::string label;
    std::map<std::string, double> match;
};

struct Profile {
    std::string id, name, confidence;
    int referenceCount = 0;
    std::map<std::string, Stat> metrics;
    std::vector<double> curveHz, curveMedian, curveLow, curveHigh;
    std::vector<RefTrack> tracks;
};

struct MetricRow {
    std::string key, area, label, unit;
    int decimals = 1;
    double value = 0, target = 0, low = 0, high = 0, score = 0;
    std::string status;  // ok, low, high
};

struct Finding {
    std::string kind;  // problem, missing, adjust
    std::string metric, area, label, status, text;
    double impact = 0;
};

struct Report {
    bool valid = false;
    std::string error;
    std::string genreId, genreName;
    int overall = 0;
    std::string verdict;
    std::map<std::string, int> areas;
    std::vector<Finding> problems, missing, adjust;
    std::vector<std::string> nextSteps;
    std::vector<std::string> soundsLike;
    std::vector<MetricRow> metrics;
};

extern const std::vector<std::string> kAreaOrder;
std::string areaLabel(const std::string& area);
std::string verdictFor(int score);

// RMS distance from the profile in half-ranges (profiles.distance).
double distanceTo(const Snapshot& s, const Profile& p, int* compared = nullptr);

Report compare(const Snapshot& s, const Profile& p);

}  // namespace mix
