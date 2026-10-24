#include "StretchEngine.h"

#if CUTBENCH_HAS_STRETCH
 #include "signalsmith-stretch.h"
#endif

struct StretchEngine::Impl
{
#if CUTBENCH_HAS_STRETCH
    signalsmith::stretch::SignalsmithStretch<float> stretch;
#endif
};

StretchEngine::StretchEngine() = default;
StretchEngine::~StretchEngine() = default;
StretchEngine::StretchEngine (StretchEngine&&) noexcept = default;
StretchEngine& StretchEngine::operator= (StretchEngine&&) noexcept = default;

#if CUTBENCH_HAS_STRETCH

bool StretchEngine::isAvailable() noexcept { return true; }

void StretchEngine::prepare (double sampleRate)
{
    impl = std::make_unique<Impl>();
    impl->stretch.presetDefault (2, (float) sampleRate);
}

void StretchEngine::setTransposeFactor (float factor)
{
    if (impl != nullptr)
        impl->stretch.setTransposeFactor (factor);
}

int StretchEngine::startLength (double playbackRate) const
{
    return impl != nullptr ? (int) impl->stretch.outputSeekLength ((float) playbackRate) : 0;
}

void StretchEngine::start (const float* inL, const float* inR, int inFrames)
{
    if (impl == nullptr)
        return;

    const float* channels[2] = { inL, inR };
    const float** inputs = channels;
    impl->stretch.outputSeek (inputs, inFrames);   // includes a reset()
}

void StretchEngine::process (const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames)
{
    if (impl == nullptr)
        return;

    const float* inChannels[2] = { inL, inR };
    float* outChannels[2]      = { outL, outR };
    const float** inputs = inChannels;
    float** outputs = outChannels;
    impl->stretch.process (inputs, inFrames, outputs, outFrames);
}

#else   // library not compiled in: stubs that are never called

bool StretchEngine::isAvailable() noexcept { return false; }
void StretchEngine::prepare (double) {}
void StretchEngine::setTransposeFactor (float) {}
int  StretchEngine::startLength (double) const { return 0; }
void StretchEngine::start (const float*, const float*, int) {}
void StretchEngine::process (const float*, const float*, int, float*, float*, int) {}

#endif
