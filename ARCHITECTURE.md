# SingScribe — DiffSinger vocal synth VST3 for FL Studio
### Architecture & threading model (implementation v1)

Codename `SingScribe`. Generator plugin for the FL Channel Rack: hosts DiffSinger ONNX voices,
distributes typed lyrics across notes, and writes its own lyrics with a local llama.cpp LLM.

---

## 1. Thread map

```
┌──────────────────────────── UI / MESSAGE THREAD ─────────────────────────────┐
│  PluginEditor (TabbedComponent)                                              │
│   ├─ VOICE tab ........ voice ComboBox, model status light, rescan button    │
│   ├─ PIANO ROLL tab ... InternalPianoRoll (edit notes/lyrics/expressions)    │
│   └─ AI LYRICIST tab .. prompt box, Genre Mode toggle, Generate, console     │
│  Receives results ONLY via juce::MessageManager::callAsync / ChangeBroadcaster│
└───────┬───────────────────────────────────────────────┬─────────────────────┘
        │ APVTS params, VocalSequence edits (locked,     │ LyricRequest
        │ revision++)                                    ▼
        │                                   ┌── LLM WORKER THREAD ───────────┐
        │                                   │ LLMService (abstract)          │
        │                                   │  ├─ LlamaCppService (local GGUF)│
        │                                   │  └─ RestLLMService  (cloud API) │
        │                                   │ stream tokens → LyricGenerator │
        │                                   │ → SyllableBudgeter → G2P →     │
        │                                   │ VocalSequence (lyrics assigned)│
        │                                   └────────────────────────────────┘
┌───────▼──────────────── AUDIO THREAD (hard real-time) ───────────────────────┐
│  processBlock():                                                             │
│   1. HostNoteCapture::processHostBlock(midi, playhead)  ← FL piano roll MIDI │
│   2. read transport (ppq, bpm, playing)                                      │
│   3. RenderCache::pull(ppqWindow) → copy PCM into output buffer              │
│  NEVER: allocates, locks contended mutexes, touches ONNX/llama, scans disk   │
└───────┬──────────────────────────────────────────────────────────────────────┘
        │ lock-free: revision atomics + render job FIFO
┌───────▼──────────── DIFFSINGER RENDER THREAD ────────────────────────────────┐
│  Wakes on (sequence revision change | voice change | expression edit).      │
│  Debounce ~150 ms → pipeline:                                                │
│    notes → phoneme timing → VARIANCE.onnx (dur/pitch/energy)                 │
│          → ACOUSTIC.onnx (mel) → VOCODER.onnx (PCM)                          │
│  Writes PCM into RenderCache back buffer, atomically swaps front/back.       │
└──────────────────────────────────────────────────────────────────────────────┘
┌─────────────────── MODEL LOADER THREAD (DiffSingerModelManager) ─────────────┐
│  Scans ~/Music/DiffSinger_Voices/, parses dsconfig/character files,          │
│  builds Ort::Session set off-thread, std::shared_ptr atomic swap into        │
│  "active voice" slot. UI ComboBox repopulated via async callback.            │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Golden rules**
- Audio thread only ever *reads*: `std::atomic` revisions, a lock-free FIFO, and the front
  RenderCache buffer. All inference (ONNX + llama) lives on worker threads.
- Workers never touch JUCE components; they marshal results to the message thread.
- `VocalSequence` is the single source of truth. Every mutation bumps `revision`
  (atomic int). The render thread re-renders when it notices a new revision.

## 2. Module layout

```
Source/
  Plugin/   PluginProcessor.{h}   ← host glue, processBlock, APVTS
            PluginEditor.{h}      ← tabs, ComboBox, AI Lyricist UI
  Model/    VocalNote.h           ← lyric-enabled note + VocalSequence
            DiffSingerModelManager.h
            VocalRenderEngine.h   ← render thread + RenderCache
  Host/     HostNoteCapture.h     ← FL MIDI capture hooks (bidirectional story)
  Text/     G2PEngine.h           ← EN/JP G2P + EN↔JP transliteration bridge
  AI/       LLMService.h          ← abstract interface (the swap point)
            LlamaCppService.h     ← local GGUF backend (llama.cpp, pimpl)
            RestLLMService.h      ← drop-in cloud fallback
            LyricGenerator.h      ← mode prompts, syllable budgeting, G2P feed
```

## 3. FL Studio "bidirectional" sync — what is actually possible

VST3 has **no API to enumerate or edit the host's piano roll**. FL never hands a plugin
the full pattern; it streams note-on/off events at play time. Synth V / Vocaloid have the
same constraint. So:

- **FL → plugin**: `HostNoteCapture` listens during playback. Every on/off pair is
  timestamped with host PPQ and merged into `VocalSequence` (bar-range replace policy, so
  re-playing an edited pattern updates the internal roll instead of duplicating). Playing
  the pattern once = full sync. A MIDI-file drop target on the editor is the instant
  alternative (drag a `.mid` onto the plugin).
- **plugin → FL**: internal edits export via `performExternalDragDropOfFiles` (drag MIDI
  into the FL piano roll), same mechanism as proven in EX2 Chords. True direct injection
  into FL's piano roll is impossible from VST3 — documented limitation, not a TODO.

## 4. DiffSinger voice folder contract

```
~/Music/DiffSinger_Voices/<VoiceName>/
   dsconfig.yaml | character.yaml      (display name, language, sample rate, hop)
   acoustic.onnx                        (phoneme+f0 → mel)
   variance.onnx        [optional]      (duration / pitch / energy predictors)
   vocoder.onnx | nsf_hifigan.onnx      (mel → waveform; may live in shared folder)
   phonemes.txt / dict yaml             (phoneme inventory + dictionary)
```
`DiffSingerModelManager::scanForVoices()` treats any subfolder containing ≥1 `.onnx` +
a config file as a candidate voice; malformed folders are listed but greyed out with the
parse error as tooltip.

## 5. Latency & realtime strategy

DiffSinger is *not* realtime-causal — a phrase's pitch curve depends on the whole phrase.
Strategy: **render-ahead cache**. Audio thread plays the last finished render aligned by
PPQ; edits trigger re-render with a debounce; if the playhead enters an unrendered region
it outputs silence rather than blocking (status chip in UI shows "rendering…").
Crossfade 10 ms when a fresh render swaps in mid-playback.

## 6. LLM swap path (requirement 6)

`LyricGenerator` owns a `std::unique_ptr<LLMService>`. Construction:

```cpp
lyricGen.setBackend(std::make_unique<LlamaCppService>(modelFile));   // local, default
lyricGen.setBackend(std::make_unique<RestLLMService>(endpointUrl));  // cloud fallback
```

UI / G2P / sequence code never see the concrete type. If llama.cpp spikes CPU in a
session, switch backends at runtime — same callbacks, same request struct, zero refactor.

## 7. Build dependencies (CMake sketch in repo root)

| Dep | How | Note |
|---|---|---|
| JUCE 8 | FetchContent | as in EX2 plugins |
| ONNX Runtime | prebuilt package per-OS via `ORT_ROOT` | universal mac dylib needed for arm64+x86_64 |
| llama.cpp | FetchContent (static) | adds ~10–25 MB; GGUF loaded from disk at runtime |

GGUF + voice models ship **outside** the binary (user folders), keeping the plugin itself small.
