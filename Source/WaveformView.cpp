#include "WaveformView.h"
#include "Theme.h"
#include <cmath>

namespace
{
    constexpr int kScrollBarHeight = 12;
    constexpr double kMinSamplesPerPixel = 1.0;
}

WaveformView::WaveformView (CutbenchProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);

    hbar.setAutoHide (false);
    hbar.addListener (this);
    addAndMakeVisible (hbar);

    startTimerHz (30);
}

WaveformView::~WaveformView()
{
    stopTimer();
    hbar.removeListener (this);
}

//==============================================================================
// Model sync
//==============================================================================
void WaveformView::modelChanged()
{
    auto s = proc.getSample();
    const bool newSample = (s != data);

    data = std::move (s);
    slices = proc.getSlices();
    selectedSlice = proc.getSelectedSlice();

    if (newSample)
    {
        cursor = 0;
        selStart = selEnd = -1;
        zoomToFit();
        notifyViewState();
    }

    updateScrollBar();
    repaint();
}

std::optional<std::pair<juce::int64, juce::int64>> WaveformView::getSelection() const
{
    if (data != nullptr && selStart >= 0 && selEnd - selStart >= 16)
        return std::make_pair (selStart, selEnd);
    return std::nullopt;
}

void WaveformView::timerCallback()
{
    const auto ph = proc.getPlayhead();
    if (ph != lastPlayhead)
    {
        lastPlayhead = ph;
        repaint();
    }
}

//==============================================================================
// Playback requests
//==============================================================================
void WaveformView::play()
{
    if (data == nullptr)
        return;

    if (auto sel = getSelection())
        proc.previewRegion (sel->first, sel->second);
    else
        proc.previewRegion (cursor, (juce::int64) data->numFrames());   // from the cursor to the end
}

void WaveformView::togglePlayback()
{
    if (proc.getPlayhead() >= 0)
        proc.stopPreview();
    else
        play();
}

//==============================================================================
// View geometry
//==============================================================================
juce::Rectangle<int> WaveformView::getWaveArea() const
{
    return getLocalBounds().withTrimmedBottom (kScrollBarHeight);
}

juce::int64 WaveformView::clampSample (double s) const
{
    const double total = data != nullptr ? (double) data->numFrames() : 0.0;
    return (juce::int64) juce::jlimit (0.0, total, s);
}

void WaveformView::clampView()
{
    if (data == nullptr)
        return;

    const double total = (double) data->numFrames();
    const double w = (double) juce::jmax (1, getWaveArea().getWidth());
    const double maxSpp = juce::jmax (kMinSamplesPerPixel, total / w);

    samplesPerPixel = juce::jlimit (kMinSamplesPerPixel, maxSpp, samplesPerPixel);
    const double visible = samplesPerPixel * w;
    viewStart = juce::jlimit (0.0, juce::jmax (0.0, total - visible), viewStart);
}

void WaveformView::updateScrollBar()
{
    if (data == nullptr)
    {
        hbar.setRangeLimits (0.0, 1.0, juce::dontSendNotification);
        hbar.setCurrentRange (0.0, 1.0, juce::dontSendNotification);
        return;
    }

    const double total = (double) data->numFrames();
    const double visible = samplesPerPixel * (double) getWaveArea().getWidth();
    hbar.setRangeLimits (0.0, total, juce::dontSendNotification);
    hbar.setCurrentRange (viewStart, visible, juce::dontSendNotification);
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    viewStart = newRangeStart;
    clampView();
    repaint();
}

void WaveformView::zoomAround (float x, double factor)
{
    if (data == nullptr)
        return;

    fitted = false;
    const double anchor = xToSample (x);
    samplesPerPixel *= factor;
    clampView();                                    // clamps samplesPerPixel first ...
    viewStart = anchor - (double) x * samplesPerPixel;   // ... then re-anchors under the pointer
    clampView();
    updateScrollBar();
    repaint();
}

void WaveformView::zoomBy (double factor)
{
    zoomAround ((float) getWaveArea().getWidth() * 0.5f, factor);
}

void WaveformView::zoomToFit()
{
    fitted = true;
    if (data != nullptr)
    {
        viewStart = 0.0;
        samplesPerPixel = (double) data->numFrames() / (double) juce::jmax (1, getWaveArea().getWidth());
        clampView();
    }
    updateScrollBar();
    repaint();
}

void WaveformView::resized()
{
    hbar.setBounds (getLocalBounds().removeFromBottom (kScrollBarHeight));

    if (fitted)
    {
        zoomToFit();
    }
    else
    {
        clampView();
        updateScrollBar();
    }
}

//==============================================================================
// Hit testing
//==============================================================================
int WaveformView::sliceAt (juce::int64 position) const
{
    for (size_t i = 0; i < slices.size(); ++i)
        if (position >= slices[i].start && position < slices[i].end)
            return (int) i;
    return -1;
}

std::optional<juce::int64> WaveformView::hitBoundary (float x) const
{
    std::optional<juce::int64> best;
    float bestDistance = kEdgeGrabPixels;

    for (const auto& s : slices)
        for (const juce::int64 pos : { s.start, s.end })
        {
            const float d = std::abs (sampleToX ((double) pos) - x);
            if (d <= bestDistance)
            {
                bestDistance = d;
                best = pos;
            }
        }
    return best;
}

//==============================================================================
// Painting
//==============================================================================
void WaveformView::drawWaveform (juce::Graphics& g, juce::Rectangle<int> body) const
{
    const int w = body.getWidth();
    const float halfH = (float) body.getHeight() * 0.5f - 2.0f;
    const float midY = (float) body.getCentreY();
    const double total = (double) data->numFrames();

    g.setColour (Theme::wave);

    for (int x = 0; x < w; ++x)
    {
        const double s0 = viewStart + (double) x * samplesPerPixel;
        if (s0 >= total)
            break;

        float mn = 0.0f, mx = 0.0f;
        data->getMinMax (s0, s0 + samplesPerPixel, mn, mx);

        const float y0 = midY - mx * halfH;
        const float y1 = midY - mn * halfH;
        g.fillRect ((float) (body.getX() + x), y0, 1.0f, juce::jmax (1.0f, y1 - y0));
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::waveBg);
    const auto area = getWaveArea();

    if (data == nullptr)
    {
        g.setColour (Theme::dim);
        g.setFont (16.0f);
        g.drawFittedText ("Load or drop an audio file", area, juce::Justification::centred, 1);
        return;
    }

    const auto body = area.withTrimmedTop (kStripHeight);
    const float w = (float) area.getWidth();

    g.setColour (Theme::grid);
    g.drawHorizontalLine (body.getCentreY(), 0.0f, w);

    // Slice tints
    for (size_t i = 0; i < slices.size(); ++i)
    {
        const float x0 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) slices[i].start));
        const float x1 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) slices[i].end));
        if (x1 <= x0)
            continue;

        g.setColour (Theme::sliceColour ((int) i).withAlpha ((int) i == selectedSlice ? 0.22f : 0.09f));
        g.fillRect (x0, (float) body.getY(), x1 - x0, (float) body.getHeight());
    }

    drawWaveform (g, body);

    // Region selection
    if (auto sel = getSelection())
    {
        const float x0 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) sel->first));
        const float x1 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) sel->second));
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.fillRect (x0, (float) area.getY(), x1 - x0, (float) area.getHeight());
    }

    // Slice strip with numbers (and the MIDI key that plays each slice), then boundary lines
    const int baseNote = juce::roundToInt (proc.apvts.getRawParameterValue ("baseNote")->load());
    g.setFont (11.0f);
    for (size_t i = 0; i < slices.size(); ++i)
    {
        const auto colour = Theme::sliceColour ((int) i);
        const float rx0 = sampleToX ((double) slices[i].start);
        const float rx1 = sampleToX ((double) slices[i].end);
        const float x0 = juce::jlimit (0.0f, w, rx0);
        const float x1 = juce::jlimit (0.0f, w, rx1);

        if (x1 > x0)
        {
            const juce::Rectangle<float> strip (x0, (float) area.getY(), x1 - x0, (float) kStripHeight - 1.0f);
            g.setColour (colour.withAlpha ((int) i == selectedSlice ? 0.95f : 0.55f));
            g.fillRect (strip);

            if (strip.getWidth() > 16.0f)
            {
                g.setColour (juce::Colours::black.withAlpha (0.85f));
                juce::String label (juce::String ((int) i + 1));
                const int note = baseNote + (int) i;
                if (strip.getWidth() > 48.0f && note <= 127)
                    label += "   " + juce::MidiMessage::getMidiNoteName (note, true, true, 3);

                g.drawText (label, strip.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
            }
        }

        g.setColour (colour);
        for (const float x : { rx0, rx1 })
            if (x >= 0.0f && x <= w)
                g.drawLine (x, (float) area.getY(), x, (float) area.getBottom(), 1.0f);
    }

    // Cursor (click position)
    const float cx = sampleToX ((double) cursor);
    if (cx >= 0.0f && cx <= w)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawVerticalLine ((int) cx, (float) body.getY(), (float) area.getBottom());
    }

    // Playhead
    if (lastPlayhead >= 0)
    {
        const float px = sampleToX ((double) lastPlayhead);
        if (px >= 0.0f && px <= w)
        {
            g.setColour (Theme::accent);
            g.fillRect (px - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight());
        }
    }
}

//==============================================================================
// Mouse / keyboard
//==============================================================================
void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag = Drag::none;
    dragged = false;

    if (data == nullptr)
        return;

    if (auto b = hitBoundary (e.position.x))
    {
        drag = Drag::boundary;
        boundaryPos = *b;
        return;
    }

    const auto pos = clampSample (xToSample (e.position.x));

    if (e.position.y < (float) kStripHeight)    // numbered strip: select + audition
    {
        const int idx = sliceAt (pos);
        if (idx >= 0)
        {
            proc.setSelectedSlice (idx);
            proc.previewSlice (idx);
        }
        return;
    }

    drag = Drag::select;
    selAnchor = pos;
    cursor = pos;
    selStart = selEnd = -1;
    notifyViewState();
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (data == nullptr)
        return;

    const auto pos = clampSample (xToSample (e.position.x));

    if (drag == Drag::boundary)
    {
        dragged = true;
        boundaryPos = proc.moveBoundary (boundaryPos, pos);
    }
    else if (drag == Drag::select)
    {
        if (! dragged && std::abs (e.getDistanceFromDragStartX()) < 3)
            return;

        dragged = true;
        selStart = juce::jmin (selAnchor, pos);
        selEnd   = juce::jmax (selAnchor, pos);
        cursor   = selStart;
        notifyViewState();
        repaint();
    }
}

void WaveformView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::select && ! dragged && data != nullptr)
    {
        const int idx = sliceAt (cursor);
        if (idx >= 0)
            proc.setSelectedSlice (idx);
    }
    drag = Drag::none;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (data == nullptr || e.position.y < (float) kStripHeight || hitBoundary (e.position.x))
        return;

    proc.splitAt (clampSample (xToSample (e.position.x)));
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const bool overEdge = data != nullptr && hitBoundary (e.position.x).has_value();
    setMouseCursor (overEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (data == nullptr)
        return;

    if (e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown())
    {
        zoomAround (e.position.x, std::exp (-(double) wheel.deltaY * 3.0));
        return;
    }

    const float d = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    viewStart -= (double) d * samplesPerPixel * (double) getWaveArea().getWidth() * 0.6;
    clampView();
    updateScrollBar();
    repaint();
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (scaleFactor > 0.0f)
        zoomAround (e.position.x, 1.0 / (double) scaleFactor);
}

bool WaveformView::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlayback();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        proc.removeSlice (proc.getSelectedSlice());
        return true;
    }
    return false;
}
