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

    // Cue   : a pad plays from its cue to the end of the file; pressing any pad cuts the others (DJ-style cues)
    // Slice : a pad plays from its cue to the next cue; different pads can overlap (drum-style slices)
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "padMode", 1 }, "Pad Mode",
                                                        StringArray { "Cue", "Slice" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fadeMs", 1 }, "Fade",
                                                       NormalisableRange<float> (0.5f, 20.0f, 0.1f), 2.0f,
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
void CutbenchProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    gainSmoothed.reset (sampleRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParam->load(), -60.0f));

    for (auto& v : voices)
        v = Voice {};
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

    fadeSamples = juce::jmax (1.0f, fadeParam->load() * 0.001f * (float) currentSampleRate);
    gateMode    = playModeParam->load() > 0.5f;
    sliceMode   = padModeParam->load() > 0.5f;
    baseNote    = juce::roundToInt (baseNoteParam->load());
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
            ph = (juce::int64) v.pos;
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
            if (v.active)
                v.releasing = true;
    }
}

juce::int64 CutbenchProcessor::resolveEnd (int pad, bool slice, juce::int64 length) const
{
    const juce::int64 start = padStart[(size_t) pad].load (std::memory_order_relaxed);
    const juce::int64 explicitEnd = padEnd[(size_t) pad].load (std::memory_order_relaxed);

    if (explicitEnd > start)
        return juce::jmin (explicitEnd, length);

    if (slice)   // up to the next cue point
    {
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

    return length;
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
            padStart[(size_t) pad].store (juce::jlimit<juce::int64> (0, length - 1, (juce::int64) reference->pos),
                                          std::memory_order_relaxed);
            padEnd[(size_t) pad].store (-1, std::memory_order_relaxed);
            bumpPadVersion();
        }
        return;
    }

    // Pad with a cue: jump there. The source playback is always replaced; in Cue mode every other voice is
    // cut as well, in Slice mode only an earlier hit of the same pad.
    const juce::int64 end = resolveEnd (pad, sliceMode, length);
    if (end - start < 2)
        return;

    for (auto& v : voices)
        if (v.active && (v.preview || ! sliceMode || v.pad == pad))
            v.releasing = true;

    startVoice (pad, start, end, velocity, false);
}

void CutbenchProcessor::padOff (int pad)
{
    if (! gateMode)   // One-Shot ignores key release
        return;

    for (auto& v : voices)
        if (v.active && ! v.preview && v.pad == pad)
            v.releasing = true;
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
                v.releasing = true;

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
    slot->active  = true;
    slot->preview = preview;
    slot->pad     = pad;
    slot->pos     = (double) start;
    slot->end     = end;
    slot->gain    = juce::jlimit (0.0f, 1.0f, velocity);
    slot->age     = ++voiceCounter;
}

void CutbenchProcessor::renderVoices (juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    if (numSamples <= 0 || activeSample == nullptr)
        return;

    const auto& audio = activeSample->audio;
    const float* srcL = audio.getReadPointer (0);
    const float* srcR = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : srcL;
    const juce::int64 lastFrame = audio.getNumSamples() - 1;

    float* outL = out.getWritePointer (0, startSample);
    float* outR = out.getNumChannels() > 1 ? out.getWritePointer (1, startSample) : nullptr;

    // Source-rate -> host-rate conversion (linear interpolation; no pitch control yet)
    const double step = activeSample->sampleRate / currentSampleRate;
    const float envInc = 1.0f / fadeSamples;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            if (v.releasing)
            {
                v.env -= envInc;
                if (v.env <= 0.0f) { v.active = false; break; }
            }
            else if (v.env < 1.0f)
            {
                v.env = juce::jmin (1.0f, v.env + envInc);
            }

            const juce::int64 idx  = (juce::int64) v.pos;
            const juce::int64 idx2 = juce::jmin (idx + 1, lastFrame);
            const float frac = (float) (v.pos - (double) idx);

            // Fade out the tail so the end of a region never clicks
            const float remaining = (float) ((double) v.end - v.pos);
            const float tail = remaining >= fadeSamples ? 1.0f : juce::jmax (0.0f, remaining / fadeSamples);
            const float g = v.gain * v.env * tail;

            outL[i] += (srcL[idx] + frac * (srcL[idx2] - srcL[idx])) * g;
            if (outR != nullptr)
                outR[i] += (srcR[idx] + frac * (srcR[idx2] - srcR[idx])) * g;

            v.pos += step;
            if (v.pos >= (double) v.end) { v.active = false; break; }
        }
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
            case Command::Type::stopAll:
                for (auto& v : voices)
                    if (v.active)
                        v.releasing = true;
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

void CutbenchProcessor::clearPad (int pad)
{
    if (pad < 0 || pad >= kNumPads)
        return;

    padStart[(size_t) pad].store (-1);
    padEnd[(size_t) pad].store (-1);
    bumpPadVersion();
}

void CutbenchProcessor::clearAllPads()
{
    for (size_t i = 0; i < (size_t) kNumPads; ++i)
    {
        padStart[i].store (-1);
        padEnd[i].store (-1);
    }
    bumpPadVersion();
}

void CutbenchProcessor::chopEqual (int numPads)
{
    const juce::int64 len = currentLength();
    if (len <= kMinRegionLength)
        return;

    const int n = juce::jlimit (1, kNumPads, numPads);
    for (int i = 0; i < kNumPads; ++i)
    {
        if (i < n)
        {
            padStart[(size_t) i].store ((len * i) / n);
            padEnd[(size_t) i].store ((len * (i + 1)) / n);
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
    return resolveEnd (pad, isSliceMode(), len);
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
