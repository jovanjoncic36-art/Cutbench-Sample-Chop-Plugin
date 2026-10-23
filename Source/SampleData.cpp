#include "SampleData.h"
#include <algorithm>
#include <cmath>

void SampleData::buildPeaks()
{
    const int n = numFrames();
    const int blocks = (n + peakBlock - 1) / peakBlock;
    peakMin.assign ((size_t) blocks, 0.0f);
    peakMax.assign ((size_t) blocks, 0.0f);

    for (int b = 0; b < blocks; ++b)
    {
        const int a   = b * peakBlock;
        const int len = std::min (peakBlock, n - a);

        auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (0) + a, len);
        for (int ch = 1; ch < numChannels(); ++ch)
            range = range.getUnionWith (juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch) + a, len));

        peakMin[(size_t) b] = range.getStart();
        peakMax[(size_t) b] = range.getEnd();
    }
}

void SampleData::getMinMax (double s0, double s1, float& mn, float& mx) const
{
    const juce::int64 total = numFrames();
    mn = 0.0f;
    mx = 0.0f;
    if (total <= 0)
        return;

    const juce::int64 a = juce::jlimit<juce::int64> (0, total - 1, (juce::int64) std::floor (s0));
    const juce::int64 b = juce::jlimit<juce::int64> (a + 1, total, (juce::int64) std::ceil (s1));

    float lo =  1.0e9f;
    float hi = -1.0e9f;

    auto scanRaw = [&] (juce::int64 from, juce::int64 to)
    {
        if (to <= from)
            return;
        for (int ch = 0; ch < numChannels(); ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch) + from,
                                                                       (int) (to - from));
            lo = std::min (lo, r.getStart());
            hi = std::max (hi, r.getEnd());
        }
    };

    const juce::int64 firstBlock = (a + peakBlock - 1) / peakBlock;
    const juce::int64 lastBlock  = b / peakBlock;   // exclusive

    if (lastBlock - firstBlock < 2)
    {
        scanRaw (a, b);   // tiny range: at most a few hundred frames
    }
    else
    {
        scanRaw (a, firstBlock * peakBlock);
        for (juce::int64 k = firstBlock; k < lastBlock; ++k)
        {
            lo = std::min (lo, peakMin[(size_t) k]);
            hi = std::max (hi, peakMax[(size_t) k]);
        }
        scanRaw (lastBlock * peakBlock, b);
    }

    if (lo <= hi)
    {
        mn = lo;
        mx = hi;
    }
}
