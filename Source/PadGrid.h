#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

/*
    4x4 pad grid. Pad 1 is bottom-left (like a Drum Rack / MPC), numbering runs left to right, bottom to top.
    A pad press behaves exactly like its MIDI key: with a cue it plays, when empty and something is playing
    it sets a cue (live chop).  Right-click (or Ctrl-click) clears a pad.
*/
class PadGrid : public juce::Component,
                private juce::Timer
{
public:
    explicit PadGrid (CutbenchProcessor& processor);
    ~PadGrid() override;

    void modelChanged();   // sample loaded

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> cellBounds (int pad) const;
    int padAt (juce::Point<float> p) const;
    void refresh();
    void timerCallback() override;

    CutbenchProcessor& proc;
    std::shared_ptr<const SampleData> data;
    CutbenchProcessor::PadArray pads;
    int selectedPad = 0;
    juce::uint32 seenVersion = ~(juce::uint32) 0;
    juce::uint32 activeMask = 0;
    int heldPad = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadGrid)
};
