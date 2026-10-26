#include "SampleData.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

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


namespace
{
    constexpr int   kOnsetHop = 256;       // frames per analysis step (about 6 ms)
    constexpr float kOnsetMinDb = 4.0f;    // a hit must rise at least this far above what came before
    constexpr float kOnsetCapDb = 30.0f;   // rises of this size or more count as full strength
}

// Simple, fast onset detection: energy of the high-passed signal per hop, in dB; an onset is a hop that rises well
// above the loudest of the three hops before it. Peaks closer than 40 ms are merged, then each position is refined
// to the first sample of the attack and stepped back to the nearest zero crossing.
void SampleData::findTransients()
{
    transients.clear();

    const int n = numFrames();
    const int numHops = n / kOnsetHop;
    if (numHops < 8)
        return;

    const float* l = audio.getReadPointer (0);
    const float* r = numChannels() > 1 ? audio.getReadPointer (1) : l;
    auto mono = [l, r] (int i) { return 0.5f * (l[i] + r[i]); };

    std::vector<float> db ((size_t) numHops);
    for (int h = 0; h < numHops; ++h)
    {
        const int start = h * kOnsetHop;
        float previous = start > 0 ? mono (start - 1) : 0.0f;
        double energy = 0.0;
        for (int i = 0; i < kOnsetHop; ++i)
        {
            const float s = mono (start + i);
            const float d = s - previous;
            energy += (double) d * (double) d;
            previous = s;
        }
        db[(size_t) h] = 10.0f * std::log10 ((float) (energy / kOnsetHop) + 1.0e-8f);
    }

    std::vector<float> onset ((size_t) numHops, 0.0f);
    for (int h = 3; h < numHops; ++h)
    {
        const float reference = std::max ({ db[(size_t) h - 1], db[(size_t) h - 2], db[(size_t) h - 3] });
        onset[(size_t) h] = std::max (0.0f, db[(size_t) h] - reference);
    }

    struct Peak { int hop; float onset; };
    std::vector<Peak> peaks;
    const int minGapHops = std::max (1, (int) std::lround (0.040 * sampleRate / kOnsetHop));

    for (int h = 3; h < numHops - 1; ++h)
    {
        const float o = onset[(size_t) h];
        if (o < kOnsetMinDb || o < onset[(size_t) h - 1] || o <= onset[(size_t) h + 1])
            continue;

        if (! peaks.empty() && h - peaks.back().hop < minGapHops)
        {
            if (o > peaks.back().onset)
                peaks.back() = Peak { h, o };
            continue;
        }
        peaks.push_back (Peak { h, o });
    }

    for (const auto& p : peaks)
    {
        const int from = std::max (1, (p.hop - 2) * kOnsetHop);
        const int to = std::min (n - 1, (p.hop + 1) * kOnsetHop);

        float maxStep = 0.0f;
        for (int i = from; i <= to; ++i)
            maxStep = std::max (maxStep, std::abs (mono (i) - mono (i - 1)));

        int position = from;
        for (int i = from; i <= to; ++i)
            if (std::abs (mono (i) - mono (i - 1)) >= 0.25f * maxStep)
            {
                position = i;
                break;
            }

        for (int k = 0; k < 64 && position - k > 1; ++k)   // nearest zero crossing before the attack
            if ((mono (position - k - 1) <= 0.0f) != (mono (position - k) <= 0.0f))
            {
                position -= k;
                break;
            }

        transients.push_back (Transient { (juce::int64) position, std::min (p.onset, kOnsetCapDb) / kOnsetCapDb });
    }

    std::sort (transients.begin(), transients.end(),
               [] (const Transient& a, const Transient& b) { return a.position < b.position; });
    transients.erase (std::unique (transients.begin(), transients.end(),
                                   [] (const Transient& a, const Transient& b) { return a.position == b.position; }),
                      transients.end());
}
