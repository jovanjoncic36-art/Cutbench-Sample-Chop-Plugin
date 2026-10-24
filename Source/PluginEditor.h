#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "WaveformView.h"
#include "PadGrid.h"

class CutbenchEditor : public juce::AudioProcessorEditor,
                       private juce::ChangeListener,
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
    void chooseFile();
    void refreshStatus();

    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    CutbenchProcessor& proc;
    CutbenchLookAndFeel lnf;   // declared first so it outlives every component using it

    WaveformView waveform;
    PadGrid padGrid;

    juce::TextButton loadButton { "Load..." }, playButton { "Play" }, stopButton { "Stop" },
                     setPadButton { "Set Pad" }, clearPadButton { "Clear Pad" }, clearAllButton { "Clear All" },
                     chopButton { "Chop Equal" },
                     zoomInButton { "+" }, zoomOutButton { "-" }, fitButton { "Fit" };

    juce::Label fileLabel, infoLabel, statusLabel, hintLabel;
    juce::Label gainLabel, noteLabel, modeLabel, padModeLabel, fadeLabel;
    juce::Slider gainSlider, noteSlider, fadeSlider, chopCountSlider;
    juce::ComboBox modeBox, padModeBox;

    std::unique_ptr<SliderAttachment> gainAttachment, noteAttachment, fadeAttachment;
    std::unique_ptr<ComboBoxAttachment> modeAttachment, padModeAttachment;

    juce::Rectangle<int> controlsArea;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CutbenchEditor)
};
