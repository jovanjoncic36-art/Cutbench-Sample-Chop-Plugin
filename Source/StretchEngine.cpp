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

void StretchEngine::reset()
{
    if (impl != nullptr)
        impl->stretch.reset();
}

void StretchEngine::setTransposeFactor (float factor)
{
    if (impl != nullptr)
        impl->stretch.setTransposeFactor (factor);
}

int StretchEngine::latencySamples() const
{
    return impl != nullptr ? (int) (impl->stretch.inputLatency() + impl->stretch.outputLatency()) : 0;
}

void StretchEngine::process (const float* inL, const float* inR, int inFrames, float* outL, float* outR, int outFrames)
{
    if (impl == nullptr)
        return;

    const float* inputs[2]  = { inL, inR };
    float* outputs[2]       = { outL, outR };
    impl->stretch.process (inputs, inFrames, outputs, outFrames);
}

#else   // library not compiled in: stubs that are never called

bool StretchEngine::isAvailable() noexcept { return false; }
void StretchEngine::prepare (double) {}
void StretchEngine::reset() {}
void StretchEngine::setTransposeFactor (float) {}
int  StretchEngine::latencySamples() const { return 0; }
void StretchEngine::process (const float*, const float*, int, float*, float*, int) {}

#endif
