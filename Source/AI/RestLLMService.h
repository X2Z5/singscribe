#pragma once
// RestLLMService — the cloud fallback (requirement 6). Same interface, zero
// changes anywhere else. Use when llama.cpp competes with the DAW for CPU.
#include "LLMService.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace ss
{

class RestLLMService : public LLMService,
                       private juce::Thread
{
public:
    RestLLMService (const juce::URL& endpointIn, const juce::String& apiKeyIn,
                    const juce::String& modelIdIn = "small-fast-model")
        : juce::Thread ("SS-LLM-REST"),
          endpoint (endpointIn), apiKey (apiKeyIn), modelId (modelIdIn)
    {
        startThread();
    }

    ~RestLLMService() override
    {
        cancelFlag.store (true);
        signalThreadShouldExit();
        notify();
        stopThread (8000);
    }

    bool isAvailable() const override        { return endpoint.isWellFormed() && apiKey.isNotEmpty(); }
    juce::String backendName() const override { return "cloud: " + modelId; }

    void generate (const LyricRequest& request,
                   std::function<void (const juce::String&)> onToken,
                   std::function<void (bool, const juce::String&, const juce::String&)> onDone) override
    {
        cancel();
        {
            const juce::ScopedLock sl (jobLock);
            pending = std::make_unique<Pending> (Pending { request, std::move (onToken), std::move (onDone) });
        }
        cancelFlag.store (false);
        notify();
    }

    void cancel() override { cancelFlag.store (true); }

private:
    struct Pending
    {
        LyricRequest request;
        std::function<void (const juce::String&)> onToken;
        std::function<void (bool, const juce::String&, const juce::String&)> onDone;
    };

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            if (threadShouldExit()) return;

            std::unique_ptr<Pending> p;
            {
                const juce::ScopedLock sl (jobLock);
                p = std::move (pending);
            }
            if (p == nullptr) continue;

            // Build JSON body — system prompt logic is shared with the local
            // backend conceptually; both consume the same LyricRequest.
            // TODO: POST via juce::URL::withPOSTData(...).createInputStream
            //       (withExtraHeaders "Authorization: Bearer <apiKey>"),
            //       honour cancelFlag, parse response JSON, stream if SSE.
            const juce::String result = "(cloud output placeholder)";

            if (p->onToken)
                juce::MessageManager::callAsync ([cb = p->onToken, result] { cb (result); });
            if (p->onDone)
                juce::MessageManager::callAsync ([cb = p->onDone, result,
                                                  ok = ! cancelFlag.load()]
                                                 { cb (ok, result, ok ? juce::String() : "cancelled"); });
        }
    }

    juce::URL endpoint;
    juce::String apiKey, modelId;
    std::unique_ptr<Pending> pending;
    juce::CriticalSection jobLock;
    std::atomic<bool> cancelFlag { false };
};

} // namespace ss
