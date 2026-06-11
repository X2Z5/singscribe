#pragma once
// LlamaCppService — local GGUF backend behind LLMService.
// llama.cpp types live ONLY in LlamaCppService.cpp (pimpl), so swapping to
// RestLLMService never touches anything else.
#include "LLMService.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <memory>
#include <atomic>

namespace ss
{

class LlamaCppService : public LLMService,
                        private juce::Thread
{
public:
    /** Model loads lazily on first generate(); plugin instantiation stays fast. */
    explicit LlamaCppService (const juce::File& ggufFile);
    ~LlamaCppService() override;

    bool isAvailable() const override;
    juce::String backendName() const override;

    void generate (const LyricRequest& request,
                   std::function<void (const juce::String&)> onToken,
                   std::function<void (bool, const juce::String&, const juce::String&)> onDone) override;

    void cancel() override { cancelFlag.store (true); }

    /** Shared with RestLLMService so both backends sing from the same sheet. */
    static juce::String systemPromptFor (const LyricRequest&);

private:
    struct Job
    {
        LyricRequest request;
        std::function<void (const juce::String&)> onToken;
        std::function<void (bool, const juce::String&, const juce::String&)> onDone;
    };

    void run() override;
    bool ensureModelLoaded (juce::String& error);
    void runInference (Job&);
    void emitToken (Job&, const juce::String& chunk);
    void finish (Job&, bool ok, const juce::String& text, const juce::String& err);

    struct Impl;                       // llama_model*, etc. — defined in the .cpp
    std::shared_ptr<Impl> impl;

    juce::File modelFile;
    std::unique_ptr<Job> job;
    juce::CriticalSection jobLock;
    std::atomic<bool> cancelFlag { false }, modelLoaded { false };
};

} // namespace ss
