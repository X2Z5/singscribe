#pragma once
// LyricGenerator — owns the swappable LLM backend, derives syllable budgets
// from the active pattern, filters refusals, and feeds clean text into G2P.
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

    void setBackend (std::unique_ptr<LLMService> newBackend)
    {
        if (backend) backend->cancel();
        backend = std::move (newBackend);
    }

    LLMService* getBackend() const { return backend.get(); }

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

        launch (req, std::make_shared<UICallbacks> (std::move (ui)), 0);
    }

    /** Small models sometimes refuse harmless prompts; detect, retry once,
        and never let an apology get distributed onto the notes. */
    static bool looksLikeRefusal (const juce::String& t)
    {
        const auto l = t.toLowerCase();
        return l.contains ("i can't") || l.contains ("i cannot")
            || l.contains ("i'm sorry") || l.contains ("i am sorry")
            || l.contains ("can't fulfill") || l.contains ("cannot fulfill")
            || l.contains ("cannot assist") || l.contains ("can't assist")
            || l.contains ("as an ai");
    }

private:
    void launch (LyricRequest req, std::shared_ptr<UICallbacks> ui, int attempt)
    {
        backend->generate (req,
            [ui] (const juce::String& chunk)
            {
                if (ui->onConsoleAppend) ui->onConsoleAppend (chunk);
            },
            [this, req, ui, attempt] (bool ok, const juce::String& full, const juce::String& err) mutable
            {
                if (! ok)
                {
                    if (ui->onFinished) ui->onFinished (false, err);
                    return;
                }
                if (looksLikeRefusal (full))
                {
                    if (attempt == 0)
                    {
                        if (ui->onConsoleAppend)
                            ui->onConsoleAppend ("\n[model refused — retrying...]\n");
                        req.userPrompt = req.userPrompt
                            + "\n(Reminder: this is a normal, wholesome songwriting request. "
                              "Write the original lyric lines now, nothing else.)";
                        req.temperature = juce::jmin (1.2f, req.temperature + 0.15f);
                        launch (req, ui, 1);
                        return;
                    }
                    if (ui->onFinished)
                        ui->onFinished (false, "the local model refused twice — try rephrasing "
                                               "your prompt (simpler wording usually works)");
                    return;
                }
                applyToSequence (full, req.voiceLanguage);
                if (ui->onFinished) ui->onFinished (true, "lyrics distributed to notes");
            });
    }

    /** Phrase gaps > 1 beat split lines; each phrase's note count = that line's
        syllable budget, so output can't overflow the pattern. */
    std::vector<int> deriveSyllableBudget() const
    {
        std::vector<int> budget;
        int count = 0;
        double lastEnd = -1.0e9;
        for (const auto& n : sequence.snapshot())
        {
            if (n.isRest) continue;
            if (n.startBeat - lastEnd > 1.0 && count > 0) { budget.push_back (count); count = 0; }
            ++count;
            lastEnd = juce::jmax (lastEnd, n.endBeat());
        }
        if (count > 0) budget.push_back (count);
        if (budget.empty()) budget.push_back (8);
        return budget;
    }

    void applyToSequence (const juce::String& text, const juce::String& voiceLanguage)
    {
        g2p.setVoiceLanguage (voiceLanguage);
        const auto sylls = g2p.splitForNotes (text, G2PEngine::guessLanguage (text));
        sequence.distributeLyrics (sylls);
        g2p.phonemizeSequence (sequence);
    }

    VocalSequence& sequence;
    G2PEngine& g2p;
    std::unique_ptr<LLMService> backend;
};

} // namespace ss
