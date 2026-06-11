#pragma once
// DiffSingerModelManager — scans the voice folder, loads ONNX sessions on a
// worker thread, exposes the active voice via atomic shared_ptr swap.
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "OrtLoader.h"
#include <memory>
#include <vector>
#include <map>
#include <atomic>

namespace ss
{

//==============================================================================
struct LoadedVoice
{
    juce::String name, language { "jpn" };
    double  outputSampleRate = 44100.0;
    int     hopSize  = 512;
    int64_t cfgSpeedup = 10, cfgDepth = 1000;

    std::unique_ptr<Ort::Session> acoustic;   // tokens+durations+f0 → mel
    std::unique_ptr<Ort::Session> variance;   // loaded if present; unused in v1
    std::unique_ptr<Ort::Session> vocoder;    // mel+f0 → PCM

    std::map<juce::String, int64_t> phonemeMap;   // token → id

    // multi-speaker support (spk_embed): one entry per .emb found in the bank
    juce::StringArray speakerNames;
    std::vector<std::vector<float>> speakerEmbeds;

    const std::vector<float>* embedForSpeaker (int index) const
    {
        if (speakerEmbeds.empty()) return nullptr;
        return &speakerEmbeds[(size_t) juce::jlimit (0, (int) speakerEmbeds.size() - 1, index)];
    }
};

//==============================================================================
class DiffSingerModelManager : private juce::Thread
{
public:
    struct VoiceInfo
    {
        juce::String name, language;
        juce::File   folder, acousticOnnx, varianceOnnx, vocoderOnnx, configFile, dictFile;
        bool   valid = false;
        juce::String error;
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void voicesRescanned (const std::vector<VoiceInfo>&) {}
        virtual void voiceLoadStarted (const juce::String&) {}
        virtual void voiceLoadFinished (const juce::String&, bool, const juce::String&) {}
    };

    // NOTE: 8 MB stack — ORT's graph loading/optimization recurses deeply on
    // large diffusion models; the macOS default side-thread stack (512 KB)
    // overflows and crashes inside libonnxruntime.
    DiffSingerModelManager() : juce::Thread ("SS-ModelLoader", 8 * 1024 * 1024) { startThread(); }

    ~DiffSingerModelManager() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (8000);
    }

    static juce::File defaultVoiceDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("DiffSinger_Voices");
    }

    //==========================================================================
    void scanForVoices (const juce::File& root = defaultVoiceDirectory())
    {
        std::vector<VoiceInfo> found;

        for (const auto& sub : root.findChildFiles (juce::File::findDirectories, false))
        {
            VoiceInfo v;
            v.folder = sub;
            v.name   = sub.getFileName();

            for (const auto& f : sub.findChildFiles (juce::File::findFiles, true, "*.onnx"))
            {
                const auto n = f.getFileName().toLowerCase();
                if      (n.contains ("acoustic"))                          v.acousticOnnx = f;
                else if (n.contains ("variance") || n.contains ("dur"))    v.varianceOnnx = f;
                else if (n.contains ("vocoder") || n.contains ("hifigan")
                                                || n.contains ("nsf"))     v.vocoderOnnx  = f;
                else if (v.acousticOnnx == juce::File())                   v.acousticOnnx = f;
            }

            for (const char* cfg : { "dsconfig.yaml", "character.yaml", "config.yaml" })
                if (sub.getChildFile (cfg).existsAsFile()) { v.configFile = sub.getChildFile (cfg); break; }
            for (const char* d : { "phonemes.txt", "phoneme_list.txt" })
                if (sub.getChildFile (d).existsAsFile())   { v.dictFile = sub.getChildFile (d); break; }
            if (v.dictFile == juce::File())
                for (const auto& f : sub.findChildFiles (juce::File::findFiles, true, "*.txt"))
                    if (f.getFileName().containsIgnoreCase ("phoneme")) { v.dictFile = f; break; }

            if      (v.acousticOnnx == juce::File()) v.error = "no acoustic .onnx found";
            else if (v.vocoderOnnx  == juce::File()) v.error = "no vocoder/nsf_hifigan .onnx in this folder (copy one in)";
            else if (v.dictFile     == juce::File()) v.error = "no phonemes.txt found";
            else                                     v.valid = true;

            if (v.configFile != juce::File())
            {
                const auto cfg = parseFlatYaml (v.configFile);
                if (cfg.count ("language"))  v.language = cfg.at ("language");
                if (cfg.count ("name"))      v.name     = cfg.at ("name");
            }
            found.push_back (std::move (v));
        }

        {
            const juce::ScopedLock sl (infoLock);
            knownVoices = found;
        }
        listeners.call ([&] (Listener& l) { l.voicesRescanned (found); });
    }

    std::vector<VoiceInfo> getKnownVoices() const
    {
        const juce::ScopedLock sl (infoLock);
        return knownVoices;
    }

    /** Find a voice index by display name (used by state restore). */
    int indexOfVoice (const juce::String& name) const
    {
        const juce::ScopedLock sl (infoLock);
        for (size_t i = 0; i < knownVoices.size(); ++i)
            if (knownVoices[i].name == name) return (int) i;
        return -1;
    }

    void requestLoad (int voiceIndex)
    {
        pendingLoadIndex.store (voiceIndex);
        notify();
    }

    std::shared_ptr<const LoadedVoice> getActiveVoice() const
    {
        return std::atomic_load_explicit (&activeVoice, std::memory_order_acquire);
    }

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    /** Flat "key: value" YAML subset parser — enough for dsconfig/character. */
    static std::map<juce::String, juce::String> parseFlatYaml (const juce::File& f)
    {
        std::map<juce::String, juce::String> out;
        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());
        for (auto line : lines)
        {
            line = line.upToFirstOccurrenceOf ("#", false, false);
            const int colon = line.indexOfChar (':');
            if (colon <= 0) continue;
            auto key = line.substring (0, colon).trim();
            auto val = line.substring (colon + 1).trim().unquoted();
            if (key.isNotEmpty() && ! key.startsWith ("-") && val.isNotEmpty())
                out[key] = val;
        }
        return out;
    }

private:
    //==========================================================================
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            if (threadShouldExit()) return;

            const int idx = pendingLoadIndex.exchange (-1);
            if (idx < 0) continue;

            VoiceInfo info;
            {
                const juce::ScopedLock sl (infoLock);
                if (idx >= (int) knownVoices.size() || ! knownVoices[(size_t) idx].valid) continue;
                info = knownVoices[(size_t) idx];
            }

            juce::MessageManager::callAsync ([this, n = info.name]
                { listeners.call ([&] (Listener& l) { l.voiceLoadStarted (n); }); });

            juce::String error;
            auto voice = buildVoice (info, error);
            const bool ok = (voice != nullptr);
            if (ok)
                std::atomic_store_explicit (&activeVoice,
                    std::shared_ptr<const LoadedVoice> (std::move (voice)),
                    std::memory_order_release);

            juce::MessageManager::callAsync ([this, n = info.name, ok, error]
                { listeners.call ([&] (Listener& l) { l.voiceLoadFinished (n, ok, error); }); });
        }
    }

    std::shared_ptr<LoadedVoice> buildVoice (const VoiceInfo& info, juce::String& errorOut)
    {
        if (! OrtRuntime::ensure (errorOut))
            return nullptr;

        try
        {
            if (env == nullptr)
                env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "SingScribe");

            auto v = std::make_shared<LoadedVoice>();
            v->name     = info.name;
            v->language = info.language.isNotEmpty() ? info.language : "jpn";

            if (info.configFile != juce::File())
            {
                const auto cfg = parseFlatYaml (info.configFile);
                if (cfg.count ("sample_rate")) v->outputSampleRate = cfg.at ("sample_rate").getDoubleValue();
                if (cfg.count ("hop_size"))    v->hopSize    = cfg.at ("hop_size").getIntValue();
                if (cfg.count ("speedup"))     v->cfgSpeedup = (int64_t) cfg.at ("speedup").getLargeIntValue();
                if (cfg.count ("depth"))       v->cfgDepth   = (int64_t) cfg.at ("depth").getLargeIntValue();
            }

            // phoneme inventory: one token per line, id = line index
            juce::StringArray lines;
            lines.addLines (info.dictFile.loadFileAsString());
            int64_t id = 0;
            for (auto ln : lines)
            {
                ln = ln.trim();
                if (ln.isNotEmpty())
                    v->phonemeMap[ln] = id++;
            }
            if (v->phonemeMap.empty()) { errorOut = "phoneme list is empty"; return nullptr; }

            // speaker embeddings (.emb = raw float32 or .npy) for spk_embed models
            for (const auto& f : info.folder.findChildFiles (juce::File::findFiles, true, "*.emb"))
            {
                auto data = loadEmbedFile (f);
                if (! data.empty())
                {
                    v->speakerNames.add (f.getFileNameWithoutExtension());
                    v->speakerEmbeds.push_back (std::move (data));
                }
            }

            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumCpus() / 2));
            // BASIC instead of ALL: extended graph fusion recurses hard on big
            // diffusion graphs (slow load + deep stacks) for minimal gain here.
            opts.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_BASIC);

            v->acoustic = makeSession (info.acousticOnnx, opts);
            v->vocoder  = makeSession (info.vocoderOnnx,  opts);
            if (info.varianceOnnx != juce::File())
                v->variance = makeSession (info.varianceOnnx, opts);   // reserved for v2

            return v;
        }
        catch (const Ort::Exception& e) { errorOut = juce::String ("ONNX: ") + e.what(); }
        catch (const std::exception& e) { errorOut = e.what(); }
        return nullptr;
    }

    /** .emb files are raw little-endian float32 vectors, sometimes .npy. */
    static std::vector<float> loadEmbedFile (const juce::File& f)
    {
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb) || mb.getSize() < 8)
            return {};
        const auto* bytes = (const char*) mb.getData();
        size_t offset = 0;
        if (mb.getSize() > 10 && memcmp (bytes, "\x93NUMPY", 6) == 0)
        {
            const auto headerLen = (size_t) juce::ByteOrder::littleEndianShort (bytes + 8);
            offset = 10 + headerLen;                       // skip npy v1 header
            if (offset >= mb.getSize()) return {};
        }
        const size_t n = (mb.getSize() - offset) / sizeof (float);
        if (n < 8 || n > 4096) return {};
        std::vector<float> out (n);
        memcpy (out.data(), bytes + offset, n * sizeof (float));
        return out;
    }

    std::unique_ptr<Ort::Session> makeSession (const juce::File& f, Ort::SessionOptions& opts)
    {
       #if JUCE_WINDOWS
        return std::make_unique<Ort::Session> (*env, f.getFullPathName().toWideCharPointer(), opts);
       #else
        return std::make_unique<Ort::Session> (*env, f.getFullPathName().toRawUTF8(), opts);
       #endif
    }

    //==========================================================================
    std::unique_ptr<Ort::Env> env;                   // created after OrtRuntime::ensure
    std::shared_ptr<const LoadedVoice> activeVoice;
    std::vector<VoiceInfo> knownVoices;
    mutable juce::CriticalSection infoLock;
    std::atomic<int> pendingLoadIndex { -1 };
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiffSingerModelManager)
};

} // namespace ss
