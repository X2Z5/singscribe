# SingScribe — DiffSinger vocal synth VST3 with an on-board AI lyricist

A generator plugin for FL Studio (Windows + macOS): hosts **DiffSinger ONNX voicebanks**,
captures notes from FL's piano roll, distributes typed lyrics one syllable per note,
and writes its own lyrics with a **local llama.cpp LLM** (Traditional Vocaloid /
Underground modes). Audio renders on a background thread and plays back in sync
with the FL transport.

> v1 honesty: rule-based phoneme timing (variance.onnx not used yet), single-speaker
> models only, CPU-only LLM, coarse EN→JP transliteration. FL→plugin note sync happens
> by playing the pattern once (VST3 cannot read the host piano roll directly — no
> plugin can) or by dropping a .mid on the plugin window.

---

## 1. Build it with GitHub (same flow as the EX2 plugins)

1. Create a new **public repo** on github.com (e.g. `singscribe`).
2. **Add file → Upload files** → drag everything in this folder in → Commit.
3. Make sure `.github/workflows/build.yml` exists in the repo (create it via
   **Add file → Create new file** and paste its contents if your upload skipped it).
4. **Actions** tab → wait for the green check on "Build SingScribe VST3" (~15–20 min;
   it compiles llama.cpp too).
5. Download the artifact for your OS: **SingScribe-Windows** / **SingScribe-macOS**.
   The ONNX Runtime library is already packed inside the `.vst3` — no extra installs.

## 2. Install

- **Windows**: copy `SingScribe.vst3` (the whole folder) to `C:\Program Files\Common Files\VST3`
- **macOS**: copy `SingScribe.vst3` to `/Library/Audio/Plug-Ins/VST3`, then in Terminal:
  ```bash
  sudo xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/SingScribe.vst3"
  ```
- FL Studio: Options → Manage plugins → Find installed plugins → search "SingScribe"
  (appears under Generators).

## 3. Get the models (required — the plugin ships with none)

**A. DiffSinger voicebank** → folder: `Music/DiffSinger_Voices/<VoiceName>/`

Use an **OpenUtau-format DiffSinger ONNX voicebank** (community banks are published
on the openvpi / DiffSinger community pages — search "DiffSinger OpenUtau voicebank").
The folder must end up containing:

```
Music/DiffSinger_Voices/MyVoice/
   acoustic.onnx          (or *acoustic*.onnx anywhere in the folder)
   nsf_hifigan.onnx       (vocoder — if the bank ships it separately, copy it in here)
   phonemes.txt           (the bank's phoneme list)
   dsconfig.yaml          (optional but recommended)
```

**B. Lyric LLM** → folder: `Music/SingScribe_LLM/`

Download a small **instruct GGUF** (1–3B recommended — e.g. a Qwen 2.5 1.5B-Instruct
or Llama 3.2 3B-Instruct Q4 quant from Hugging Face) and drop the `.gguf` file in.
Bigger models = better lyrics but heavy next to a DAW. Optional: a `cmudict.txt`
in the same folder upgrades English pronunciation.

## 4. Using it in FL Studio

1. Add **SingScribe** to the Channel Rack. Open it → **VOICE** tab → pick your voice.
2. Write notes in **FL's piano roll** for the SingScribe channel, then **press play
   once** — the notes appear in the plugin's PIANO ROLL tab (capture-on-playback).
   Or draw notes directly in the plugin, or drop a .mid file on it.
3. **PIANO ROLL** tab: type a phrase in the lyric box → **APPLY LYRICS** — one
   syllable per note ("-" holds the previous syllable). Double-click any note to
   edit its syllable; drag to move, edge-drag to resize, Delete to remove.
4. Or go to **AI LYRICIST**: type a theme, pick **Traditional Vocaloid** or
   **Underground**, hit **GENERATE** — lyrics stream into the console and drop
   onto your notes automatically, sized to your phrase lengths.
5. Press play. First render takes a few seconds ("rendering phrase x/y" in the
   status bar), then it sings in sync. Edits re-render only the phrase you touched.
6. **DRAG MIDI OUT** puts your tuned notes back into FL's piano roll.

## 5. Troubleshooting

- "no voice loaded / no voices found" → check the folder layout in §3A, hit RESCAN.
- "phoneme set mismatch" → your bank uses a different phoneme scheme; tell me the
  bank name + a few lines of its phonemes.txt and I'll add a mapping.
- "acoustic model has unsupported input 'X'" → send me that exact message.
- LLM says "no GGUF model" → the `.gguf` goes in `Music/SingScribe_LLM/` (not a subfolder).
- Silence on play → wait for "ready" in the status bar; check a voice is loaded;
  make sure the pattern actually has notes captured (PIANO ROLL tab).
- Mac "can't be opened" → run the `xattr` command from §2.

DiffSinger voicebanks have their own licenses — respect each bank's terms.
Built on JUCE 8, ONNX Runtime 1.20.1, llama.cpp (b9596).
