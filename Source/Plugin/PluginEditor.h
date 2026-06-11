#pragma once
// SingScribeEditor — tabs: VOICE | PIANO ROLL | AI LYRICIST. Theme-aware.
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
        lyricEdit.onReturnKey = [this] { commitLyricEdit(); };
        lyricEdit.onEscapeKey = [this] { lyricEdit.setVisible (false); };
        lyricEdit.onFocusLost = [this] { commitLyricEdit(); };
        addChildComponent (lyricEdit);
        updateCanvasSize();
        startTimerHz (15);
    }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int   pitchLo = 36, pitchHi = 96;
    static constexpr int   rowH = 14;
    static constexpr float beatToX = 28.0f;

    float  beatToPx (double b) const { return (float) (b * beatToX); }
    double pxToBeat (float x)  const { return x / beatToX; }
    int    pitchToY (int p)    const { return (pitchHi - p) * rowH; }
    int    yToPitch (int y)    const { return juce::jlimit (pitchLo, pitchHi, pitchHi - y / rowH); }

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
    void auditionSelected();
    static double snap (double b) { return std::round (b * 4.0) / 4.0; }

    SingScribeProcessor& proc;
    juce::TextEditor lyricEdit;
    juce::Uuid selectedId, editingId;
    enum class DragMode { none, move, resize };
    DragMode dragMode = DragMode::none;
    bool dragChanged = false;
    VocalNote dragOrig;
    double grabBeatOffset = 0.0;
    int lastSeenRev = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollCanvas)
};

//==============================================================================
class MidiDragOutSpot : public juce::Component
{
public:
    explicit MidiDragOutSpot (SingScribeProcessor& p) : proc (p)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }
    void paint (juce::Graphics& g) override
    {
        const auto& th = proc.theme();
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (th.panel);
        g.fillRoundedRectangle (r, 9.0f);
        g.setColour (th.accent2.withAlpha (hover ? 0.95f : 0.55f));
        g.drawRoundedRectangle (r, 9.0f, 1.3f);
        g.setColour (th.text.withAlpha (0.9f));
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

        lyricInput.setTextToShowWhenEmpty ("type lyrics — one syllable per note (\"-\" = hold). English or romaji/kana.",
                                           juce::Colours::grey);
        addAndMakeVisible (lyricInput);

        applyBtn.onClick = [this] { applyLyrics(); };
        addAndMakeVisible (applyBtn);

        translateBtn.onClick = [this] { translateLyrics(); };
        translateBtn.setTooltip ("AI-translate the text to Japanese (romaji), then put it on the notes");
        addAndMakeVisible (translateBtn);

        addAndMakeVisible (dragOut);
        addAndMakeVisible (status);
        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromBottom (78);
        viewport.setBounds (r);
        bar.reduce (12, 6);
        auto row1 = bar.removeFromTop (30);
        lyricInput.setBounds (row1.removeFromLeft (juce::jmax (180, row1.getWidth() - 460)));
        applyBtn.setBounds     (row1.removeFromLeft (120).reduced (4, 0));
        translateBtn.setBounds (row1.removeFromLeft (180).reduced (4, 0));
        dragOut.setBounds      (row1.removeFromLeft (150).reduced (2, 0));
        status.setBounds (bar.removeFromTop (26));
    }

private:
    void timerCallback() override
    {
        status.setColour (juce::Label::textColourId, proc.theme().textDim);
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

    void translateLyrics()
    {
        translateBtn.setEnabled (false);
        LyricGenerator::UICallbacks ui;
        ui.onConsoleAppend = {};
        ui.onFinished = [this] (bool ok, const juce::String& s)
        {
            translateBtn.setEnabled (true);
            status.setText (ok ? "translated to Japanese + distributed" : s,
                            juce::dontSendNotification);
            if (ok) proc.renderEngine.requestRender (proc.lastBpm.load());
        };
        proc.lyricGen.translateAndApply (lyricInput.getText(), std::move (ui));
    }

    SingScribeProcessor& proc;
    PianoRollCanvas canvas;
    juce::Viewport viewport;
    juce::TextEditor lyricInput;
    juce::TextButton applyBtn { "APPLY LYRICS" }, translateBtn { "TRANSLATE \xe2\x86\x92 JAPANESE" };
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
        backendLabel.setColour (juce::Label::textColourId, proc.theme().textDim);
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

        speakerBox.onChange = [this]
        {
            proc.renderEngine.speakerIndex.store (juce::jmax (0, speakerBox.getSelectedItemIndex()));
            proc.renderEngine.requestRender (proc.lastBpm.load());
            previewVoice();
        };

        unisonBox.addItem ("Solo",     1);
        unisonBox.addItem ("Unison x2", 2);
        unisonBox.addItem ("Unison x3", 3);
        unisonBox.setSelectedId (proc.renderEngine.unisonMode.load() + 1, juce::dontSendNotification);
        unisonBox.onChange = [this]
        {
            proc.renderEngine.unisonMode.store (unisonBox.getSelectedId() - 1);
            proc.renderEngine.requestRender (proc.lastBpm.load());
        };

        for (const auto& t : themes())
            themeBox.addItem (t.name, themeBox.getNumItems() + 1);
        themeBox.setSelectedId (proc.themeIndex.load() + 1, juce::dontSendNotification);
        themeBox.onChange = [this]
        {
            proc.themeIndex.store (themeBox.getSelectedId() - 1);
            if (auto* top = getTopLevelComponent()) top->repaint();
            getParentComponent()->repaint();
        };

        previewBtn.onClick = [this] { previewVoice(); };

        addAndMakeVisible (voiceBox);
        addAndMakeVisible (rescanBtn);
        addAndMakeVisible (openBtn);
        addAndMakeVisible (speakerBox);
        addAndMakeVisible (previewBtn);
        addAndMakeVisible (unisonBox);
        addAndMakeVisible (themeBox);
        addAndMakeVisible (status);
        addAndMakeVisible (bridgeToggle);
        addAndMakeVisible (hint);

        bridgeToggle.setToggleState (true, juce::dontSendNotification);
        bridgeToggle.onClick = [this] { proc.g2p.bridge.enabled = bridgeToggle.getToggleState(); };

        hint.setText ("Voices: Music/DiffSinger_Voices/<Name>/ (acoustic + vocoder .onnx, phonemes.txt, .emb speakers). "
                      "Lyric AI: a .gguf in Music/SingScribe_LLM/. Click a speaker to hear a preview.",
                      juce::dontSendNotification);

        refreshList (proc.modelManager.getKnownVoices());
        refreshSpeakers();
    }

    ~VoicePanel() override { proc.modelManager.removeListener (this); }

    void paint (juce::Graphics& g) override
    {
        const auto& th = proc.theme();
        g.fillAll (th.bg1);
        status.setColour (juce::Label::textColourId, th.text.withAlpha (0.85f));
        hint.setColour (juce::Label::textColourId, th.textDim);
        bridgeToggle.setColour (juce::ToggleButton::textColourId, th.text.withAlpha (0.8f));
        bridgeToggle.setColour (juce::ToggleButton::tickColourId, th.accent);

        auto label = [&] (const char* t, juce::Component& c)
        {
            g.setColour (th.textDim);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (t, c.getX(), c.getY() - 16, c.getWidth(), 14, juce::Justification::left);
        };
        label ("VOICEBANK", voiceBox);
        label ("SPEAKER / VOICE COLOR", speakerBox);
        label ("UNISON", unisonBox);
        label ("THEME", themeBox);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        r.removeFromTop (18);
        auto row = r.removeFromTop (32);
        voiceBox.setBounds (row.removeFromLeft (340));
        rescanBtn.setBounds (row.removeFromLeft (90).withTrimmedLeft (10));
        openBtn.setBounds (row.removeFromLeft (120).withTrimmedLeft (10));

        r.removeFromTop (24);
        auto row2 = r.removeFromTop (32);
        speakerBox.setBounds (row2.removeFromLeft (240));
        previewBtn.setBounds (row2.removeFromLeft (110).withTrimmedLeft (10));
        unisonBox.setBounds (row2.removeFromLeft (130).withTrimmedLeft (16));
        themeBox.setBounds (row2.removeFromLeft (130).withTrimmedLeft (16));

        status.setBounds (r.removeFromTop (30).withTrimmedTop (8));
        bridgeToggle.setBounds (r.removeFromTop (28));
        hint.setBounds (r.removeFromTop (52));
    }

private:
    void previewVoice()
    {
        VocalNote n;
        n.midiPitch = 67;            // G4
        n.lengthBeats = 1.5;
        n.lyric = "ra";
        proc.renderEngine.requestAudition (n);
    }

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
        status.setText (ok ? n + " ready — click PREVIEW or place a note"
                           : "FAILED: " + err, juce::dontSendNotification);
        refreshSpeakers();
        if (ok)
        {
            proc.renderEngine.requestRender (proc.lastBpm.load());
            previewVoice();
        }
    }

    void refreshSpeakers()
    {
        speakerBox.clear (juce::dontSendNotification);
        if (auto v = proc.modelManager.getActiveVoice())
        {
            int id = 1;
            for (const auto& s : v->speakerNames)
                speakerBox.addItem (s, id++);
            if (v->speakerNames.isEmpty())
                speakerBox.addItem ("(single voice)", 1);
            speakerBox.setSelectedItemIndex (
                juce::jlimit (0, juce::jmax (0, speakerBox.getNumItems() - 1),
                              proc.renderEngine.speakerIndex.load()),
                juce::dontSendNotification);
        }
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
    juce::ComboBox voiceBox, speakerBox, unisonBox, themeBox;
    juce::TextButton rescanBtn { "RESCAN" }, openBtn { "OPEN FOLDER" }, previewBtn { "PREVIEW" };
    juce::Label status, hint;
    juce::ToggleButton bridgeToggle { "Sing English text on a Japanese voice (transliterate)" };
};

//==============================================================================
class SingScribeEditor : public juce::AudioProcessorEditor,
                         public juce::DragAndDropContainer,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    explicit SingScribeEditor (SingScribeProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p),
          tabs (juce::TabbedButtonBar::TabsAtTop),
          voicePanel (p), rollTab (p), lyricist (p)
    {
        applyTheme();
        tabs.addTab ("VOICE",       juce::Colours::transparentBlack, &voicePanel, false);
        tabs.addTab ("PIANO ROLL",  juce::Colours::transparentBlack, &rollTab,    false);
        tabs.addTab ("AI LYRICIST", juce::Colours::transparentBlack, &lyricist,   false);
        tabs.setCurrentTabIndex (1);
        addAndMakeVisible (tabs);
        setSize (980, 600);
        setResizable (true, true);
        setResizeLimits (760, 440, 2400, 1600);
        startTimerHz (4);
    }

    ~SingScribeEditor() override { setLookAndFeel (nullptr); }

    void resized() override { tabs.setBounds (getLocalBounds()); }

    void paint (juce::Graphics& g) override
    {
        const auto& th = proc.theme();
        g.setGradientFill (juce::ColourGradient (th.bg1, 0, 0, th.bg2,
                                                 (float) getWidth(), (float) getHeight(), false));
        g.fillAll();
    }

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
    void timerCallback() override
    {
        if (proc.themeIndex.load() != appliedTheme)
            applyTheme();
    }

    void applyTheme()
    {
        appliedTheme = proc.themeIndex.load();
        const auto& th = themeAt (appliedTheme);

        lnf.setColourScheme (th.light ? juce::LookAndFeel_V4::getLightColourScheme()
                                      : juce::LookAndFeel_V4::getDarkColourScheme());
        lnf.setColour (juce::ComboBox::backgroundColourId, th.panel);
        lnf.setColour (juce::ComboBox::textColourId, th.text);
        lnf.setColour (juce::ComboBox::outlineColourId, th.panelLine);
        lnf.setColour (juce::ComboBox::arrowColourId, th.accent);
        lnf.setColour (juce::PopupMenu::backgroundColourId, th.panel);
        lnf.setColour (juce::PopupMenu::textColourId, th.text);
        lnf.setColour (juce::PopupMenu::highlightedBackgroundColourId, th.accent.withAlpha (0.35f));
        lnf.setColour (juce::TextButton::buttonColourId, th.panel);
        lnf.setColour (juce::TextButton::textColourOffId, th.text);
        lnf.setColour (juce::TextButton::buttonOnColourId, th.accent);
        lnf.setColour (juce::TextEditor::backgroundColourId, th.panel);
        lnf.setColour (juce::TextEditor::textColourId, th.text);
        lnf.setColour (juce::TextEditor::outlineColourId, th.panelLine);
        lnf.setColour (juce::TextEditor::focusedOutlineColourId, th.accent);
        lnf.setColour (juce::Label::textColourId, th.text);
        lnf.setColour (juce::TabbedButtonBar::tabTextColourId, th.textDim);
        lnf.setColour (juce::TabbedButtonBar::frontTextColourId, th.accent);
        lnf.setColour (juce::TabbedComponent::backgroundColourId, juce::Colours::transparentBlack);
        lnf.setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
        lnf.setColour (juce::ScrollBar::thumbColourId, th.accent.withAlpha (0.5f));

        setLookAndFeel (&lnf);
        sendLookAndFeelChange();
        repaint();
    }

    SingScribeProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    int appliedTheme = -1;
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
