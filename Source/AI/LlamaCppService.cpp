// LlamaCppService.cpp — the only file that includes llama.h.
// Written against llama.cpp tag b9596 (pinned in CMakeLists.txt).
#include "LlamaCppService.h"
#include <llama.h>
#include <string>
#include <vector>

namespace ss
{

//==============================================================================
struct LlamaCppService::Impl
{
    llama_model* model = nullptr;
    const llama_vocab* vocab = nullptr;

    ~Impl()
    {
        if (model != nullptr) llama_model_free (model);
        llama_backend_free();
    }
};

//==============================================================================
LlamaCppService::LlamaCppService (const juce::File& ggufFile)
    : juce::Thread ("SS-LLM"), modelFile (ggufFile)
{
    startThread();
}

LlamaCppService::~LlamaCppService()
{
    cancelFlag.store (true);
    signalThreadShouldExit();
    notify();
    stopThread (15000);
}

bool LlamaCppService::isAvailable() const { return modelFile.existsAsFile(); }

juce::String LlamaCppService::backendName() const
{
    if (! modelFile.existsAsFile())
        return "local: no .gguf found in Music/SingScribe_LLM";
    return "local: " + modelFile.getFileNameWithoutExtension()
         + (modelLoaded.load() ? "" : " (loads on first generate)");
}

void LlamaCppService::generate (const LyricRequest& request,
                                std::function<void (const juce::String&)> onToken,
                                std::function<void (bool, const juce::String&, const juce::String&)> onDone)
{
    cancel();
    {
        const juce::ScopedLock sl (jobLock);
        job = std::make_unique<Job> (Job { request, std::move (onToken), std::move (onDone) });
    }
    cancelFlag.store (false);
    notify();
}

//==============================================================================
void LlamaCppService::run()
{
    while (! threadShouldExit())
    {
        wait (-1);
        if (threadShouldExit()) break;

        std::unique_ptr<Job> j;
        {
            const juce::ScopedLock sl (jobLock);
            j = std::move (job);
        }
        if (j == nullptr) continue;

        juce::String loadError;
        if (! ensureModelLoaded (loadError))
        {
            finish (*j, false, {}, loadError);
            continue;
        }
        runInference (*j);
    }
    impl.reset();   // frees model + backend on this thread
}

bool LlamaCppService::ensureModelLoaded (juce::String& error)
{
    if (impl != nullptr && impl->model != nullptr) return true;
    if (! modelFile.existsAsFile())
    {
        error = "no GGUF model at " + modelFile.getFullPathName()
              + " — put a small instruct model (1-3B recommended) in Music/SingScribe_LLM/";
        return false;
    }

    llama_backend_init();
    auto fresh = std::make_shared<Impl>();

    auto mp = llama_model_default_params();
    mp.n_gpu_layers = 0;                      // CPU-only v1: predictable next to a DAW

    fresh->model = llama_model_load_from_file (modelFile.getFullPathName().toRawUTF8(), mp);
    if (fresh->model == nullptr)
    {
        error = "failed to load GGUF: " + modelFile.getFileName();
        return false;
    }
    fresh->vocab = llama_model_get_vocab (fresh->model);
    impl = std::move (fresh);
    modelLoaded.store (true);
    return true;
}

//==============================================================================
juce::String LlamaCppService::systemPromptFor (const LyricRequest& r)
{
    juce::String constraints;
    constraints << "Write exactly " << r.targetLines << " short lyric lines. ";
    if (! r.syllablesPerLine.empty())
    {
        constraints << "Syllable budget per line: ";
        for (size_t i = 0; i < r.syllablesPerLine.size(); ++i)
            constraints << (i ? ", " : "") << r.syllablesPerLine[i];
        constraints << " — never exceed a line's budget. ";
    }
    if (r.voiceLanguage == "jpn")
        constraints << "Prefer open consonant-vowel syllables (ka, shi, no, ru...) and "
                       "vowel endings so a Japanese voicebank can sing them cleanly. ";
    constraints << "Output ONLY the lyric lines, one per line: no titles, numbering, "
                   "quotes or commentary. All text must be original — never reproduce "
                   "existing song lyrics.";

    if (r.mode == LyricRequest::Mode::TraditionalVocaloid)
        return "You are a lyricist for classic virtual singers in the style of the "
               "Hatsune Miku / Kasane Teto era of vocal synth music: emotional "
               "storytelling, bittersweet and hopeful imagery, themes of memory, "
               "distance, digital hearts and connection. Clear narrative arc, gentle "
               "internal rhyme, flowing singable cadence. " + constraints;

    return "You are a lyricist for underground rage / opium-aesthetic electronic rap "
           "hooks, drawing cadence inspiration from Playboi Carti's 'Music'-era "
           "delivery, Ken Carson's 'A Great Chaos', and most heavily Slayr's "
           "'Halfblood / Bloodluxe' deluxe style: short punchy lines of 3-6 syllables, "
           "hypnotic repetition and chant-like phrasing, dark nocturnal high-energy "
           "imagery, occasional ad-libs in (parentheses), minimal melodic vocabulary "
           "with hard consonant hits. Style inspiration only — strictly original text. "
         + constraints;
}

//==============================================================================
void LlamaCppService::runInference (Job& j)
{
    const std::string sys  = systemPromptFor (j.request).toStdString();
    const std::string user = j.request.userPrompt.isNotEmpty()
                                 ? j.request.userPrompt.toStdString()
                                 : std::string ("Write the lyrics now.");

    // ---- chat-template the prompt (fallback to a plain format) ----
    std::string prompt;
    {
        llama_chat_message msgs[2] = { { "system", sys.c_str() },
                                       { "user",   user.c_str() } };
        const char* tmpl = llama_model_chat_template (impl->model, nullptr);
        if (tmpl != nullptr)
        {
            std::vector<char> buf ((sys.size() + user.size()) * 2 + 1024);
            const int n = llama_chat_apply_template (tmpl, msgs, 2, true,
                                                     buf.data(), (int) buf.size());
            if (n > 0 && n < (int) buf.size())
                prompt.assign (buf.data(), (size_t) n);
        }
        if (prompt.empty())
            prompt = "### System:\n" + sys + "\n\n### User:\n" + user + "\n\n### Assistant:\n";
    }

    // ---- fresh context per request: simple, leak-proof KV state ----
    auto cp = llama_context_default_params();
    cp.n_ctx           = 4096;
    cp.n_threads       = (int32_t) juce::jmax (2, juce::SystemStats::getNumCpus() / 2);
    cp.n_threads_batch = cp.n_threads;

    llama_context* ctx = llama_init_from_model (impl->model, cp);
    if (ctx == nullptr) { finish (j, false, {}, "could not create llama context"); return; }

    auto scp = llama_sampler_chain_default_params();
    llama_sampler* chain = llama_sampler_chain_init (scp);
    llama_sampler_chain_add (chain, llama_sampler_init_top_k (40));
    llama_sampler_chain_add (chain, llama_sampler_init_top_p (0.95f, 1));
    llama_sampler_chain_add (chain, llama_sampler_init_temp (juce::jlimit (0.1f, 1.8f, j.request.temperature)));
    llama_sampler_chain_add (chain, llama_sampler_init_dist ((uint32_t) juce::Time::getMillisecondCounter()));

    auto cleanup = [&] { llama_sampler_free (chain); llama_free (ctx); };

    // ---- tokenize + prefill ----
    int n = llama_tokenize (impl->vocab, prompt.c_str(), (int32_t) prompt.size(),
                            nullptr, 0, true, true);
    if (n == INT32_MIN || n == 0) { cleanup(); finish (j, false, {}, "tokenize failed"); return; }
    std::vector<llama_token> toks ((size_t) std::abs (n));
    n = llama_tokenize (impl->vocab, prompt.c_str(), (int32_t) prompt.size(),
                        toks.data(), (int32_t) toks.size(), true, true);
    if (n <= 0) { cleanup(); finish (j, false, {}, "tokenize failed"); return; }
    toks.resize ((size_t) n);
    if ((int) toks.size() >= (int) cp.n_ctx - j.request.maxTokens)
    { cleanup(); finish (j, false, {}, "prompt too long for context"); return; }

    if (llama_decode (ctx, llama_batch_get_one (toks.data(), (int32_t) toks.size())) != 0)
    { cleanup(); finish (j, false, {}, "prompt decode failed"); return; }

    // ---- token loop ----
    juce::String full, pendingChunk;
    int produced = 0, newlines = 0;
    const int wantLines = juce::jmax (1, j.request.targetLines);

    while (produced < j.request.maxTokens && ! cancelFlag.load() && ! threadShouldExit())
    {
        llama_token tok = llama_sampler_sample (chain, ctx, -1);
        if (llama_vocab_is_eog (impl->vocab, tok))
            break;

        char piece[512];
        const int pn = llama_token_to_piece (impl->vocab, tok, piece, (int) sizeof (piece), 0, false);
        if (pn > 0)
        {
            const auto text = juce::String::fromUTF8 (piece, pn);
            full += text;
            pendingChunk += text;
            newlines = 0;
            for (auto c : full) if (c == '\n') ++newlines;

            if (pendingChunk.length() > 12 || text.contains ("\n"))
            {
                emitToken (j, pendingChunk);
                pendingChunk.clear();
            }
            if (newlines >= wantLines && full.trim().isNotEmpty() && full.endsWith ("\n"))
                break;                                   // got all requested lines
            if (full.contains ("\n\n\n"))
                break;
        }

        if (llama_decode (ctx, llama_batch_get_one (&tok, 1)) != 0)
            break;
        ++produced;
    }

    if (pendingChunk.isNotEmpty()) emitToken (j, pendingChunk);
    cleanup();

    const bool cancelled = cancelFlag.load();
    finish (j, ! cancelled && full.trim().isNotEmpty(), full.trim(),
            cancelled ? "cancelled" : (full.trim().isEmpty() ? "model produced no text" : juce::String()));
}

//==============================================================================
void LlamaCppService::emitToken (Job& j, const juce::String& chunk)
{
    if (j.onToken)
        juce::MessageManager::callAsync ([cb = j.onToken, chunk] { cb (chunk); });
}

void LlamaCppService::finish (Job& j, bool ok, const juce::String& text, const juce::String& err)
{
    if (j.onDone)
        juce::MessageManager::callAsync ([cb = j.onDone, ok, text, err] { cb (ok, text, err); });
}

} // namespace ss
