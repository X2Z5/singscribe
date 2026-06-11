#pragma once
// VocalRenderEngine — render thread + the lock-free RenderCache the audio
// thread plays from. Per-phrase result caching keeps edits cheap: only the
// phrase you touched re-renders.
#include <juce_audio_basics/juce_audio_basics.h>
#include "VocalNote.h"
#include "DiffSingerModelManager.h"
#include "DiffSingerPipeline.h"
#include "../Text/G2PEngine.h"
#include <map>

namespace ss
{

//==============================================================================
class RenderCache
{
public:
    struct Buffer
    {
        juce::AudioBuffer<float> pcm;     // mono, voice sample rate
        double startBeat   = 0.0;
        double sampleRate  = 44100.0;
        int    forRevision = -1;
    };

    void publish (std::unique_ptr<Buffer> fresh)
    {
        std::shared_ptr<const Buffer> sp (std::move (fresh));
        std::atomic_store_explicit (&front, sp, std::memory_order_release);
    }

    void clear() { std::shared_ptr<const Buffer> n; std::atomic_store (&front, n); }

    /** Audio thread: copy samples for [beatPos, ...) with linear resampling
        when the host rate differs from the render rate. */
    int pull (float* dest, int numSamples, double beatPos, double bpm, double hostSr) const
    {
        auto buf = std::atomic_load_explicit (&front, std::memory_order_acquire);
        juce::FloatVectorOperations::clear (dest, numSamples);
        if (buf == nullptr || buf->pcm.getNumSamples() == 0)
            return -1;

        const double srcPerBeat = buf->sampleRate * 60.0 / juce::jmax (20.0, bpm);
        const double ratio      = buf->sampleRate / hostSr;          // src samples per dest sample
        const double srcStart   = (beatPos - buf->startBeat) * srcPerBeat;
        const float* src        = buf->pcm.getReadPointer (0);
        const int    avail      = buf->pcm.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const double sPos = srcStart + i * ratio;
            const auto   s0   = (juce::int64) std::floor (sPos);
            if (s0 < 0 || s0 + 1 >= avail) continue;
            const float frac = (float) (sPos - (double) s0);
            dest[i] = src[s0] * (1.0f - frac) + src[s0 + 1] * frac;
        }
        return buf->forRevision;
    }

private:
    std::shared_ptr<const Buffer> front;
};

//==============================================================================
class VocalRenderEngine : private juce::Thread
{
public:
    VocalRenderEngine (VocalSequence& seq, DiffSingerModelManager& mgr, G2PEngine& g2pRef)
        : juce::Thread ("SS-DiffSingerRender"), sequence (seq), models (mgr), g2p (g2pRef)
    {
        startThread();
    }

    ~VocalRenderEngine() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (15000);
    }

    RenderCache cache;

    void requestRender (double bpm)
    {
        targetBpm.store (bpm);
        notify();
    }

    bool isRendering() const noexcept { return rendering.load(); }

    juce::String getStatus() const
    {
        const juce::ScopedLock sl (statusLock);
        return status;
    }

private:
    void setStatus (const juce::String& s)
    {
        const juce::ScopedLock sl (statusLock);
        status = s;
    }

    void run() override
    {
        int lastRendered = -1;
        juce::String lastVoice;
        double lastBpm = 0.0;

        while (! threadShouldExit())
        {
            wait (150);
            if (threadShouldExit()) return;

            const double bpm = targetBpm.load();
            auto voice = models.getActiveVoice();

            if (voice == nullptr) { setStatus ("no voice loaded"); continue; }

            g2p.setVoiceLanguage (voice->language);
            g2p.phonemizeSequence (sequence);   // bumps revision only if it filled something

            const int rev = sequence.getRevision();
            const bool voiceChanged = voice->name != lastVoice;
            const bool bpmChanged   = std::abs (bpm - lastBpm) > 0.01;
            if (rev == lastRendered && ! voiceChanged && ! bpmChanged)
                continue;

            if (voiceChanged || bpmChanged) phraseCache.clear();

            rendering.store (true);
            int snapRev = 0;
            auto notes = sequence.snapshot (snapRev);

            renderAll (notes, *voice, bpm, snapRev);

            lastRendered = snapRev;
            lastVoice    = voice->name;
            lastBpm      = bpm;
            rendering.store (false);
        }
    }

    //==========================================================================
    void renderAll (const std::vector<VocalNote>& all, const LoadedVoice& voice,
                    double bpm, int revision)
    {
        // singable notes only, split into phrases on gaps > 1 beat
        std::vector<std::vector<VocalNote>> phrases;
        {
            std::vector<VocalNote> cur;
            double lastEnd = -1.0e9;
            for (const auto& n : all)
            {
                if (n.isRest) continue;
                if (! cur.empty() && n.startBeat - lastEnd > 1.0)
                {
                    phrases.push_back (std::move (cur));
                    cur = {};
                }
                cur.push_back (n);
                lastEnd = juce::jmax (lastEnd, n.endBeat());
            }
            if (! cur.empty()) phrases.push_back (std::move (cur));
        }

        if (phrases.empty()) { cache.clear(); setStatus ("no notes"); return; }

        struct Piece { std::shared_ptr<juce::AudioBuffer<float>> pcm; double startBeat; };
        std::vector<Piece> pieces;
        const double secPerBeat = 60.0 / juce::jmax (20.0, bpm);

        auto abortFn = [this, revision] { return threadShouldExit()
                                              || sequence.getRevision() != revision; };

        int idx = 0;
        for (auto& ph : phrases)
        {
            ++idx;
            setStatus ("rendering phrase " + juce::String (idx) + "/" + juce::String ((int) phrases.size()));
            if (abortFn()) { setStatus ("edit detected — restarting"); return; }

            const auto key = phraseHash (ph, voice.name, bpm);
            const double expectedStart = ph.front().startBeat - 0.20 / secPerBeat;

            if (auto it = phraseCache.find (key); it != phraseCache.end())
            {
                pieces.push_back ({ it->second, expectedStart });
                continue;
            }

            auto pcm = std::make_shared<juce::AudioBuffer<float>>();
            double startBeat = 0.0;
            const auto res = DiffSingerPipeline::renderPhrase (voice, ph, bpm, *pcm, startBeat, abortFn);
            if (! res.ok)
            {
                if (res.error == "aborted") { setStatus ("edit detected — restarting"); return; }
                setStatus ("phrase " + juce::String (idx) + " failed: " + res.error);
                continue;                                  // sing the phrases that worked
            }
            phraseCache[key] = pcm;
            if ((int) phraseCache.size() > 64) phraseCache.clear();   // crude cap
            pieces.push_back ({ pcm, startBeat });
        }

        if (pieces.empty()) { cache.clear(); return; }

        // assemble one buffer spanning all phrases
        double globalStart = 1.0e18, globalEndSec = 0.0;
        for (auto& p : pieces) globalStart = juce::jmin (globalStart, p.startBeat);
        for (auto& p : pieces)
            globalEndSec = juce::jmax (globalEndSec,
                (p.startBeat - globalStart) * secPerBeat
                    + p.pcm->getNumSamples() / voice.outputSampleRate);

        auto out = std::make_unique<RenderCache::Buffer>();
        out->startBeat   = globalStart;
        out->sampleRate  = voice.outputSampleRate;
        out->forRevision = revision;
        out->pcm.setSize (1, (int) (globalEndSec * voice.outputSampleRate) + 64);
        out->pcm.clear();

        for (auto& p : pieces)
        {
            const int at = (int) ((p.startBeat - globalStart) * secPerBeat * voice.outputSampleRate);
            const int n  = juce::jmin (p.pcm->getNumSamples(), out->pcm.getNumSamples() - at);
            if (n > 0 && at >= 0)
                out->pcm.addFrom (0, at, *p.pcm, 0, 0, n);
        }

        cache.publish (std::move (out));
        setStatus ("ready (" + juce::String ((int) pieces.size()) + " phrases)");
    }

    /** Content hash of a phrase, relative to its own start (so moving a whole
        phrase later in the song reuses the cached audio). */
    static juce::int64 phraseHash (const std::vector<VocalNote>& ph,
                                   const juce::String& voiceName, double bpm)
    {
        juce::int64 h = 1469598103934665603LL;
        auto mix = [&h] (juce::int64 v) { h ^= v; h *= 1099511628211LL; };
        mix (voiceName.hashCode64());
        mix ((juce::int64) std::llround (bpm * 100.0));
        const double t0 = ph.front().startBeat;
        for (const auto& n : ph)
        {
            mix (n.midiPitch);
            mix ((juce::int64) std::llround ((n.startBeat - t0) * 960.0));
            mix ((juce::int64) std::llround (n.lengthBeats * 960.0));
            mix (n.lyric.hashCode64());
            mix (n.phonemes.joinIntoString (" ").hashCode64());
            mix (n.isMelisma ? 7 : 3);
            mix ((juce::int64) std::llround (n.vibratoDepth * 100.0f));
            mix ((juce::int64) std::llround (n.vibratoRate * 100.0f));
            mix ((juce::int64) std::llround (n.pitchBendSemis * 100.0f));
        }
        return h;
    }

    //==========================================================================
    VocalSequence& sequence;
    DiffSingerModelManager& models;
    G2PEngine& g2p;
    std::atomic<double> targetBpm { 120.0 };
    std::atomic<bool> rendering { false };
    std::map<juce::int64, std::shared_ptr<juce::AudioBuffer<float>>> phraseCache;
    juce::String status { "idle" };
    mutable juce::CriticalSection statusLock;
};

} // namespace ss
