// Reads the genre profiles the Analyzer App builds (~/AnalyzerApp/profiles/*.json).
#pragma once

#include <juce_core/juce_core.h>

#include "MixScore.h"

namespace mix {

constexpr int kProfileSchemaVersion = 1;

juce::File defaultProfilesFolder();

// nullopt-style: returns false and sets error if the file isn't a usable profile.
bool parseProfile(const juce::String& json, Profile& out, juce::String& error);

struct LoadResult {
    std::vector<Profile> profiles;
    juce::StringArray problems;  // one line per file that couldn't be read
};

LoadResult loadProfiles(const juce::File& folder);

}  // namespace mix
