#pragma once

#include <memory>

/*
    Thin wrapper around Signalsmith Stretch (independent pitch shifting / time stretching, MIT licence).
    The rest of the plugin never touches the library directly. When it is not compiled in
    (CUTBENCH_HAS_STRETCH undefined) isAvailable() is false and the engine is never used.

    Realtime rules: prepare() allocates, so call it from prepareToPlay() only. setTransposeFactor(), start()
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
    void setTransposeFactor (float factor);   // 1 = no pitch change, 2 = one octave up

    // Input frames needed to start aligned, which is also how far the audible output lags behind the input
    // that has been fed in so far. `playbackRate` = source frames consumed per output frame.
    int startLength (double playbackRate) const;

    // Resets the engine and primes it with the first `inFrames` frames (use startLength()), so that the
    // next process() call already outputs the beginning of the sound without extra latency.
    void start (const float* inL, const float* inR, int inFrames);

    // Consumes inFrames input frames and produces outFrames output frames (their ratio sets the time stretch).
    void process (const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
