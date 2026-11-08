#include "ProfileStore.h"

namespace mix {

juce::File defaultProfilesFolder() {
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile("AnalyzerApp")
        .getChildFile("profiles");
}

static bool isNumber(const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64(); }

static std::vector<double> numbers(const juce::var& v) {
    std::vector<double> out;
    if (auto* a = v.getArray())
        for (auto& x : *a) out.push_back(isNumber(x) ? (double)x : NAN);
    return out;
}

bool parseProfile(const juce::String& json, Profile& out, juce::String& error) {
    juce::var root;
    auto result = juce::JSON::parse(json, root);
    if (result.failed() || !root.isObject()) {
        error = "not a JSON file";
        return false;
    }
    if ((int)root["schema_version"] != kProfileSchemaVersion) {
        error = "made by a different version of the app";
        return false;
    }
    auto info = root["profile"];
    out = Profile();
    out.id = info["id"].toString().toStdString();
    out.name = info["name"].toString().toStdString();
    out.confidence = root["quality"]["confidence"].toString().toStdString();
    out.referenceCount = (int)root["source"]["used_count"];

    if (auto* m = root["metrics"].getDynamicObject()) {
        for (auto& prop : m->getProperties()) {
            auto s = prop.value;
            if (!isNumber(s["median"]) || !isNumber(s["p10"]) || !isNumber(s["p90"])) continue;
            out.metrics[prop.name.toString().toStdString()] = {(double)s["median"], (double)s["p10"], (double)s["p90"]};
        }
    }
    auto curve = root["curves"]["third_octave_db"];
    out.curveHz = numbers(curve["freqs_hz"]);
    out.curveMedian = numbers(curve["median"]);
    out.curveLow = numbers(curve["p10"]);
    out.curveHigh = numbers(curve["p90"]);

    if (auto* tracks = root["source"]["tracks"].getArray()) {
        for (auto& t : *tracks) {
            RefTrack rt;
            rt.label = t["label"].toString().toStdString();
            if (auto* m = t["match"].getDynamicObject())
                for (auto& prop : m->getProperties())
                    if (isNumber(prop.value)) rt.match[prop.name.toString().toStdString()] = (double)prop.value;
            out.tracks.push_back(std::move(rt));
        }
    }
    if (out.id.empty() || out.metrics.empty()) {
        error = "has no genre numbers in it";
        return false;
    }
    if (out.name.empty()) out.name = out.id;
    return true;
}

LoadResult loadProfiles(const juce::File& folder) {
    LoadResult r;
    auto files = folder.findChildFiles(juce::File::findFiles, false, "*.json");
    files.sort();
    for (auto& f : files) {
        Profile p;
        juce::String error;
        if (parseProfile(f.loadFileAsString(), p, error)) r.profiles.push_back(std::move(p));
        else r.problems.add(f.getFileName() + ": " + error);
    }
    std::sort(r.profiles.begin(), r.profiles.end(),
              [](const Profile& a, const Profile& b) { return a.name < b.name; });
    return r;
}

}  // namespace mix
