#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include "SampleData.h"
#include "StretchEngine.h"

/*
    Model
    -----
    16 pads = 16 cue points into one loaded sample. Pad n is played by MIDI note (baseNote + n - 1).
    The pad table (start/end per pad) lives in atomics so that BOTH the audio thread (live chop)
    and the message thread (editing) can change it without locks; `padVersion` tells the UI
    when something changed.

    Behaviour of a pad key
      pad has a cue   -> play from the cue to its end (explicit region end, else the next cue, else end of file)
      pad is empty    -> LIVE CHOP: if something is playing, the cue is set at the current play position
                         and playback simply continues.

    Playback engines (per voice)
      Vinyl     : resampling with cubic interpolation. Pitch and speed are linked (tape / turntable style).
      Keep Length : Signalsmith Stretch. Pitch and speed are independent. Costs CPU and adds some latency,
                    so the number of such voices is limited (kStretchSlots); extra voices fall back to Vinyl.

    Threading
      Audio thread : never locks or allocates. Reads `audioSample`, the pad atomics, the UI command FIFO
                     and parameters. Replaced SampleData objects stay alive in `graveyard` for a few seconds.
      Loader thread: decodes the file and builds the peak cache, then hands over to the message thread.
*/
class CutbenchProcessor : public juce::AudioProcessor,
                          public juce::ChangeBroadcaster
{
public:
    static constexpr int kMaxVoices = 32;
    static constexpr int kStretchSlots = 8;
    static constexpr int kStretchChunk = 512;                    // output frames per stretch call
    static constexpr int kStretchMaxInput = kStretchChunk * 32;  // input frames per stretch call
    static constexpr juce::int64 kMinRegionLength = 64;          // frames
    using PadArray = std::array<Pad, kNumPads>;
    using EndArray = std::array<juce::int64, kNumPads>;

    CutbenchProcessor();
    ~CutbenchProcessor() override;

    // ---- juce::AudioProcessor ----
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- Message-thread API (editor) ----
    void loadFile (const juce::File& file);
    std::shared_ptr<const SampleData> getSample() const;
    juce::String getStatusText() const;
    juce::AudioFormatManager& getFormatManager() { return formatManager; }

    PadArray getPads() const;                                    // snapshot
    juce::uint32 getPadVersion() const noexcept { return padVersion.load (std::memory_order_acquire); }
    juce::uint32 getActivePadMask() const noexcept { return activePadMask.load (std::memory_order_relaxed); }
    int  getSelectedPad() const noexcept { return selectedPad.load (std::memory_order_relaxed); }
    void setSelectedPad (int pad);
    int  getBaseNote() const noexcept { return juce::roundToInt (baseNoteParam->load()); }
    // Transients weaker than this are ignored (Auto Slice, Snap, and the ticks in the waveform). 0.9 .. 0.02
    float getTransientThreshold() const noexcept { return 0.02f + 0.9f * (1.0f - sensitivityParam->load()); }

    void setPadStart (int pad, juce::int64 position);            // cue point, automatic end
    void setPadRegion (int pad, juce::int64 start, juce::int64 end);
    void setPadEnd (int pad, juce::int64 end);                   // explicit end for a pad that has a cue
    void clearPadEnd (int pad);                                  // back to the automatic end
    void clearPad (int pad);
    void clearAllPads();
    void autoSlice (int maxPads, juce::int64 from, juce::int64 to);   // cues on the detected transients inside [from, to); to < 0 = end of file
    void chopEqual (int numPads, juce::int64 from, juce::int64 to);   // pads 1..N = N equal regions inside [from, to); to < 0 = end of file
    juce::int64 movePadStart (int pad, juce::int64 newPosition); // returns the applied position
    juce::int64 getEffectiveEnd (int pad) const;                 // -1 if the pad is empty
    EndArray getEffectiveEnds() const;                           // all pads at once (-1 = empty)
    int nextEmptyPad (int after) const;                          // first empty pad after `after` (wraps), or -1

    // Play controls (executed on the audio thread via a lock-free queue)
    void padPressed (int pad);
    void padReleased (int pad);
    void previewRegion (juce::int64 start, juce::int64 end);
    void stopPreview();
    juce::int64 getPlayhead() const noexcept { return playhead.load (std::memory_order_relaxed); }   // -1 = idle

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "CUTBENCH", createLayout() };

private:
    struct Voice
    {
        bool active = false, releasing = false, preview = false;
        bool flushing = false;
        int pad = -1;
        int stretchSlot = -1;          // index into stretchPool, or -1 for the Vinyl engine
        int flushLeft = 0;             // stretch engine: output frames left after the region ended
        double pos = 0.0;              // source frame position (fractional; integer-valued for stretch voices)
        double curRate = 1.0;          // smoothed source frames per output frame (Vinyl engine)
        double inFrac = 0.0;           // stretch engine: fractional input frames carried between calls
        juce::int64 start = 0, end = 0;
        float env = 0.0f, gain = 1.0f, relInc = 0.0f, wrapRamp = 1.0f;
        juce::uint64 age = 0;
    };

    struct Command
    {
        enum class Type { padOn, padOff, playRegion, stopAll, releasePad } type = Type::stopAll;
        int pad = -1;
        juce::int64 start = 0, end = 0;
    };

    struct Retired
    {
        std::shared_ptr<const SampleData> data;
        juce::uint32 time = 0;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // loading / model (message thread)
    void loadFileInternal (const juce::File&, std::optional<PadArray> restore);
    void decodeFile (const juce::File&, std::shared_ptr<SampleData>& out, juce::String& error);
    void finishLoad (std::shared_ptr<SampleData> data, juce::String error,
                     std::optional<PadArray> restore, juce::uint32 generation);
    void purgeGraveyard();
    void setStatus (const juce::String&);
    juce::int64 currentLength() const;

    // shared (either thread)
    void bumpPadVersion() noexcept { padVersion.fetch_add (1, std::memory_order_release); }
    juce::int64 resolveEnd (int pad, juce::int64 length) const;

    // audio thread
    void pushCommand (const Command&);
    void drainCommands();
    void handleMidi (const juce::MidiMessage&);
    void padOn (int pad, float velocity);
    void padOff (int pad);
    void releaseVoice (Voice&);
    void startVoice (int pad, juce::int64 start, juce::int64 end, float velocity, bool preview);
    int  acquireStretchSlot() const;
    double audiblePosition (const Voice&) const;
    void fillStretchInput (Voice&, float* inL, float* inR, int frames, int flushOutFrames);
    bool advanceEnvelope (Voice&) const;
    void renderVoices (juce::AudioBuffer<float>&, int startSample, int numSamples);
    void renderVinylVoice (Voice&, juce::AudioBuffer<float>&, int startSample, int numSamples);
    void renderStretchVoice (Voice&, juce::AudioBuffer<float>&, int startSample, int numSamples);

    // ---- message-thread state ----
    mutable juce::CriticalSection modelLock;
    std::shared_ptr<const SampleData> currentSample;
    juce::String samplePath;
    juce::String statusText;
    std::vector<Retired> graveyard;
    juce::uint32 loadGeneration = 0;
    juce::AudioFormatManager formatManager;

    // ---- shared, lock-free ----
    std::atomic<const SampleData*> audioSample { nullptr };
    std::array<std::atomic<juce::int64>, kNumPads> padStart, padEnd;
    std::atomic<juce::uint32> padVersion { 0 };
    std::atomic<juce::uint32> activePadMask { 0 };
    std::atomic<int> selectedPad { 0 };
    std::atomic<juce::int64> playhead { -1 };
    juce::AbstractFifo cmdFifo { 64 };
    std::array<Command, 64> cmdBuffer;

    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* baseNoteParam = nullptr;
    std::atomic<float>* playModeParam = nullptr;
    std::atomic<float>* padModeParam = nullptr;
    std::atomic<float>* fadeParam = nullptr;
    std::atomic<float>* pitchParam = nullptr;
    std::atomic<float>* fineParam = nullptr;
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* engineParam = nullptr;
    std::atomic<float>* reverseParam = nullptr;
    std::atomic<float>* loopParam = nullptr;
    std::atomic<float>* attackParam = nullptr;
    std::atomic<float>* releaseParam = nullptr;
    std::atomic<float>* sensitivityParam = nullptr;
    std::atomic<float>* snapParam = nullptr;

    // ---- audio-thread-only state ----
    const SampleData* activeSample = nullptr;
    std::array<Voice, kMaxVoices> voices;
    std::array<StretchEngine, kStretchSlots> stretchPool;
    std::array<std::vector<float>, 2> stretchIn, stretchOut;
    juce::uint64 voiceCounter = 0;

    double currentSampleRate = 44100.0;
    double pitchRatio = 1.0, speedValue = 1.0, fileToHost = 1.0, rateSmooth = 0.001;
    float fadeSamples = 64.0f, attackSamples = 64.0f, releaseSamples = 64.0f, attackInc = 0.01f;
    bool gateMode = false, polyMode = false, keepLength = false, reverseOn = false, loopOn = false;
    int baseNote = 36;
    juce::SmoothedValue<float> gainSmoothed;

    juce::ThreadPool loadPool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (CutbenchProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CutbenchProcessor)
};
