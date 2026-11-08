// AnalyzerScreenshot <audio.wav> <profiles folder> <out.png> [whole]
// Plays the song through the plugin and saves a picture of its window.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(argv[1])));
    if (!reader) return 1;

    std::unique_ptr<AnalyzerProcessor> proc(static_cast<AnalyzerProcessor*>(createPluginFilter()));
    proc->setProfilesFolder(juce::File(argv[2]));
    proc->recentMode = !(argc > 4 && juce::String(argv[4]) == "whole");
    proc->prepareToPlay(reader->sampleRate, 512);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer midi;
    auto stop = std::min<juce::int64>(reader->lengthInSamples, (juce::int64)(reader->sampleRate * 120));
    for (juce::int64 pos = 0; pos < stop; pos += 512) {
        reader->read(&buf, 0, 512, pos, true, true);
        proc->processBlock(buf, midi);
        if ((pos / 512) % 32 == 0) juce::Thread::sleep(1);
    }
    juce::Thread::sleep(1500);
    std::unique_ptr<AnalyzerEditor> ed(static_cast<AnalyzerEditor*>(proc->createEditor()));
    ed->refreshNow();
    auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
    juce::File out(argv[3]);
    out.deleteFile();
    juce::FileOutputStream os(out);
    juce::PNGImageFormat().writeImageToStream(img, os);
    return 0;
}
