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

    // First MIDI note that maps to slice 1 (36 = C1 in Ableton's naming, like a Drum Rack)
    layout.add (std::make_unique<AudioParameterInt> (ParameterID { "baseNote", 1 }, "Base Note", 0, 96, 36));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "playMode", 1 }, "Play Mode",
                                                        StringArray { "One-Shot", "Gate" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "fadeMs", 1 }, "Fade",
                                                       NormalisableRange<float> (0.5f, 20.0f, 0.1f), 2.0f,
                                                       AudioParameterFloatAttributes().withLabel ("ms")));
    return layout;
}

CutbenchProcessor::CutbenchProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    for (size_t i = 0; i < (size_t) kMaxSlices; ++i)
    {
        sliceStart[i].store (0);
        sliceEnd[i].store (0);
    }

    formatManager.registerBasicFormats();

    gainParam     = apvts.getRawParameterValue ("gain");
    baseNoteParam = apvts.getRawParameterValue ("baseNote");
    playModeParam = apvts.getRawParameterValue ("playMode");
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
    baseNote    = juce::roundToInt (baseNoteParam->load());
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainParam->load(), -60.0f));

    drainCommands();

    // Sample-accurate MIDI: render up to each event, then apply it.
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

    // Report the newest voice's position to the UI
    juce::int64 ph = -1;
    juce::uint64 newest = 0;
    for (const auto& v : voices)
        if (v.active && v.age >= newest)
        {
            newest = v.age;
            ph = (juce::int64) v.pos;
        }
    playhead.store (ph, std::memory_order_relaxed);
}

void CutbenchProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        juce::int64 s = 0, e = 0;
        if (getSliceRange (m.getNoteNumber() - baseNote, s, e))
            startVoice (m.getNoteNumber(), s, e, m.getFloatVelocity(), false);
    }
    else if (m.isNoteOff())
    {
        // One-Shot ignores note-off; Gate releases the voices of that note.
        if (gateMode)
            for (auto& v : voices)
                if (v.active && ! v.preview && v.note == m.getNoteNumber())
                    v.releasing = true;
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            if (v.active)
                v.releasing = true;
    }
}

bool CutbenchProcessor::getSliceRange (int index, juce::int64& start, juce::int64& end) const
{
    if (activeSample == nullptr || index < 0 || index >= sliceCount.load (std::memory_order_acquire))
        return false;

    // The UI may be mid-update, so every value is validated before use.
    const juce::int64 total = activeSample->numFrames();
    start = juce::jlimit<juce::int64> (0, total, sliceStart[(size_t) index].load (std::memory_order_relaxed));
    end   = juce::jlimit<juce::int64> (0, total, sliceEnd[(size_t) index].load (std::memory_order_relaxed));
    return end - start >= 2;
}

void CutbenchProcessor::startVoice (int note, juce::int64 start, juce::int64 end, float velocity, bool preview)
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
    slot->note    = note;
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

            // Fade out the tail of the slice so boundaries never click
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
            case Command::Type::playSlice:
            {
                juce::int64 s = 0, e = 0;
                if (getSliceRange (c.slice, s, e))
                    startVoice (-1, s, e, 1.0f, true);
                break;
            }
            case Command::Type::playRegion:
                startVoice (-1, c.start, c.end, 1.0f, true);
                break;
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
// Preview API (message thread -> audio thread)
//==============================================================================
void CutbenchProcessor::previewSlice (int index)
{
    Command c;
    c.type = Command::Type::playSlice;
    c.slice = index;
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

void CutbenchProcessor::loadFileInternal (const juce::File& file, std::optional<std::vector<Slice>> restore)
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

        if (frames <= kMinSliceLength || reader->sampleRate <= 0.0)
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
                                    std::optional<std::vector<Slice>> restore, juce::uint32 generation)
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

    std::vector<Slice> clean;
    if (restore.has_value())
        for (auto s : *restore)
        {
            s.start = juce::jlimit<juce::int64> (0, len, s.start);
            s.end   = juce::jlimit<juce::int64> (0, len, s.end);
            if (s.end - s.start >= kMinSliceLength && (int) clean.size() < kMaxSlices)
                clean.push_back (s);
        }
    if (clean.empty())
        clean.push_back ({ 0, len });

    {
        const juce::ScopedLock sl (modelLock);

        sliceCount.store (0, std::memory_order_release);
        if (currentSample != nullptr)
            graveyard.push_back ({ currentSample, juce::Time::getMillisecondCounter() });

        currentSample = data;
        samplePath = data->file.getFullPathName();
        slices = std::move (clean);
        selectedSlice = 0;
        statusText = {};

        audioSample.store (data.get(), std::memory_order_release);
        publishSlices();
    }

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
void CutbenchProcessor::publishSlices()
{
    const int n = (int) slices.size();
    for (int i = 0; i < n; ++i)
    {
        sliceStart[(size_t) i].store (slices[(size_t) i].start, std::memory_order_relaxed);
        sliceEnd[(size_t) i].store (slices[(size_t) i].end, std::memory_order_relaxed);
    }
    sliceCount.store (n, std::memory_order_release);
}

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

std::vector<Slice> CutbenchProcessor::getSlices() const
{
    const juce::ScopedLock sl (modelLock);
    return slices;
}

int CutbenchProcessor::getSelectedSlice() const
{
    const juce::ScopedLock sl (modelLock);
    return selectedSlice;
}

void CutbenchProcessor::setSelectedSlice (int index)
{
    {
        const juce::ScopedLock sl (modelLock);
        selectedSlice = juce::jlimit (-1, (int) slices.size() - 1, index);
    }
    sendChangeMessage();
}

int CutbenchProcessor::addSlice (juce::int64 start, juce::int64 end)
{
    int result = -1;
    {
        const juce::ScopedLock sl (modelLock);
        if (currentSample == nullptr || (int) slices.size() >= kMaxSlices)
            return -1;

        const juce::int64 len = currentSample->numFrames();
        start = juce::jlimit<juce::int64> (0, len, start);
        end   = juce::jlimit<juce::int64> (0, len, end);
        if (end - start < kMinSliceLength)
            return -1;

        auto it = std::upper_bound (slices.begin(), slices.end(), start,
                                    [] (juce::int64 v, const Slice& s) { return v < s.start; });
        result = (int) (it - slices.begin());
        slices.insert (it, Slice { start, end });
        selectedSlice = result;
        publishSlices();
    }
    sendChangeMessage();
    return result;
}

bool CutbenchProcessor::splitAt (juce::int64 position)
{
    bool done = false;
    {
        const juce::ScopedLock sl (modelLock);
        if (currentSample == nullptr || (int) slices.size() >= kMaxSlices)
            return false;

        for (size_t i = 0; i < slices.size(); ++i)
        {
            const Slice s = slices[i];
            if (position >= s.start + kMinSliceLength && position <= s.end - kMinSliceLength)
            {
                slices[i].end = position;
                slices.insert (slices.begin() + (std::ptrdiff_t) i + 1, Slice { position, s.end });
                selectedSlice = (int) i + 1;
                done = true;
                break;
            }
        }
        if (done)
            publishSlices();
    }
    if (done)
        sendChangeMessage();
    return done;
}

void CutbenchProcessor::removeSlice (int index)
{
    bool done = false;
    {
        const juce::ScopedLock sl (modelLock);
        if (index >= 0 && index < (int) slices.size())
        {
            slices.erase (slices.begin() + index);
            selectedSlice = juce::jmin (selectedSlice, (int) slices.size() - 1);
            publishSlices();
            done = true;
        }
    }
    if (done)
        sendChangeMessage();
}

void CutbenchProcessor::resetSlices()
{
    {
        const juce::ScopedLock sl (modelLock);
        if (currentSample == nullptr)
            return;
        slices.assign (1, Slice { 0, (juce::int64) currentSample->numFrames() });
        selectedSlice = 0;
        publishSlices();
    }
    sendChangeMessage();
}

juce::int64 CutbenchProcessor::moveBoundary (juce::int64 oldPos, juce::int64 newPos)
{
    juce::int64 applied = oldPos;
    {
        const juce::ScopedLock sl (modelLock);
        if (currentSample == nullptr)
            return oldPos;

        juce::int64 lo = 0, hi = currentSample->numFrames();

        // Every slice that touches this boundary constrains how far it can move.
        for (const auto& s : slices)
        {
            if (s.start == oldPos) hi = juce::jmin (hi, s.end - kMinSliceLength);
            if (s.end   == oldPos) lo = juce::jmax (lo, s.start + kMinSliceLength);
        }

        if (lo <= hi)
        {
            applied = juce::jlimit (lo, hi, newPos);
            for (auto& s : slices)
            {
                if (s.start == oldPos) s.start = applied;
                else if (s.end == oldPos) s.end = applied;
            }
            publishSlices();
        }
    }
    sendChangeMessage();
    return applied;
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
        for (const auto& s : slices)
        {
            juce::ValueTree n ("SLICE");
            n.setProperty ("start", (juce::int64) s.start, nullptr);
            n.setProperty ("end",   (juce::int64) s.end, nullptr);
            sample.appendChild (n, nullptr);
        }
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

    std::vector<Slice> restored;
    for (int i = 0; i < node.getNumChildren(); ++i)
    {
        const auto c = node.getChild (i);
        restored.push_back ({ (juce::int64) c["start"], (juce::int64) c["end"] });
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
            samplePath = path;          // keep the data so re-saving the project doesn't lose it
            slices = restored;
        }
        setStatus ("Sample file not found: " + path);
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CutbenchProcessor();
}
