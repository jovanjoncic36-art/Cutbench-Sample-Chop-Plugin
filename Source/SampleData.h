#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>

// A region of the source audio, in source-file sample frames. Slices never modify the audio.
struct Slice
{
    juce::int64 start = 0;
    juce::int64 end   = 0;
};

// Decoded audio held in memory (1 or 2 channels, float) plus a min/max peak cache for drawing.
// Built on a background thread, then treated as immutable (shared via shared_ptr<const>).
struct SampleData
{
    static constexpr int peakBlock = 64;   // frames per peak entry

    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::File file;
    juce::String name;
    std::vector<float> peakMin, peakMax;

    int numFrames() const noexcept   { return audio.getNumSamples(); }
    int numChannels() const noexcept { return audio.getNumChannels(); }

    void buildPeaks();

    // Min/max over [startSample, endSample) across all channels. Safe for any input range.
    void getMinMax (double startSample, double endSample, float& minOut, float& maxOut) const;
};
