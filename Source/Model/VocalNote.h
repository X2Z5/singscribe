#pragma once
// VocalNote — the lyric-enabled note + the thread-shared sequence (single source of truth)
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <vector>
#include <atomic>
#include <algorithm>

namespace ss
{

//==============================================================================
struct VocalNote
{
    // -- time/pitch (host PPQ domain: 1.0 = quarter note) --
    int    midiPitch   = 60;
    double startBeat   = 0.0;
    double lengthBeats = 1.0;
    float  velocity    = 0.8f;

    // -- lyric layer --
    juce::String      lyric { "ra" };
    juce::StringArray phonemes;          // filled by G2PEngine, e.g. {"r","a"}
    bool   isMelisma   = false;          // "-" continuation of previous syllable
    bool   isRest      = false;

    // -- expressions --
    float vibratoDepth = 0.35f, vibratoRate = 0.5f;
    float breathiness  = 0.30f, tension     = 0.50f;
    float genderShift  = 0.50f;
    float pitchBendSemis = 0.0f;

    // -- bookkeeping --
    juce::Uuid id;
    bool fromHostCapture = false;

    double endBeat() const noexcept { return startBeat + lengthBeats; }
    bool overlaps (double t0, double t1) const noexcept { return startBeat < t1 && endBeat() > t0; }

    juce::ValueTree toValueTree() const
    {
        juce::ValueTree t ("note");
        t.setProperty ("pitch", midiPitch, nullptr);
        t.setProperty ("start", startBeat, nullptr);
        t.setProperty ("len",   lengthBeats, nullptr);
        t.setProperty ("vel",   velocity, nullptr);
        t.setProperty ("lyric", lyric, nullptr);
        t.setProperty ("ph",    phonemes.joinIntoString (" "), nullptr);
        t.setProperty ("mel",   isMelisma, nullptr);
        t.setProperty ("vibD",  vibratoDepth, nullptr);
        t.setProperty ("vibR",  vibratoRate, nullptr);
        t.setProperty ("bend",  pitchBendSemis, nullptr);
        t.setProperty ("host",  fromHostCapture, nullptr);
        return t;
    }

    static VocalNote fromValueTree (const juce::ValueTree& t)
    {
        VocalNote n;
        n.midiPitch     = (int)    t.getProperty ("pitch", 60);
        n.startBeat     = (double) t.getProperty ("start", 0.0);
        n.lengthBeats   = (double) t.getProperty ("len", 1.0);
        n.velocity      = (float)(double) t.getProperty ("vel", 0.8);
        n.lyric         = t.getProperty ("lyric", "ra").toString();
        n.phonemes      = juce::StringArray::fromTokens (t.getProperty ("ph", "").toString(), " ", {});
        n.phonemes.removeEmptyStrings();
        n.isMelisma     = (bool) t.getProperty ("mel", false);
        n.vibratoDepth  = (float)(double) t.getProperty ("vibD", 0.35);
        n.vibratoRate   = (float)(double) t.getProperty ("vibR", 0.5);
        n.pitchBendSemis= (float)(double) t.getProperty ("bend", 0.0);
        n.fromHostCapture = (bool) t.getProperty ("host", false);
        return n;
    }
};

//==============================================================================
class VocalSequence
{
public:
    void addNote (VocalNote n)
    {
        const juce::ScopedLock sl (lock);
        notes.push_back (std::move (n));
        sortNotes();
        bumpRevision();
    }

    void removeNote (const juce::Uuid& id)
    {
        const juce::ScopedLock sl (lock);
        notes.erase (std::remove_if (notes.begin(), notes.end(),
                       [&] (const VocalNote& n) { return n.id == id; }), notes.end());
        bumpRevision();
    }

    /** General locked mutation: fn receives std::vector<VocalNote>&. */
    template <typename Fn>
    void mutate (Fn&& fn)
    {
        const juce::ScopedLock sl (lock);
        fn (notes);
        sortNotes();
        bumpRevision();
    }

    /** Locked mutation where fn returns true only if it changed something —
        revision is bumped only then (prevents re-render feedback loops). */
    template <typename Fn>
    bool mutateIfChanged (Fn&& fn)
    {
        const juce::ScopedLock sl (lock);
        const bool changed = fn (notes);
        if (changed) { sortNotes(); bumpRevision(); }
        return changed;
    }

    /** Replace captured notes overlapping [t0,t1); user-authored notes survive. */
    void replaceHostRange (double t0, double t1, std::vector<VocalNote> incoming)
    {
        const juce::ScopedLock sl (lock);
        notes.erase (std::remove_if (notes.begin(), notes.end(),
                       [&] (const VocalNote& n)
                       { return n.fromHostCapture && n.overlaps (t0, t1); }), notes.end());
        for (auto& n : incoming)
            notes.push_back (std::move (n));
        sortNotes();
        bumpRevision();
    }

    /** One syllable/word per sequential note; extra notes → melisma "-";
        returns syllables that didn't fit. */
    juce::StringArray distributeLyrics (const juce::StringArray& syllables)
    {
        const juce::ScopedLock sl (lock);
        int si = 0;
        for (auto& n : notes)
        {
            if (n.isRest) continue;
            if (si < syllables.size())
            {
                n.lyric = syllables[si++];
                n.isMelisma = (n.lyric == "-");
                n.phonemes.clear();
            }
            else
            {
                n.lyric = "-";
                n.isMelisma = true;
                n.phonemes.clear();
            }
        }
        bumpRevision();
        juce::StringArray leftover;
        for (; si < syllables.size(); ++si) leftover.add (syllables[si]);
        return leftover;
    }

    std::vector<VocalNote> snapshot (int& revisionOut) const
    {
        const juce::ScopedLock sl (lock);
        revisionOut = revision.load();
        return notes;
    }

    std::vector<VocalNote> snapshot() const { int r; return snapshot (r); }

    int  getRevision() const noexcept { return revision.load(); }
    void bumpRevision() noexcept      { revision.fetch_add (1); }

    int countSingableNotes() const
    {
        const juce::ScopedLock sl (lock);
        int c = 0;
        for (auto& n : notes) if (! n.isRest) ++c;
        return c;
    }

    juce::ValueTree toValueTree() const
    {
        const juce::ScopedLock sl (lock);
        juce::ValueTree t ("sequence");
        for (const auto& n : notes)
            t.addChild (n.toValueTree(), -1, nullptr);
        return t;
    }

    void restoreFromValueTree (const juce::ValueTree& t)
    {
        const juce::ScopedLock sl (lock);
        notes.clear();
        for (int i = 0; i < t.getNumChildren(); ++i)
            if (t.getChild (i).hasType ("note"))
                notes.push_back (VocalNote::fromValueTree (t.getChild (i)));
        sortNotes();
        bumpRevision();
    }

private:
    void sortNotes()
    {
        std::sort (notes.begin(), notes.end(),
                   [] (const VocalNote& a, const VocalNote& b)
                   { return a.startBeat < b.startBeat; });
    }

    std::vector<VocalNote> notes;
    juce::CriticalSection lock;
    std::atomic<int> revision { 0 };
};

} // namespace ss
