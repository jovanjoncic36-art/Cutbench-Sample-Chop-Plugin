#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include "SampleData.h"

/*
    Model
    -----
    16 pads = 16 cue points into one loaded sample. Pad n is played by MIDI note (baseNote + n).
    The pad table (start/end per pad) lives in atomics so that BOTH the audio thread (live chop)
    and the message thread (editing) can change it without locks; `padVersion` tells the UI
    when something changed.

    Behaviour of a pad key
      pad has a cue   -> play from the cue (Cue mode: to the end of the file, Slice mode: to the next cue)
      pad is empty    -> LIVE CHOP: if something is playing, the cue is set at the current play position
                         and playback simply continues.

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
    static constexpr juce::int64 kMinRegionLength = 64;   // frames
    using PadArray = std::array<Pad, kNumPads>;

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
    bool isSliceMode() const noexcept { return padModeParam->load() > 0.5f; }
    int  getBaseNote() const noexcept { return juce::roundToInt (baseNoteParam->load()); }

    void setPadStart (int pad, juce::int64 position);            // cue point, automatic end
    void setPadRegion (int pad, juce::int64 start, juce::int64 end);
    void clearPad (int pad);
    void clearAllPads();
    void chopEqual (int numPads);                                // pads 1..N = N equal regions
    juce::int64 movePadStart (int pad, juce::int64 newPosition); // returns the applied position
    juce::int64 getEffectiveEnd (int pad) const;                 // -1 if the pad is empty
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
        int pad = -1;
        double pos = 0.0;
        juce::int64 end = 0;
        float env = 0.0f, gain = 1.0f;
        juce::uint64 age = 0;
    };

    struct Command
    {
        enum class Type { padOn, padOff, playRegion, stopAll } type = Type::stopAll;
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
    juce::int64 resolveEnd (int pad, bool sliceMode, juce::int64 length) const;

    // audio thread
    void pushCommand (const Command&);
    void drainCommands();
    void handleMidi (const juce::MidiMessage&);
    void padOn (int pad, float velocity);
    void padOff (int pad);
    void startVoice (int pad, juce::int64 start, juce::int64 end, float velocity, bool preview);
    void renderVoices (juce::AudioBuffer<float>&, int startSample, int numSamples);

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

    // ---- audio-thread-only state ----
    const SampleData* activeSample = nullptr;
    std::array<Voice, kMaxVoices> voices;
    juce::uint64 voiceCounter = 0;
    double currentSampleRate = 44100.0;
    float fadeSamples = 64.0f;
    bool gateMode = false;
    bool sliceMode = false;
    int baseNote = 36;
    juce::SmoothedValue<float> gainSmoothed;

    juce::ThreadPool loadPool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (CutbenchProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CutbenchProcessor)
};
