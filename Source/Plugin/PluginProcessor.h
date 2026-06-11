#pragma once
// SingScribeProcessor — host glue: FL MIDI capture in, rendered vocal out.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Model/VocalNote.h"
#include "../Model/DiffSingerModelManager.h"
#include "../Model/VocalRenderEngine.h"
#include "../Host/HostNoteCapture.h"
#include "../Text/G2PEngine.h"
#include "../AI/LyricGenerator.h"
#include "../AI/LlamaCppService.h"

namespace ss
{

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

        juce::AudioPlayHead::PositionInfo pos;
        if (auto* ph = getPlayHead())
            if (auto p = ph->getPosition())
                pos = *p;

        capture.processHostBlock (midi, pos, buffer.getNumSamples(), captureHooks);

        if (pos.getIsPlaying() && buffer.getNumSamples() > 0)
        {
            const double ppq = pos.getPpqPosition().orFallback (0.0);
            const double bpm = pos.getBpm().orFallback (120.0);
            renderEngine.cache.pull (buffer.getWritePointer (0),
                                     buffer.getNumSamples(), ppq, bpm, currentSampleRate);
            if (buffer.getNumChannels() > 1)
                buffer.copyFrom (1, 0, buffer, 0, 0, buffer.getNumSamples());
        }

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

    std::atomic<double> lastPpq { 0.0 }, lastBpm { 120.0 };
    std::atomic<bool>   isPlaying { false };
    std::atomic<int>    genreModeId { 1 };          // remembered UI choice

private:
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingScribeProcessor)
};

} // namespace ss
