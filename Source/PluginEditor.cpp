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
                     &chopButton, &autoSliceButton, &snapButton,
                     &reverseButton, &loopButton, &pitchDownButton, &pitchUpButton })
        addAndMakeVisible (*b);
    for (auto* l : { &fileLabel, &infoLabel, &versionLabel, &statusLabel, &hintLabel,
                     &padsCaption, &chopCaption, &autoCaption,
                     &pitchLabel, &fineLabel, &speedLabel, &engineLabel, &directionLabel, &repeatLabel,
                     &attackLabel, &releaseLabel, &gainLabel, &modeLabel, &padModeLabel })
        addAndMakeVisible (*l);
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &attackSlider, &releaseSlider, &gainSlider,
                     &chopCountSlider, &sensitivitySlider })
        addAndMakeVisible (*s);
    for (auto* c : { &engineBox, &modeBox, &padModeBox })
        addAndMakeVisible (*c);

    // Drag bars draw their own value text (no text box), so drag and double-click-to-reset always work.
    for (auto* s : { &pitchSlider, &fineSlider, &speedSlider, &chopCountSlider, &sensitivitySlider })
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
    small (padsCaption, "PADS", juce::Justification::centredLeft);
    small (chopCaption, "CHOP EQUAL (even parts)", juce::Justification::centredLeft);
    small (autoCaption, "AUTO SLICE (on the hits)", juce::Justification::centredLeft);
    small (statusLabel, {}, juce::Justification::centredLeft);
    small (hintLabel, "play, then hit EMPTY pads to set cues | pad with cue: jump + play | dbl-click: set cue | right-click pad: clear",
           juce::Justification::centredRight);
    small (infoLabel, "BPM --    KEY --", juce::Justification::centredRight);   // detection arrives in a later phase
    small (versionLabel, juce::String ("Cutbench v") + JucePlugin_VersionString, juce::Justification::centredLeft);

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
    sensitivityAttachment = std::make_unique<SliderAttachment> (apvts, "sensitivity", sensitivitySlider);

    engineBox.addItemList ({ "Vinyl", "Keep Length" }, 1);
    engineAttachment = std::make_unique<ComboBoxAttachment> (apvts, "engine", engineBox);
    if (! StretchEngine::isAvailable())
        engineBox.setItemEnabled (2, false);   // this build was made without the time-stretch library

    modeBox.addItemList ({ "One-Shot", "Gate" }, 1);
    modeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "playMode", modeBox);

    padModeBox.addItemList ({ "Mono", "Poly" }, 1);
    padModeAttachment = std::make_unique<ComboBoxAttachment> (apvts, "padMode", padModeBox);

    for (auto* b : { &reverseButton, &loopButton, &snapButton })
        b->setClickingTogglesState (true);
    snapAttachment = std::make_unique<ButtonAttachment> (apvts, "snap", snapButton);
    snapButton.setButtonText (snapButton.getToggleState() ? "Snap: On" : "Snap: Off");
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
    sensitivitySlider.setDoubleClickReturnValue (true, 0.5);

    // The attachments install their own text functions, so ours go after them.
    pitchSlider.textFromValueFunction   = [] (double v) { return signedInt (v); };   // just the number
    fineSlider.textFromValueFunction    = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " ct"; };
    speedSlider.textFromValueFunction   = [] (double v) { return juce::String (v, 2) + "x"; };
    attackSlider.textFromValueFunction  = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    releaseSlider.textFromValueFunction = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    sensitivitySlider.textFromValueFunction = [] (double v) { return "Sens " + juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    gainSlider.textFromValueFunction    = [] (double v) { return v <= -59.95 ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; };

    // --- buttons ---
    loadButton.onClick       = [this] { chooseFile(); };
    playStopButton.onClick   = [this] { waveform.togglePlayback(); };
    settingsButton.onClick   = [this] { openSettings(); };
    setPadButton.onClick     = [this] { waveform.assignToSelectedPad(); };
    clearPadButton.onClick   = [this] { proc.clearPad (proc.getSelectedPad()); };
    clearAllButton.onClick   = [this] { proc.clearAllPads(); };

    // Chop Equal divides the selected region, or the whole sample when nothing is selected
    chopButton.onClick = [this]
    {
        const auto selection = waveform.getSelection();
        proc.chopEqual ((int) chopCountSlider.getValue(),
                        selection ? selection->first : 0,
                        selection ? selection->second : -1);
    };

    // Auto Slice puts cues on the detected transients of the selected region (or of the whole sample), at most
    // as many as the pad count next to Chop Equal; Sens sets how weak a hit may be and still count
    autoSliceButton.onClick = [this]
    {
        const auto selection = waveform.getSelection();
        proc.autoSlice ((int) chopCountSlider.getValue(),
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

    for (const char* id : { "pitch", "fine", "speed", "engine" })
        proc.apvts.addParameterListener (id, this);
    pendingParams = 4;       // make Speed match Pitch right away when the editor opens in Vinyl mode
    triggerAsyncUpdate();

    setResizable (true, true);
    setResizeLimits (900, 590, 2400, 1400);
    setSize (1100, 700);

    changeListenerCallback (nullptr);   // initial sync
}

CutbenchEditor::~CutbenchEditor()
{
    for (const char* id : { "pitch", "fine", "speed", "engine" })
        proc.apvts.removeParameterListener (id, this);
    cancelPendingUpdate();
    stopTimer();
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void CutbenchEditor::parameterChanged (const juce::String& parameterID, float)
{
    if (syncing.load())
        return;   // our own adjustment below

    int bit = 0;
    if (parameterID == "pitch" || parameterID == "fine") bit = 1;
    else if (parameterID == "speed")                      bit = 2;
    else if (parameterID == "engine")                     bit = 4;

    if (bit != 0)
    {
        pendingParams.fetch_or (bit);
        triggerAsyncUpdate();
    }
}

void CutbenchEditor::setParameterValue (const char* id, float value)
{
    if (auto* parameter = proc.apvts.getParameter (id))
    {
        const float normalised = parameter->convertTo0to1 (value);
        if (std::abs (normalised - parameter->getValue()) < 1.0e-6f)
            return;

        syncing = true;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (normalised);
        parameter->endChangeGesture();
        syncing = false;
    }
}

void CutbenchEditor::handleAsyncUpdate()
{
    const int changed = pendingParams.exchange (0);
    if (changed == 0)
        return;

    // Keep Length: pitch and speed are independent, nothing to follow
    if (proc.apvts.getRawParameterValue ("engine")->load() > 0.5f)
        return;

    // Vinyl: speed 2x is one octave up, so the two controls show the same thing and follow each other.
    // 0.25x..4x is exactly -24..+24 semitones.
    const double pitch = (double) proc.apvts.getRawParameterValue ("pitch")->load();
    const double fine  = (double) proc.apvts.getRawParameterValue ("fine")->load();
    const double speed = (double) proc.apvts.getRawParameterValue ("speed")->load();

    if ((changed & (1 | 4)) != 0)   // pitch / fine changed, or just switched to Vinyl: speed follows the pitch
    {
        const double semitones = pitch + fine / 100.0;
        setParameterValue ("speed", (float) juce::jlimit (0.25, 4.0, std::pow (2.0, semitones / 12.0)));
    }
    else                            // speed changed: pitch and fine follow it
    {
        const double semitones = 12.0 * std::log2 (juce::jmax (0.01, speed));
        const double wholeSemitones = std::round (semitones);
        setParameterValue ("pitch", (float) juce::jlimit (-24.0, 24.0, wholeSemitones));
        setParameterValue ("fine", (float) juce::jlimit (-100.0, 100.0, (semitones - wholeSemitones) * 100.0));
    }
}

void CutbenchEditor::timerCallback()
{
    playStopButton.setPlaying (proc.getPlayhead() >= 0);
    snapButton.setButtonText (snapButton.getToggleState() ? "Snap: On" : "Snap: Off");   // also follows host automation
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

    for (const auto& rack : { rack1Area, rack2Area })
    {
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (rack.toFloat(), 6.0f);
        g.setColour (Theme::grid);
        g.drawRoundedRectangle (rack.toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    // dividers between the groups of the first rack
    g.setColour (Theme::grid);
    for (const int x : { dividerX1, dividerX2 })
        g.drawVerticalLine (x, (float) rack1Area.getY() + 10.0f, (float) rack1Area.getBottom() - 10.0f);
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

    // two racks at the bottom: 1) pads, chop equal, auto slice   2) playback (envelope, pitch, direction ...)
    const int rack1Height = 60, rack2Height = 92, rackGap = 8;
    rack2Area = r.removeFromBottom (rack2Height);
    r.removeFromBottom (rackGap);
    rack1Area = r.removeFromBottom (rack1Height);
    r.removeFromBottom (10);

    // pad grid on the right (a square area), waveform takes everything else
    const int padSide = juce::jlimit (200, 360, juce::jmin (r.getHeight(), r.getWidth() / 3));
    auto padArea = r.removeFromRight (padSide);
    r.removeFromRight (10);
    padGrid.setBounds (padArea.removeFromTop (padSide));   // four square pads per row and column
    waveform.setBounds (r);

    // ---- rack 1: three groups side by side, each with a title above it ----
    {
        auto inner = rack1Area.reduced (14, 6);
        const auto captionRow = inner.removeFromTop (14);
        const auto controlRow = inner.removeFromTop (30);
        int x = inner.getX();

        auto put = [&] (juce::Component& comp, int width, int gapAfter = 6)
        {
            comp.setBounds (x, controlRow.getY(), width, controlRow.getHeight());
            x += width + gapAfter;
        };

        const int groupGap = 14;

        int start = x;
        put (setPadButton, 76);
        put (clearPadButton, 80);
        put (clearAllButton, 76, 0);
        padsCaption.setBounds (start, captionRow.getY(), x - start, captionRow.getHeight());
        x += groupGap;
        dividerX1 = x;
        x += groupGap;

        start = x;
        put (chopButton, 90);
        put (chopCountSlider, 84, 0);
        chopCaption.setBounds (start, captionRow.getY(), x - start + 60, captionRow.getHeight());
        x += groupGap;
        dividerX2 = x;
        x += groupGap;

        start = x;
        put (autoSliceButton, 90);
        put (sensitivitySlider, 92);
        put (snapButton, 84, 0);
        autoCaption.setBounds (start, captionRow.getY(), x - start, captionRow.getHeight());
    }

    // ---- rack 2: knobs and parameters, the control on top and its label below it ----
    auto c = rack2Area.reduced (12, 8);

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
