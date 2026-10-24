#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>

constexpr int kNumPads = 16;

// One pad = one cue point in the source audio (frames of the source file). Never modifies the audio.
//   start < 0  -> the pad is empty
//   end   < 0  -> automatic end (end of file in Cue mode, next cue point in Slice mode)
struct Pad
{
    juce::int64 start = -1;
    juce::int64 end   = -1;
    bool isSet() const noexcept { return start >= 0; }
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
