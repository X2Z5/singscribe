#pragma once
// HostNoteCapture — the "FL piano roll → plugin" half of bidirectional sync.
//
// REALITY CHECK (see ARCHITECTURE.md §3): VST3 cannot enumerate the host's
// piano roll. FL streams note events during playback. This class converts that
// stream into timestamped VocalNotes and merges them into the VocalSequence
// with a bar-range replace policy, so one playback pass = full sync, and
// re-playing an edited pattern updates rather than duplicates.
//
// The reverse direction (plugin → FL) is MIDI drag-out from the editor
// (DragAndDropContainer::performExternalDragDropOfFiles), as in EX2 Chords.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Model/VocalNote.h"

namespace ss
{

class HostNoteCapture
{
public:
    /** All hooks are invoked on the AUDIO thread — implementations must be
        realtime-safe (the defaults below only flip atomics / push to a queue). */
    struct Hooks
    {
        std::function<void (double ppq, double bpm, bool playing)> onTransport;
        std::function<void (int capturedCount)>                    onCapturePass;
    };

    explicit HostNoteCapture (VocalSequence& seq) : sequence (seq) {}

    void prepare (double sampleRateIn) { sampleRate = sampleRateIn; reset(); }

    void reset()
    {
        hanging.clear();
        passStartPpq = -1.0;
        captured.clear();
    }

    bool captureEnabled = true;   // UI toggle "Learn from FL piano roll"

    //==========================================================================
    /** TEMPLATE HOOK — call this first inside processBlock().

        @param midi      the host MIDI buffer (FL piano roll events arrive here)
        @param pos       playhead position for this block (ppq/bpm/isPlaying)
        @param numSamples block length, for sample→ppq offset conversion
    */
    void processHostBlock (const juce::MidiBuffer& midi,
                           const juce::AudioPlayHead::PositionInfo& pos,
                           int numSamples,
                           Hooks& hooks)
    {
        const bool playing = pos.getIsPlaying();
        const double bpm   = pos.getBpm().orFallback (120.0);
        const double ppq   = pos.getPpqPosition().orFallback (0.0);

        if (hooks.onTransport)
            hooks.onTransport (ppq, bpm, playing);

        if (! captureEnabled)
            return;

        if (playing && passStartPpq < 0.0)
            passStartPpq = ppq;                       // a capture pass begins

        const double ppqPerSample = bpm / 60.0 / sampleRate;

        for (const auto meta : midi)
        {
            const auto msg = meta.getMessage();
            const double evPpq = ppq + meta.samplePosition * ppqPerSample;

            if (msg.isNoteOn())
            {
                hanging.push_back ({ msg.getNoteNumber(), evPpq, msg.getFloatVelocity() });
            }
            else if (msg.isNoteOff())
            {
                for (auto it = hanging.begin(); it != hanging.end(); ++it)
                {
                    if (it->note == msg.getNoteNumber())
                    {
                        VocalNote n;
                        n.midiPitch       = it->note;
                        n.startBeat       = quantizeForIdentity (it->startPpq);
                        n.lengthBeats     = juce::jmax (1.0 / 32.0, evPpq - it->startPpq);
                        n.velocity        = it->velocity;
                        n.fromHostCapture = true;
                        n.lyric           = "la";     // placeholder until lyrics distributed
                        captured.push_back (std::move (n));
                        hanging.erase (it);
                        break;
                    }
                }
            }
        }

        // Transport stopped (or looped back): commit the pass as one merge.
        const bool passEnded = (! playing && passStartPpq >= 0.0)
                            || (playing && ppq + 0.5 < lastPpq);   // loop jump back
        if (passEnded && ! captured.empty())
        {
            // SKELETON SIMPLIFICATION: this allocates on the audio thread.
            // Production: push into a lock-free FIFO here and drain it from a
            // message-thread Timer that performs the replaceHostRange merge.
            const int count = (int) captured.size();
            auto notesToMerge = std::make_shared<std::vector<VocalNote>> (std::move (captured));
            captured.clear();
            const double t0 = passStartPpq, t1 = juce::jmax (lastPpq, ppq);
            auto* seqPtr = &sequence;
            juce::MessageManager::callAsync ([seqPtr, notesToMerge, t0, t1]
            {
                seqPtr->replaceHostRange (t0, t1, std::move (*notesToMerge));
            });
            if (hooks.onCapturePass)
                hooks.onCapturePass (count);
            passStartPpq = playing ? ppq : -1.0;
        }
        if (! playing)
            passStartPpq = -1.0;

        lastPpq = ppq;
    }

private:
    static double quantizeForIdentity (double ppq)
    {
        // light 1/64 grid snap so the same FL note re-captured lands identically
        return std::round (ppq * 16.0) / 16.0;
    }

    struct Hanging { int note; double startPpq; float velocity; };

    VocalSequence& sequence;
    std::vector<Hanging> hanging;
    std::vector<VocalNote> captured;
    double passStartPpq = -1.0, lastPpq = 0.0;
    double sampleRate = 44100.0;
};

} // namespace ss
