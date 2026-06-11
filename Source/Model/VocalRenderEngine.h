#pragma once
// VocalRenderEngine — render thread + lock-free stereo RenderCache + note
// audition. Per-phrase, per-unison-pass caching keeps edits cheap.
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
        juce::AudioBuffer<float> pcm;     // stereo, voice sample rate
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

    int pull (float* destL, float* destR, int numSamples,
              double beatPos, double bpm, double hostSr) const
    {
        auto buf = std::atomic_load_explicit (&front, std::memory_order_acquire);
        juce::FloatVectorOperations::clear (destL, numSamples);
        juce::FloatVectorOperations::clear (destR, numSamples);
        if (buf == nullptr || buf->pcm.getNumSamples() == 0)
            return -1;

        const double srcPerBeat = buf->sampleRate * 60.0 / juce::jmax (20.0, bpm);
        const double ratio      = buf->sampleRate / hostSr;
        const double srcStart   = (beatPos - buf->startBeat) * srcPerBeat;
        const float* srcL       = buf->pcm.getReadPointer (0);
        const float* srcR       = buf->pcm.getReadPointer (buf->pcm.getNumChannels() > 1 ? 1 : 0);
        const int    avail      = buf->pcm.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const double sPos = srcStart + i * ratio;
            const auto   s0   = (juce::int64) std::floor (sPos);
            if (s0 < 0 || s0 + 1 >= avail) continue;
            const float frac = (float) (sPos - (double) s0);
            destL[i] = srcL[s0] * (1.0f - frac) + srcL[s0 + 1] * frac;
            destR[i] = srcR[s0] * (1.0f - frac) + srcR[s0 + 1] * frac;
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
        : juce::Thread ("SS-DiffSingerRender", 8 * 1024 * 1024),
          sequence (seq), models (mgr), g2p (g2pRef)
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

    // set by the processor: receives finished audition audio (any thread)
    std::function<void (std::shared_ptr<juce::AudioBuffer<float>>, double sourceSampleRate)> onAuditionReady;

    std::atomic<int> speakerIndex { 0 };
    std::atomic<int> unisonMode   { 0 };   // 0=solo, 1=2 voices, 2=3 voices

    void requestRender (double bpm)
    {
        targetBpm.store (bpm);
        forceRender.store (true);
        notify();
    }

    /** Sing one note immediately (note placement / preview). */
    void requestAudition (VocalNote note)
    {
        note.lengthBeats = juce::jlimit (0.25, 2.0, note.lengthBeats);
        {
            const juce::ScopedLock sl (auditionLock);
            auditionNote = std::make_unique<VocalNote> (std::move (note));
        }
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

    struct PassPlan { double cents; float gainL, gainR; };

    std::vector<PassPlan> passesForUnison (int mode) const
    {
        switch (mode)
        {
            case 1:  return { { -9.0, 0.95f, 0.35f }, { +9.0, 0.35f, 0.95f } };
            case 2:  return { { -12.0, 0.9f, 0.25f }, { 0.0, 0.62f, 0.62f }, { +12.0, 0.25f, 0.9f } };
            default: return { { 0.0, 0.72f, 0.72f } };
        }
    }

    void run() override
    {
        int lastRendered = -1;
        juce::String lastConfig;

        while (! threadShouldExit())
        {
            wait (150);
            if (threadShouldExit()) return;

            auto voice = models.getActiveVoice();

            // ---- audition has priority ----
            std::unique_ptr<VocalNote> aud;
            {
                const juce::ScopedLock sl (auditionLock);
                aud = std::move (auditionNote);
            }
            if (aud != nullptr && voice != nullptr)
                renderAudition (*aud, *voice);

            if (voice == nullptr) { setStatus ("no voice loaded"); continue; }

            g2p.setVoiceLanguage (voice->language);
            g2p.phonemizeSequence (sequence);

            const double bpm = targetBpm.load();
            const int rev = sequence.getRevision();
            const juce::String config = voice->name + "|" + juce::String (speakerIndex.load())
                                      + "|" + juce::String (unisonMode.load())
                                      + "|" + juce::String (bpm, 2);
            const bool configChanged = (config != lastConfig);

            if (rev == lastRendered && ! configChanged && ! forceRender.exchange (false))
                continue;

            if (configChanged) phraseCache.clear();

            rendering.store (true);
            int snapRev = 0;
            auto notes = sequence.snapshot (snapRev);
            renderAll (notes, *voice, bpm, snapRev);
            lastRendered = snapRev;
            lastConfig   = config;
            rendering.store (false);
        }
    }

    //==========================================================================
    void renderAudition (VocalNote note, const LoadedVoice& voice)
    {
        if (note.phonemes.isEmpty() && ! note.isMelisma)
        {
            // quick inline phonemize for the single note
            VocalSequence tmp;
            tmp.addNote (note);
            g2p.setVoiceLanguage (voice.language);
            g2p.phonemizeSequence (tmp);
            auto v = tmp.snapshot();
            if (! v.empty()) note = v[0];
        }
        note.isMelisma = false;

        DiffSingerPipeline::RenderOptions ro;
        ro.spkEmbed = voice.embedForSpeaker (speakerIndex.load());

        auto pcm = std::make_shared<juce::AudioBuffer<float>>();
        double startBeat = 0.0;
        setStatus ("auditioning...");
        const auto res = DiffSingerPipeline::renderPhrase (voice, { note }, 120.0, *pcm, startBeat,
                                                           [this] { return threadShouldExit(); }, ro);
        if (res.ok && onAuditionReady)
            onAuditionReady (pcm, voice.outputSampleRate);
        setStatus (res.ok ? "ready" : ("audition failed: " + res.error));
    }

    //==========================================================================
    void renderAll (const std::vector<VocalNote>& all, const LoadedVoice& voice,
                    double bpm, int revision)
    {
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

        const auto passes = passesForUnison (unisonMode.load());
        const auto* spk   = voice.embedForSpeaker (speakerIndex.load());
        const double secPerBeat = 60.0 / juce::jmax (20.0, bpm);

        struct Piece { std::shared_ptr<juce::AudioBuffer<float>> pcm; double startBeat; float gL, gR; };
        std::vector<Piece> pieces;

        auto abortFn = [this, revision] { return threadShouldExit()
                                              || sequence.getRevision() != revision; };

        int idx = 0;
        for (auto& ph : phrases)
        {
            ++idx;
            for (size_t pi = 0; pi < passes.size(); ++pi)
            {
                setStatus ("rendering phrase " + juce::String (idx) + "/"
                           + juce::String ((int) phrases.size())
                           + (passes.size() > 1 ? " voice " + juce::String ((int) pi + 1) : juce::String()));
                if (abortFn()) { setStatus ("edit detected — restarting"); return; }

                const auto key = phraseHash (ph, voice.name, bpm,
                                             speakerIndex.load(), passes[pi].cents);
                const double expectedStart = ph.front().startBeat - 0.20 / secPerBeat;

                if (auto it = phraseCache.find (key); it != phraseCache.end())
                {
                    pieces.push_back ({ it->second, expectedStart, passes[pi].gainL, passes[pi].gainR });
                    continue;
                }

                DiffSingerPipeline::RenderOptions ro;
                ro.spkEmbed    = spk;
                ro.centsOffset = passes[pi].cents;

                auto pcm = std::make_shared<juce::AudioBuffer<float>>();
                double startBeat = 0.0;
                const auto res = DiffSingerPipeline::renderPhrase (voice, ph, bpm, *pcm, startBeat, abortFn, ro);
                if (! res.ok)
                {
                    if (res.error == "aborted") { setStatus ("edit detected — restarting"); return; }
                    setStatus ("phrase " + juce::String (idx) + " failed: " + res.error);
                    goto nextPhrase;                       // skip remaining passes of this phrase
                }
                phraseCache[key] = pcm;
                if ((int) phraseCache.size() > 96) phraseCache.clear();
                pieces.push_back ({ pcm, startBeat, passes[pi].gainL, passes[pi].gainR });
            }
            nextPhrase:;
        }

        if (pieces.empty()) { cache.clear(); return; }

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
        out->pcm.setSize (2, (int) (globalEndSec * voice.outputSampleRate) + 64);
        out->pcm.clear();

        for (auto& p : pieces)
        {
            const int at = (int) ((p.startBeat - globalStart) * secPerBeat * voice.outputSampleRate);
            const int n  = juce::jmin (p.pcm->getNumSamples(), out->pcm.getNumSamples() - at);
            if (n > 0 && at >= 0)
            {
                out->pcm.addFromWithRamp (0, at, p.pcm->getReadPointer (0), n, p.gL, p.gL);
                out->pcm.addFromWithRamp (1, at, p.pcm->getReadPointer (0), n, p.gR, p.gR);
            }
        }

        cache.publish (std::move (out));
        setStatus ("ready (" + juce::String ((int) phrases.size()) + " phrases, "
                   + juce::String ((int) passes.size()) + " voice"
                   + (passes.size() > 1 ? "s)" : ")"));
    }

    static juce::int64 phraseHash (const std::vector<VocalNote>& ph,
                                   const juce::String& voiceName, double bpm,
                                   int speaker, double cents)
    {
        juce::int64 h = 1469598103934665603LL;
        auto mix = [&h] (juce::int64 v) { h ^= v; h *= 1099511628211LL; };
        mix (voiceName.hashCode64());
        mix ((juce::int64) std::llround (bpm * 100.0));
        mix (speaker);
        mix ((juce::int64) std::llround (cents * 10.0));
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
    std::atomic<bool> rendering { false }, forceRender { false };
    std::map<juce::int64, std::shared_ptr<juce::AudioBuffer<float>>> phraseCache;
    std::unique_ptr<VocalNote> auditionNote;
    juce::CriticalSection auditionLock;
    juce::String status { "idle" };
    mutable juce::CriticalSection statusLock;
};

} // namespace ss
