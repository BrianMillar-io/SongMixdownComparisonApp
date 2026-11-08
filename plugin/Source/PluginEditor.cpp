#include "PluginEditor.h"

#include <cmath>

namespace {

namespace colours {
const juce::Colour bg{0xff14161b}, panel{0xff1d2027}, line{0xff2c313b}, text{0xffe9ebef},
    dim{0xff8b919d}, good{0xff3ecf8e}, warn{0xfff5a524}, bad{0xffef5350}, accent{0xff7b93ff};
}

juce::Font font(float size, bool bold = false) {
    return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Colour scoreColour(double score) {
    return score >= 85 ? colours::good : score >= 60 ? colours::warn : colours::bad;
}

juce::Colour statusColour(const mix::MetricRow& m) {
    if (m.status == "ok") return colours::good;
    return m.score >= 50 ? colours::warn : colours::bad;
}

juce::String number(double v, int decimals, bool sign = false) {
    juce::String s(v, decimals);
    if (sign && v > 0) s = "+" + s;
    return s;
}

const mix::MetricRow* findRow(const mix::Report& r, const std::string& key) {
    for (auto& m : r.metrics)
        if (m.key == key) return &m;
    return nullptr;
}

const char* kBandLabels[mix::kNumBands] = {"Sub", "Bass", "Low", "Lo-mid", "Mid",
                                           "Hi-mid", "Pres.", "High", "Air", "Top"};
// The middle of each band, short enough to fit under its column.
const char* kBandHz[mix::kNumBands] = {"40", "85", "175", "350", "700", "1.4k", "2.8k", "5.7k", "10k", "16k"};

}  // namespace

AnalyzerEditor::AnalyzerEditor(AnalyzerProcessor& p) : AudioProcessorEditor(&p), proc(p) {
    addAndMakeVisible(genreBox);
    genreBox.onChange = [this] {
        int id = genreBox.getSelectedId();
        if (id == 1) proc.genreId = "auto";
        else if (id >= 2 && id - 2 < (int)proc.profiles().size())
            proc.genreId = juce::String(proc.profiles()[(size_t)(id - 2)].id);
        repaint();
    };

    for (auto* b : {&recentButton, &wholeButton}) {
        addAndMakeVisible(*b);
        b->setClickingTogglesState(true);
        b->setRadioGroupId(1);
        b->setColour(juce::TextButton::buttonOnColourId, colours::accent.withAlpha(0.85f));
    }
    recentButton.setTooltip("Follow the last 10 seconds, so you hear each change you make");
    wholeButton.setTooltip("Everything since you pressed play, like checking the whole song");
    recentButton.setToggleState(proc.recentMode, juce::dontSendNotification);
    wholeButton.setToggleState(!proc.recentMode, juce::dontSendNotification);
    recentButton.onClick = [this] { proc.recentMode = recentButton.getToggleState(); };
    wholeButton.onClick = [this] { proc.recentMode = !wholeButton.getToggleState(); };

    addAndMakeVisible(resetButton);
    resetButton.onClick = [this] { proc.analyzer.requestReset(); };
    addAndMakeVisible(menuButton);
    menuButton.onClick = [this] { showMenu(); };

    for (auto* c : std::initializer_list<juce::Component*>{&genreBox, &recentButton, &wholeButton, &resetButton, &menuButton})
        c->setColour(juce::ComboBox::backgroundColourId, colours::panel);
    for (auto* b : {&recentButton, &wholeButton, &resetButton, &menuButton})
        b->setColour(juce::TextButton::buttonColourId, colours::panel);
    genreBox.setColour(juce::ComboBox::outlineColourId, colours::line);

    refreshGenreList();
    setResizable(true, true);
    setResizeLimits(820, 560, 1600, 1100);
    setSize(1040, 640);
    startTimerHz(10);
}

AnalyzerEditor::~AnalyzerEditor() { stopTimer(); }

void AnalyzerEditor::refreshGenreList() {
    genreBox.clear(juce::dontSendNotification);
    genreBox.addItem("Closest genre (auto)", 1);
    int selected = 1;
    for (size_t i = 0; i < proc.profiles().size(); ++i) {
        auto& p = proc.profiles()[i];
        genreBox.addItem(juce::String(p.name) + "  (" + juce::String(p.referenceCount) + " songs)", (int)i + 2);
        if (proc.genreId == juce::String(p.id)) selected = (int)i + 2;
    }
    genreBox.setSelectedId(selected, juce::dontSendNotification);
}

void AnalyzerEditor::showMenu() {
    juce::PopupMenu m;
    m.addItem(1, "Reload genres");
    m.addItem(2, "Choose the genre folder...");
    m.addItem(3, "Use the Analyzer App's genre folder", true, proc.profilesFolder() == mix::defaultProfilesFolder());
    m.addItem(4, "Show the genre folder in Finder");
    m.addSeparator();
    m.addItem(5, "Start over each time Logic starts playing", true, proc.resetOnPlay);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menuButton), [this](int r) {
        if (r == 1) { proc.reloadProfiles(); refreshGenreList(); }
        if (r == 2) {
            chooser = std::make_unique<juce::FileChooser>("Choose the folder with your genre profiles",
                                                          proc.profilesFolder());
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                 [this](const juce::FileChooser& fc) {
                                     auto f = fc.getResult();
                                     if (f.isDirectory()) { proc.setProfilesFolder(f); refreshGenreList(); }
                                 });
        }
        if (r == 3) { proc.setProfilesFolder(mix::defaultProfilesFolder()); refreshGenreList(); }
        if (r == 4) {
            auto f = proc.profilesFolder();
            f.createDirectory();
            f.revealToUser();
        }
        if (r == 5) proc.resetOnPlay = !proc.resetOnPlay;
        repaint();
    });
}

const mix::Profile* AnalyzerEditor::chosenProfile() {
    auto& all = proc.profiles();
    if (all.empty()) return nullptr;
    if (proc.genreId != "auto") {
        for (auto& p : all)
            if (juce::String(p.id) == proc.genreId) return &p;
    }
    // Closest genre, switching only when another is clearly closer.
    const mix::Profile* best = nullptr;
    const mix::Profile* current = nullptr;
    double bestD = INFINITY, currentD = INFINITY;
    for (auto& p : all) {
        double d = mix::distanceTo(snap, p);
        if (d < bestD) { bestD = d; best = &p; }
        if (p.id == autoGenreId) { current = &p; currentD = d; }
    }
    if (current && currentD <= bestD * 1.15) return current;
    if (best) autoGenreId = best->id;
    return best ? best : &all.front();
}

void AnalyzerEditor::timerCallback() {
    snap = proc.analyzer.snapshot(proc.recentMode);
    if (auto* p = chosenProfile()) report = mix::compare(snap, *p);
    else report = mix::Report();
    // The advice changes slowly, so it is easy to read while the song plays.
    if (ticks++ % 15 == 0 || shownSteps.empty()) {
        shownSteps = report.nextSteps;
        shownNear = report.soundsLike;
    }
    if (genreBox.getSelectedId() == 1 && report.valid)
        genreBox.setText("Closest: " + juce::String(report.genreName), juce::dontSendNotification);
    repaint();
}

void AnalyzerEditor::resized() {
    auto r = getLocalBounds().reduced(14);
    auto top = r.removeFromTop(30);
    menuButton.setBounds(top.removeFromRight(64));
    top.removeFromRight(8);
    resetButton.setBounds(top.removeFromRight(64));
    top.removeFromRight(16);
    wholeButton.setBounds(top.removeFromRight(100));
    recentButton.setBounds(top.removeFromRight(120));
    top.removeFromRight(16);
    top.removeFromLeft(150);  // title
    genreBox.setBounds(top.removeFromLeft(std::min(280, top.getWidth())));

    r.removeFromTop(12);
    bodyArea = r;
    auto left = r.removeFromLeft(230);
    scoreArea = left;
    r.removeFromLeft(12);
    stepsArea = r.removeFromBottom(150);
    r.removeFromBottom(12);
    meterArea = r.removeFromRight(std::max(290, r.getWidth() * 2 / 5 - 20));
    r.removeFromRight(12);
    toneArea = r;
}

void AnalyzerEditor::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    g.setColour(colours::text);
    g.setFont(font(17, true));
    g.drawText("Analyzer App", 14, 14, 150, 30, juce::Justification::centredLeft);

    if (proc.profiles().empty()) {
        juce::String body = "Add reference songs in the Analyzer App so it can build genre profiles, then click More > "
                            "Reload genres.\n\nLooking in: " + proc.profilesFolder().getFullPathName();
        if (!proc.profileProblems().isEmpty()) body += "\n\nCouldn't read: " + proc.profileProblems().joinIntoString(", ");
        paintMessage(g, bodyArea, "No genres yet", body);
        return;
    }
    if (!report.valid) {
        paintMessage(g, bodyArea, "Press play in Logic",
                     "Put this on the Stereo Out, after Ozone, and play your song. The score and advice appear "
                     "after a second or two of music.");
        return;
    }
    paintScore(g, scoreArea);
    paintTone(g, toneArea);
    paintMeters(g, meterArea);
    paintSteps(g, stepsArea);
}

void AnalyzerEditor::paintMessage(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title,
                                  const juce::String& body) {
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r.toFloat(), 10);
    auto inner = r.reduced(40).withSizeKeepingCentre(std::min(560, r.getWidth() - 80), 220);
    g.setColour(colours::text);
    g.setFont(font(22, true));
    g.drawText(title, inner.removeFromTop(36), juce::Justification::centred);
    g.setColour(colours::dim);
    g.setFont(font(15));
    g.drawFittedText(body, inner, juce::Justification::centredTop, 8);
}

void AnalyzerEditor::paintScore(juce::Graphics& g, juce::Rectangle<int> r) {
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r.toFloat(), 10);
    r = r.reduced(16);

    auto ringArea = r.removeFromTop(170).toFloat().withSizeKeepingCentre(150, 150);
    // A 270 degree gauge; JUCE measures angles clockwise from 12 o'clock.
    const float start = -juce::MathConstants<float>::pi * 0.75f, sweep = juce::MathConstants<float>::pi * 1.5f;
    juce::Path track, value;
    track.addCentredArc(ringArea.getCentreX(), ringArea.getCentreY(), 68, 68, 0, start, start + sweep, true);
    g.setColour(colours::line);
    g.strokePath(track, juce::PathStrokeType(10, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    float frac = juce::jlimit(0.0f, 1.0f, report.overall / 100.0f);
    value.addCentredArc(ringArea.getCentreX(), ringArea.getCentreY(), 68, 68, 0, start, start + sweep * frac, true);
    g.setColour(scoreColour(report.overall));
    g.strokePath(value, juce::PathStrokeType(10, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(colours::text);
    g.setFont(font(46, true));
    g.drawText(juce::String(report.overall), ringArea.translated(0, -8), juce::Justification::centred);
    g.setFont(font(13));
    g.setColour(colours::dim);
    g.drawText("out of 100", ringArea.translated(0, 30), juce::Justification::centred);

    g.setColour(scoreColour(report.overall));
    g.setFont(font(18, true));
    g.drawText(report.verdict, r.removeFromTop(26), juce::Justification::centred);
    g.setColour(colours::dim);
    g.setFont(font(13));
    g.drawText("vs " + juce::String(report.genreName), r.removeFromTop(20), juce::Justification::centred);
    r.removeFromTop(14);

    for (auto& a : mix::kAreaOrder) {
        auto it = report.areas.find(a);
        if (it == report.areas.end()) continue;
        auto row = r.removeFromTop(30);
        auto label = row.removeFromTop(16);
        g.setColour(colours::text);
        g.setFont(font(13));
        g.drawText(mix::areaLabel(a), label, juce::Justification::centredLeft);
        g.drawText(juce::String(it->second), label, juce::Justification::centredRight);
        auto bar = row.removeFromTop(6).toFloat();
        g.setColour(colours::line);
        g.fillRoundedRectangle(bar, 3);
        g.setColour(scoreColour(it->second));
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * juce::jlimit(0, 100, it->second) / 100.0f), 3);
    }

    // What the meter reads right now, for riding the master while it plays.
    auto now = r.removeFromBottom(54);
    if (now.getHeight() < 54) return;
    g.setColour(colours::line);
    g.drawHorizontalLine(now.getY(), (float)now.getX(), (float)now.getRight());
    now.removeFromTop(8);
    auto half = now.getWidth() / 2;
    auto drawNow = [&](juce::Rectangle<int> a, const juce::String& label, const juce::String& v, juce::Colour c) {
        g.setColour(colours::dim);
        g.setFont(font(11.5f));
        g.drawText(label, a.removeFromTop(16), juce::Justification::centred);
        g.setColour(c);
        g.setFont(font(19, true));
        g.drawText(v, a, juce::Justification::centred);
    };
    drawNow(now.removeFromLeft(half), "Now (LUFS)", snap.shortTermLufs > -70 ? number(snap.shortTermLufs, 1) : "-",
            colours::text);
    auto tpc = snap.truePeakNow > 0 ? colours::bad : snap.truePeakNow > -1 ? colours::warn : colours::text;
    drawNow(now, "Peak now (dBTP)", snap.truePeakNow > -70 ? number(snap.truePeakNow, 1, true) : "-", tpc);
}

void AnalyzerEditor::paintTone(juce::Graphics& g, juce::Rectangle<int> r) {
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r.toFloat(), 10);
    r = r.reduced(16);
    g.setColour(colours::text);
    g.setFont(font(15, true));
    g.drawText("Tonal balance vs the genre", r.removeFromTop(20), juce::Justification::centredLeft);
    g.setColour(colours::dim);
    g.setFont(font(12));
    g.drawText("Shaded: the genre's range. Dot: your mix. Above the line: more than the genre.",
               r.removeFromTop(18), juce::Justification::centredLeft);
    r.removeFromTop(8);

    auto labels = r.removeFromBottom(34);
    auto plot = r.reduced(0, 16).toFloat();
    const float range = 9.0f;  // dB shown above and below the genre median
    auto yFor = [&](double db) {
        return plot.getCentreY() - (float)juce::jlimit(-range, range, (float)db) / range * plot.getHeight() / 2;
    };
    g.setColour(colours::line);
    for (int db : {-6, -3, 3, 6}) g.drawHorizontalLine((int)yFor(db), plot.getX(), plot.getRight());
    g.setColour(colours::dim.withAlpha(0.6f));
    g.drawHorizontalLine((int)yFor(0), plot.getX(), plot.getRight());

    const float colW = plot.getWidth() / mix::kNumBands;
    for (int b = 0; b < mix::kNumBands; ++b) {
        float cx = plot.getX() + colW * (b + 0.5f);
        auto lab = labels.withX((int)(cx - colW / 2)).withWidth((int)colW);
        g.setColour(colours::text);
        g.setFont(font(11.5f));
        g.drawText(kBandLabels[b], lab.removeFromTop(16), juce::Justification::centred);
        g.setColour(colours::dim);
        g.setFont(font(10.5f));
        g.drawText(kBandHz[b], lab, juce::Justification::centred);

        auto* m = findRow(report, std::string("tonal.bands_db.") + mix::kBandIds[b]);
        if (!m) continue;
        float y1 = yFor(m->high - m->target), y0 = yFor(m->low - m->target);
        g.setColour(colours::good.withAlpha(0.16f));
        g.fillRoundedRectangle(cx - colW * 0.3f, y1, colW * 0.6f, std::max(2.0f, y0 - y1), 4);
        double dev = m->value - m->target;
        float y = yFor(dev);
        auto c = statusColour(*m);
        g.setColour(c.withAlpha(0.5f));
        g.drawLine(cx, yFor(0), cx, y, 2);
        g.setColour(c);
        g.fillEllipse(cx - 6, y - 6, 12, 12);
        g.setFont(font(11, true));
        float ty = dev >= 0 ? y - 22 : y + 8;
        g.drawText(number(dev, 1, true), juce::Rectangle<float>(cx - colW / 2, ty, colW, 14), juce::Justification::centred);
    }
}

void AnalyzerEditor::paintMeters(juce::Graphics& g, juce::Rectangle<int> r) {
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r.toFloat(), 10);
    r = r.reduced(16);
    g.setColour(colours::text);
    g.setFont(font(15, true));
    g.drawText("Level, punch and width", r.removeFromTop(20), juce::Justification::centredLeft);
    r.removeFromTop(8);

    struct Row { const char* key; const char* label; };
    const Row rows[] = {{"loudness.integrated_lufs", "Loudness (LUFS)"},
                        {"loudness.max_short_term_lufs", "Loudest part (LUFS)"},
                        {"dynamics.crest_db", "Punch (dB)"},
                        {"dynamics.section_contrast_lu", "Drop impact (LU)"},
                        {"stereo.width", "Stereo width"},
                        {"stereo.correlation", "Mono compatibility"},
                        {"low_end.sub_to_bass_db", "Sub vs bass (dB)"},
                        {"low_end.low_width", "Bass width"}};
    const int rowH = juce::jlimit(30, 44, r.getHeight() / 9);

    auto drawBar = [&](juce::Rectangle<int> area, double lo, double hi, double target, double v, juce::Colour c) {
        double pad = std::max((hi - lo) * 0.8, 1e-3);
        double a = std::min(lo, v) - pad, b = std::max(hi, v) + pad;
        auto xFor = [&](double x) { return area.getX() + (float)((x - a) / (b - a)) * area.getWidth(); };
        auto bar = area.toFloat().withSizeKeepingCentre((float)area.getWidth(), 8);
        g.setColour(colours::line);
        g.fillRoundedRectangle(bar, 4);
        g.setColour(colours::good.withAlpha(0.3f));
        g.fillRoundedRectangle(juce::Rectangle<float>(xFor(lo), bar.getY(), std::max(2.0f, xFor(hi) - xFor(lo)), 8), 4);
        g.setColour(colours::dim);
        g.drawVerticalLine((int)xFor(target), bar.getY() - 2, bar.getBottom() + 2);
        g.setColour(c);
        g.fillEllipse(xFor(v) - 6, bar.getCentreY() - 6, 12, 12);
    };

    for (auto& row : rows) {
        auto* m = findRow(report, row.key);
        if (!m) continue;
        auto line = r.removeFromTop(rowH);
        auto text = line.removeFromTop(16);
        g.setColour(colours::text);
        g.setFont(font(12.5f));
        g.drawText(row.label, text, juce::Justification::centredLeft);
        g.setColour(statusColour(*m));
        g.setFont(font(12.5f, true));
        juce::String status = m->status == "ok" ? "in range" : (m->status == "low" ? "low" : "high");
        g.drawText(number(m->value, m->decimals) + "   " + status, text, juce::Justification::centredRight);
        drawBar(line.removeFromTop(16).reduced(6, 0), m->low, m->high, m->target, m->value, statusColour(*m));
    }

    // True peak is judged against the streaming limit, not the genre.
    if (snap.has("loudness.true_peak_dbtp") && r.getHeight() >= rowH) {
        double tp = snap.get("loudness.true_peak_dbtp");
        auto line = r.removeFromTop(rowH);
        auto text = line.removeFromTop(16);
        auto c = tp > 0 ? colours::bad : tp > -1 ? colours::warn : colours::good;
        g.setColour(colours::text);
        g.setFont(font(12.5f));
        g.drawText("True peak (dBTP)", text, juce::Justification::centredLeft);
        g.setColour(c);
        g.setFont(font(12.5f, true));
        g.drawText(number(tp, 1, true) + (tp > 0 ? "   clips" : tp > -1 ? "   above -1" : "   ok"), text,
                   juce::Justification::centredRight);
        drawBar(line.removeFromTop(16).reduced(6, 0), -6.0, -1.0, -1.0, tp, c);
    }
}

void AnalyzerEditor::paintSteps(juce::Graphics& g, juce::Rectangle<int> r) {
    g.setColour(colours::panel);
    g.fillRoundedRectangle(r.toFloat(), 10);
    r = r.reduced(16, 12);

    auto status = r.removeFromBottom(18);
    g.setColour(colours::dim);
    g.setFont(font(12));
    int secs = (int)snap.seconds;
    juce::String measured = proc.recentMode ? "Following the last 10 seconds" : "Measured " + juce::String(secs / 60) + ":" +
                                                  juce::String(secs % 60).paddedLeft('0', 2) + " since play";
    const mix::Profile* p = nullptr;
    for (auto& x : proc.profiles())
        if (x.id == report.genreId) p = &x;
    juce::String basis;
    if (p) {
        basis = "Genre built from " + juce::String(p->referenceCount) + " songs";
        if (p->confidence == "low") basis += " (a rough guide until it has 10)";
    }
    g.drawText(measured + "   ·   " + basis, status, juce::Justification::centredLeft, true);

    if (!shownNear.empty()) {
        juce::StringArray near;
        for (auto& s : shownNear) near.add(s);
        g.drawText("Sounds closest to: " + near.joinIntoString(",  "), r.removeFromBottom(18),
                   juce::Justification::centredLeft, true);
    }

    g.setColour(colours::text);
    g.setFont(font(15, true));
    g.drawText("Fix these first", r.removeFromTop(22), juce::Justification::centredLeft);
    r.removeFromTop(4);
    if (shownSteps.empty()) {
        g.setColour(colours::good);
        g.setFont(font(14));
        g.drawText("Nothing stands out. This sits inside the genre's range.", r.removeFromTop(22),
                   juce::Justification::centredLeft);
        return;
    }
    const int lineH = std::max(18, r.getHeight() / 3);
    for (size_t i = 0; i < shownSteps.size(); ++i) {
        auto line = r.removeFromTop(lineH);
        g.setColour(colours::accent);
        g.setFont(font(14, true));
        g.drawText(juce::String((int)i + 1) + ".", line.removeFromLeft(22), juce::Justification::topLeft);
        g.setColour(colours::text);
        g.setFont(font(14));
        g.drawFittedText(juce::String(shownSteps[i]), line, juce::Justification::topLeft, 2);
    }
}
