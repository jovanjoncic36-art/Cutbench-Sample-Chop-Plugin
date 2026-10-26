#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <optional>
#include <utility>
#include "PluginProcessor.h"

/*
    The main visual element: full-file waveform with zoom/scroll, click-to-seek (cursor),
    drag-to-select, and one marker flag per pad that has a cue point.

    Interaction
      click            set cursor (play start)
      drag             select a region
      double-click     set the SELECTED pad's cue at that position (then the next empty pad is selected)
      drag a marker    move that pad's cue point
      drag the handle  at the bottom edge of the waveform: set where the selected pad stops (double-click: automatic)
      click a flag     select that pad and play it (hold = gate)
      Cmd/Ctrl+scroll  zoom around the pointer     scroll / two-finger swipe: pan
      pinch            zoom                        Space: play / stop    Delete: clear the selected pad
*/
class WaveformView : public juce::Component,
                     private juce::ScrollBar::Listener,
                     private juce::Timer
{
public:
    explicit WaveformView (CutbenchProcessor& processor);
    ~WaveformView() override;

    void modelChanged();   // sample loaded / status changed

    std::optional<std::pair<juce::int64, juce::int64>> getSelection() const;
    juce::int64 getCursor() const noexcept { return cursor; }

    void assignToSelectedPad();    // selection -> region, otherwise cursor -> cue point
    void play();
    void togglePlayback();
    void zoomBy (double factor);   // < 1 zooms in, > 1 zooms out
    void zoomToFit();

    std::function<void()> onViewStateChanged;   // cursor / selection changed

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    static constexpr int kStripHeight = 18;
    static constexpr float kEdgeGrabPixels = 5.0f;
    static constexpr float kFlagWidth = 46.0f;

    enum class Drag { none, select, marker, flag, endHandle };

    juce::Rectangle<int> getWaveArea() const;
    double xToSample (float x) const noexcept { return viewStart + (double) x * samplesPerPixel; }
    float sampleToX (double s) const noexcept { return (float) ((s - viewStart) / samplesPerPixel); }
    juce::int64 clampSample (double s) const;
    int hitMarker (float x) const;     // marker line under x (-1 = none)
    int flagAt (float x) const;        // flag body under x (-1 = none)
    bool inHandleZone (float y) const; // bottom strip of the waveform where the end handle lives
    bool hitEndHandle (float x) const; // end handle of the selected pad under x

    void refreshPads();
    void advanceSelection();
    void clampView();
    void updateScrollBar();
    void zoomAround (float x, double factor);
    void drawWaveform (juce::Graphics&, juce::Rectangle<int> body) const;
    void drawPadMarker (juce::Graphics&, juce::Rectangle<int> area, int pad, int baseNote) const;
    void notifyViewState() { if (onViewStateChanged) onViewStateChanged(); }

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    CutbenchProcessor& proc;
    juce::ScrollBar hbar { false };

    std::shared_ptr<const SampleData> data;
    CutbenchProcessor::PadArray pads;
    int selectedPad = 0;
    juce::uint32 seenPadVersion = ~(juce::uint32) 0;
    float seenThreshold = -1.0f;

    double viewStart = 0.0;
    double samplesPerPixel = 1.0;
    bool fitted = true;

    juce::int64 cursor = 0;
    juce::int64 selStart = -1, selEnd = -1, selAnchor = 0;
    juce::int64 lastPlayhead = -2;
    Drag drag = Drag::none;
    int dragPad = -1;
    bool dragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
