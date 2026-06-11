#pragma once
// DiffSingerPipeline — one phrase of notes → PCM, via tokens/durations/f0 →
// acoustic.onnx → mel → vocoder.onnx. Runs ONLY on the render thread.
//
// v1 scope: rule-based phoneme durations + note-derived f0 (variance.onnx is
// loaded but not consulted yet). Input discovery is dynamic so the common
// OpenUtau-style DiffSinger acoustic exports load without per-model code.
#include <juce_audio_basics/juce_audio_basics.h>
#include "VocalNote.h"
#include "DiffSingerModelManager.h"
#include <functional>
#include <cmath>

namespace ss
{

class DiffSingerPipeline
{
public:
    struct Result { bool ok = false; juce::String error; };

    struct RenderOptions
    {
        const std::vector<float>* spkEmbed = nullptr;   // required by spk_embed models
        double centsOffset = 0.0;                       // unison detune
    };

    static Result renderPhrase (const LoadedVoice& v,
                                const std::vector<VocalNote>& phrase,   // sorted, non-rest
                                double bpm,
                                juce::AudioBuffer<float>& pcmOut,
                                double& startBeatOut,
                                const std::function<bool()>& shouldAbort,
                                const RenderOptions& ro)
    {
        Result r;
        if (phrase.empty())                      { r.error = "empty phrase"; return r; }
        if (! v.acoustic || ! v.vocoder)         { r.error = "voice sessions missing"; return r; }

        const double secPerBeat = 60.0 / juce::jmax (20.0, bpm);
        const double frameSec   = (double) v.hopSize / v.outputSampleRate;
        const double leadInSec  = 0.20, tailSec = 0.25;

        startBeatOut = phrase.front().startBeat - leadInSec / secPerBeat;

        auto noteStartSec = [&] (const VocalNote& n) { return (n.startBeat - startBeatOut) * secPerBeat; };
        auto noteEndSec   = [&] (const VocalNote& n) { return (n.endBeat()  - startBeatOut) * secPerBeat; };
        auto framesFor    = [&] (double sec) { return (int64_t) juce::jmax (1.0, std::round (sec / frameSec)); };

        const int64_t spId = [&]
        {
            for (const char* s : { "SP", "sp", "pau", "sil" })
                if (auto it = v.phonemeMap.find (s); it != v.phonemeMap.end()) return it->second;
            return (int64_t) 0;
        }();

        int unknown = 0;
        juce::String firstUnknown;
        auto idFor = [&] (const juce::String& ph) -> int64_t
        {
            if (auto it = v.phonemeMap.find (ph); it != v.phonemeMap.end()) return it->second;
            if (auto it = v.phonemeMap.find (ph.toLowerCase()); it != v.phonemeMap.end()) return it->second;
            ++unknown;
            if (firstUnknown.isEmpty()) firstUnknown = ph;
            return spId;
        };

        //==== 1. token + duration stream =====================================
        struct Tok { int64_t id; int64_t frames; };
        std::vector<Tok> toks;
        toks.push_back ({ spId, framesFor (leadInSec) });
        int lastVowelTok = -1, phonemeCount = 0;

        for (size_t ni = 0; ni < phrase.size(); ++ni)
        {
            const auto& n = phrase[ni];

            if (ni > 0)
            {
                const double gap = noteStartSec (n) - noteEndSec (phrase[ni - 1]);
                if (gap > 0.03)
                    toks.push_back ({ spId, framesFor (gap) });
            }

            int64_t noteFrames = framesFor (n.lengthBeats * secPerBeat);

            if (n.isMelisma)
            {
                if (lastVowelTok >= 0) toks[(size_t) lastVowelTok].frames += noteFrames;
                continue;
            }

            juce::StringArray phs = n.phonemes;
            if (phs.isEmpty()) phs.add ("a");
            phonemeCount += phs.size();

            int vowelIdx = phs.size() - 1;
            for (int i = phs.size() - 1; i >= 0; --i)
            {
                const auto p = phs[i].toLowerCase();
                const auto c = p.isNotEmpty() ? p[0] : 'x';
                if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') { vowelIdx = i; break; }
            }

            // onsets: stolen from the previous token's tail (sung-ahead consonants)
            for (int i = 0; i < vowelIdx; ++i)
            {
                int64_t take = framesFor (0.045);
                if (! toks.empty() && toks.back().frames > take + 2)
                    toks.back().frames -= take;
                else
                {
                    take = juce::jmax ((int64_t) 1, juce::jmin (take, noteFrames / 3));
                    noteFrames -= take;
                }
                toks.push_back ({ idFor (phs[i]), juce::jmax ((int64_t) 1, take) });
            }

            // codas: taken from the note tail
            std::vector<Tok> codas;
            int64_t codaTotal = 0;
            for (int i = vowelIdx + 1; i < phs.size(); ++i)
            {
                const int64_t cf = juce::jmax ((int64_t) 1, juce::jmin (framesFor (0.05), noteFrames / 4));
                codas.push_back ({ idFor (phs[i]), cf });
                codaTotal += cf;
            }

            toks.push_back ({ idFor (phs[vowelIdx]), juce::jmax ((int64_t) 2, noteFrames - codaTotal) });
            lastVowelTok = (int) toks.size() - 1;
            for (auto& c : codas) toks.push_back (c);
        }
        toks.push_back ({ spId, framesFor (tailSec) });

        if (phonemeCount > 0 && unknown * 2 > phonemeCount)
        {
            r.error = "phoneme set mismatch: '" + firstUnknown + "' (and "
                    + juce::String (unknown - 1) + " more) missing from this voice's phonemes.txt";
            return r;
        }

        const size_t P = toks.size();
        int64_t F = 0;
        for (auto& t : toks) F += t.frames;
        if (F < 4)                                  { r.error = "phrase too short"; return r; }
        if ((double) F * frameSec > 120.0)          { r.error = "phrase exceeds 120 s"; return r; }

        //==== 2. f0 curve =====================================================
        const double pi = juce::MathConstants<double>::pi;
        std::vector<float> f0 ((size_t) F);

        auto pitchAt = [&] (double tSec) -> double
        {
            const VocalNote* cur = nullptr;
            const VocalNote* prev = nullptr;
            for (const auto& n : phrase)
            {
                if (tSec >= noteStartSec (n) - 1.0e-9 && tSec < noteEndSec (n)) { cur = &n; break; }
                if (noteEndSec (n) <= tSec) prev = &n;
            }
            const VocalNote* ref = cur != nullptr ? cur : (prev != nullptr ? prev : &phrase.front());
            double m = ref->midiPitch + ref->pitchBendSemis;

            if (cur != nullptr)
            {
                const double tIn = tSec - noteStartSec (*cur);
                if (prev != nullptr && prev != cur
                    && noteStartSec (*cur) - noteEndSec (*prev) < 0.05)
                {
                    const double porta = 0.07;
                    if (tIn < porta)
                    {
                        const double pm = prev->midiPitch + prev->pitchBendSemis;
                        const double a  = 0.5 - 0.5 * std::cos (pi * tIn / porta);
                        m = pm + (m - pm) * a;
                    }
                }
                const double vibStart = 0.25;
                if (tIn > vibStart && cur->vibratoDepth > 0.01f)
                {
                    const double ramp = juce::jmin (1.0, (tIn - vibStart) / 0.3);
                    const double rate = 4.0 + cur->vibratoRate * 3.0;
                    m += cur->vibratoDepth * 0.6 * ramp * std::sin (2.0 * pi * rate * (tIn - vibStart));
                }
            }
            return 440.0 * std::pow (2.0, (m - 69.0) / 12.0);
        };

        const double tuneMul = std::pow (2.0, ro.centsOffset / 1200.0);
        for (int64_t f = 0; f < F; ++f)
            f0[(size_t) f] = (float) (pitchAt (((double) f + 0.5) * frameSec) * tuneMul);

        if (shouldAbort && shouldAbort()) { r.error = "aborted"; return r; }

        //==== 3. tensors → acoustic → vocoder ================================
        try
        {
            auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

            std::vector<int64_t> tokIds (P), durs (P);
            for (size_t i = 0; i < P; ++i) { tokIds[i] = toks[i].id; durs[i] = toks[i].frames; }

            const int64_t shapeP[2] = { 1, (int64_t) P };
            const int64_t shapeF[2] = { 1, F };

            std::vector<float>   zerosF ((size_t) F, 0.0f), onesF ((size_t) F, 1.0f);
            std::vector<int64_t> zerosP (P, 0);
            std::vector<float>   spkTiled;                 // kept alive through Run
            int64_t speedupVal = v.cfgSpeedup, depthVal = v.cfgDepth;

            Ort::AllocatorWithDefaultOptions alloc;
            std::vector<std::string> inNames;
            std::vector<Ort::Value>  inVals;

            auto pushF = [&] (const char* nm, float* data)
            {
                inNames.emplace_back (nm);
                inVals.push_back (Ort::Value::CreateTensor<float> (mem, data, (size_t) F, shapeF, 2));
            };

            const size_t nIn = v.acoustic->GetInputCount();
            for (size_t i = 0; i < nIn; ++i)
            {
                const std::string nm = v.acoustic->GetInputNameAllocated (i, alloc).get();
                const auto dims = v.acoustic->GetInputTypeInfo (i)
                                      .GetTensorTypeAndShapeInfo().GetShape();

                if (nm == "tokens")
                {
                    inNames.push_back (nm);
                    inVals.push_back (Ort::Value::CreateTensor<int64_t> (mem, tokIds.data(), P, shapeP, 2));
                }
                else if (nm == "durations" || nm == "ph_dur")
                {
                    inNames.push_back (nm);
                    inVals.push_back (Ort::Value::CreateTensor<int64_t> (mem, durs.data(), P, shapeP, 2));
                }
                else if (nm == "f0")              pushF ("f0", f0.data());
                else if (nm == "speedup" || nm == "depth")
                {
                    auto& val = (nm == "speedup") ? speedupVal : depthVal;
                    inNames.push_back (nm);
                    if (dims.empty())
                        inVals.push_back (Ort::Value::CreateTensor<int64_t> (mem, &val, 1, nullptr, 0));
                    else
                    {
                        static const int64_t one[1] = { 1 };
                        inVals.push_back (Ort::Value::CreateTensor<int64_t> (mem, &val, 1, one, 1));
                    }
                }
                else if (nm == "velocity")        pushF (nm.c_str(), onesF.data());
                else if (nm == "gender" || nm == "energy" || nm == "breathiness"
                      || nm == "tension" || nm == "voicing" || nm == "mouth_opening")
                                                  pushF (nm.c_str(), nm == "voicing" ? onesF.data() : zerosF.data());
                else if (nm == "languages")
                {
                    inNames.push_back (nm);
                    inVals.push_back (Ort::Value::CreateTensor<int64_t> (mem, zerosP.data(), P, shapeP, 2));
                }
                else if (nm == "spk_embed")
                {
                    if (ro.spkEmbed == nullptr || ro.spkEmbed->empty())
                    {
                        r.error = "this voice needs a speaker (spk_embed) but no .emb files "
                                  "were found in the voicebank folder";
                        return r;
                    }
                    int64_t D = (int64_t) ro.spkEmbed->size();
                    if (dims.size() == 3 && dims[2] > 0 && dims[2] != D)
                    {
                        r.error = "speaker embed size " + juce::String ((int) D)
                                + " doesn't match model (" + juce::String ((int) dims[2]) + ")";
                        return r;
                    }
                    spkTiled.resize ((size_t) (F * D));
                    for (int64_t fr = 0; fr < F; ++fr)
                        memcpy (spkTiled.data() + fr * D, ro.spkEmbed->data(), (size_t) D * sizeof (float));
                    const int64_t spkShape[3] = { 1, F, D };
                    inNames.push_back (nm);
                    inVals.push_back (Ort::Value::CreateTensor<float> (mem, spkTiled.data(),
                                          spkTiled.size(), spkShape, 3));
                }
                else
                {
                    r.error = "acoustic model has unsupported input '" + juce::String (nm.c_str())
                            + "' — send me this name and I can add it";
                    return r;
                }
            }

            std::vector<const char*> inPtrs;
            for (auto& s : inNames) inPtrs.push_back (s.c_str());

            const std::string melName = v.acoustic->GetOutputNameAllocated (0, alloc).get();
            const char* melOut[1] = { melName.c_str() };

            auto acOut = v.acoustic->Run (Ort::RunOptions { nullptr },
                                          inPtrs.data(), inVals.data(), inVals.size(),
                                          melOut, 1);

            if (shouldAbort && shouldAbort()) { r.error = "aborted"; return r; }

            // ---- vocoder: feed mel through untouched + a fresh f0 tensor ----
            std::vector<std::string> vNames;
            std::vector<Ort::Value>  vVals;
            const size_t vIn = v.vocoder->GetInputCount();
            for (size_t i = 0; i < vIn; ++i)
            {
                const std::string nm = v.vocoder->GetInputNameAllocated (i, alloc).get();
                if (nm == "f0")
                {
                    vNames.push_back (nm);
                    vVals.push_back (Ort::Value::CreateTensor<float> (mem, f0.data(), (size_t) F, shapeF, 2));
                }
                else        // "mel" (or first non-f0 input)
                {
                    vNames.push_back (nm);
                    vVals.push_back (std::move (acOut[0]));
                }
            }
            std::vector<const char*> vPtrs;
            for (auto& s : vNames) vPtrs.push_back (s.c_str());

            const std::string wavName = v.vocoder->GetOutputNameAllocated (0, alloc).get();
            const char* wavOut[1] = { wavName.c_str() };

            auto voOut = v.vocoder->Run (Ort::RunOptions { nullptr },
                                         vPtrs.data(), vVals.data(), vVals.size(),
                                         wavOut, 1);

            const auto shape = voOut[0].GetTensorTypeAndShapeInfo().GetShape();
            int64_t total = 1;
            for (auto d : shape) total *= juce::jmax ((int64_t) 1, d);
            if (total <= 0) { r.error = "vocoder produced no samples"; return r; }

            const float* w = voOut[0].GetTensorData<float>();
            pcmOut.setSize (1, (int) total);
            pcmOut.copyFrom (0, 0, w, (int) total);
        }
        catch (const Ort::Exception& e) { r.error = juce::String ("ONNX: ") + e.what(); return r; }
        catch (const std::exception& e) { r.error = e.what(); return r; }

        r.ok = true;
        return r;
    }
};

} // namespace ss
