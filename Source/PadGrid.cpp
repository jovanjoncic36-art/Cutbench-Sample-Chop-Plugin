#include "PadGrid.h"
#include "Theme.h"

namespace
{
    constexpr int kCols = 4;
    constexpr int kRows = 4;
    constexpr float kGap = 6.0f;
}

PadGrid::PadGrid (CutbenchProcessor& p) : proc (p)
{
    refresh();
    startTimerHz (30);
}

PadGrid::~PadGrid()
{
    stopTimer();
}

void PadGrid::refresh()
{
    seenVersion = proc.getPadVersion();   // read first, so a change during the snapshot is caught next tick
    pads = proc.getPads();
    selectedPad = proc.getSelectedPad();
    activeMask = proc.getActivePadMask();
}

void PadGrid::modelChanged()
{
    data = proc.getSample();
    refresh();
    repaint();
}

void PadGrid::timerCallback()
{
    if (proc.getPadVersion() != seenVersion || proc.getActivePadMask() != activeMask)
    {
        refresh();
        repaint();
    }
}

juce::Rectangle<float> PadGrid::cellBounds (int pad) const
{
    // Every pad is a square: the side is the largest that fits, the grid is centred horizontally, top-aligned
    const auto area = getLocalBounds().toFloat();
    const float side = juce::jmax (1.0f, juce::jmin ((area.getWidth()  - kGap * (kCols - 1)) / (float) kCols,
                                                     (area.getHeight() - kGap * (kRows - 1)) / (float) kRows));
    const float gridWidth = side * (float) kCols + kGap * (float) (kCols - 1);
    const float originX = area.getX() + (area.getWidth() - gridWidth) * 0.5f;

    const int col = pad % kCols;
    const int row = (kRows - 1) - pad / kCols;   // pad 1 at the bottom
    return { originX + (float) col * (side + kGap), area.getY() + (float) row * (side + kGap), side, side };
}

int PadGrid::padAt (juce::Point<float> p) const
{
    for (int i = 0; i < kNumPads; ++i)
        if (cellBounds (i).contains (p))
            return i;
    return -1;
}

void PadGrid::paint (juce::Graphics& g)
{
    const int baseNote = proc.getBaseNote();

    for (int i = 0; i < kNumPads; ++i)
    {
        const auto cell = cellBounds (i);
        const auto colour = Theme::padColour (i);
        const bool isSet = pads[(size_t) i].isSet();
        const bool sounding = isSet && ((activeMask >> i) & 1u) != 0;   // a cleared pad is never lit
        const bool selected = (i == selectedPad);

        g.setColour (sounding ? colour.withAlpha (0.95f)
                              : isSet ? colour.withAlpha (0.30f) : Theme::panelHi);
        g.fillRoundedRectangle (cell, 6.0f);

        g.setColour (selected ? Theme::accent : Theme::grid);
        g.drawRoundedRectangle (cell.reduced (1.0f), 6.0f, selected ? 2.5f : 1.0f);

        const auto textColour = sounding ? juce::Colours::black : (isSet ? Theme::text : Theme::dim);
        g.setColour (textColour);

        g.setFont (g.getCurrentFont().withHeight (18.0f).boldened());
        g.drawText (juce::String (i + 1), cell.reduced (8.0f, 6.0f), juce::Justification::topLeft, false);

        g.setFont (g.getCurrentFont().withHeight (12.0f));
        const int note = baseNote + i;
        if (note >= 0 && note <= 127)
            g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 3),
                        cell.reduced (8.0f, 6.0f), juce::Justification::bottomLeft, false);

        if (! isSet)
            g.drawText ("empty", cell.reduced (8.0f, 6.0f), juce::Justification::topRight, false);
    }
}

void PadGrid::mouseDown (const juce::MouseEvent& e)
{
    heldPad = padAt (e.position);
    if (heldPad < 0)
        return;

    if (e.mods.isPopupMenu())   // right-click / ctrl-click: clear this pad
    {
        proc.clearPad (heldPad);
        heldPad = -1;
        refresh();
        repaint();
        return;
    }

    proc.setSelectedPad (heldPad);
    proc.padPressed (heldPad);
    refresh();
    repaint();
}

void PadGrid::mouseUp (const juce::MouseEvent&)
{
    if (heldPad >= 0)
        proc.padReleased (heldPad);
    heldPad = -1;
}
