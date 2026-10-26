#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <vector>

constexpr int kNumPads = 16;

// One pad = one cue point in the source audio (frames of the source file). Never modifies the audio.
//   start < 0  -> the pad is empty
//   end   < 0  -> automatic end (the next cue point, or the end of the file for the last cue)
struct Pad
{
    juce::int64 start = -1;
    juce::int64 end   = -1;
    bool isSet() const noexcept { return start >= 0; }
};

// A detected onset (drum hit / sudden change). strength is 0..1: how far the level jumps above what came just
// before it (30 dB or more = 1).
struct Transient
{
    juce::int64 position = 0;
    float strength = 0.0f;
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
    std::vector<Transient> transients;   // sorted by position

    int numFrames() const noexcept   { return audio.getNumSamples(); }
    int numChannels() const noexcept { return audio.getNumChannels(); }

    void buildPeaks();
    void findTransients();   // fills `transients`; runs once on the loader thread

    // Min/max over [startSample, endSample) across all channels. Safe for any input range.
    void getMinMax (double startSample, double endSample, float& minOut, float& maxOut) const;
};
