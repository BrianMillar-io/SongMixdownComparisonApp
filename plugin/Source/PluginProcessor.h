#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "LiveAnalyzer.h"
#include "ProfileStore.h"

// Passes audio through untouched and measures it on a background thread.
class AnalyzerProcessor : public juce::AudioProcessor {
public:
    AnalyzerProcessor();
    ~AnalyzerProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Analyzer App"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override;
    void setStateInformation(const void* data, int size) override;

    // Message thread only.
    void reloadProfiles();
    const std::vector<mix::Profile>& profiles() const { return loaded.profiles; }
    const juce::StringArray& profileProblems() const { return loaded.problems; }
    juce::File profilesFolder() const;
    void setProfilesFolder(const juce::File& f);

    mix::LiveAnalyzer analyzer;

    // Settings kept with the Logic project.
    juce::String genreId = "auto";      // a profile id, or "auto" for the closest
    bool recentMode = true;             // last ~10 s, or everything since play / reset
    bool resetOnPlay = true;

private:
    struct Worker : juce::Thread {
        explicit Worker(AnalyzerProcessor& p) : juce::Thread("Analyzer"), owner(p) {}
        void run() override {
            while (!threadShouldExit()) {
                owner.analyzer.process();
                wait(50);
            }
        }
        AnalyzerProcessor& owner;
    };

    Worker worker{*this};
    mix::LoadResult loaded;
    juce::String customFolder;
    bool wasPlaying = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyzerProcessor)
};
