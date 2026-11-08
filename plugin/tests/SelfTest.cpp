// AnalyzerSelfTest <audio.wav> [profile.json]
//
// Plays the file through the live analyzer in 512-sample buffers, as a DAW
// would, and prints the measurements (and the report, given a profile) as
// JSON. tests/compare_with_python.py checks them against analyzer_app.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <iostream>

#include "LiveAnalyzer.h"
#include "MixScore.h"
#include "ProfileStore.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: AnalyzerSelfTest <audio.wav> [profile.json]\n";
        return 2;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(argv[1])));
    if (!reader) {
        std::cerr << "can't read " << argv[1] << "\n";
        return 1;
    }

    mix::LiveAnalyzer an;
    an.prepare(reader->sampleRate);
    const int block = 512;
    juce::AudioBuffer<float> buf(2, block);
    for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += block) {
        int n = (int)std::min<juce::int64>(block, reader->lengthInSamples - pos);
        reader->read(&buf, 0, n, pos, true, true);
        const float* r = reader->numChannels > 1 ? buf.getReadPointer(1) : buf.getReadPointer(0);
        an.push(buf.getReadPointer(0), r, n);
        if ((pos / block) % 64 == 0) an.process();  // the worker runs every ~0.7 s here
    }
    an.process();

    auto snap = an.snapshot(false);
    auto* out = new juce::DynamicObject();
    auto* values = new juce::DynamicObject();
    for (auto& [k, v] : snap.values) values->setProperty(juce::String(k), v);
    out->setProperty("values", juce::var(values));
    out->setProperty("seconds", snap.seconds);
    out->setProperty("stereo", snap.stereo);

    if (argc >= 3) {
        mix::Profile p;
        juce::String error;
        if (!mix::parseProfile(juce::File(argv[2]).loadFileAsString(), p, error)) {
            std::cerr << "profile: " << error << "\n";
            return 1;
        }
        auto rep = mix::compare(snap, p);
        auto* r = new juce::DynamicObject();
        r->setProperty("overall", rep.overall);
        r->setProperty("verdict", juce::String(rep.verdict));
        auto* areas = new juce::DynamicObject();
        for (auto& [a, v] : rep.areas) areas->setProperty(juce::String(a), v);
        r->setProperty("areas", juce::var(areas));
        juce::Array<juce::var> steps;
        for (auto& s : rep.nextSteps) steps.add(juce::String(s));
        r->setProperty("next_steps", steps);
        juce::Array<juce::var> near;
        for (auto& s : rep.soundsLike) near.add(juce::String(s));
        r->setProperty("sounds_like", near);
        r->setProperty("distance", mix::distanceTo(snap, p));
        out->setProperty("report", juce::var(r));
    }
    std::cout << juce::JSON::toString(juce::var(out)) << std::endl;
    return 0;
}
