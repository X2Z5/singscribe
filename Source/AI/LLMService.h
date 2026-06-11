#pragma once
// LLMService — the abstract seam (requirement 6). UI, LyricGenerator and G2P
// depend on THIS, never on llama.cpp or any HTTP client.
#include <juce_core/juce_core.h>
#include <functional>

namespace ss
{

struct LyricRequest
{
    enum class Mode { TraditionalVocaloid, Underground, TranslateToRomaji };

    juce::String userPrompt;          // what the user typed
    Mode mode = Mode::TraditionalVocaloid;

    // constraints derived from the active pattern, so output fits the notes:
    int targetLines = 4;
    std::vector<int> syllablesPerLine;   // e.g. {7,7,5,7}, from note phrase grouping
    juce::String voiceLanguage { "jpn" };   // "jpn" → favour open CV syllables

    // generation params
    float temperature = 0.9f;
    int   maxTokens   = 256;
};

//==============================================================================
class LLMService
{
public:
    virtual ~LLMService() = default;

    /** True once a model/endpoint is ready to take requests. */
    virtual bool isAvailable() const = 0;

    /** Human-readable backend label for the UI status chip ("local: llama3-8B Q4"). */
    virtual juce::String backendName() const = 0;

    /** Async generation. MUST return immediately.
        - onToken: streamed text chunks (ANY thread → marshal in the caller).
        - onDone : final full text or error. Called exactly once.            */
    virtual void generate (const LyricRequest& request,
                           std::function<void (const juce::String& tokenChunk)> onToken,
                           std::function<void (bool ok, const juce::String& fullText,
                                               const juce::String& error)> onDone) = 0;

    /** Abort the in-flight request (user pressed Generate again / closed UI). */
    virtual void cancel() = 0;
};

} // namespace ss
