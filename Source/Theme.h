#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>

namespace Theme
{
    const juce::Colour bg      { 0xff111317 };
    const juce::Colour panel   { 0xff1a1d23 };
    const juce::Colour panelHi { 0xff272b34 };
    const juce::Colour waveBg  { 0xff0d0f12 };
    const juce::Colour grid    { 0xff2a2e37 };
    const juce::Colour text    { 0xffe8e9ec };
    const juce::Colour dim     { 0xff8b919c };
    const juce::Colour accent  { 0xffffa726 };
    const juce::Colour wave    { 0xff79d7c4 };

    // Golden-ratio hue stepping keeps neighbouring pads visually distinct.
    inline juce::Colour padColour (int index)
    {
        return juce::Colour::fromHSV (std::fmod ((float) index * 0.61803f, 1.0f), 0.55f, 0.95f, 1.0f);
    }

    inline juce::String formatTime (double seconds)
    {
        seconds = std::max (0.0, seconds);
        const int mins = (int) (seconds / 60.0);
        return juce::String (mins) + ":" + juce::String (seconds - mins * 60.0, 3).paddedLeft ('0', 6);
    }
}

class CutbenchLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CutbenchLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, Theme::bg);

        setColour (juce::TextButton::buttonColourId,   Theme::panelHi);
        setColour (juce::TextButton::buttonOnColourId, Theme::accent);
        setColour (juce::TextButton::textColourOffId,  Theme::text);
        setColour (juce::TextButton::textColourOnId,   juce::Colours::black);

        setColour (juce::Label::textColourId, Theme::text);

        setColour (juce::Slider::trackColourId,          Theme::accent.darker (0.55f));
        setColour (juce::Slider::backgroundColourId,     Theme::panelHi);
        setColour (juce::Slider::textBoxTextColourId,    Theme::text);
        setColour (juce::Slider::textBoxOutlineColourId, Theme::grid);

        setColour (juce::ComboBox::backgroundColourId, Theme::panelHi);
        setColour (juce::ComboBox::textColourId,       Theme::text);
        setColour (juce::ComboBox::outlineColourId,    Theme::grid);
        setColour (juce::ComboBox::arrowColourId,      Theme::dim);

        setColour (juce::PopupMenu::backgroundColourId,            Theme::panel);
        setColour (juce::PopupMenu::textColourId,                  Theme::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.darker (0.4f));

        setColour (juce::ScrollBar::thumbColourId, Theme::accent.withAlpha (0.55f));

        setColour (juce::Slider::rotarySliderFillColourId,    Theme::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, Theme::panelHi);
        setColour (juce::Slider::thumbColourId,               Theme::text);
    }

    // Knobs are drawn to fill their whole (clickable) circle: body, value ring and pointer
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        juce::ignoreUnused (slider);

        const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (1.5f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + sliderPos * (endAngle - startAngle);

        g.setColour (Theme::panelHi);
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

        const float ringWidth = juce::jmax (3.0f, radius * 0.17f);
        const float ringRadius = radius - ringWidth * 0.5f;
        const juce::PathStrokeType ringStroke (ringWidth, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, startAngle, endAngle, true);
        g.setColour (Theme::grid);
        g.strokePath (track, ringStroke);

        juce::Path value;
        value.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f, startAngle, angle, true);
        g.setColour (Theme::accent);
        g.strokePath (value, ringStroke);

        const float inner = radius * 0.28f;
        const float outer = radius - ringWidth - radius * 0.12f;
        juce::Path pointer;
        pointer.startNewSubPath (centre.x + inner * std::sin (angle), centre.y - inner * std::cos (angle));
        pointer.lineTo          (centre.x + outer * std::sin (angle), centre.y - outer * std::cos (angle));
        g.setColour (Theme::text);
        g.strokePath (pointer, juce::PathStrokeType (juce::jmax (2.5f, radius * 0.09f),
                                                     juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Drag bars: value text drawn by us (no text box), so drag and double-click-to-reset always reach the slider.
    // Ranges that include zero fill from the centre.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearBar)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const juce::Rectangle<float> area ((float) x, (float) y, (float) width, (float) height);
        g.setColour (Theme::panelHi);
        g.fillRoundedRectangle (area, 4.0f);

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float zeroX = bipolar ? area.getX() + area.getWidth() * (float) slider.valueToProportionOfLength (0.0)
                                    : area.getX();
        const float a = juce::jmin (zeroX, sliderPos);
        const float b = juce::jmax (zeroX, sliderPos);

        g.setColour (Theme::accent.darker (0.55f));
        g.fillRoundedRectangle (juce::Rectangle<float> (a, area.getY(), juce::jmax (0.0f, b - a), area.getHeight()), 4.0f);

        g.setColour (Theme::grid);
        g.drawRoundedRectangle (area.reduced (0.5f), 4.0f, 1.0f);

        g.setColour (slider.isEnabled() ? Theme::text : Theme::dim);
        g.setFont (13.0f);
        g.drawText (slider.getTextFromValue (slider.getValue()), area, juce::Justification::centred, false);
    }
};
