#include "PluginEditor.h"

CutbenchEditor::CutbenchEditor (CutbenchProcessor& p)
    : AudioProcessorEditor (&p), proc (p), waveform (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    for (auto* b : { &loadButton, &playButton, &stopButton, &addButton, &splitButton,
                     &deleteButton, &resetButton, &chopButton, &zoomInButton, &zoomOutButton, &fitButton })
        addAndMakeVisible (*b);
    for (auto* l : { &fileLabel, &infoLabel, &statusLabel, &hintLabel,
                     &gainLabel, &noteLabel, &modeLabel, &retriggerLabel, &fadeLabel })
        addAndMakeVisible (*l);
    for (auto* s : { &gainSlider, &noteSlider, &fadeSlider, &chopCountSlider })
    {
        s->setSliderStyle (juce::Slider::LinearBar);
        addAndMakeVisible (*s);
    }
    addAndMakeVisible (modeBox);
    addAndMakeVisible (retriggerBox);

    // --- labels ---
    auto small = [] (juce::Label& l, const juce::String& text, juce::Justification j = juce::Justification::centredLeft)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Theme::dim);
        l.setFont (l.getFont().withHeight (12.0f));
        l.setJustificationType (j);
    };
    small (gainLabel, "GAIN");
    small (noteLabel, "BASE NOTE (SLICE 1)");
    small (modeLabel, "PLAY MODE");
    small (retriggerLabel, "RETRIGGER");
    small (fadeLabel, "DECLICK FADE");
    small (statusLabel, {});
    small (hintLabel, "keys play slices left to right | dbl-click: split | drag: select + Add Slice | drag edges: adjust | space: play",
           juce::Justification::centredRight);
    small (infoLabel, "BPM --    KEY --", juce::Justification::centredRight);   // detection arrives in a later phase

    fileLabel.setFont (fileLabel.getFont().withHeight (16.0f));
    fileLabel.setJustificationType (juce::Justification::centredLeft);

    // --- parameters ---
    gainAttachment = std::make_unique<SliderAttachment> (proc.apvts, "gain", gainSlider);
    noteAttachment = std::make_unique<SliderAttachment> (proc.apvts, "baseNote", noteSlider);
    fadeAttachment = std::make_unique<SliderAttachment> (proc.apvts, "fadeMs", fadeSlider);

    modeBox.addItemList ({ "One-Shot", "Gate" }, 1);
    modeAttachment = std::make_unique<ComboBoxAttachment> (proc.apvts, "playMode", modeBox);

    retriggerBox.addItemList ({ "Cut", "Layer" }, 1);
    retriggerAttachment = std::make_unique<ComboBoxAttachment> (proc.apvts, "retrigger", retriggerBox);

    chopCountSlider.setRange (2.0, (double) CutbenchProcessor::kMaxSlices, 1.0);
    chopCountSlider.setValue (8.0, juce::dontSendNotification);
    chopCountSlider.setTextValueSuffix (" slices");

    // The attachments install their own text functions, so ours go after them.
    gainSlider.textFromValueFunction = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };
    fadeSlider.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " ms"; };
    noteSlider.textFromValueFunction = [] (double v)
    {
        const int n = juce::roundToInt (v);
        return juce::MidiMessage::getMidiNoteName (n, true, true, 3) + "  (" + juce::String (n) + ")";   // C3 = 60, as in Ableton
    };
    noteSlider.setTextBoxIsEditable (false);
    for (auto* s : { &gainSlider, &noteSlider, &fadeSlider })
        s->updateText();

    // --- buttons ---
    loadButton.onClick    = [this] { chooseFile(); };
    playButton.onClick    = [this] { waveform.play(); };
    stopButton.onClick    = [this] { proc.stopPreview(); };
    addButton.onClick     = [this] { if (auto sel = waveform.getSelection()) proc.addSlice (sel->first, sel->second); };
    splitButton.onClick   = [this] { proc.splitAt (waveform.getCursor()); };
    deleteButton.onClick  = [this] { proc.removeSlice (proc.getSelectedSlice()); };
    resetButton.onClick   = [this] { proc.resetSlices(); };
    chopButton.onClick    = [this] { proc.chopEqual ((int) chopCountSlider.getValue()); };
    zoomInButton.onClick  = [this] { waveform.zoomBy (0.5); };
    zoomOutButton.onClick = [this] { waveform.zoomBy (2.0); };
    fitButton.onClick     = [this] { waveform.zoomToFit(); };

    waveform.onViewStateChanged = [this] { refreshStatus(); };

    proc.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (820, 420, 2400, 1400);
    setSize (1000, 600);

    changeListenerCallback (nullptr);   // initial sync
}

CutbenchEditor::~CutbenchEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void CutbenchEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    waveform.modelChanged();
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
    infoLabel.setBounds (top.removeFromRight (190));
    fileLabel.setBounds (top);
    r.removeFromTop (10);

    // status row
    auto statusRow = r.removeFromBottom (20);
    hintLabel.setBounds (statusRow.removeFromRight (juce::jmin (560, statusRow.getWidth() / 2)));
    statusLabel.setBounds (statusRow);
    r.removeFromBottom (6);

    // controls + waveform (the waveform takes all remaining space)
    controlsArea = r.removeFromBottom (92);
    r.removeFromBottom (10);
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
    place (addButton, 90);
    place (splitButton, 64);
    place (deleteButton, 70);
    place (resetButton, 70, 18);
    place (chopButton, 90);
    place (chopCountSlider, 100);

    auto row2 = c;
    const int gap = 12;
    const int colW = (row2.getWidth() - 4 * gap) / 5;
    auto column = [&row2, colW, gap] (juce::Label& label, juce::Component& comp)
    {
        auto col = row2.removeFromLeft (colW);
        row2.removeFromLeft (gap);
        label.setBounds (col.removeFromTop (16));
        comp.setBounds (col.removeFromTop (24));
    };
    column (gainLabel, gainSlider);
    column (noteLabel, noteSlider);
    column (modeLabel, modeBox);
    column (retriggerLabel, retriggerBox);
    column (fadeLabel, fadeSlider);
}
