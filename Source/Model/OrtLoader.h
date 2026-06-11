#pragma once
// OrtLoader — plugin-safe ONNX Runtime bootstrap.
//
// Plugins can't rely on the host's DLL search path, so we ship the ORT shared
// library INSIDE the .vst3 bundle (CI does the copy) and load it manually:
//   1. dlopen/LoadLibrary the lib found next to the plugin binary
//   2. resolve OrtGetApiBase()
//   3. Ort::InitApi(...)  (enabled by ORT_API_MANUAL_INIT below)
//
// ALWAYS include this header instead of <onnxruntime_cxx_api.h> directly.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <juce_core/juce_core.h>
#include <mutex>

namespace ss
{

class OrtRuntime
{
public:
    /** Idempotent; safe from any non-realtime thread. */
    static bool ensure (juce::String& errorOut)
    {
        static OrtRuntime instance;
        std::call_once (instance.once, [&] { instance.load(); });
        errorOut = instance.error;
        return instance.ok;
    }

private:
    void load()
    {
        const auto names = candidateNames();
        for (const auto& dir : candidateDirs())
        {
            for (const auto& name : names)
            {
                const auto f = dir.getChildFile (name);
                if (! f.existsAsFile()) continue;
                if (lib.open (f.getFullPathName()))
                {
                    using GetBaseFn = const OrtApiBase* (ORT_API_CALL*)();
                    if (auto* fn = (GetBaseFn) lib.getFunction ("OrtGetApiBase"))
                    {
                        if (const auto* base = fn())
                        {
                            if (const auto* api = base->GetApi (ORT_API_VERSION))
                            {
                                Ort::InitApi (api);
                                ok = true;
                                return;
                            }
                            error = "onnxruntime too old for API version "
                                  + juce::String (ORT_API_VERSION);
                            return;
                        }
                    }
                    lib.close();
                }
            }
        }
        error = "onnxruntime library not found next to the plugin binary "
                "(expected inside the .vst3 bundle)";
    }

    static juce::StringArray candidateNames()
    {
       #if JUCE_WINDOWS
        return { "onnxruntime.dll" };
       #elif JUCE_MAC
        return { "libonnxruntime.1.20.1.dylib", "libonnxruntime.dylib" };
       #else
        return { "libonnxruntime.so.1.20.1", "libonnxruntime.so" };
       #endif
    }

    static juce::Array<juce::File> candidateDirs()
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                             .getParentDirectory();
        return { exe,                                            // next to binary
                 exe.getSiblingFile ("Resources"),               // mac bundle resources
                 exe.getParentDirectory(),                       // Contents/
                 exe.getParentDirectory().getParentDirectory() };// alongside .vst3
    }

    juce::DynamicLibrary lib;   // intentionally never closed: lifetime = process
    std::once_flag once;
    bool ok = false;
    juce::String error;
};

} // namespace ss
