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

    // Golden-ratio hue stepping keeps neighbouring slices visually distinct.
    inline juce::Colour sliceColour (int index)
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
    }
};
