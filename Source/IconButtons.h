#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Theme.h"

// One round transport button: shows a play triangle when idle and a stop square while something is playing.
class PlayStopButton : public juce::Button
{
public:
    PlayStopButton() : juce::Button ("playStop") {}

    void setPlaying (bool isPlaying)
    {
        if (playing != isPlaying)
        {
            playing = isPlaying;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const float d = juce::jmin (area.getWidth(), area.getHeight());
        const auto circle = area.withSizeKeepingCentre (d, d);

        auto fill = playing ? Theme::accent : Theme::panelHi;
        if (down)             fill = fill.darker (0.25f);
        else if (highlighted) fill = fill.brighter (0.15f);
        g.setColour (fill);
        g.fillEllipse (circle);
        g.setColour (Theme::grid);
        g.drawEllipse (circle, 1.0f);

        const auto icon = circle.reduced (d * 0.30f);
        g.setColour (playing ? juce::Colours::black : Theme::text);

        if (playing)
        {
            g.fillRoundedRectangle (icon.reduced (icon.getWidth() * 0.06f), 2.0f);
        }
        else
        {
            juce::Path triangle;
            const float left = icon.getX() + icon.getWidth() * 0.12f;
            triangle.addTriangle (left, icon.getY(), left, icon.getBottom(), icon.getRight(), icon.getCentreY());
            g.fillPath (triangle);
        }
    }

private:
    bool playing = false;
};

// Gear icon for the settings menu
class GearButton : public juce::Button
{
public:
    GearButton() : juce::Button ("settings") {}

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        const float r = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        const auto c = area.getCentre();

        g.setColour (highlighted || down ? Theme::text : Theme::dim);

        for (int i = 0; i < 8; ++i)   // teeth
        {
            juce::Path tooth;
            tooth.addRectangle (-r * 0.17f, -r, r * 0.34f, r * 0.34f);
            g.fillPath (tooth, juce::AffineTransform::rotation ((float) i * juce::MathConstants<float>::twoPi / 8.0f)
                                                     .translated (c.x, c.y));
        }

        const float ringRadius = r * 0.62f;   // body with a hole in the middle
        g.drawEllipse (c.x - ringRadius, c.y - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f, r * 0.34f);
    }
};
