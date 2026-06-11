#pragma once
// Theme — switchable color palettes. "Cloud" is the soothe-style soft look.
#include <juce_graphics/juce_graphics.h>
#include <vector>

namespace ss
{

struct Theme
{
    const char* name;
    juce::Colour bg1, bg2;          // window gradient
    juce::Colour panel, panelLine;  // cards / strokes
    juce::Colour accent, accent2;   // primary (buttons/notes), secondary
    juce::Colour text, textDim;
    juce::Colour noteUser, noteHost, playhead;
    bool light = false;
};

inline const std::vector<Theme>& themes()
{
    static const std::vector<Theme> t = {
        { "Night",
          juce::Colour (0xff141221), juce::Colour (0xff1d1830),
          juce::Colour (0xff232033), juce::Colour (0xff3a3550),
          juce::Colour (0xffff5fa2), juce::Colour (0xff59e3ff),
          juce::Colours::white, juce::Colour (0xff9a93b5),
          juce::Colour (0xff59e3ff), juce::Colour (0xffff5fa2), juce::Colour (0xff7dffb0),
          false },
        { "Cloud",   // soothe3-inspired: airy blues, dark readable text
          juce::Colour (0xffdfe8f6), juce::Colour (0xffcfdcf0),
          juce::Colour (0xfff4f7fc), juce::Colour (0xffb9c8e2),
          juce::Colour (0xff5b8fd6), juce::Colour (0xff8fb7e8),
          juce::Colour (0xff2b3a52), juce::Colour (0xff6b7c96),
          juce::Colour (0xff5b8fd6), juce::Colour (0xff7fa8dd), juce::Colour (0xff3c6db3),
          true },
        { "Sakura",
          juce::Colour (0xfffdeef4), juce::Colour (0xfff9dce9),
          juce::Colour (0xfffff8fb), juce::Colour (0xffe8bcd0),
          juce::Colour (0xffe06ba0), juce::Colour (0xff9f7fd1),
          juce::Colour (0xff4a2d3c), juce::Colour (0xff9c7186),
          juce::Colour (0xffe06ba0), juce::Colour (0xff9f7fd1), juce::Colour (0xffd14b86),
          true },
        { "Slate",
          juce::Colour (0xff15191e), juce::Colour (0xff1c2228),
          juce::Colour (0xff232a32), juce::Colour (0xff38424d),
          juce::Colour (0xff63d8a4), juce::Colour (0xffd8c463),
          juce::Colours::white, juce::Colour (0xff8b98a5),
          juce::Colour (0xff63d8a4), juce::Colour (0xffd8c463), juce::Colour (0xff63b0d8),
          false },
    };
    return t;
}

inline const Theme& themeAt (int idx)
{
    const auto& all = themes();
    return all[(size_t) juce::jlimit (0, (int) all.size() - 1, idx)];
}

} // namespace ss
