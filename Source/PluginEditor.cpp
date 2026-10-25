#include "PluginEditor.h"
#include <functional>
#include <vector>

namespace
{
    juce::String signedInt (double v)
    {
        const int n = juce::roundToInt (v);
        return (n > 0 ? "+" : "") + juce::String (n);
    }

    // Content of the settings pop-up (gear button): base note and declick fade
    class SettingsPanel : public juce::Component
    {
    public:
        explicit SettingsPanel (juce::AudioProcessorValueTreeState& apvts)
        {
            setSize (290, 122);

            for (auto* l : { &noteLabel, &fadeLabel })
            {
                l->setColour (juce::Label::textColourId, Theme::dim);
                l->setFont (l->getFont().withHeight (12.0f));
                addAndMakeVisible (*l);
            }
            noteLabel.setText ("BASE NOTE (MIDI KEY OF PAD 1)", juce::dontSendNotification);
            fadeLabel.setText ("DECLICK FADE", juce::dontSendNotification);

            for (auto* s : { &noteSlider, &fadeSlider })
            {
                s->setSliderStyle (juce::Slider::LinearBar);
                s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                addAndMakeVisible (*s);
            }

            noteAttachment = std::make_unique<SliderAttachment> (apvts, "baseNote", noteSlider);
            fadeAttachment = std::make_unique<SliderAttachment> (apvts, "fadeMs", fadeSlider);

            // the attachments install their own text functions, so ours go after them
            noteSlider.textFromValueFunction = [] (double v)
            {
                const int n = juce::roundToInt (v);
                return juce::MidiMessage::getMidiNoteName (n, true, true, 3) + "  (" + juce::String (n) + ")";   // C3 = 60, as in Ableton
            };
            fadeSlider.textFromValueFunction = [] (double v) { return juce::String (v, 1) + " ms"; };
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (14, 12);
            noteLabel.setBounds (r.removeFromTop (14));
            noteSlider.setBounds (r.removeFromTop (26));
            r.removeFromTop (10);
            fadeLabel.setBounds (r.removeFromTop (14));
            fadeSlider.setBounds (r.removeFromTop (26));
        }

    private:
        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

        juce::Label noteLabel, fadeLabel;
        juce::Slider noteSlider, fadeSlider;
        std::unique_ptr<SliderAttachment> noteAttachment, fadeAttachment;
    };
}

CutbenchEditor::CutbenchEditor (CutbenchProcessor& p)
    : AudioProcessorEditor (&p), proc (p), waveform (p), padGrid (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (waveform);
    addAndMakeVisible (padGrid);
    addAndMakeVisible (playStopButton);
    addAndMakeVisible (settingsButton);
    for (auto* b : { &loadButton, &setPadButton, &clearPadButton, &clearAllButton,
                     &chopButton, &zoomInButton, &zoomOutButton, &fitButton,
                     &reverseButton, &loopButton, &pitchDownButton, &pitchUpButton })
        addAndMakeVisible (*b);
    for (auto* l : { &fileLabel, &infoLabel, &versionLabel, &statusLabel, &hintLabel,
                     &pitchLabel, &fineLabel, &speedLabel, &engineLabel, &directionLabel, &repeatLabel,
                     &attackLabel, &releaseLabel, &gainLabel, &modeLabel, &padModeLabel })
        addAndMakeVisible (*l);
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &attackSlider, &releaseSlider, &gainSlider, &chopCountSlider })
        addAndMakeVisible (*s);
    for (auto* c : { &engineBox, &modeBox, &padModeBox })
        addAndMakeVisible (*c);

    // Drag bars draw their own value text (no text box), so drag and double-click-to-reset always work.
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &chopCountSlider })
    {
        s->setSliderStyle (juce::Slider::LinearBar);
        s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    }
    // Knobs: value shown as a bubble while dragging or hovering
    for (auto* k : { &gainSlider, &attackSlider, &releaseSlider })
    {
        k->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k->setPopupDisplayEnabled (true, true, this, 1500);
    }

    // --- labels ---
    auto small = [] (juce::Label& l, const juce::String& text, juce::Justification j = juce::Justification::centred)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, Theme::dim);
        l.setFont (l.getFont().withHeight (12.0f));
        l.setJustificationType (j);
    };
    small (attackLabel, "ATTACK");
    small (releaseLabel, "RELEASE");
    small (gainLabel, "GAIN");
    small (pitchLabel, "PITCH");
    small (fineLabel, "FINE TUNE");
    small (speedLabel, "SPEED");
    small (engineLabel, StretchEngine::isAvailable() ? "ENGINE" : "ENGINE (Keep Length n/a)");
    small (directionLabel, "DIRECTION");
    small (repeatLabel, "REPEAT");
    small (modeLabel, "PLAY MODE");
    small (padModeLabel, "VOICES");
    small (statusLabel, {}, juce::Justification::centredLeft);
    small (hintLabel, "play, then hit EMPTY pads to set cues | pad with cue: jump + play | dbl-click: set cue | right-click pad: clear",
           juce::Justification::centredRight);
    small (infoLabel, "BPM --    KEY --", juce::Justification::centredRight);   // detection arrives in a later phase
    small (versionLabel, juce::String ("Cutbench v") + JucePlugin_VersionString
                         + (StretchEngine::isAvailable() ? "  |  Keep Length: on" : "  |  Keep Length: off"),
           juce::Justification::centredLeft);

    fileLabel.setFont (fileLabel.getFont().withHeight (16.0f));
    fileLabel.setJustificationType (juce::Justification::centredLeft);

    // --- parameters ---
    auto& apvts = proc.apvts;
    pitchAttachment   = std::make_unique<SliderAttachment> (apvts, "pitch", pitchSlider);
    fineAttachment    = std::make_unique<SliderAttachment> (apvts, "fine", fineSlider);
    speedAttachment   = std::make_unique<SliderAttachment> (apvts, "speed", speedSlider);
    attackAttachment  = std::make_unique<SliderAttachment> (apvts, "attackMs", attackSlider);
    releaseAttachment = std::make_unique<SliderAttachment> (apvts, "releaseMs", releaseSlider);
    gainAttachment    = std::make_unique<SliderAttachment> (apvts, "gain", gainSlider);

    engineBox.addItemList ({ "Vinyl", "Keep Length" }, 1);
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

    // Double-click resets to the default value (the attachments set this too; stated explicitly here)
    pitchSlider.setDoubleClickReturnValue (true, 0.0);
    fineSlider.setDoubleClickReturnValue (true, 0.0);
    speedSlider.setDoubleClickReturnValue (true, 1.0);
    attackSlider.setDoubleClickReturnValue (true, 0.0);
    releaseSlider.setDoubleClickReturnValue (true, 0.0);
    gainSlider.setDoubleClickReturnValue (true, 0.0);

    // The attachments install their own text functions, so ours go after them.
    pitchSlider.textFromValueFunction   = [] (double v) { return signedInt (v); };   // just the number
    fineSlider.textFromValueFunction    = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " ct"; };
    speedSlider.textFromValueFunction   = [] (double v) { return juce::String (v, 2) + "x"; };
    attackSlider.textFromValueFunction  = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    releaseSlider.textFromValueFunction = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    gainSlider.textFromValueFunction    = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };

    // --- buttons ---
    loadButton.onClick       = [this] { chooseFile(); };
    playStopButton.onClick   = [this] { waveform.togglePlayback(); };
    settingsButton.onClick   = [this] { openSettings(); };
    setPadButton.onClick     = [this] { waveform.assignToSelectedPad(); };
    clearPadButton.onClick   = [this] { proc.clearPad (proc.getSelectedPad()); };
    clearAllButton.onClick   = [this] { proc.clearAllPads(); };
    zoomInButton.onClick     = [this] { waveform.zoomBy (0.5); };
    zoomOutButton.onClick    = [this] { waveform.zoomBy (2.0); };
    fitButton.onClick        = [this] { waveform.zoomToFit(); };

    // Chop Equal divides the selected region, or the whole sample when nothing is selected
    chopButton.onClick = [this]
    {
        const auto selection = waveform.getSelection();
        proc.chopEqual ((int) chopCountSlider.getValue(),
                        selection ? selection->first : 0,
                        selection ? selection->second : -1);
    };

    pitchDownButton.onClick = [this] { nudgePitch (-1); };
    pitchUpButton.onClick   = [this] { nudgePitch (+1); };
    for (auto* b : { &pitchDownButton, &pitchUpButton })
    {
        b->setTriggeredOnMouseDown (true);
        b->setRepeatSpeed (400, 90);   // hold to keep stepping
    }

    waveform.onViewStateChanged = [this] { refreshStatus(); };

    proc.addChangeListener (this);
    startTimerHz (20);   // keeps the play / stop icon in sync

    setResizable (true, true);
    setResizeLimits (900, 540, 2400, 1400);
    setSize (1100, 700);

    changeListenerCallback (nullptr);   // initial sync
}

CutbenchEditor::~CutbenchEditor()
{
    stopTimer();
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void CutbenchEditor::timerCallback()
{
    playStopButton.setPlaying (proc.getPlayhead() >= 0);
}

void CutbenchEditor::openSettings()
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<SettingsPanel> (proc.apvts), settingsButton.getBounds(), this);
}

void CutbenchEditor::nudgePitch (int semitones)
{
    if (auto* parameter = proc.apvts.getParameter ("pitch"))
    {
        const float current = parameter->convertFrom0to1 (parameter->getValue());
        const float target = juce::jlimit (-24.0f, 24.0f, current + (float) semitones);

        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (target));
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

    // Top bar. Left: Load + file name + version. Centre: play / stop. Right: BPM/key placeholder, settings, GAIN knob.
    auto topRow = r.removeFromTop (60);
    const int half = topRow.getWidth() / 2;
    auto leftPart  = topRow.removeFromLeft (half - 34);
    auto rightPart = topRow.removeFromRight (half - 34);
    playStopButton.setBounds (topRow.withSizeKeepingCentre (48, 48));

    loadButton.setBounds (leftPart.removeFromLeft (96).withSizeKeepingCentre (96, 30));
    leftPart.removeFromLeft (12);
    fileLabel.setBounds (leftPart.removeFromTop (34));
    versionLabel.setBounds (leftPart);

    auto gainColumn = rightPart.removeFromRight (60);
    gainLabel.setBounds (gainColumn.removeFromBottom (14));
    gainSlider.setBounds (gainColumn.withSizeKeepingCentre (44, 44));
    rightPart.removeFromRight (8);
    settingsButton.setBounds (rightPart.removeFromRight (34).withSizeKeepingCentre (34, 34));
    rightPart.removeFromRight (12);
    infoLabel.setBounds (rightPart);
    r.removeFromTop (10);

    // status row
    auto statusRow = r.removeFromBottom (20);
    hintLabel.setBounds (statusRow.removeFromRight (juce::jmin (700, (statusRow.getWidth() * 3) / 5)));
    statusLabel.setBounds (statusRow);
    r.removeFromBottom (6);

    // controls: one row of buttons, one row of knobs and parameters
    controlsArea = r.removeFromBottom (132);
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
    place (setPadButton, 80);
    place (clearPadButton, 86);
    place (clearAllButton, 80, 18);
    place (chopButton, 90);
    place (chopCountSlider, 100);

    // Parameter row: columns sized by weight, the control on top and its label below it
    using Place = std::function<void (juce::Rectangle<int>)>;
    struct Cell { juce::Label* label; Place place; int weight; };

    auto bar = [] (juce::Component& comp) -> Place
    {
        return [&comp] (juce::Rectangle<int> zone) { comp.setBounds (zone.withSizeKeepingCentre (zone.getWidth(), 24)); };
    };
    auto knob = [] (juce::Component& comp) -> Place
    {
        return [&comp] (juce::Rectangle<int> zone)
        {
            const int d = juce::jmin (60, zone.getWidth(), zone.getHeight());
            comp.setBounds (zone.withSizeKeepingCentre (d, d));
        };
    };

    // pitch: [-] number [+]
    Place pitchPlace = [this] (juce::Rectangle<int> zone)
    {
        auto z = zone.withSizeKeepingCentre (zone.getWidth(), 24);
        pitchDownButton.setBounds (z.removeFromLeft (26));
        pitchUpButton.setBounds (z.removeFromRight (26));
        pitchSlider.setBounds (z.reduced (3, 0));
    };

    const std::vector<Cell> cells {
        { &attackLabel,    knob (attackSlider),   5 },
        { &releaseLabel,   knob (releaseSlider),  5 },
        { &pitchLabel,     pitchPlace,            9 },
        { &fineLabel,      bar (fineSlider),      7 },
        { &speedLabel,     bar (speedSlider),     7 },
        { &engineLabel,    bar (engineBox),       8 },
        { &directionLabel, bar (reverseButton),   6 },
        { &repeatLabel,    bar (loopButton),      5 },
        { &modeLabel,      bar (modeBox),         8 },
        { &padModeLabel,   bar (padModeBox),      6 }
    };

    const int gap = 12;
    const int zoneHeight = 60;
    int totalWeight = 0;
    for (const auto& cell : cells)
        totalWeight += cell.weight;

    auto row = c.removeFromTop (zoneHeight + 14);
    const int usable = row.getWidth() - ((int) cells.size() - 1) * gap;
    for (const auto& cell : cells)
    {
        auto col = row.removeFromLeft (usable * cell.weight / totalWeight);
        row.removeFromLeft (gap);
        cell.place (col.removeFromTop (zoneHeight));
        cell.label->setBounds (col.removeFromTop (14));
    }
}
