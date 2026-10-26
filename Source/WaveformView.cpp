#include "WaveformView.h"
#include "Theme.h"
#include <algorithm>
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

    refreshPads();
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
void WaveformView::refreshPads()
{
    const auto version = proc.getPadVersion();   // read first, so a change during the snapshot is caught next tick
    pads = proc.getPads();
    selectedPad = proc.getSelectedPad();
    seenPadVersion = version;
}

void WaveformView::modelChanged()
{
    auto s = proc.getSample();
    const bool newSample = (s != data);
    data = std::move (s);
    refreshPads();

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
    bool needsRepaint = false;

    const auto ph = proc.getPlayhead();
    if (ph != lastPlayhead)
    {
        lastPlayhead = ph;
        needsRepaint = true;
    }

    // Pads can also change on the audio thread (live chop), so poll the version.
    if (proc.getPadVersion() != seenPadVersion)
    {
        refreshPads();
        needsRepaint = true;
    }

    const float threshold = proc.getTransientThreshold();   // changes when the Sens control moves
    if (threshold != seenThreshold)
    {
        seenThreshold = threshold;
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

//==============================================================================
// Pad assignment / playback requests
//==============================================================================
void WaveformView::advanceSelection()
{
    const int next = proc.nextEmptyPad (proc.getSelectedPad());
    if (next >= 0)
        proc.setSelectedPad (next);
}

void WaveformView::assignToSelectedPad()
{
    if (data == nullptr)
        return;

    const int pad = proc.getSelectedPad();
    if (auto sel = getSelection())
        proc.setPadRegion (pad, sel->first, sel->second);   // region: plays exactly this part
    else
        proc.setPadStart (pad, cursor);                     // cue point at the cursor

    advanceSelection();
    refreshPads();
    repaint();
}

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
    clampView();                                         // clamps samplesPerPixel first ...
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
int WaveformView::hitMarker (float x) const
{
    int best = -1;
    float bestDistance = kEdgeGrabPixels;

    for (int i = 0; i < kNumPads; ++i)
    {
        if (! pads[(size_t) i].isSet())
            continue;

        const float d = std::abs (sampleToX ((double) pads[(size_t) i].start) - x);
        if (d <= bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

int WaveformView::flagAt (float x) const
{
    int best = -1;   // later flags are drawn on top, so the last match wins
    for (int i = 0; i < kNumPads; ++i)
    {
        if (! pads[(size_t) i].isSet())
            continue;

        const float mx = sampleToX ((double) pads[(size_t) i].start);
        if (x >= mx && x <= mx + kFlagWidth)
            best = i;
    }
    return best;
}

bool WaveformView::inHandleZone (float y) const
{
    return y > (float) getWaveArea().getBottom() - 26.0f;
}

bool WaveformView::hitEndHandle (float x) const
{
    if (selectedPad < 0 || ! pads[(size_t) selectedPad].isSet())
        return false;

    const juce::int64 end = proc.getEffectiveEnd (selectedPad);
    return end >= 0 && std::abs (sampleToX ((double) end) - x) <= kEdgeGrabPixels + 2.0f;
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

void WaveformView::drawPadMarker (juce::Graphics& g, juce::Rectangle<int> area, int pad, int baseNote) const
{
    const float w = (float) area.getWidth();
    const float x = sampleToX ((double) pads[(size_t) pad].start);
    if (x < -kFlagWidth || x > w)
        return;

    const auto colour = Theme::padColour (pad);
    const bool selected = (pad == selectedPad);

    g.setColour (colour.withAlpha (selected ? 1.0f : 0.8f));
    g.drawLine (x, (float) area.getY(), x, (float) area.getBottom(), selected ? 2.0f : 1.0f);

    const juce::Rectangle<float> flag (x, (float) area.getY(), kFlagWidth, (float) kStripHeight - 1.0f);
    g.setColour (colour.withAlpha (selected ? 1.0f : 0.65f));
    g.fillRect (flag);

    juce::String label (pad + 1);
    const int note = baseNote + pad;
    if (note >= 0 && note <= 127)
        label += "  " + juce::MidiMessage::getMidiNoteName (note, true, true, 3);

    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.drawText (label, flag.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
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

    // Every chop is tinted in its pad colour (from its cue to where it stops); the selected one is brighter
    const auto ends = proc.getEffectiveEnds();
    for (int i = 0; i < kNumPads; ++i)
    {
        if (! pads[(size_t) i].isSet() || ends[(size_t) i] <= pads[(size_t) i].start)
            continue;

        const float x0 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) pads[(size_t) i].start));
        const float x1 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) ends[(size_t) i]));
        g.setColour (Theme::padColour (i).withAlpha (i == selectedPad ? 0.30f : 0.17f));
        g.fillRect (x0, (float) body.getY(), juce::jmax (0.0f, x1 - x0), (float) body.getHeight());
    }

    drawWaveform (g, body);

    // End handle of the selected pad: a line plus a grip at the bottom edge
    if (selectedPad >= 0 && pads[(size_t) selectedPad].isSet()
        && ends[(size_t) selectedPad] > pads[(size_t) selectedPad].start)
    {
        const float ex = sampleToX ((double) ends[(size_t) selectedPad]);
        if (ex >= 0.0f && ex <= w)
        {
            const auto colour = Theme::padColour (selectedPad);
            g.setColour (colour.withAlpha (0.7f));
            g.drawLine (ex, (float) body.getY(), ex, (float) body.getBottom(), 1.0f);
            g.setColour (colour);
            g.fillRect (ex - 5.0f, (float) body.getBottom() - 16.0f, 10.0f, 16.0f);
        }
    }

    // Region selection
    if (auto sel = getSelection())
    {
        const float x0 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) sel->first));
        const float x1 = juce::jlimit (-2.0f, w + 2.0f, sampleToX ((double) sel->second));
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.fillRect (x0, (float) area.getY(), x1 - x0, (float) area.getHeight());
    }

    // Faint ticks at the detected transients: the ones Auto Slice and Snap use (they follow the Sens setting)
    {
        const auto& t = data->transients;
        const float threshold = proc.getTransientThreshold();
        auto it = std::lower_bound (t.begin(), t.end(), (juce::int64) xToSample (-2.0f),
                                    [] (const Transient& a, juce::int64 p) { return a.position < p; });

        float lastX = -10.0f;
        g.setColour (Theme::dim.withAlpha (0.85f));
        for (; it != t.end(); ++it)
        {
            const float x = sampleToX ((double) it->position);
            if (x > w + 2.0f)
                break;
            if (it->strength < threshold || x - lastX < 3.0f)
                continue;

            lastX = x;
            g.fillRect (x - 0.5f, (float) body.getY(), 1.0f, 6.0f + 10.0f * it->strength);
        }
    }

    // Pad markers; the selected one is drawn last so it stays on top
    g.setFont (11.0f);
    const int baseNote = proc.getBaseNote();
    for (int i = 0; i < kNumPads; ++i)
        if (pads[(size_t) i].isSet() && i != selectedPad)
            drawPadMarker (g, area, i, baseNote);
    if (selectedPad >= 0 && pads[(size_t) selectedPad].isSet())
        drawPadMarker (g, area, selectedPad, baseNote);

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
    dragPad = -1;
    dragged = false;

    if (data == nullptr)
        return;

    // 0) The end handle of the selected pad (bottom edge): drag to set where it stops
    if (inHandleZone (e.position.y) && hitEndHandle (e.position.x))
    {
        drag = Drag::endHandle;
        dragPad = selectedPad;
        return;
    }

    // 1) A marker line: grab it to move that pad's cue point
    const int marker = hitMarker (e.position.x);
    if (marker >= 0)
    {
        drag = Drag::marker;
        dragPad = marker;
        proc.setSelectedPad (marker);
        refreshPads();
        repaint();
        return;
    }

    // 2) A flag in the top strip: select the pad and play it (released on mouse-up, for Gate mode)
    if (e.position.y < (float) kStripHeight)
    {
        const int flag = flagAt (e.position.x);
        if (flag >= 0)
        {
            drag = Drag::flag;
            dragPad = flag;
            proc.setSelectedPad (flag);
            proc.padPressed (flag);
            refreshPads();
            repaint();
        }
        return;
    }

    // 3) The waveform itself: set the cursor, start a region selection
    drag = Drag::select;
    selAnchor = clampSample (xToSample (e.position.x));
    cursor = selAnchor;
    selStart = selEnd = -1;
    notifyViewState();
    repaint();
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (data == nullptr)
        return;

    const auto pos = clampSample (xToSample (e.position.x));

    if (drag == Drag::marker)
    {
        dragged = true;
        proc.movePadStart (dragPad, pos);
        refreshPads();
        repaint();
    }
    else if (drag == Drag::endHandle)
    {
        dragged = true;
        proc.setPadEnd (dragPad, pos);
        refreshPads();
        repaint();
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
    if (drag == Drag::flag && dragPad >= 0)
        proc.padReleased (dragPad);

    drag = Drag::none;
    dragPad = -1;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (data == nullptr || e.position.y < (float) kStripHeight)
        return;

    if (inHandleZone (e.position.y) && hitEndHandle (e.position.x))   // back to the automatic end
    {
        proc.clearPadEnd (selectedPad);
        refreshPads();
        repaint();
        return;
    }

    if (hitMarker (e.position.x) >= 0)
        return;

    proc.setPadStart (proc.getSelectedPad(), clampSample (xToSample (e.position.x)));
    advanceSelection();
    refreshPads();
    repaint();
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const bool overMarker = data != nullptr && ((inHandleZone (e.position.y) && hitEndHandle (e.position.x))
                                                || hitMarker (e.position.x) >= 0);
    setMouseCursor (overMarker ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
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
        proc.clearPad (proc.getSelectedPad());
        refreshPads();
        repaint();
        return true;
    }
    return false;
}
