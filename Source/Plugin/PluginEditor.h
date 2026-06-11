#pragma once
// SingScribeEditor — tabs: VOICE | PIANO ROLL | AI LYRICIST.
#include "PluginProcessor.h"

namespace ss
{

juce::MidiFile buildMidiFromSequence (VocalSequence&, double bpm);
int importMidiIntoSequence (const juce::File&, VocalSequence&);

//==============================================================================
class PianoRollCanvas : public juce::Component,
                        private juce::Timer
{
public:
    explicit PianoRollCanvas (SingScribeProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (true);
        lyricEdit.setVisible (false);
        lyricEdit.onReturnKey   = [this] { commitLyricEdit(); };
        lyricEdit.onEscapeKey   = [this] { lyricEdit.setVisible (false); };
        lyricEdit.onFocusLost   = [this] { commitLyricEdit(); };
        addChildComponent (lyricEdit);
        updateCanvasSize();
        startTimerHz (15);
    }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    // geometry
    static constexpr int   pitchLo = 36, pitchHi = 96;
    static constexpr int   rowH = 14;
    static constexpr float beatToX = 28.0f;

    float  beatToPx  (double b) const { return (float) (b * beatToX); }
    double pxToBeat  (float x)  const { return x / beatToX; }
    int    pitchToY  (int p)    const { return (pitchHi - p) * rowH; }
    int    yToPitch  (int y)    const { return juce::jlimit (pitchLo, pitchHi, pitchHi - y / rowH); }

private:
    void timerCallback() override
    {
        const int rev = proc.sequence.getRevision();
        if (rev != lastSeenRev) { lastSeenRev = rev; updateCanvasSize(); }
        repaint();
    }

    void updateCanvasSize()
    {
        double lastEnd = 16.0;
        for (const auto& n : proc.sequence.snapshot())
            lastEnd = juce::jmax (lastEnd, n.endBeat());
        setSize ((int) ((lastEnd + 8.0) * beatToX), (pitchHi - pitchLo + 1) * rowH);
    }

    const VocalNote* hitTest (juce::Point<int> pos, const std::vector<VocalNote>& notes) const
    {
        for (auto& n : notes)
        {
            juce::Rectangle<float> r (beatToPx (n.startBeat), (float) pitchToY (n.midiPitch),
                                      (float) (n.lengthBeats * beatToX), (float) rowH);
            if (r.contains (pos.toFloat())) return &n;
        }
        return nullptr;
    }

    void commitLyricEdit();
    static double snap (double b) { return std::round (b * 4.0) / 4.0; }

    SingScribeProcessor& proc;
    juce::TextEditor lyricEdit;
    juce::Uuid selectedId, editingId;
    enum class DragMode { none, move, resize };
    DragMode dragMode = DragMode::none;
    VocalNote dragOrig;
    double grabBeatOffset = 0.0;
    int lastSeenRev = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollCanvas)
};

//==============================================================================
/** Small external-drag source: drag this onto FL's piano roll to export MIDI. */
class MidiDragOutSpot : public juce::Component
{
public:
    explicit MidiDragOutSpot (SingScribeProcessor& p) : proc (p)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (juce::Colour (0xff2a2440));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (juce::Colour (0xff59e3ff).withAlpha (hover ? 0.9f : 0.5f));
        g.drawRoundedRectangle (r, 8.0f, 1.2f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText ("DRAG MIDI OUT", getLocalBounds(), juce::Justification::centred);
    }
    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp    (const juce::MouseEvent&) override { dragging = false; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 8) return;
        dragging = true;
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("SingScribe.mid");
        f.deleteFile();
        juce::FileOutputStream os (f);
        if (os.openedOk() && buildMidiFromSequence (proc.sequence, proc.lastBpm.load()).writeTo (os))
        {
            os.flush();
            juce::DragAndDropContainer::performExternalDragDropOfFiles (
                { f.getFullPathName() }, false, this, nullptr);
        }
    }
private:
    SingScribeProcessor& proc;
    bool hover = false, dragging = false;
};

//==============================================================================
class RollTab : public juce::Component,
                private juce::Timer
{
public:
    explicit RollTab (SingScribeProcessor& p)
        : proc (p), canvas (p), dragOut (p)
    {
        viewport.setViewedComponent (&canvas, false);
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        lyricInput.setTextToShowWhenEmpty ("type lyrics here — one syllable lands on each note (\"-\" = hold)",
                                           juce::Colours::grey);
        addAndMakeVisible (lyricInput);

        applyBtn.onClick = [this] { applyLyrics(); };
        addAndMakeVisible (applyBtn);
        addAndMakeVisible (dragOut);

        status.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (status);

        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (76);
        viewport.setBounds (r);
        bar.reduce (12, 6);
        auto row1 = bar.removeFromTop (30);
        lyricInput.setBounds (row1.removeFromLeft (juce::jmax (200, row1.getWidth() - 300)));
        applyBtn.setBounds (row1.removeFromLeft (140).reduced (6, 0));
        dragOut.setBounds (row1.removeFromLeft (150).reduced (2, 0));
        status.setBounds (bar.removeFromTop (24));
    }

private:
    void timerCallback() override
    {
        status.setText ("engine: " + proc.renderEngine.getStatus()
                        + "   |   notes: " + juce::String (proc.sequence.countSingableNotes()),
                        juce::dontSendNotification);
    }

    void applyLyrics()
    {
        const auto text = lyricInput.getText();
        if (text.isEmpty()) return;
        const auto sylls = proc.g2p.splitForNotes (text, G2PEngine::guessLanguage (text));
        const auto leftover = proc.sequence.distributeLyrics (sylls);
        proc.renderEngine.requestRender (proc.lastBpm.load());
        status.setText (leftover.isEmpty()
                            ? "lyrics distributed"
                            : juce::String (leftover.size()) + " syllables didn't fit (add notes)",
                        juce::dontSendNotification);
    }

    SingScribeProcessor& proc;
    PianoRollCanvas canvas;
    juce::Viewport viewport;
    juce::TextEditor lyricInput;
    juce::TextButton applyBtn { "APPLY LYRICS" };
    MidiDragOutSpot dragOut;
    juce::Label status;
};

//==============================================================================
class AILyricistPanel : public juce::Component
{
public:
    explicit AILyricistPanel (SingScribeProcessor& p) : proc (p)
    {
        promptBox.setMultiLine (true);
        promptBox.setReturnKeyStartsNewLine (true);
        promptBox.setTextToShowWhenEmpty ("describe the song: theme, mood, hook idea...",
                                          juce::Colours::grey);
        addAndMakeVisible (promptBox);

        genreMode.addItem ("Traditional Vocaloid", 1);
        genreMode.addItem ("Underground",          2);
        genreMode.setSelectedId (juce::jlimit (1, 2, proc.genreModeId.load()), juce::dontSendNotification);
        genreMode.onChange = [this] { proc.genreModeId.store (genreMode.getSelectedId()); };
        addAndMakeVisible (genreMode);

        generateBtn.onClick = [this] { startGeneration(); };
        addAndMakeVisible (generateBtn);

        console.setMultiLine (true);
        console.setReadOnly (true);
        addAndMakeVisible (console);

        backendLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
        refreshBackendLabel();
        addAndMakeVisible (backendLabel);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        promptBox.setBounds (r.removeFromTop (90));
        auto row = r.removeFromTop (42).reduced (0, 6);
        genreMode.setBounds (row.removeFromLeft (220));
        generateBtn.setBounds (row.removeFromLeft (140).withTrimmedLeft (12));
        backendLabel.setBounds (row.withTrimmedLeft (12));
        console.setBounds (r.withTrimmedTop (8));
    }

private:
    void refreshBackendLabel()
    {
        if (auto* b = proc.lyricGen.getBackend())
            backendLabel.setText (b->backendName(), juce::dontSendNotification);
    }

    void startGeneration()
    {
        console.clear();
        refreshBackendLabel();
        const auto mode = genreMode.getSelectedId() == 2
                              ? LyricRequest::Mode::Underground
                              : LyricRequest::Mode::TraditionalVocaloid;

        LyricGenerator::UICallbacks ui;
        ui.onConsoleAppend = [this] (const juce::String& chunk)
        {
            console.moveCaretToEnd();
            console.insertTextAtCaret (chunk);
        };
        ui.onFinished = [this] (bool ok, const juce::String& statusText)
        {
            console.moveCaretToEnd();
            console.insertTextAtCaret ("\n\n[" + statusText + "]\n");
            generateBtn.setEnabled (true);
            if (ok)
                proc.renderEngine.requestRender (proc.lastBpm.load());
        };

        generateBtn.setEnabled (false);
        const auto lang = proc.modelManager.getActiveVoice() != nullptr
                              ? proc.modelManager.getActiveVoice()->language : juce::String ("jpn");
        proc.lyricGen.generateForPattern (promptBox.getText(), mode, lang, std::move (ui));
    }

    SingScribeProcessor& proc;
    juce::TextEditor promptBox, console;
    juce::ComboBox   genreMode;
    juce::TextButton generateBtn { "GENERATE" };
    juce::Label backendLabel;
};

//==============================================================================
class VoicePanel : public juce::Component,
                   private DiffSingerModelManager::Listener
{
public:
    explicit VoicePanel (SingScribeProcessor& p) : proc (p)
    {
        proc.modelManager.addListener (this);

        rescanBtn.onClick = [this] { proc.modelManager.scanForVoices(); };
        openBtn.onClick = [this]
        {
            auto d = DiffSingerModelManager::defaultVoiceDirectory();
            d.createDirectory();
            d.revealToUser();
        };
        voiceBox.onChange = [this]
        {
            const int idx = voiceBox.getSelectedItemIndex();
            if (idx >= 0) proc.modelManager.requestLoad (idx);
        };

        bridgeToggle.setToggleState (true, juce::dontSendNotification);
        bridgeToggle.onClick = [this]
        {
            proc.g2p.bridge.enabled = bridgeToggle.getToggleState();
        };

        addAndMakeVisible (voiceBox);
        addAndMakeVisible (rescanBtn);
        addAndMakeVisible (openBtn);
        addAndMakeVisible (status);
        addAndMakeVisible (bridgeToggle);
        addAndMakeVisible (hint);
        hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
        hint.setText ("Voices live in Music/DiffSinger_Voices/<VoiceName>/ "
                      "(acoustic .onnx + vocoder .onnx + phonemes.txt). "
                      "GGUF lyric model lives in Music/SingScribe_LLM/.",
                      juce::dontSendNotification);

        refreshList (proc.modelManager.getKnownVoices());
    }

    ~VoicePanel() override { proc.modelManager.removeListener (this); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto row = r.removeFromTop (32);
        voiceBox.setBounds (row.removeFromLeft (380));
        rescanBtn.setBounds (row.removeFromLeft (100).withTrimmedLeft (12));
        openBtn.setBounds (row.removeFromLeft (130).withTrimmedLeft (12));
        status.setBounds (r.removeFromTop (30).withTrimmedTop (8));
        bridgeToggle.setBounds (r.removeFromTop (28));
        hint.setBounds (r.removeFromTop (48));
    }

private:
    void voicesRescanned (const std::vector<DiffSingerModelManager::VoiceInfo>& v) override
    {
        refreshList (v);
    }
    void voiceLoadStarted (const juce::String& n) override
    {
        status.setText ("loading " + n + "...", juce::dontSendNotification);
    }
    void voiceLoadFinished (const juce::String& n, bool ok, const juce::String& err) override
    {
        status.setText (ok ? n + " ready — press play in FL to hear it"
                           : "FAILED: " + err, juce::dontSendNotification);
        if (ok) proc.renderEngine.requestRender (proc.lastBpm.load());
    }

    void refreshList (const std::vector<DiffSingerModelManager::VoiceInfo>& voices)
    {
        voiceBox.clear (juce::dontSendNotification);
        int id = 1;
        for (const auto& v : voices)
        {
            voiceBox.addItem (v.name + (v.valid ? "" : "  (" + v.error + ")"), id);
            voiceBox.setItemEnabled (id, v.valid);
            ++id;
        }
        if (voices.empty())
            status.setText ("no voices found — click OPEN FOLDER and drop a DiffSinger voicebank in",
                            juce::dontSendNotification);
    }

    SingScribeProcessor& proc;
    juce::ComboBox voiceBox;
    juce::TextButton rescanBtn { "RESCAN" }, openBtn { "OPEN FOLDER" };
    juce::Label status, hint;
    juce::ToggleButton bridgeToggle { "Sing English text on a Japanese voice (transliterate)" };
};

//==============================================================================
class SingScribeEditor : public juce::AudioProcessorEditor,
                         public juce::DragAndDropContainer,
                         public juce::FileDragAndDropTarget
{
public:
    explicit SingScribeEditor (SingScribeProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p),
          tabs (juce::TabbedButtonBar::TabsAtTop),
          voicePanel (p), rollTab (p), lyricist (p)
    {
        const auto bg = juce::Colour (0xff141221);
        tabs.addTab ("VOICE",       bg, &voicePanel, false);
        tabs.addTab ("PIANO ROLL",  bg, &rollTab,    false);
        tabs.addTab ("AI LYRICIST", bg, &lyricist,   false);
        tabs.setCurrentTabIndex (1);
        addAndMakeVisible (tabs);
        setSize (980, 600);
        setResizable (true, true);
        setResizeLimits (720, 420, 2400, 1600);
    }

    void resized() override { tabs.setBounds (getLocalBounds()); }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (f.endsWithIgnoreCase (".mid")) return true;
        return false;
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        for (const auto& path : files)
        {
            juce::File f (path);
            if (f.hasFileExtension ("mid"))
                if (importMidiIntoSequence (f, proc.sequence) > 0)
                    proc.renderEngine.requestRender (proc.lastBpm.load());
        }
    }

private:
    SingScribeProcessor& proc;
    juce::TabbedComponent tabs;
    VoicePanel voicePanel;
    RollTab rollTab;
    AILyricistPanel lyricist;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SingScribeEditor)
};

inline juce::AudioProcessorEditor* SingScribeProcessor::createEditor()
{
    return new SingScribeEditor (*this);
}

} // namespace ss
