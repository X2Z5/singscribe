// PluginMain.cpp — piano roll interaction, MIDI import/export, VST3 entry point.
#include "PluginEditor.h"

namespace ss
{

//==============================================================================
// MIDI helpers
//==============================================================================
juce::MidiFile buildMidiFromSequence (VocalSequence& seq, double bpm)
{
    const int ppq = 960;
    juce::MidiMessageSequence track;
    auto tempo = juce::MidiMessage::tempoMetaEvent ((int) std::round (60000000.0 / juce::jmax (20.0, bpm)));
    tempo.setTimeStamp (0.0);
    track.addEvent (tempo);

    for (const auto& n : seq.snapshot())
    {
        if (n.isRest) continue;
        auto lyricMeta = juce::MidiMessage::textMetaEvent (5, n.lyric);
        lyricMeta.setTimeStamp (n.startBeat * ppq);
        track.addEvent (lyricMeta);

        auto on = juce::MidiMessage::noteOn (1, n.midiPitch,
                       (juce::uint8) juce::jlimit (1, 127, (int) (n.velocity * 127.0f)));
        on.setTimeStamp (n.startBeat * ppq);
        track.addEvent (on);
        auto off = juce::MidiMessage::noteOff (1, n.midiPitch);
        off.setTimeStamp (n.endBeat() * ppq);
        track.addEvent (off);
    }
    track.sort();
    track.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);
    mf.addTrack (track);
    return mf;
}

int importMidiIntoSequence (const juce::File& f, VocalSequence& seq)
{
    juce::FileInputStream is (f);
    if (! is.openedOk()) return 0;
    juce::MidiFile mf;
    if (! mf.readFrom (is)) return 0;

    const double tpq = mf.getTimeFormat() > 0 ? (double) mf.getTimeFormat() : 960.0;
    std::vector<VocalNote> imported;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* track = mf.getTrack (t);
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            const auto& ev = track->getEventPointer (i)->message;
            if (! ev.isNoteOn()) continue;
            const double offTs = [&]
            {
                if (auto* off = track->getEventPointer (i)->noteOffObject)
                    return off->message.getTimeStamp();
                return ev.getTimeStamp() + tpq;
            }();

            VocalNote n;
            n.midiPitch   = ev.getNoteNumber();
            n.startBeat   = ev.getTimeStamp() / tpq;
            n.lengthBeats = juce::jmax (0.125, (offTs - ev.getTimeStamp()) / tpq);
            n.velocity    = ev.getFloatVelocity();
            imported.push_back (std::move (n));
        }
    }

    if (! imported.empty())
        seq.mutate ([&imported] (std::vector<VocalNote>& notes)
        {
            notes = std::move (imported);
        });
    return (int) imported.size();
}

//==============================================================================
// PianoRollCanvas
//==============================================================================
void PianoRollCanvas::paint (juce::Graphics& g)
{
    const auto& th = proc.theme();
    g.fillAll (th.bg1);

    for (int p = pitchLo; p <= pitchHi; ++p)
    {
        const int y = pitchToY (p);
        const int pc = p % 12;
        const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
        g.setColour (black ? th.bg2.contrasting (0.04f).withAlpha (0.35f)
                           : th.text.withAlpha (0.025f));
        g.fillRect (0, y, getWidth(), rowH);
        if (pc == 0)
        {
            g.setColour (th.text.withAlpha (0.08f));
            g.fillRect (0, y + rowH - 1, getWidth(), 1);
            g.setColour (th.textDim);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText ("C" + juce::String (p / 12 - 1), 4, y, 30, rowH, juce::Justification::centredLeft);
        }
    }

    const int beats = (int) (getWidth() / beatToX) + 1;
    for (int b = 0; b <= beats; ++b)
    {
        g.setColour (th.text.withAlpha (b % 4 == 0 ? 0.13f : 0.05f));
        g.fillRect (beatToPx (b), 0.0f, 1.0f, (float) getHeight());
    }

    const auto notes = proc.sequence.snapshot();
    for (const auto& n : notes)
    {
        if (n.isRest) continue;
        juce::Rectangle<float> r (beatToPx (n.startBeat), (float) pitchToY (n.midiPitch),
                                  (float) (n.lengthBeats * beatToX) - 1.0f, (float) rowH - 1.0f);
        const auto base = n.fromHostCapture ? th.noteHost : th.noteUser;
        g.setColour (n.isMelisma ? base.withAlpha (0.35f) : base.withAlpha (0.85f));
        g.fillRoundedRectangle (r, 3.0f);
        if (n.id == selectedId)
        {
            g.setColour (th.text);
            g.drawRoundedRectangle (r, 3.0f, 1.6f);
        }
        if (r.getWidth() > 22.0f)
        {
            g.setColour (base.contrasting (0.9f));
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (n.lyric, r.toNearestInt().reduced (3, 0), juce::Justification::centredLeft);
        }
    }

    if (proc.isPlaying.load())
    {
        g.setColour (th.playhead.withAlpha (0.9f));
        g.fillRect (beatToPx (proc.lastPpq.load()), 0.0f, 1.6f, (float) getHeight());
    }
}

void PianoRollCanvas::auditionSelected()
{
    if (selectedId.isNull()) return;
    for (const auto& n : proc.sequence.snapshot())
        if (n.id == selectedId) { proc.renderEngine.requestAudition (n); break; }
}

void PianoRollCanvas::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (lyricEdit.isVisible()) commitLyricEdit();
    dragChanged = false;

    const auto notes = proc.sequence.snapshot();
    if (const auto* hit = hitTest (e.getPosition(), notes))
    {
        if (e.mods.isRightButtonDown())
        {
            proc.sequence.removeNote (hit->id);
            if (selectedId == hit->id) selectedId = juce::Uuid::null();
            dragMode = DragMode::none;
            proc.renderEngine.requestRender (proc.lastBpm.load());
            return;
        }
        selectedId = hit->id;
        dragOrig   = *hit;
        const float rightEdge = beatToPx (hit->endBeat());
        dragMode = (rightEdge - (float) e.x < 7.0f) ? DragMode::resize : DragMode::move;
        grabBeatOffset = pxToBeat ((float) e.x) - hit->startBeat;
        repaint();
        return;
    }

    if (e.mods.isRightButtonDown())
        return;

    VocalNote n;
    n.startBeat   = snap (pxToBeat ((float) e.x));
    n.lengthBeats = 1.0;
    n.midiPitch   = yToPitch (e.y);
    n.lyric       = "ra";
    selectedId    = n.id;
    dragOrig      = n;
    dragMode      = DragMode::resize;
    dragChanged   = true;                      // new note: audition on mouse-up
    grabBeatOffset = 0.0;
    proc.sequence.addNote (n);
    proc.renderEngine.requestRender (proc.lastBpm.load());
}

void PianoRollCanvas::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown())
    {
        const auto notes = proc.sequence.snapshot();
        if (const auto* hit = hitTest (e.getPosition(), notes))
        {
            proc.sequence.removeNote (hit->id);
            proc.renderEngine.requestRender (proc.lastBpm.load());
        }
        return;
    }
    if (dragMode == DragMode::none || selectedId.isNull()) return;

    const double beatAt  = pxToBeat ((float) e.x);
    const int    pitchAt = yToPitch (e.y);
    const auto   id   = selectedId;
    const auto   mode = dragMode;
    const auto   orig = dragOrig;
    const double grab = grabBeatOffset;

    proc.sequence.mutate ([&] (std::vector<VocalNote>& notes)
    {
        for (auto& n : notes)
        {
            if (n.id != id) continue;
            if (mode == DragMode::move)
            {
                n.startBeat = juce::jmax (0.0, snap (beatAt - grab));
                n.midiPitch = pitchAt;
            }
            else
            {
                n.lengthBeats = juce::jmax (0.25, snap (beatAt - orig.startBeat));
            }
            break;
        }
    });
    dragChanged = true;
    proc.renderEngine.requestRender (proc.lastBpm.load());
}

void PianoRollCanvas::mouseUp (const juce::MouseEvent&)
{
    if (dragChanged && dragMode != DragMode::none)
        auditionSelected();                    // hear the note you just placed/moved
    dragMode = DragMode::none;
    dragChanged = false;
}

void PianoRollCanvas::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto notes = proc.sequence.snapshot();
    if (const auto* hit = hitTest (e.getPosition(), notes))
    {
        editingId = hit->id;
        juce::Rectangle<int> r ((int) beatToPx (hit->startBeat), pitchToY (hit->midiPitch),
                                juce::jmax (70, (int) (hit->lengthBeats * beatToX)), rowH + 6);
        lyricEdit.setBounds (r);
        lyricEdit.setText (hit->lyric, juce::dontSendNotification);
        lyricEdit.setVisible (true);
        lyricEdit.grabKeyboardFocus();
        lyricEdit.selectAll();
    }
}

void PianoRollCanvas::commitLyricEdit()
{
    if (! lyricEdit.isVisible() || editingId.isNull()) return;
    const auto text = lyricEdit.getText().trim();
    lyricEdit.setVisible (false);
    if (text.isEmpty()) return;

    const auto id = editingId;
    proc.sequence.mutate ([&] (std::vector<VocalNote>& notes)
    {
        for (auto& n : notes)
            if (n.id == id)
            {
                n.lyric = text;
                n.isMelisma = (text == "-");
                n.phonemes.clear();
                break;
            }
    });
    proc.renderEngine.requestRender (proc.lastBpm.load());
    selectedId = id;
    auditionSelected();                        // hear the new syllable immediately
}

bool PianoRollCanvas::keyPressed (const juce::KeyPress& k)
{
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        && ! selectedId.isNull())
    {
        proc.sequence.removeNote (selectedId);
        selectedId = juce::Uuid::null();
        proc.renderEngine.requestRender (proc.lastBpm.load());
        return true;
    }
    return false;
}

} // namespace ss

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ss::SingScribeProcessor();
}
