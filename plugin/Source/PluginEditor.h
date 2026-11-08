#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MixScore.h"
#include "PluginProcessor.h"

class AnalyzerEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit AnalyzerEditor(AnalyzerProcessor&);
    ~AnalyzerEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Read the latest measurements now (the screenshot tool uses this).
    void refreshNow() { timerCallback(); }

private:
    void timerCallback() override;
    void refreshGenreList();
    void showMenu();
    const mix::Profile* chosenProfile();

    void paintScore(juce::Graphics&, juce::Rectangle<int>);
    void paintTone(juce::Graphics&, juce::Rectangle<int>);
    void paintMeters(juce::Graphics&, juce::Rectangle<int>);
    void paintSteps(juce::Graphics&, juce::Rectangle<int>);
    void paintMessage(juce::Graphics&, juce::Rectangle<int>, const juce::String& title, const juce::String& body);

    AnalyzerProcessor& proc;

    juce::ComboBox genreBox;
    juce::TextButton recentButton{"Last 10 seconds"}, wholeButton{"Since play"};
    juce::TextButton resetButton{"Reset"}, menuButton{"More"};

    std::unique_ptr<juce::FileChooser> chooser;
    mix::Snapshot snap;
    mix::Report report;
    std::string autoGenreId;
    std::vector<std::string> shownSteps, shownNear;
    int ticks = 0;

    juce::Rectangle<int> scoreArea, toneArea, meterArea, stepsArea, bodyArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalyzerEditor)
};
