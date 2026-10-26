#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

//==============================================================================
// Construction / parameters
//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CutbenchProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Gain",
                                                       NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));

    // MIDI note of pad 1 (36 = C1 in Ableton's naming, like a Drum Rack); pad n = base note + n - 1
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "baseNote", 1 }, "Base Note", 0, 112, 36));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "playMode", 1 }, "Play Mode",
                                                        StringArray { "One-Shot", "Gate" }, 0));

    // Mono : pressing a pad cuts every other sounding pad (one chop at a time)
    // Poly : different pads can sound at the same time; only an earlier hit of the same pad is cut
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "padMode", 1 }, "Voices",
                                                        StringArray { "Mono", "Poly" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fadeMs", 1 }, "Fade",
                                                       NormalisableRange<float> (0.5f, 20.0f, 0.1f), 2.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));

    // --- Phase 3: pitch, speed, direction, loop, envelope ---
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "pitch", 1 }, "Pitch", -24, 24, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fine", 1 }, "Fine Tune",
                                                       NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("cent")));

    NormalisableRange<float> speedRange (0.25f, 4.0f, 0.001f);
    speedRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Speed", speedRange, 1.0f));

    // Vinyl     : pitch and speed are linked (tape style), cheap
    // Keep Length : pitch and speed are independent (time-stretch engine), needs more CPU
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "engine", 1 }, "Engine",
                                                        StringArray { "Vinyl", "Keep Length" }, 0));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "reverse", 1 }, "Reverse", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Loop", false));

    NormalisableRange<float> attackRange (0.0f, 1000.0f, 0.1f);
    attackRange.setSkewForCentre (60.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "attackMs", 1 }, "Attack", attackRange, 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));

    NormalisableRange<float> releaseRange (0.0f, 2000.0f, 0.1f);
    releaseRange.setSkewForCentre (150.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "releaseMs", 1 }, "Release", releaseRange, 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    return layout;
}

CutbenchProcessor::CutbenchProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (size_t i = 0; i < (size_t) kNumPads; ++i)
    {
        padStart[i].store (-1);
        padEnd[i].store (-1);
    }

    formatManager.registerBasicFormats();

    gainParam     = apvts.getRawParameterValue ("gain");
    baseNoteParam = apvts.getRawParameterValue ("baseNote");
    playModeParam = apvts.getRawParameterValue ("playMode");
    padModeParam  = apvts.getRawParameterValue ("padMode");
    fadeParam     = apvts.getRawParameterValue ("fadeMs");
    pitchParam    = apvts.getRawParameterValue ("pitch");
    fineParam     = apvts.getRawParameterValue ("fine");
    speedParam    = apvts.getRawParameterValue ("speed");
    engineParam   = apvts.getRawParameterValue ("engine");
    reverseParam  = apvts.getRawParameterValue ("reverse");
    loopParam     = apvts.getRawParameterValue ("loop");
    attackParam   = apvts.getRawParameterValue ("attackMs");
    releaseParam  = apvts.getRawParameterValue ("releaseMs");

    for (auto& buffer : stretchIn)  buffer.assign ((size_t) kStretchMaxInput, 0.0f);
    for (auto& buffer : stretchOut) buffer.assign ((size_t) kStretchChunk, 0.0f);
}

CutbenchProcessor::~CutbenchProcessor()
{
    loadPool.removeAllJobs (true, 10000);
    audioSample.store (nullptr);
}

bool CutbenchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

juce::AudioProcessorEditor* CutbenchProcessor::createEditor()
{
    return new CutbenchEditor (*this);
}

//==============================================================================
// Audio thread
//==============================================================================
namespace
{
    // Catmull-Rom spline through y1..y2, t in [0,1)
    inline float cubic (float y0, float y1, float y2, float y3, float t) noexcept
    {
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    inline float readCubic (const float* d, juce::int64 last, juce::int64 i1, float t) noexcept
    {
        auto at = [d, last] (juce::int64 i) { return d[juce::jlimit<juce::int64> (0, last, i)]; };
        return cubic (at (i1 - 1), at (i1), at (i1 + 1), at (i1 + 2), t);
    }
}

void CutbenchProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    gainSmoothed.reset (sampleRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParam->load(), -60.0f));
    rateSmooth = 1.0 - std::exp (-1.0 / (0.010 * sampleRate));   // ~10 ms glide for pitch / speed changes

    for (auto& v : voices)
        v = Voice {};

    for (auto& engine : stretchPool)   // allocates, which is fine here and forbidden in processBlock
        engine.prepare (sampleRate);
}

void CutbenchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // Pick up a newly loaded sample. Voices refer to the old data, so they are dropped.
    const SampleData* sample = audioSample.load (std::memory_order_acquire);
    if (sample != activeSample)
    {
        for (auto& v : voices)
            v.active = false;
        activeSample = sample;
    }

    // Parameters for this block
    const float sr = (float) currentSampleRate;
    fadeSamples    = juce::jmax (1.0f, fadeParam->load() * 0.001f * sr);
    attackSamples  = juce::jmax (fadeSamples, attackParam->load() * 0.001f * sr);
    releaseSamples = juce::jmax (fadeSamples, releaseParam->load() * 0.001f * sr);
    attackInc      = 1.0f / attackSamples;

    gateMode   = playModeParam->load() > 0.5f;
    polyMode   = padModeParam->load() > 0.5f;
    baseNote   = juce::roundToInt (baseNoteParam->load());
    reverseOn  = reverseParam->load() > 0.5f;
    loopOn     = loopParam->load() > 0.5f;
    keepLength = engineParam->load() > 0.5f && StretchEngine::isAvailable();

    const double semitones = (double) pitchParam->load() + (double) fineParam->load() / 100.0;
    pitchRatio = std::pow (2.0, semitones / 12.0);
    speedValue = (double) speedParam->load();
    fileToHost = activeSample != nullptr ? activeSample->sampleRate / currentSampleRate : 1.0;

    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainParam->load(), -60.0f));

    drainCommands();

    // Sample-accurate MIDI: render up to each event, then apply it. This is also what makes live
    // chopping exact: the play position of the running voice is read at the very sample of the key press.
    int pos = 0;
    for (const auto metadata : midi)
    {
        const int eventPos = juce::jlimit (pos, numSamples, metadata.samplePosition);
        renderVoices (buffer, pos, eventPos - pos);
        pos = eventPos;
        handleMidi (metadata.getMessage());
    }
    renderVoices (buffer, pos, numSamples - pos);

    // Smoothed master gain
    float* left  = buffer.getWritePointer (0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    if (gainSmoothed.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gainSmoothed.getNextValue();
            left[i] *= g;
            if (right != nullptr)
                right[i] *= g;
        }
    }
    else
    {
        buffer.applyGain (gainSmoothed.getTargetValue());
    }

    // Status for the UI: newest voice position + which pads are sounding
    juce::int64 ph = -1;
    juce::uint64 newest = 0;
    juce::uint32 mask = 0;
    for (const auto& v : voices)
    {
        if (! v.active)
            continue;
        if (v.age >= newest)
        {
            newest = v.age;
            ph = (juce::int64) juce::jmax (0.0, audiblePosition (v));
        }
        if (! v.preview && ! v.releasing && v.pad >= 0)
            mask |= (juce::uint32) 1 << v.pad;
    }
    playhead.store (ph, std::memory_order_relaxed);
    activePadMask.store (mask, std::memory_order_relaxed);
}

void CutbenchProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        padOn (m.getNoteNumber() - baseNote, m.getFloatVelocity());
    }
    else if (m.isNoteOff())
    {
        padOff (m.getNoteNumber() - baseNote);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            releaseVoice (v);
    }
}

juce::int64 CutbenchProcessor::resolveEnd (int pad, juce::int64 length) const
{
    const juce::int64 start = padStart[(size_t) pad].load (std::memory_order_relaxed);
    const juce::int64 explicitEnd = padEnd[(size_t) pad].load (std::memory_order_relaxed);

    if (explicitEnd > start)   // region set via "Set Pad" / "Chop Equal" / the end handle
        return juce::jmin (explicitEnd, length);

    // Otherwise the chop ends where the next cue point begins (the last cue runs to the end of the file)
    juce::int64 next = length;
    for (size_t i = 0; i < (size_t) kNumPads; ++i)
    {
        if ((int) i == pad)
            continue;
        const juce::int64 s = padStart[i].load (std::memory_order_relaxed);
        if (s > start && s < next)
            next = s;
    }
    return next;
}

double CutbenchProcessor::audiblePosition (const Voice& v) const
{
    if (v.stretchSlot < 0)
        return v.pos;

    // A stretch voice reads its input ahead of what is audible by the engine's start length.
    const double lag = (double) stretchPool[(size_t) v.stretchSlot].startLength (speedValue * fileToHost);
    return reverseOn ? v.pos + lag : v.pos - lag;
}

void CutbenchProcessor::padOn (int pad, float velocity)
{
    if (activeSample == nullptr || pad < 0 || pad >= kNumPads)
        return;

    const juce::int64 length = activeSample->numFrames();
    const juce::int64 start = padStart[(size_t) pad].load (std::memory_order_relaxed);

    if (start < 0)
    {
        // LIVE CHOP: an empty pad grabs the position of whatever is playing right now.
        // Nothing is retriggered, playback just carries on.
        const Voice* reference = nullptr;
        for (const auto& v : voices)
            if (v.active && ! v.releasing && (reference == nullptr || v.age > reference->age))
                reference = &v;

        if (reference != nullptr)
        {
            const auto position = (juce::int64) audiblePosition (*reference);
            padStart[(size_t) pad].store (juce::jlimit<juce::int64> (0, length - 1, position), std::memory_order_relaxed);
            padEnd[(size_t) pad].store (-1, std::memory_order_relaxed);
            bumpPadVersion();
        }
        return;
    }

    // Pad with a cue: jump there. The source playback is always replaced; in Mono mode every other voice is
    // cut as well, in Poly mode only an earlier hit of the same pad.
    const juce::int64 end = resolveEnd (pad, length);
    if (end - start < 2)
        return;

    for (auto& v : voices)
        if (v.active && (v.preview || ! polyMode || v.pad == pad))
            releaseVoice (v);

    startVoice (pad, start, end, velocity, false);
}

void CutbenchProcessor::padOff (int pad)
{
    if (! gateMode)   // One-Shot ignores key release
        return;

    for (auto& v : voices)
        if (v.active && ! v.preview && v.pad == pad)
            releaseVoice (v);
}

// Fades a voice out over the Release time (never shorter than the declick fade). Used for everything that ends a
// sound early: key release in Gate mode, another pad cutting it in Mono mode, Stop, clearing the pad.
void CutbenchProcessor::releaseVoice (Voice& v)
{
    if (! v.active || v.releasing)
        return;

    v.releasing = true;
    v.relInc = 1.0f / releaseSamples;
}

int CutbenchProcessor::acquireStretchSlot() const
{
    for (int s = 0; s < kStretchSlots; ++s)
    {
        bool used = false;
        for (const auto& v : voices)
            if (v.active && v.stretchSlot == s)
            {
                used = true;
                break;
            }
        if (! used)
            return s;
    }
    return -1;   // pool exhausted: the voice falls back to the Vinyl engine
}

void CutbenchProcessor::startVoice (int pad, juce::int64 start, juce::int64 end, float velocity, bool preview)
{
    if (activeSample == nullptr)
        return;

    const juce::int64 total = activeSample->numFrames();
    start = juce::jlimit<juce::int64> (0, total - 1, start);
    end   = juce::jlimit<juce::int64> (start + 1, total, end);
    if (end - start < 2)
        return;

    if (preview)   // a new preview replaces the previous one
        for (auto& v : voices)
            if (v.active && v.preview)
                releaseVoice (v);

    Voice* slot = nullptr;
    for (auto& v : voices)
        if (! v.active) { slot = &v; break; }

    if (slot == nullptr)   // steal the oldest voice
    {
        slot = &voices[0];
        for (auto& v : voices)
            if (v.age < slot->age)
                slot = &v;
    }

    *slot = Voice {};
    slot->active   = true;
    slot->preview  = preview;
    slot->pad      = pad;
    slot->start    = start;
    slot->end      = end;
    slot->pos      = reverseOn ? (double) (end - 1) : (double) start;
    slot->curRate  = (keepLength ? speedValue : 1.0) * pitchRatio * fileToHost;
    slot->gain     = juce::jlimit (0.0f, 1.0f, velocity);
    slot->age      = ++voiceCounter;

    if (keepLength)
    {
        slot->stretchSlot = acquireStretchSlot();
        if (slot->stretchSlot >= 0)
        {
            // Prime the engine with the first frames of the region, so the sound starts right on the cue point
            auto& engine = stretchPool[(size_t) slot->stretchSlot];
            const double ratio = speedValue * fileToHost;
            engine.setTransposeFactor ((float) (pitchRatio * fileToHost));

            const int n = juce::jlimit (1, kStretchMaxInput, engine.startLength (ratio));
            const int flushOut = juce::jmax (1, juce::roundToInt ((double) engine.startLength (ratio) / ratio));
            fillStretchInput (*slot, stretchIn[0].data(), stretchIn[1].data(), n, flushOut);
            engine.start (stretchIn[0].data(), stretchIn[1].data(), n);
        }
    }
}

bool CutbenchProcessor::advanceEnvelope (Voice& v) const
{
    if (v.releasing)
    {
        v.env -= v.relInc;
        if (v.env <= 0.0f)
        {
            v.active = false;
            return false;
        }
    }
    else if (v.env < 1.0f)
    {
        v.env = juce::jmin (1.0f, v.env + attackInc);
    }
    return true;
}

void CutbenchProcessor::renderVoices (juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    if (numSamples <= 0 || activeSample == nullptr)
        return;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        if (v.stretchSlot >= 0)
            renderStretchVoice (v, out, startSample, numSamples);
        else
            renderVinylVoice (v, out, startSample, numSamples);
    }
}

// Vinyl engine: cubic-interpolated resampling. Pitch and speed together set one playback rate.
void CutbenchProcessor::renderVinylVoice (Voice& v, juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    const auto& audio = activeSample->audio;
    const float* srcL = audio.getReadPointer (0);
    const float* srcR = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : srcL;
    const juce::int64 last = audio.getNumSamples() - 1;

    float* outL = out.getWritePointer (0, startSample);
    float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1, startSample) : nullptr;

    // Vinyl: the pitch alone sets the rate (the Speed control just mirrors it, see the editor). A voice that fell back
    // to this engine while Keep Length is selected also honours Speed.
    const double targetRate = (keepLength ? speedValue : 1.0) * pitchRatio * fileToHost;
    const double regionLen = (double) (v.end - v.start);
    const double startPos = (double) v.start;
    const double endPos = (double) v.end;
    const float wrapInc = 1.0f / fadeSamples;

    // Fade at the end of the region: the Release time (at least the declick fade), at most the whole region
    const double rate0 = juce::jmax (v.curRate, 1.0e-6);
    const float tailLen = loopOn ? fadeSamples
                                 : juce::jmax (1.0f, (float) juce::jmin ((double) releaseSamples, regionLen / rate0));

    for (int i = 0; i < numSamples; ++i)
    {
        if (! advanceEnvelope (v))
            break;

        v.curRate += (targetRate - v.curRate) * rateSmooth;

        const juce::int64 idx = (juce::int64) std::floor (v.pos);
        const float frac = (float) (v.pos - (double) idx);
        const float l = readCubic (srcL, last, idx, frac);
        const float r = outR != nullptr ? readCubic (srcR, last, idx, frac) : 0.0f;

        const double remainingSrc = reverseOn ? (v.pos - startPos) : (endPos - v.pos);
        const double remainingOut = remainingSrc / juce::jmax (v.curRate, 1.0e-6);
        const float tail = remainingOut >= (double) tailLen ? 1.0f : (float) juce::jmax (0.0, remainingOut / (double) tailLen);

        v.wrapRamp = juce::jmin (1.0f, v.wrapRamp + wrapInc);
        const float g = v.gain * v.env * tail * v.wrapRamp;

        outL[i] += l * g;
        if (outR != nullptr)
            outR[i] += r * g;

        v.pos += reverseOn ? -v.curRate : v.curRate;

        const bool past = reverseOn ? (v.pos < startPos) : (v.pos >= endPos);
        if (past)
        {
            if (loopOn && ! v.releasing)
            {
                v.pos += reverseOn ? regionLen : -regionLen;
                v.pos = juce::jlimit (startPos, endPos - 0.001, v.pos);
                v.wrapRamp = 0.0f;   // short fade-in after the jump hides the loop seam
            }
            else
            {
                v.active = false;
                break;
            }
        }
    }
}

// Reads `frames` source frames in playback direction into the stretch input buffers. Outside the region the
// input is silence, which lets the engine play out its tail after the region ended (the "flush").
void CutbenchProcessor::fillStretchInput (Voice& v, float* inL, float* inR, int frames, int flushOutFrames)
{
    const auto& audio = activeSample->audio;
    const float* srcL = audio.getReadPointer (0);
    const float* srcR = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : srcL;

    const double step = reverseOn ? -1.0 : 1.0;
    const double startPos = (double) v.start;
    const double endPos = (double) v.end;

    for (int k = 0; k < frames; ++k)
    {
        const juce::int64 idx = (juce::int64) v.pos;
        if (v.pos >= startPos && v.pos < endPos)
        {
            inL[k] = srcL[idx];
            inR[k] = srcR[idx];
        }
        else
        {
            inL[k] = 0.0f;
            inR[k] = 0.0f;
        }

        v.pos += step;

        if (! v.flushing)
        {
            const bool past = reverseOn ? (v.pos < startPos) : (v.pos >= endPos);
            if (past)
            {
                if (loopOn && ! v.releasing)
                    v.pos = reverseOn ? (double) (v.end - 1) : startPos;
                else
                {
                    v.flushing = true;
                    v.flushLeft = flushOutFrames;
                }
            }
        }
    }
}

// Keep Length engine: the stretcher gets source frames at (speed x rate) per output frame and shifts pitch
// independently of that. After the region ends it is fed silence until its tail has played out.
void CutbenchProcessor::renderStretchVoice (Voice& v, juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    float* outL = out.getWritePointer (0, startSample);
    float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1, startSample) : nullptr;

    auto& engine = stretchPool[(size_t) v.stretchSlot];
    engine.setTransposeFactor ((float) (pitchRatio * fileToHost));   // fileToHost compensates a file/host rate mismatch

    const double ratio = speedValue * fileToHost;   // source frames consumed per output frame
    const int flushOut = juce::jmax (1, juce::roundToInt ((double) engine.startLength (ratio) / ratio));

    float* inL = stretchIn[0].data();
    float* inR = stretchIn[1].data();
    float* tmpL = stretchOut[0].data();
    float* tmpR = stretchOut[1].data();

    int done = 0;
    while (done < numSamples && v.active)
    {
        const int n = juce::jmin (kStretchChunk, numSamples - done);

        const double wanted = (double) n * ratio + v.inFrac;
        const int inFrames = juce::jlimit (1, kStretchMaxInput, (int) wanted);
        v.inFrac = wanted - (double) inFrames;
        if (inFrames == kStretchMaxInput)
            v.inFrac = 0.0;

        fillStretchInput (v, inL, inR, inFrames, flushOut);
        engine.process (inL, inR, inFrames, tmpL, tmpR, n);

        for (int i = 0; i < n; ++i)
        {
            if (! advanceEnvelope (v))
                break;

            float tail = 1.0f;
            if (v.flushing)
            {
                tail = juce::jmin (1.0f, (float) v.flushLeft / fadeSamples);
                if (--v.flushLeft <= 0)
                {
                    v.active = false;
                    break;
                }
            }

            const float g = v.gain * v.env * tail;
            outL[done + i] += tmpL[i] * g;
            if (outR != nullptr)
                outR[done + i] += tmpR[i] * g;
        }

        done += n;
    }
}

void CutbenchProcessor::pushCommand (const Command& c)
{
    int s1, n1, s2, n2;
    cmdFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)      cmdBuffer[(size_t) s1] = c;
    else if (n2 > 0) cmdBuffer[(size_t) s2] = c;
    else return;     // queue full: drop the command rather than block
    cmdFifo.finishedWrite (n1 + n2);
}

void CutbenchProcessor::drainCommands()
{
    const int ready = cmdFifo.getNumReady();
    if (ready == 0)
        return;

    int s1, n1, s2, n2;
    cmdFifo.prepareToRead (ready, s1, n1, s2, n2);

    auto handle = [this] (const Command& c)
    {
        switch (c.type)
        {
            case Command::Type::padOn:      padOn (c.pad, 1.0f); break;
            case Command::Type::padOff:     padOff (c.pad); break;
            case Command::Type::playRegion: startVoice (-1, c.start, c.end, 1.0f, true); break;
            case Command::Type::releasePad:   // pad cleared: its sound fades out too (pad < 0 = every pad)
                for (auto& v : voices)
                    if (v.active && ! v.preview && (c.pad < 0 || v.pad == c.pad))
                        releaseVoice (v);
                break;
            case Command::Type::stopAll:
                for (auto& v : voices)
                    releaseVoice (v);
                break;
        }
    };

    for (int i = 0; i < n1; ++i) handle (cmdBuffer[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) handle (cmdBuffer[(size_t) (s2 + i)]);
    cmdFifo.finishedRead (n1 + n2);
}

//==============================================================================
// Play controls (message thread -> audio thread)
//==============================================================================
void CutbenchProcessor::padPressed (int pad)
{
    Command c;
    c.type = Command::Type::padOn;
    c.pad = pad;
    pushCommand (c);
}

void CutbenchProcessor::padReleased (int pad)
{
    Command c;
    c.type = Command::Type::padOff;
    c.pad = pad;
    pushCommand (c);
}

void CutbenchProcessor::previewRegion (juce::int64 start, juce::int64 end)
{
    Command c;
    c.type = Command::Type::playRegion;
    c.start = start;
    c.end = end;
    pushCommand (c);
}

void CutbenchProcessor::stopPreview()
{
    Command c;
    c.type = Command::Type::stopAll;
    pushCommand (c);
}

//==============================================================================
// Loading (background thread -> message thread)
//==============================================================================
void CutbenchProcessor::loadFile (const juce::File& file)
{
    loadFileInternal (file, std::nullopt);
}

void CutbenchProcessor::loadFileInternal (const juce::File& file, std::optional<PadArray> restore)
{
    const auto generation = ++loadGeneration;
    setStatus ("Loading " + file.getFileName() + " ...");

    juce::WeakReference<CutbenchProcessor> weak (this);

    loadPool.addJob ([this, weak, file, restore, generation]() mutable
    {
        std::shared_ptr<SampleData> data;
        juce::String error;
        decodeFile (file, data, error);

        juce::MessageManager::callAsync ([weak, data, error, restore, generation]() mutable
        {
            if (auto* self = weak.get())
                self->finishLoad (std::move (data), error, std::move (restore), generation);
        });
    });
}

void CutbenchProcessor::decodeFile (const juce::File& file, std::shared_ptr<SampleData>& out, juce::String& error)
{
    try
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        if (reader == nullptr)
        {
            error = "Could not read \"" + file.getFileName() + "\" (unsupported or damaged file)";
            return;
        }

        const juce::int64 frames = reader->lengthInSamples;
        const int channels = juce::jlimit (1, 2, (int) reader->numChannels);
        constexpr juce::int64 maxBytes = 1LL << 30;   // keep the in-memory copy under ~1 GiB

        if (frames <= kMinRegionLength || reader->sampleRate <= 0.0)
        {
            error = "\"" + file.getFileName() + "\" is empty or too short";
            return;
        }
        if (frames * channels * (juce::int64) sizeof (float) > maxBytes)
        {
            error = "\"" + file.getFileName() + "\" is too large to hold in memory (limit about 1 GiB)";
            return;
        }

        auto d = std::make_shared<SampleData>();
        d->audio.setSize (channels, (int) frames);
        if (! reader->read (&d->audio, 0, (int) frames, 0, true, true))
        {
            error = "Reading \"" + file.getFileName() + "\" failed";
            return;
        }

        d->sampleRate = reader->sampleRate;
        d->file = file;
        d->name = file.getFileName();
        d->buildPeaks();
        out = std::move (d);
    }
    catch (const std::exception& e)
    {
        error = "Loading failed: " + juce::String (e.what());
    }
}

void CutbenchProcessor::finishLoad (std::shared_ptr<SampleData> data, juce::String error,
                                    std::optional<PadArray> restore, juce::uint32 generation)
{
    if (generation != loadGeneration)
        return;   // a newer load superseded this one

    if (data == nullptr)
    {
        setStatus (error);
        sendChangeMessage();
        return;
    }

    const juce::int64 len = data->numFrames();

    // Validate restored pads against the real file length
    PadArray clean;
    bool any = false;
    if (restore.has_value())
        for (size_t i = 0; i < (size_t) kNumPads; ++i)
        {
            const Pad p = (*restore)[i];
            if (p.start >= 0 && p.start < len - kMinRegionLength)
            {
                clean[i].start = p.start;
                if (p.end > p.start + kMinRegionLength && p.end <= len)
                    clean[i].end = p.end;
                any = true;
            }
        }
    if (! any)
        clean[0].start = 0;   // fresh load: pad 1 plays the sample from the beginning

    {
        const juce::ScopedLock sl (modelLock);

        if (currentSample != nullptr)
            graveyard.push_back ({ currentSample, juce::Time::getMillisecondCounter() });

        currentSample = data;
        samplePath = data->file.getFullPathName();
        statusText = {};

        for (size_t i = 0; i < (size_t) kNumPads; ++i)
        {
            padStart[i].store (clean[i].start, std::memory_order_relaxed);
            padEnd[i].store (clean[i].end, std::memory_order_relaxed);
        }
        audioSample.store (data.get(), std::memory_order_release);
    }

    const int firstEmpty = nextEmptyPad (-1);
    selectedPad.store (firstEmpty >= 0 ? firstEmpty : 0);
    bumpPadVersion();

    // Free replaced samples once the audio thread has certainly moved on.
    juce::WeakReference<CutbenchProcessor> weak (this);
    juce::Timer::callAfterDelay (6000, [weak] { if (auto* self = weak.get()) self->purgeGraveyard(); });

    sendChangeMessage();
}

void CutbenchProcessor::purgeGraveyard()
{
    const juce::ScopedLock sl (modelLock);
    const auto now = juce::Time::getMillisecondCounter();
    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [now] (const Retired& r) { return now - r.time > 5000; }),
                     graveyard.end());
}

//==============================================================================
// Model access (message thread)
//==============================================================================
void CutbenchProcessor::setStatus (const juce::String& s)
{
    {
        const juce::ScopedLock sl (modelLock);
        statusText = s;
    }
    sendChangeMessage();
}

juce::String CutbenchProcessor::getStatusText() const
{
    const juce::ScopedLock sl (modelLock);
    return statusText;
}

std::shared_ptr<const SampleData> CutbenchProcessor::getSample() const
{
    const juce::ScopedLock sl (modelLock);
    return currentSample;
}

juce::int64 CutbenchProcessor::currentLength() const
{
    const juce::ScopedLock sl (modelLock);
    return currentSample != nullptr ? (juce::int64) currentSample->numFrames() : 0;
}

CutbenchProcessor::PadArray CutbenchProcessor::getPads() const
{
    PadArray out;
    for (size_t i = 0; i < (size_t) kNumPads; ++i)
    {
        out[i].start = padStart[i].load (std::memory_order_relaxed);
        out[i].end   = padEnd[i].load (std::memory_order_relaxed);
    }
    return out;
}

void CutbenchProcessor::setSelectedPad (int pad)
{
    selectedPad.store (juce::jlimit (0, kNumPads - 1, pad));
    bumpPadVersion();
}

void CutbenchProcessor::setPadStart (int pad, juce::int64 position)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength || pad < 0 || pad >= kNumPads)
        return;

    padStart[(size_t) pad].store (juce::jlimit<juce::int64> (0, len - kMinRegionLength, position));
    padEnd[(size_t) pad].store (-1);
    bumpPadVersion();
}

void CutbenchProcessor::setPadRegion (int pad, juce::int64 start, juce::int64 end)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength || pad < 0 || pad >= kNumPads)
        return;

    start = juce::jlimit<juce::int64> (0, len, start);
    end   = juce::jlimit<juce::int64> (0, len, end);
    if (end - start < kMinRegionLength)
        return;

    padStart[(size_t) pad].store (start);
    padEnd[(size_t) pad].store (end);
    bumpPadVersion();
}

void CutbenchProcessor::setPadEnd (int pad, juce::int64 end)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength || pad < 0 || pad >= kNumPads)
        return;

    const juce::int64 start = padStart[(size_t) pad].load();
    if (start < 0)
        return;

    padEnd[(size_t) pad].store (juce::jlimit<juce::int64> (start + kMinRegionLength, len, end));
    bumpPadVersion();
}

void CutbenchProcessor::clearPadEnd (int pad)
{
    if (pad < 0 || pad >= kNumPads)
        return;

    padEnd[(size_t) pad].store (-1);
    bumpPadVersion();
}

void CutbenchProcessor::clearPad (int pad)
{
    if (pad < 0 || pad >= kNumPads)
        return;

    padStart[(size_t) pad].store (-1);
    padEnd[(size_t) pad].store (-1);
    bumpPadVersion();

    Command c;
    c.type = Command::Type::releasePad;
    c.pad = pad;
    pushCommand (c);
}

void CutbenchProcessor::clearAllPads()
{
    for (size_t i = 0; i < (size_t) kNumPads; ++i)
    {
        padStart[i].store (-1);
        padEnd[i].store (-1);
    }
    bumpPadVersion();

    Command c;
    c.type = Command::Type::releasePad;
    c.pad = -1;
    pushCommand (c);
}

void CutbenchProcessor::chopEqual (int numPads, juce::int64 from, juce::int64 to)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength)
        return;

    // The range to divide: the given selection, or the whole sample when `to` is negative
    from = juce::jlimit<juce::int64> (0, len, from);
    to   = (to < 0) ? len : juce::jlimit<juce::int64> (0, len, to);
    if (to - from < kMinRegionLength)
        return;

    const juce::int64 span = to - from;
    const int n = juce::jlimit (1, kNumPads, (int) juce::jmin<juce::int64> (numPads, span / kMinRegionLength));

    for (int i = 0; i < kNumPads; ++i)
    {
        if (i < n)
        {
            padStart[(size_t) i].store (from + (span * i) / n);
            padEnd[(size_t) i].store (from + (span * (i + 1)) / n);
        }
        else
        {
            padStart[(size_t) i].store (-1);
            padEnd[(size_t) i].store (-1);
        }
    }
    selectedPad.store (juce::jmin (n, kNumPads - 1));
    bumpPadVersion();
}

juce::int64 CutbenchProcessor::movePadStart (int pad, juce::int64 newPosition)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength || pad < 0 || pad >= kNumPads)
        return -1;

    const juce::int64 old = padStart[(size_t) pad].load();
    if (old < 0)
        return old;

    juce::int64 lo = 0;
    juce::int64 hi = len - kMinRegionLength;

    const juce::int64 ownEnd = padEnd[(size_t) pad].load();
    if (ownEnd > old)
        hi = juce::jmin (hi, ownEnd - kMinRegionLength);

    // Pads whose region ends exactly at this cue move their end along (shared boundary)
    for (int i = 0; i < kNumPads; ++i)
        if (i != pad && padStart[(size_t) i].load() >= 0 && padEnd[(size_t) i].load() == old)
            lo = juce::jmax (lo, padStart[(size_t) i].load() + kMinRegionLength);

    if (lo > hi)
        return old;

    const juce::int64 applied = juce::jlimit (lo, hi, newPosition);
    padStart[(size_t) pad].store (applied);

    for (int i = 0; i < kNumPads; ++i)
        if (i != pad && padStart[(size_t) i].load() >= 0 && padEnd[(size_t) i].load() == old)
            padEnd[(size_t) i].store (applied);

    bumpPadVersion();
    return applied;
}

juce::int64 CutbenchProcessor::getEffectiveEnd (int pad) const
{
    const juce::int64 len = currentLength();
    if (pad < 0 || pad >= kNumPads || len <= 0 || padStart[(size_t) pad].load() < 0)
        return -1;
    return resolveEnd (pad, len);
}

CutbenchProcessor::EndArray CutbenchProcessor::getEffectiveEnds() const
{
    EndArray ends;
    ends.fill (-1);

    const juce::int64 len = currentLength();
    if (len <= 0)
        return ends;

    for (int i = 0; i < kNumPads; ++i)
        if (padStart[(size_t) i].load() >= 0)
            ends[(size_t) i] = resolveEnd (i, len);
    return ends;
}

int CutbenchProcessor::nextEmptyPad (int after) const
{
    for (int i = 1; i <= kNumPads; ++i)
    {
        const int idx = (((after + i) % kNumPads) + kNumPads) % kNumPads;
        if (padStart[(size_t) idx].load() < 0)
            return idx;
    }
    return -1;
}

//==============================================================================
// State (host save / restore)
//==============================================================================
void CutbenchProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("SAMPLE"), nullptr);

    juce::ValueTree sample ("SAMPLE");
    {
        const juce::ScopedLock sl (modelLock);
        sample.setProperty ("path", samplePath, nullptr);
    }

    const auto pads = getPads();
    for (int i = 0; i < kNumPads; ++i)
    {
        if (! pads[(size_t) i].isSet())
            continue;

        juce::ValueTree n ("PAD");
        n.setProperty ("index", i, nullptr);
        n.setProperty ("start", (juce::int64) pads[(size_t) i].start, nullptr);
        n.setProperty ("end",   (juce::int64) pads[(size_t) i].end, nullptr);
        sample.appendChild (n, nullptr);
    }
    state.appendChild (sample, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void CutbenchProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;   // ignore foreign / corrupt state instead of crashing

    const auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid())
        return;

    apvts.replaceState (state);

    const auto node = state.getChildWithName ("SAMPLE");
    if (! node.isValid())
        return;

    const juce::String path = node["path"].toString();

    PadArray restored;
    for (int i = 0; i < node.getNumChildren(); ++i)
    {
        const auto c = node.getChild (i);
        const int index = (int) c["index"];
        if (index >= 0 && index < kNumPads)
        {
            restored[(size_t) index].start = (juce::int64) c["start"];
            restored[(size_t) index].end   = (juce::int64) c["end"];
        }
    }

    if (path.isEmpty())
        return;

    const juce::File f (path);
    if (f.existsAsFile())
    {
        loadFileInternal (f, restored);
    }
    else
    {
        {
            const juce::ScopedLock sl (modelLock);
            samplePath = path;      // keep the data so re-saving the project doesn't lose it
        }
        for (size_t i = 0; i < (size_t) kNumPads; ++i)
        {
            padStart[i].store (restored[i].start);
            padEnd[i].store (restored[i].end);
        }
        bumpPadVersion();
        setStatus ("Sample file not found: " + path);
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CutbenchProcessor();
}
