#pragma once

#include <memory>

/*
    Thin wrapper around Signalsmith Stretch (independent pitch shifting / time stretching, MIT licence).
    The rest of the plugin never touches the library directly. When it is not compiled in
    (CUTBENCH_HAS_STRETCH undefined) isAvailable() is false and the engine is never used.

    Realtime rules: prepare() allocates, so call it from prepareToPlay() only. reset(), setTransposeFactor()
    and process() are meant for the audio thread.
*/
class StretchEngine
{
public:
    StretchEngine();
    ~StretchEngine();
    StretchEngine (StretchEngine&&) noexcept;
    StretchEngine& operator= (StretchEngine&&) noexcept;

    static bool isAvailable() noexcept;

    void prepare (double sampleRate);
    void reset();
    void setTransposeFactor (float factor);   // 1 = no pitch change, 2 = one octave up
    int  latencySamples() const;              // approximate delay between input and output, in samples

    // Consumes inFrames input frames and produces outFrames output frames (their ratio sets the time stretch).
    void process (const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
