# Analyzer App plugin

An Audio Unit (and VST3) for Logic Pro that you put on the **Stereo Out**,
after Ozone and your other master plugins. While the song plays it compares the
mix with your genre profiles, the same ones the Analyzer App builds from your
reference songs, and shows:

- a live score out of 100 with the same scoring as `analyzer check`
- tonal balance per band against the genre's range
- loudness, loudest part, punch, drop impact, stereo width, mono
  compatibility, sub vs bass, bass width and true peak
- the three fixes to make first, and the reference songs it sounds closest to

It doesn't change the sound. Audio passes through untouched.

## Using it

- **Genre**: pick one, or leave it on *Closest genre* to let it choose.
- **Last 10 seconds**: follows what's playing now, so you see each EQ or
  limiter move. **Since play**: measures everything since you pressed play, like
  checking the whole song. Drop impact and loudness range need this mode
  and at least 10 seconds of music.
- **Reset** starts the measurement over.
- **More** reloads the genres after you add reference songs in the app, or
  points the plugin at a different profiles folder.

It reads profiles from `~/AnalyzerApp/profiles/`.

## How it's built

| File | What it does |
| --- | --- |
| `Source/LiveAnalyzer.*` | The measurements from `analyzer_app/analyze.py`, made on a stream (K-weighted loudness with BS.1770 gating, 4x true peak, Welch spectrum, stereo) |
| `Source/MixScore.*` | The score, findings and next steps from `analyzer_app/compare.py` |
| `Source/ProfileStore.*` | Reads the genre profile JSON |
| `Source/PluginProcessor.*` | The plugin: passes audio through and measures on a background thread |
| `Source/PluginEditor.*` | The window |
| `tests/compare_with_python.py` | Checks the plugin's numbers against the Python analyzer on a real song |

GitHub builds it on a Mac (`.github/workflows/plugin.yml`), checks it with
Apple's `auval`, and puts the zips on the `plugin-builds` branch.

To build and test on Linux (VST3 and standalone):

```
cmake -S plugin -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
python plugin/tests/compare_with_python.py build/AnalyzerSelfTest_artefacts/Release/AnalyzerSelfTest song.mp3 ~/AnalyzerApp/profiles/indie-dance.json
```
