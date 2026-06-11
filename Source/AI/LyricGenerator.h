#pragma once
// LyricGenerator — owns the swappable LLM backend, derives syllable budgets
// from the active pattern, and feeds finished text into G2P + the sequence.
#include "LLMService.h"
#include "../Model/VocalNote.h"
#include "../Text/G2PEngine.h"

namespace ss
{

class LyricGenerator
{
public:
    LyricGenerator (VocalSequence& seq, G2PEngine& g2pRef)
        : sequence (seq), g2p (g2pRef) {}

    /** Requirement 6: the swap point. Call any time, including at runtime. */
    void setBackend (std::unique_ptr<LLMService> newBackend)
    {
        if (backend) backend->cancel();
        backend = std::move (newBackend);
    }

    LLMService* getBackend() const { return backend.get(); }

    /** Wire from the AI Lyricist tab. Callbacks land on the message thread. */
    struct UICallbacks
    {
        std::function<void (const juce::String& chunk)> onConsoleAppend;
        std::function<void (bool ok, const juce::String& status)> onFinished;
    };

    void generateForPattern (const juce::String& userPrompt,
                             LyricRequest::Mode mode,
                             const juce::String& voiceLanguage,
                             UICallbacks ui)
    {
        if (backend == nullptr || ! backend->isAvailable())
        {
            if (ui.onFinished) ui.onFinished (false, "no LLM backend available");
            return;
        }

        LyricRequest req;
        req.userPrompt       = userPrompt;
        req.mode             = mode;
        req.voiceLanguage    = voiceLanguage;
        req.syllablesPerLine = deriveSyllableBudget();
        req.targetLines      = juce::jmax (1, (int) req.syllablesPerLine.size());

        backend->generate (req,
            [ui] (const juce::String& chunk)            // streamed to console
            {
                if (ui.onConsoleAppend) ui.onConsoleAppend (chunk);
            },
            [this, ui, voiceLanguage] (bool ok, const juce::String& full, const juce::String& err)
            {
                if (! ok)
                {
                    if (ui.onFinished) ui.onFinished (false, err);
                    return;
                }
                applyToSequence (full, voiceLanguage);   // → G2P → notes
                if (ui.onFinished) ui.onFinished (true, "lyrics distributed to notes");
            });
    }

private:
    /** Group notes into phrases (gap > 1 beat = phrase break); each phrase's
        singable note count becomes one line's syllable budget — this is what
        keeps LLM output from overflowing note lengths (requirement 5). */
    std::vector<int> deriveSyllableBudget() const
    {
        int rev = 0;
        auto notes = sequence.snapshot (rev);
        std::vector<int> budget;
        int count = 0;
        double lastEnd = -1.0e9;
        for (const auto& n : notes)
        {
            if (n.isRest) continue;
            if (n.startBeat - lastEnd > 1.0 && count > 0) { budget.push_back (count); count = 0; }
            ++count;
            lastEnd = n.endBeat();
        }
        if (count > 0) budget.push_back (count);
        if (budget.empty()) budget.push_back (8);     // sane default: one 8-syllable line
        return budget;
    }

    void applyToSequence (const juce::String& text, const juce::String& voiceLanguage)
    {
        g2p.setVoiceLanguage (voiceLanguage);
        const auto sylls    = g2p.splitForNotes (text, G2PEngine::guessLanguage (text));
        const auto leftover = sequence.distributeLyrics (sylls);
        g2p.phonemizeSequence (sequence);
        juce::ignoreUnused (leftover);   // TODO: surface "N syllables didn't fit" in UI
    }

    VocalSequence& sequence;
    G2PEngine& g2p;
    std::unique_ptr<LLMService> backend;
};

} // namespace ss
