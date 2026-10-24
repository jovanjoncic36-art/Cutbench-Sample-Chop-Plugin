#include "PluginEditor.h"

namespace
{
    juce::String signedInt (double v)
    {
        const int n = juce::roundToInt (v);
        return (n > 0 ? "+" : "") + juce::String (n);
    }
}

CutbenchEditor::CutbenchEditor (CutbenchProcessor& p)
    : AudioProcessorEditor (&p), proc (p), waveform (p), padGrid (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    addAndMakeVisible (padGrid);
    for (auto* b : { &loadButton, &playButton, &stopButton, &setPadButton, &clearPadButton, &clearAllButton,
                     &chopButton, &zoomInButton, &zoomOutButton, &fitButton,
                     &reverseButton, &loopButton, &resetPitchButton })
        addAndMakeVisible (*b);
    for (auto* l : { &fileLabel, &infoLabel, &versionLabel, &statusLabel, &hintLabel,
                     &pitchLabel, &fineLabel, &speedLabel, &engineLabel, &directionLabel, &repeatLabel, &resetLabel,
                     &attackLabel, &releaseLabel, &fadeLabel, &gainLabel, &noteLabel, &modeLabel, &padModeLabel })
        addAndMakeVisible (*l);
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &attackSlider, &releaseSlider,
                     &fadeSlider, &gainSlider, &noteSlider, &chopCountSlider })
    {
        s->setSliderStyle (juce::Slider::LinearBar);
        addAndMakeVisible (*s);
    }
    for (auto* c : { &engineBox, &modeBox, &padModeBox })
        addAndMakeVisible (*c);

    // --- labels ---
    auto small = [] (juce::Label& l, const juce::String& text, juce::Justification j = juce::Justification::centredLeft)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Theme::dim);
        l.setFont (l.getFont().withHeight (12.0f));
        l.setJustificationType (j);
    };
    small (pitchLabel, "PITCH");
    small (fineLabel, "FINE TUNE");
    small (speedLabel, "SPEED");
    small (engineLabel, StretchEngine::isAvailable() ? "ENGINE" : "ENGINE (Keep Length n/a)");
    small (directionLabel, "DIRECTION");
    small (repeatLabel, "REPEAT");
    small (resetLabel, "PITCH");
    small (attackLabel, "ATTACK");
    small (releaseLabel, "RELEASE");
    small (fadeLabel, "DECLICK FADE");
    small (gainLabel, "GAIN");
    small (noteLabel, "BASE NOTE (PAD 1)");
    small (modeLabel, "PLAY MODE");
    small (padModeLabel, "VOICES");
    small (statusLabel, {});
    small (hintLabel, "play, then hit EMPTY pads to set cues | pad with cue: jump + play | dbl-click: set cue | right-click pad: clear",
           juce::Justification::centredRight);
    small (infoLabel, "BPM --    KEY --", juce::Justification::centredRight);   // detection arrives in a later phase
    small (versionLabel, juce::String ("Cutbench v") + JucePlugin_VersionString
                         + (StretchEngine::isAvailable() ? "  |  Keep Length: on" : "  |  Keep Length: off"),
           juce::Justification::centredRight);

    fileLabel.setFont (fileLabel.getFont().withHeight (16.0f));
    fileLabel.setJustificationType (juce::Justification::centredLeft);

    // --- parameters ---
    auto& apvts = proc.apvts;
    pitchAttachment   = std::make_unique<SliderAttachment> (apvts, "pitch", pitchSlider);
    fineAttachment    = std::make_unique<SliderAttachment> (apvts, "fine", fineSlider);
    speedAttachment   = std::make_unique<SliderAttachment> (apvts, "speed", speedSlider);
    attackAttachment  = std::make_unique<SliderAttachment> (apvts, "attackMs", attackSlider);
    releaseAttachment = std::make_unique<SliderAttachment> (apvts, "releaseMs", releaseSlider);
    fadeAttachment    = std::make_unique<SliderAttachment> (apvts, "fadeMs", fadeSlider);
    gainAttachment    = std::make_unique<SliderAttachment> (apvts, "gain", gainSlider);
    noteAttachment    = std::make_unique<SliderAttachment> (apvts, "baseNote", noteSlider);

    engineBox.addItemList ({ "Classic", "Keep Length" }, 1);
    engineAttachment = std::make_unique<ComboBoxAttachment> (apvts, "engine", engineBox);
    if (! StretchEngine::isAvailable())
        engineBox.setItemEnabled (2, false);   // this build was made without the time-stretch library

    modeBox.addItemList ({ "One-Shot", "Gate" }, 1);
    modeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "playMode", modeBox);

    padModeBox.addItemList ({ "Mono", "Poly" }, 1);
    padModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "padMode", padModeBox);

    for (auto* b : { &reverseButton, &loopButton })
        b->setClickingTogglesState (true);
    reverseAttachment = std::make_unique<ButtonAttachment> (apvts, "reverse", reverseButton);
    loopAttachment    = std::make_unique<ButtonAttachment> (apvts, "loop", loopButton);

    chopCountSlider.setRange (2.0, (double) kNumPads, 1.0);
    chopCountSlider.setValue (8.0, juce::dontSendNotification);
    chopCountSlider.setTextValueSuffix (" pads");

    // The attachments install their own text functions, so ours go after them.
    pitchSlider.textFromValueFunction   = [] (double v) { return signedInt (v) + " st"; };
    fineSlider.textFromValueFunction    = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " ct"; };
    speedSlider.textFromValueFunction   = [] (double v) { return juce::String (v, 2) + "x"; };
    attackSlider.textFromValueFunction  = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    releaseSlider.textFromValueFunction = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    fadeSlider.textFromValueFunction    = [] (double v) { return juce::String (v, 1) + " ms"; };
    gainSlider.textFromValueFunction    = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };
    noteSlider.textFromValueFunction    = [] (double v)
    {
        const int n = juce::roundToInt (v);
        return juce::MidiMessage::getMidiNoteName (n, true, true, 3) + "  (" + juce::String (n) + ")";   // C3 = 60, as in Ableton
    };
    noteSlider.setTextBoxIsEditable (false);
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &attackSlider, &releaseSlider,
                     &fadeSlider, &gainSlider, &noteSlider })
        s->updateText();

    // --- buttons ---
    loadButton.onClick       = [this] { chooseFile(); };
    playButton.onClick       = [this] { waveform.play(); };
    stopButton.onClick       = [this] { proc.stopPreview(); };
    setPadButton.onClick     = [this] { waveform.assignToSelectedPad(); };
    clearPadButton.onClick   = [this] { proc.clearPad (proc.getSelectedPad()); };
    clearAllButton.onClick   = [this] { proc.clearAllPads(); };
    chopButton.onClick       = [this] { proc.chopEqual ((int) chopCountSlider.getValue()); };
    zoomInButton.onClick     = [this] { waveform.zoomBy (0.5); };
    zoomOutButton.onClick    = [this] { waveform.zoomBy (2.0); };
    fitButton.onClick        = [this] { waveform.zoomToFit(); };
    resetPitchButton.onClick = [this] { resetPitch(); };

    waveform.onViewStateChanged = [this] { refreshStatus(); };

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (900, 520, 2400, 1400);
    setSize (1100, 700);

    changeListenerCallback (nullptr);   // initial sync
}

CutbenchEditor::~CutbenchEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void CutbenchEditor::resetPitch()
{
    for (const char* id : { "pitch", "fine" })
        if (auto* parameter = proc.apvts.getParameter (id))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->getDefaultValue());
            parameter->endChangeGesture();
        }
}

void CutbenchEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    waveform.modelChanged();
    padGrid.modelChanged();
    refreshStatus();
}

void CutbenchEditor::refreshStatus()
{
    const auto sample = proc.getSample();
    fileLabel.setText (sample != nullptr ? sample->name : juce::String ("No file loaded"), juce::dontSendNotification);

    juce::String text = proc.getStatusText();   // loading progress or an error message takes priority

    if (text.isEmpty() && sample != nullptr)
    {
        const double sr = sample->sampleRate;
        text = juce::String (sr / 1000.0, 1) + " kHz  |  "
             + juce::String (sample->numChannels() == 1 ? "mono" : "stereo") + "  |  "
             + Theme::formatTime ((double) sample->numFrames() / sr)
             + "  |  cursor " + Theme::formatTime ((double) waveform.getCursor() / sr);

        if (auto sel = waveform.getSelection())
            text += "  |  selection " + Theme::formatTime ((double) sel->first / sr)
                  + " - " + Theme::formatTime ((double) sel->second / sr)
                  + " (" + Theme::formatTime ((double) (sel->second - sel->first) / sr) + ")";
    }
    statusLabel.setText (text, juce::dontSendNotification);
}

void CutbenchEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose an audio file", juce::File(),
                                                   proc.getFormatManager().getWildcardForAllFormats());

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    juce::Component::SafePointer<CutbenchEditor> safe (this);

    chooser->launchAsync (flags, [safe] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (safe != nullptr && file.existsAsFile())
            safe->proc.loadFile (file);
    });
}

bool CutbenchEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() >= 1;   // unsupported files are reported by the loader, not silently ignored
}

void CutbenchEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() >= 1)
        proc.loadFile (juce::File (files[0]));
}

void CutbenchEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::bg);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (controlsArea.toFloat(), 6.0f);
}

void CutbenchEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    // top bar
    auto top = r.removeFromTop (30);
    loadButton.setBounds (top.removeFromLeft (96));
    top.removeFromLeft (12);
    infoLabel.setBounds (top.removeFromRight (150));
    versionLabel.setBounds (top.removeFromRight (230));
    fileLabel.setBounds (top);
    r.removeFromTop (10);

    // status row
    auto statusRow = r.removeFromBottom (20);
    hintLabel.setBounds (statusRow.removeFromRight (juce::jmin (700, (statusRow.getWidth() * 3) / 5)));
    statusLabel.setBounds (statusRow);
    r.removeFromBottom (6);

    // controls: buttons row + two rows of parameters
    controlsArea = r.removeFromBottom (146);
    r.removeFromBottom (10);

    // pad grid on the right, waveform takes everything else
    auto padArea = r.removeFromRight (juce::jlimit (230, 340, r.getWidth() / 4));
    r.removeFromRight (10);
    padGrid.setBounds (padArea);
    waveform.setBounds (r);

    auto c = controlsArea.reduced (10, 8);

    auto row1 = c.removeFromTop (28);
    c.removeFromTop (6);

    auto zoom = row1.removeFromRight (3 * 44 + 2 * 6);
    fitButton.setBounds (zoom.removeFromLeft (44));
    zoom.removeFromLeft (6);
    zoomOutButton.setBounds (zoom.removeFromLeft (44));
    zoom.removeFromLeft (6);
    zoomInButton.setBounds (zoom);

    auto place = [&row1] (juce::Component& comp, int width, int gapAfter = 6)
    {
        comp.setBounds (row1.removeFromLeft (width));
        row1.removeFromLeft (gapAfter);
    };
    place (playButton, 64);
    place (stopButton, 64, 18);
    place (setPadButton, 80);
    place (clearPadButton, 86);
    place (clearAllButton, 80, 18);
    place (chopButton, 90);
    place (chopCountSlider, 100);

    // Parameter rows: equal columns, a small label above each control
    const int gap = 12;
    using Item = std::pair<juce::Label*, juce::Component*>;
    auto layoutRow = [gap] (juce::Rectangle<int> row, std::initializer_list<Item> items)
    {
        const int colW = (row.getWidth() - ((int) items.size() - 1) * gap) / (int) items.size();
        for (const auto& item : items)
        {
            auto col = row.removeFromLeft (colW);
            row.removeFromLeft (gap);
            item.first->setBounds (col.removeFromTop (16));
            item.second->setBounds (col.removeFromTop (24));
        }
    };

    auto rowB = c.removeFromTop (40);
    c.removeFromTop (6);
    layoutRow (rowB, { Item { &pitchLabel, &pitchSlider },   Item { &fineLabel, &fineSlider },
                       Item { &speedLabel, &speedSlider },   Item { &engineLabel, &engineBox },
                       Item { &directionLabel, &reverseButton }, Item { &repeatLabel, &loopButton },
                       Item { &resetLabel, &resetPitchButton } });

    layoutRow (c.removeFromTop (40), { Item { &attackLabel, &attackSlider }, Item { &releaseLabel, &releaseSlider },
                                       Item { &fadeLabel, &fadeSlider },     Item { &gainLabel, &gainSlider },
                                       Item { &noteLabel, &noteSlider },     Item { &modeLabel, &modeBox },
                                       Item { &padModeLabel, &padModeBox } });
}
