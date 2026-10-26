#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "IconButtons.h"
#include "WaveformView.h"
#include "PadGrid.h"

class CutbenchEditor : public juce::AudioProcessorEditor,
                       private juce::ChangeListener,
                       private juce::Timer,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater,
                       public juce::FileDragAndDropTarget
{
public:
    explicit CutbenchEditor (CutbenchProcessor&);
    ~CutbenchEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void setParameterValue (const char* id, float value);
    void chooseFile();
    void openSettings();
    void refreshStatus();
    void nudgePitch (int semitones);

    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment   = juce::AudioProcessorValueTreeState::ButtonAttachment;

    CutbenchProcessor& proc;
    CutbenchLookAndFeel lnf;   // declared first so it outlives every component using it

    WaveformView waveform;
    PadGrid padGrid;

    PlayStopButton playStopButton;
    GearButton settingsButton;

    juce::TextButton loadButton { "Load..." },
                     setPadButton { "Set Pad" }, clearPadButton { "Clear Pad" }, clearAllButton { "Clear All" },
                     chopButton { "Chop Equal" }, autoSliceButton { "Auto Slice" }, snapButton { "Snap" },
                     zoomInButton { "+" }, zoomOutButton { "-" }, fitButton { "Fit" },
                     reverseButton { "Reverse" }, loopButton { "Loop" },
                     pitchDownButton { "-" }, pitchUpButton { "+" };

    juce::Label fileLabel, infoLabel, versionLabel, statusLabel, hintLabel;
    juce::Label pitchLabel, fineLabel, speedLabel, engineLabel, directionLabel, repeatLabel;
    juce::Label attackLabel, releaseLabel, gainLabel, modeLabel, padModeLabel;

    // gainSlider, attackSlider and releaseSlider are rotary knobs, the others are drag bars.
    // Base note and declick fade live in the settings menu (the gear button).
    juce::Slider pitchSlider, fineSlider, speedSlider, attackSlider, releaseSlider, gainSlider, chopCountSlider,
                 sensitivitySlider;
    juce::ComboBox engineBox, modeBox, padModeBox;

    std::unique_ptr<SliderAttachment> pitchAttachment, fineAttachment, speedAttachment, attackAttachment,
                                      releaseAttachment, gainAttachment, sensitivityAttachment;
    std::unique_ptr<ComboBoxAttachment> engineAttachment, modeAttachment, padModeAttachment;
    std::unique_ptr<ButtonAttachment> reverseAttachment, loopAttachment, snapAttachment;

    // Vinyl: pitch and speed are one thing and follow each other. Keep Length: they are independent.
    std::atomic<int> pendingParams { 0 };   // 1 = pitch / fine changed, 2 = speed changed, 4 = engine changed
    std::atomic<bool> syncing { false };

    juce::Rectangle<int> controlsArea;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CutbenchEditor)
};
