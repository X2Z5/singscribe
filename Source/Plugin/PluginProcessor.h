#pragma once
// SingScribeProcessor — host glue: FL MIDI capture in, rendered vocal out,
// instant note audition even while the transport is stopped.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Model/VocalNote.h"
#include "../Model/DiffSingerModelManager.h"
#include "../Model/VocalRenderEngine.h"
#include "../Host/HostNoteCapture.h"
#include "../Text/G2PEngine.h"
#include "../AI/LyricGenerator.h"
#include "../AI/LlamaCppService.h"
#include "Theme.h"

namespace ss
{

//==============================================================================
/** One-shot playback of audition renders, mixed into processBlock output. */
class AuditionPlayer
{
public:
    void trigger (std::shared_ptr<juce::AudioBuffer<float>> b, double sourceRate)
    {
        srcRate.store (sourceRate);
        position.store (0.0);
        std::atomic_store (&buffer, std::shared_ptr<const juce::AudioBuffer<float>> (std::move (b)));
    }

    void mixInto (float* L, float* R, int numSamples, double hostRate)
    {
        auto buf = std::atomic_load (&buffer);
        if (buf == nullptr) return;
        const float* src = buf->getReadPointer (0);
        const int avail  = buf->getNumSamples();
        const double ratio = srcRate.load() / juce::jmax (8000.0, hostRate);
        double pos = position.load();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto s0 = (juce::int64) pos;
            if (s0 + 1 >= avail)
            {
                std::shared_ptr<const juce::AudioBuffer<float>> empty;
                std::atomic_store (&buffer, empty);
                return;
            }
            const float frac = (float) (pos - (double) s0);
            const float v = (src[s0] * (1.0f - frac) + src[s0 + 1] * frac) * 0.85f;
            L[i] += v;
            R[i] += v;
            pos += ratio;
        }
        position.store (pos);
    }

private:
    std::shared_ptr<const juce::AudioBuffer<float>> buffer;
    std::atomic<double> position { 0.0 }, srcRate { 44100.0 };
};

//==============================================================================
class SingScribeProcessor : public juce::AudioProcessor
{
public:
    SingScribeProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          capture (sequence),
          renderEngine (sequence, modelManager, g2p),
          lyricGen (sequence, g2p)
    {
        lyricGen.setBackend (std::make_unique<LlamaCppService> (findFirstGguf()));
        modelManager.scanForVoices();

        renderEngine.onAuditionReady = [this] (std::shared_ptr<juce::AudioBuffer<float>> b, double sr)
        {
            audition.trigger (std::move (b), sr);
        };

        captureHooks.onTransport = [this] (double ppq, double bpm, bool playing)
        {
            lastPpq.store (ppq);
            lastBpm.store (bpm);
            isPlaying.store (playing);
        };
        captureHooks.onCapturePass = [this] (int)
        {
            renderEngine.requestRender (lastBpm.load());
        };
    }

    static juce::File llmDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("SingScribe_LLM");
    }

    static juce::File findFirstGguf()
    {
        auto files = llmDirectory().findChildFiles (juce::File::findFiles, false, "*.gguf");
        files.sort();
        return files.isEmpty() ? llmDirectory().getChildFile ("model.gguf") : files[0];
    }

    const Theme& theme() const { return themeAt (themeIndex.load()); }

    //==========================================================================
    void prepareToPlay (double sampleRate, int) override
    {
        currentSampleRate = sampleRate;
        capture.prepare (sampleRate);
        renderEngine.requestRender (lastBpm.load());
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (n == 0) { midi.clear(); return; }

        juce::AudioPlayHead::PositionInfo pos;
        if (auto* ph = getPlayHead())
            if (auto p = ph->getPosition())
                pos = *p;

        capture.processHostBlock (midi, pos, n, captureHooks);

        float* L = buffer.getWritePointer (0);
        float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;

        if (pos.getIsPlaying())
        {
            const double ppq = pos.getPpqPosition().orFallback (0.0);
            const double bpm = pos.getBpm().orFallback (120.0);
            renderEngine.cache.pull (L, R, n, ppq, bpm, currentSampleRate);
        }

        audition.mixInto (L, R, n, currentSampleRate);   // audible even when stopped
        midi.clear();
    }

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                  { return true; }
    const juce::String getName() const override      { return "SingScribe"; }
    bool acceptsMidi() const override                { return true; }
    bool producesMidi() const override               { return false; }
    double getTailLengthSeconds() const override     { return 0.0; }
    int getNumPrograms() override                    { return 1; }
    int getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override            {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        juce::ValueTree vt ("SingScribe");
        if (auto v = modelManager.getActiveVoice())
            vt.setProperty ("voice", v->name, nullptr);
        vt.setProperty ("genreMode", genreModeId.load(), nullptr);
        vt.setProperty ("speaker",   renderEngine.speakerIndex.load(), nullptr);
        vt.setProperty ("unison",    renderEngine.unisonMode.load(), nullptr);
        vt.setProperty ("theme",     themeIndex.load(), nullptr);
        vt.addChild (sequence.toValueTree(), -1, nullptr);
        if (auto xml = vt.createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
        {
            const auto vt = juce::ValueTree::fromXml (*xml);
            if (! vt.isValid()) return;
            genreModeId.store ((int) vt.getProperty ("genreMode", 1));
            renderEngine.speakerIndex.store ((int) vt.getProperty ("speaker", 0));
            renderEngine.unisonMode.store ((int) vt.getProperty ("unison", 0));
            themeIndex.store ((int) vt.getProperty ("theme", 0));
            const auto seq = vt.getChildWithName ("sequence");
            if (seq.isValid())
                sequence.restoreFromValueTree (seq);
            const juce::String voiceName = vt.getProperty ("voice", "").toString();
            if (voiceName.isNotEmpty())
            {
                const int idx = modelManager.indexOfVoice (voiceName);
                if (idx >= 0) modelManager.requestLoad (idx);
            }
            renderEngine.requestRender (lastBpm.load());
        }
    }

    //==========================================================================
    VocalSequence            sequence;
    DiffSingerModelManager   modelManager;
    G2PEngine                g2p;
    HostNoteCapture          capture;
    HostNoteCapture::Hooks   captureHooks;
    VocalRenderEngine        renderEngine;
    LyricGenerator           lyricGen;
    AuditionPlayer           audition;

    std::atomic<double> lastPpq { 0.0 }, lastBpm { 120.0 };
    std::atomic<bool>   isPlaying { false };
    std::atomic<int>    genreModeId { 1 };
    std::atomic<int>    themeIndex { 0 };

private:
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingScribeProcessor)
};

} // namespace ss
