#include "PluginProcessor.h"

#include "PluginEditor.h"

AnalyzerProcessor::AnalyzerProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    reloadProfiles();
    worker.startThread(juce::Thread::Priority::low);
}

AnalyzerProcessor::~AnalyzerProcessor() { worker.stopThread(2000); }

bool AnalyzerProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
}

void AnalyzerProcessor::prepareToPlay(double sampleRate, int) { analyzer.prepare(sampleRate); }

void AnalyzerProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), ch = getTotalNumInputChannels();
    if (n == 0 || ch == 0) return;

    if (auto* head = getPlayHead()) {
        if (auto pos = head->getPosition()) {
            bool playing = pos->getIsPlaying();
            if (playing && !wasPlaying && resetOnPlay && !recentMode) analyzer.requestReset();
            wasPlaying = playing;
        }
    }
    const float* l = buffer.getReadPointer(0);
    const float* r = ch > 1 ? buffer.getReadPointer(1) : l;
    analyzer.push(l, r, n);
    // Audio is passed through as it came in.
}

juce::File AnalyzerProcessor::profilesFolder() const {
    return customFolder.isNotEmpty() ? juce::File(customFolder) : mix::defaultProfilesFolder();
}

void AnalyzerProcessor::setProfilesFolder(const juce::File& f) {
    customFolder = f == mix::defaultProfilesFolder() ? juce::String() : f.getFullPathName();
    reloadProfiles();
}

void AnalyzerProcessor::reloadProfiles() { loaded = mix::loadProfiles(profilesFolder()); }

void AnalyzerProcessor::getStateInformation(juce::MemoryBlock& dest) {
    juce::ValueTree state("AnalyzerApp");
    state.setProperty("genre", genreId, nullptr);
    state.setProperty("recent", recentMode, nullptr);
    state.setProperty("resetOnPlay", resetOnPlay, nullptr);
    state.setProperty("folder", customFolder, nullptr);
    juce::MemoryOutputStream out(dest, false);
    state.writeToStream(out);
}

void AnalyzerProcessor::setStateInformation(const void* data, int size) {
    auto state = juce::ValueTree::readFromData(data, (size_t)size);
    if (!state.hasType("AnalyzerApp")) return;
    genreId = state.getProperty("genre", "auto").toString();
    recentMode = (bool)state.getProperty("recent", true);
    resetOnPlay = (bool)state.getProperty("resetOnPlay", true);
    auto folder = state.getProperty("folder", "").toString();
    if (folder != customFolder) {
        customFolder = folder;
        reloadProfiles();
    }
}

juce::AudioProcessorEditor* AnalyzerProcessor::createEditor() { return new AnalyzerEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AnalyzerProcessor(); }
