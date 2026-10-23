#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <optional>
#include <utility>
#include "PluginProcessor.h"

/*
    The main visual element: full-file waveform with zoom/scroll, click-to-seek (cursor),
    drag-to-select, slice regions with a numbered strip on top, and draggable slice edges.

    Interaction
      click            set cursor (play start) and select the slice under it
      drag             select a region
      double-click     split the slice at that position
      drag an edge     move a slice boundary (shared boundaries move together)
      click strip      select + audition that slice
      Cmd/Ctrl+scroll  zoom around the pointer     scroll / two-finger swipe: pan
      pinch            zoom                        Space: play / stop    Delete: remove slice
*/
class WaveformView : public juce::Component,
                     private juce::ScrollBar::Listener,
                     private juce::Timer
{
public:
    explicit WaveformView (CutbenchProcessor& processor);
    ~WaveformView() override;

    void modelChanged();   // call whenever the processor's sample / slices / selection change

    std::optional<std::pair<juce::int64, juce::int64>> getSelection() const;
    juce::int64 getCursor() const noexcept { return cursor; }

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

    enum class Drag { none, select, boundary };

    juce::Rectangle<int> getWaveArea() const;
    double xToSample (float x) const noexcept { return viewStart + (double) x * samplesPerPixel; }
    float sampleToX (double s) const noexcept { return (float) ((s - viewStart) / samplesPerPixel); }
    juce::int64 clampSample (double s) const;
    int sliceAt (juce::int64 position) const;
    std::optional<juce::int64> hitBoundary (float x) const;

    void clampView();
    void updateScrollBar();
    void zoomAround (float x, double factor);
    void drawWaveform (juce::Graphics&, juce::Rectangle<int> body) const;
    void notifyViewState() { if (onViewStateChanged) onViewStateChanged(); }

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    CutbenchProcessor& proc;
    juce::ScrollBar hbar { false };

    std::shared_ptr<const SampleData> data;
    std::vector<Slice> slices;
    int selectedSlice = -1;

    double viewStart = 0.0;
    double samplesPerPixel = 1.0;
    bool fitted = true;

    juce::int64 cursor = 0;
    juce::int64 selStart = -1, selEnd = -1, selAnchor = 0;
    juce::int64 boundaryPos = 0;
    juce::int64 lastPlayhead = -2;
    Drag drag = Drag::none;
    bool dragged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
