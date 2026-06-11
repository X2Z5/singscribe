#pragma once
// G2PEngine — grapheme→phoneme conversion + the EN↔JP transliteration bridge.
// Pure text processing: safe on message or worker threads.
#include <juce_core/juce_core.h>
#include "../Model/VocalNote.h"
#include <map>

namespace ss
{

//==============================================================================
class G2PBackend
{
public:
    virtual ~G2PBackend() = default;
    virtual juce::String language() const = 0;
    virtual juce::StringArray splitSyllables (const juce::String& text) const = 0;
    virtual juce::StringArray phonemize (const juce::String& syllable) const = 0;
};

//==============================================================================
class EnglishG2P : public G2PBackend
{
public:
    juce::String language() const override { return "eng"; }

    /** Optional CMUdict-style file ("WORD  W ER1 D"); stress digits stripped. */
    void loadDictionary (const juce::File& f)
    {
        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());
        for (const auto& ln : lines)
        {
            if (ln.startsWith (";;;")) continue;
            auto parts = juce::StringArray::fromTokens (ln, " \t", {});
            parts.removeEmptyStrings();
            if (parts.size() < 2) continue;
            juce::StringArray phs;
            for (int i = 1; i < parts.size(); ++i)
                phs.add (parts[i].retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
            dict[parts[0].toLowerCase()] = phs;
        }
    }

    juce::StringArray splitSyllables (const juce::String& text) const override
    {
        juce::StringArray out;
        for (const auto& w : juce::StringArray::fromTokens (text, " ,.!?;:\n\t", {}))
            if (w.isNotEmpty())
                out.addArray (splitWordToSyllables (w.toLowerCase()));
        return out;
    }

    juce::StringArray phonemize (const juce::String& syllable) const override
    {
        const auto key = syllable.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz'");
        if (auto it = dict.find (key); it != dict.end())
            return it->second;
        return letterToSound (key);
    }

    juce::StringArray phonemizeWord (const juce::String& word) const { return phonemize (word); }

    static juce::StringArray splitWordToSyllables (const juce::String& w)
    {
        static const juce::String vowels ("aeiouy");
        juce::StringArray sylls;
        juce::String cur;
        bool sawVowel = false;
        for (auto c : w)
        {
            const bool isV = vowels.containsChar (c);
            if (sawVowel && ! isV && cur.length() > 1)
            {
                sylls.add (cur);
                cur = juce::String::charToString (c);
                sawVowel = false;
                continue;
            }
            cur += juce::String::charToString (c);
            sawVowel = sawVowel || isV;
        }
        if (cur.isNotEmpty()) sylls.add (cur);
        if (sylls.isEmpty())  sylls.add (w);
        return sylls;
    }

private:
    /** Rough letter-to-sound for out-of-dictionary words. */
    static juce::StringArray letterToSound (juce::String s)
    {
        // silent trailing e ("make", "time")
        if (s.length() > 2 && s.endsWith ("e")
            && ! juce::String ("aeiou").containsChar (s[s.length() - 2]))
            s = s.dropLastCharacters (1);

        juce::StringArray ph;
        int i = 0;
        auto two   = [&] { return s.substring (i, i + 2); };
        auto three = [&] { return s.substring (i, i + 3); };
        while (i < s.length())
        {
            const auto t = three();
            if      (t == "igh") { ph.add ("AY"); i += 3; continue; }
            else if (t == "ght") { ph.add ("T");  i += 3; continue; }

            const auto d = two();
            if      (d == "ch") { ph.add ("CH"); i += 2; }
            else if (d == "sh") { ph.add ("SH"); i += 2; }
            else if (d == "th") { ph.add ("TH"); i += 2; }
            else if (d == "ng") { ph.add ("NG"); i += 2; }
            else if (d == "ph") { ph.add ("F");  i += 2; }
            else if (d == "ck") { ph.add ("K");  i += 2; }
            else if (d == "qu") { ph.add ("K"); ph.add ("W"); i += 2; }
            else if (d == "ee" || d == "ea") { ph.add ("IY"); i += 2; }
            else if (d == "oo") { ph.add ("UW"); i += 2; }
            else if (d == "ou" || d == "ow") { ph.add ("AW"); i += 2; }
            else if (d == "ai" || d == "ay") { ph.add ("EY"); i += 2; }
            else
            {
                const auto c = s[i];
                if (c == 'c' && i + 1 < s.length()
                    && juce::String ("eiy").containsChar (s[i + 1]))
                { ph.add ("S"); ++i; continue; }       // soft c: city, ice
                switch (c)
                {
                    case 'a': ph.add ("AE"); break;  case 'e': ph.add ("EH"); break;
                    case 'i': ph.add ("IH"); break;  case 'o': ph.add ("AA"); break;
                    case 'u': ph.add ("AH"); break;  case 'y': ph.add ("IY"); break;
                    case 'b': ph.add ("B");  break;  case 'c': ph.add ("K");  break;
                    case 'd': ph.add ("D");  break;  case 'f': ph.add ("F");  break;
                    case 'g': ph.add ("G");  break;  case 'h': ph.add ("HH"); break;
                    case 'j': ph.add ("JH"); break;  case 'k': ph.add ("K");  break;
                    case 'l': ph.add ("L");  break;  case 'm': ph.add ("M");  break;
                    case 'n': ph.add ("N");  break;  case 'p': ph.add ("P");  break;
                    case 'q': ph.add ("K");  break;  case 'r': ph.add ("R");  break;
                    case 's': ph.add ("S");  break;  case 't': ph.add ("T");  break;
                    case 'v': ph.add ("V");  break;  case 'w': ph.add ("W");  break;
                    case 'x': ph.add ("K"); ph.add ("S"); break;
                    case 'z': ph.add ("Z");  break;
                    default: break;
                }
                ++i;
            }
        }
        if (ph.isEmpty()) ph.add ("AH");
        return ph;
    }

    std::map<juce::String, juce::StringArray> dict;
};

//==============================================================================
class JapaneseG2P : public G2PBackend
{
public:
    juce::String language() const override { return "jpn"; }

    juce::StringArray splitSyllables (const juce::String& text) const override
    {
        juce::StringArray out;
        for (const auto& w : juce::StringArray::fromTokens (text, " ,.!?;:\n\t", {}))
            if (w.isNotEmpty())
                out.addArray (splitRomajiMorae (kanaToRomaji (w).toLowerCase()));
        return out;
    }

    /** Hiragana/katakana → romaji (kanji is skipped — ask the AI for romaji). */
    static juce::String kanaToRomaji (const juce::String& in)
    {
        static const char* tbl[86] = {
            "a","a","i","i","u","u","e","e","o","o",
            "ka","ga","ki","gi","ku","gu","ke","ge","ko","go",
            "sa","za","shi","ji","su","zu","se","ze","so","zo",
            "ta","da","chi","ji","","tsu","zu","te","de","to","do",
            "na","ni","nu","ne","no",
            "ha","ba","pa","hi","bi","pi","fu","bu","pu","he","be","pe","ho","bo","po",
            "ma","mi","mu","me","mo",
            "!ya","ya","!yu","yu","!yo","yo",
            "ra","ri","ru","re","ro",
            "wa","wa","i","e","o","n","bu","ka","ke" };

        juce::String out, lastMora;
        auto flush = [&] { out += lastMora; lastMora.clear(); };

        for (int i = 0; i < in.length(); ++i)
        {
            juce::juce_wchar c = in[i];
            if (c >= 0x30A1 && c <= 0x30F6) c = (juce::juce_wchar) (c - 0x60); // katakana → hiragana
            if (c == 0x30FC)                                                   // ー long vowel
            {
                if (lastMora.isNotEmpty())
                {
                    const auto v = lastMora.getLastCharacter();
                    flush();
                    lastMora = juce::String::charToString (v);
                }
                continue;
            }
            if (c >= 0x3041 && c <= 0x3096)
            {
                const char* r = tbl[(int) c - 0x3041];
                if (r[0] == '\0') continue;                                    // っ sokuon
                if (r[0] == '!')                                               // small ya/yu/yo
                {
                    if (lastMora.endsWithChar ('i'))
                    {
                        lastMora = lastMora.dropLastCharacters (1) + juce::String (r + 1);
                        lastMora = lastMora.replace ("shy", "sh")
                                           .replace ("chy", "ch")
                                           .replace ("jy",  "j");
                    }
                    continue;
                }
                flush();
                lastMora = r;
            }
            else if (c < 128)
            {
                flush();
                out += juce::String::charToString (c);
            }
            // kanji / other scripts: skipped
        }
        flush();
        return out;
    }

    juce::StringArray phonemize (const juce::String& mora) const override
    {
        static const juce::StringArray digraphs { "sh","ch","ts","ky","gy","ny","hy","by","py","my","ry","dz" };
        const auto m = mora.toLowerCase();
        if (m == "n" || m == "nn") return { "N" };
        if (m == "-")              return {};
        for (const auto& d : digraphs)
            if (m.startsWith (d) && m.length() > d.length())
                return { d, m.substring (d.length()) };
        if (m.length() >= 2 && ! isVowel (m[0]))
            return { m.substring (0, 1), m.substring (1) };
        return { m };
    }

    static bool isVowel (juce::juce_wchar c)
    {
        return c=='a' || c=='e' || c=='i' || c=='o' || c=='u';
    }

    static juce::StringArray splitRomajiMorae (const juce::String& s)
    {
        juce::StringArray out;
        int i = 0;
        while (i < s.length())
        {
            if (s[i] == 'n' && (i + 1 >= s.length() || ! isVowel (s[i + 1]) ))
            { out.add ("n"); ++i; continue; }

            int j = i;
            while (j < s.length() && ! isVowel (s[j])) ++j;
            if (j < s.length()) ++j;
            out.add (s.substring (i, j));
            i = j;
        }
        return out;
    }
};

//==============================================================================
/** EN↔JP transliteration so a Japanese voicebank can sing English phrases
    (katakana-style) and vice versa. Coarse by design — v1. */
class TransliterationBridge
{
public:
    bool enabled = true;

    juce::StringArray englishToRomajiMorae (const juce::String& english,
                                            const EnglishG2P& en) const
    {
        juce::StringArray morae;
        for (const auto& w : juce::StringArray::fromTokens (english, " ,.!?;:\n\t", {}))
        {
            if (w.isEmpty()) continue;
            const auto phs = en.phonemizeWord (w.toLowerCase());
            morae.addArray (arpabetToMorae (phs));
        }
        return morae;
    }

    juce::StringArray romajiToEnglishPhonemes (const juce::String& romaji) const
    {
        static const std::map<juce::String, juce::String> consMap = {
            {"k","K"},{"g","G"},{"s","S"},{"sh","SH"},{"z","Z"},{"j","JH"},{"t","T"},
            {"ch","CH"},{"ts","T"},{"d","D"},{"n","N"},{"h","HH"},{"f","F"},{"b","B"},
            {"p","P"},{"m","M"},{"y","Y"},{"r","R"},{"w","W"} };
        static const std::map<juce::juce_wchar, juce::String> vowMap = {
            {'a',"AA"},{'i',"IY"},{'u',"UW"},{'e',"EH"},{'o',"OW"} };

        juce::StringArray out;
        for (const auto& mora : JapaneseG2P::splitRomajiMorae (romaji.toLowerCase()))
        {
            if (mora == "n") { out.add ("N"); continue; }
            juce::String cons = mora.dropLastCharacters (1);
            const auto v = mora.getLastCharacter();
            if (cons.isNotEmpty())
            {
                auto it = consMap.find (cons);
                out.add (it != consMap.end() ? it->second : cons.toUpperCase());
            }
            if (auto it = vowMap.find (v); it != vowMap.end())
                out.add (it->second);
        }
        return out;
    }

private:
    static juce::StringArray arpabetToMorae (const juce::StringArray& phs)
    {
        static const std::map<juce::String, juce::String> cons = {
            {"B","b"},{"CH","ch"},{"D","d"},{"DH","z"},{"F","f"},{"G","g"},{"HH","h"},
            {"JH","j"},{"K","k"},{"L","r"},{"M","m"},{"N","n"},{"NG","n"},{"P","p"},
            {"R","r"},{"S","s"},{"SH","sh"},{"T","t"},{"TH","s"},{"V","b"},{"W","w"},
            {"Y","y"},{"Z","z"},{"ZH","j"} };
        static const std::map<juce::String, juce::String> vows = {
            {"AA","a"},{"AE","a"},{"AH","a"},{"AO","o"},{"AW","au"},{"AY","ai"},
            {"EH","e"},{"ER","aa"},{"EY","ei"},{"IH","i"},{"IY","i"},{"OW","ou"},
            {"OY","oi"},{"UH","u"},{"UW","u"} };

        juce::StringArray morae;
        juce::String pendingCons;

        auto addMora = [&] (const juce::String& m) { morae.add (normalizeMora (m)); };

        auto flushCons = [&] (const juce::String& c)
        {
            if (c.isEmpty()) return;
            if      (c == "t") addMora ("to");
            else if (c == "d") addMora ("do");
            else if (c == "n") addMora ("n");
            else if (c == "ch" || c == "j" || c == "sh") addMora (c == "sh" ? "shu" : c + "i");
            else               addMora (c + "u");                   // ku, su, bu...
        };

        for (const auto& p : phs)
        {
            if (auto cv = cons.find (p); cv != cons.end())
            {
                flushCons (pendingCons);          // consonant cluster → epenthetic vowel
                pendingCons = cv->second;
            }
            else if (auto vv = vows.find (p); vv != vows.end())
            {
                const auto& vphon = vv->second;   // possibly two morae ("ai")
                for (int i = 0; i < vphon.length(); ++i)
                {
                    const auto vch = juce::String::charToString (vphon[i]);
                    if (i == 0 && pendingCons.isNotEmpty())
                    {
                        addMora (pendingCons + vch);
                        pendingCons.clear();
                    }
                    else
                        addMora (vch);
                }
            }
        }
        flushCons (pendingCons);
        return morae;
    }

    /** Map non-standard combos onto the standard Japanese mora set
        (si→shi, ti→chi, tu→tsu, hu→fu, zi→ji...). */
    static juce::String normalizeMora (const juce::String& m)
    {
        static const std::map<juce::String, juce::String> fix = {
            {"si","shi"},{"ti","chi"},{"tu","tsu"},{"hu","fu"},{"zi","ji"},
            {"yi","i"},{"ye","e"},{"wu","u"},{"wi","ui"},{"we","ue"},{"du","zu"},{"di","ji"} };
        if (auto it = fix.find (m); it != fix.end()) return it->second;
        return m;
    }
};

//==============================================================================
class G2PEngine
{
public:
    G2PEngine()
    {
        en = std::make_unique<EnglishG2P>();
        jp = std::make_unique<JapaneseG2P>();

        const auto dictFile = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                  .getChildFile ("SingScribe_LLM").getChildFile ("cmudict.txt");
        if (dictFile.existsAsFile())
            en->loadDictionary (dictFile);
    }

    void setVoiceLanguage (const juce::String& lang)
    {
        const juce::ScopedLock sl (langLock);
        voiceLang = lang.startsWithIgnoreCase ("en") ? "eng" : "jpn";
    }

    juce::String getVoiceLanguage() const
    {
        const juce::ScopedLock sl (langLock);
        return voiceLang;
    }

    TransliterationBridge bridge;

    /** Split typed/generated text into note-sized syllables for the active voice. */
    juce::StringArray splitForNotes (const juce::String& text, const juce::String& textLang) const
    {
        const auto vl = getVoiceLanguage();
        if (bridge.enabled && textLang == "eng" && vl == "jpn")
            return bridge.englishToRomajiMorae (text, *en);
        if (vl == "eng" || textLang == "eng")
            return en->splitSyllables (text);
        return jp->splitSyllables (text);
    }

    /** Fill phonemes for every note that lacks them. Bumps the sequence
        revision ONLY when something was actually filled (no render loops). */
    bool phonemizeSequence (VocalSequence& seq) const
    {
        const auto vl = getVoiceLanguage();
        return seq.mutateIfChanged ([this, &vl] (std::vector<VocalNote>& notes)
        {
            bool changed = false;
            for (auto& n : notes)
            {
                if (n.isRest || n.isMelisma || ! n.phonemes.isEmpty()) continue;
                if (vl == "jpn")
                {
                    auto sylls = JapaneseG2P::splitRomajiMorae (n.lyric.toLowerCase());
                    n.phonemes = jp->phonemize (sylls.isEmpty() ? n.lyric : sylls[0]);
                    if (n.phonemes.isEmpty()) n.phonemes.add ("a");
                }
                else
                {
                    n.phonemes = en->phonemize (n.lyric);
                }
                changed = true;
            }
            return changed;
        });
    }

    static juce::String guessLanguage (const juce::String& text)
    {
        for (auto c : text)
            if (c >= 0x3040 && c <= 0x30FF) return "jpn";
        return "eng";
    }

private:
    std::unique_ptr<EnglishG2P> en;
    std::unique_ptr<JapaneseG2P> jp;
    juce::String voiceLang { "jpn" };
    mutable juce::CriticalSection langLock;
};

} // namespace ss
