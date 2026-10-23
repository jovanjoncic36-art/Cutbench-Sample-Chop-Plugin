#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>
#include "SampleData.h"

/*
    Threading model
    ---------------
    Message thread : owns the model (current sample, slice list, selection). Every mutation takes
                     `modelLock` and then republishes the slice table to lock-free atomics.
    Loader thread  : decodes the file + builds peaks, then hands the result to the message thread.
    Audio thread   : never locks, never allocates. It reads
                       - `audioSample`  (atomic pointer to immutable SampleData),
                       - the atomic slice table,
                       - UI commands via a lock-free FIFO,
                       - parameters via APVTS atomics.
                     Replaced SampleData objects are kept alive in `graveyard` for a few seconds so
                     the audio thread can never touch freed memory.
*/
class CutbenchProcessor : public juce::AudioProcessor,
                          public juce::ChangeBroadcaster
{
public:
    static constexpr int kMaxSlices = 64;
    static constexpr int kMaxVoices = 32;
    static constexpr juce::int64 kMinSliceLength = 64;   // frames

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
    std::vector<Slice> getSlices() const;
    int  getSelectedSlice() const;
    void setSelectedSlice (int index);

    int  addSlice (juce::int64 start, juce::int64 end);          // returns new index or -1
    bool splitAt (juce::int64 position);                         // split the slice containing position
    void removeSlice (int index);
    void resetSlices();                                          // one slice covering the whole file

    // Moves every slice edge located at oldPosition (shared boundaries move together).
    // Returns the position actually applied (clamped).
    juce::int64 moveBoundary (juce::int64 oldPosition, juce::int64 newPosition);

    juce::String getStatusText() const;
    juce::AudioFormatManager& getFormatManager() { return formatManager; }

    // Preview from the UI (executed on the audio thread via a lock-free queue)
    void previewSlice (int index);
    void previewRegion (juce::int64 start, juce::int64 end);
    void stopPreview();
    juce::int64 getPlayhead() const noexcept { return playhead.load (std::memory_order_relaxed); }   // -1 = idle

    juce::AudioProcessorValueTreeState apvts { *this, nullptr, "CUTBENCH", createLayout() };

private:
    struct Voice
    {
        bool active = false, releasing = false, preview = false;
        int note = -1;
        double pos = 0.0;
        juce::int64 end = 0;
        float env = 0.0f, gain = 1.0f;
        juce::uint64 age = 0;
    };

    struct Command
    {
        enum class Type { playSlice, playRegion, stopAll } type = Type::stopAll;
        int slice = -1;
        juce::int64 start = 0, end = 0;
    };

    struct Retired
    {
        std::shared_ptr<const SampleData> data;
        juce::uint32 time = 0;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // loading / model (message thread)
    void loadFileInternal (const juce::File&, std::optional<std::vector<Slice>> restore);
    void decodeFile (const juce::File&, std::shared_ptr<SampleData>& out, juce::String& error);
    void finishLoad (std::shared_ptr<SampleData> data, juce::String error,
                     std::optional<std::vector<Slice>> restore, juce::uint32 generation);
    void purgeGraveyard();
    void publishSlices();                    // caller must hold modelLock
    void setStatus (const juce::String&);

    // audio thread
    void pushCommand (const Command&);
    void drainCommands();
    void handleMidi (const juce::MidiMessage&);
    bool getSliceRange (int index, juce::int64& start, juce::int64& end) const;
    void startVoice (int note, juce::int64 start, juce::int64 end, float velocity, bool preview);
    void renderVoices (juce::AudioBuffer<float>&, int startSample, int numSamples);

    // ---- message-thread state ----
    mutable juce::CriticalSection modelLock;
    std::shared_ptr<const SampleData> currentSample;
    std::vector<Slice> slices;
    int selectedSlice = -1;
    juce::String samplePath;
    juce::String statusText;
    std::vector<Retired> graveyard;
    juce::uint32 loadGeneration = 0;
    juce::AudioFormatManager formatManager;

    // ---- shared with the audio thread (lock-free) ----
    std::atomic<const SampleData*> audioSample { nullptr };
    std::array<std::atomic<juce::int64>, kMaxSlices> sliceStart, sliceEnd;
    std::atomic<int> sliceCount { 0 };
    std::atomic<juce::int64> playhead { -1 };
    juce::AbstractFifo cmdFifo { 64 };
    std::array<Command, 64> cmdBuffer;

    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* baseNoteParam = nullptr;
    std::atomic<float>* playModeParam = nullptr;
    std::atomic<float>* fadeParam = nullptr;

    // ---- audio-thread-only state ----
    const SampleData* activeSample = nullptr;
    std::array<Voice, kMaxVoices> voices;
    juce::uint64 voiceCounter = 0;
    double currentSampleRate = 44100.0;
    float fadeSamples = 64.0f;
    bool gateMode = false;
    int baseNote = 36;
    juce::SmoothedValue<float> gainSmoothed;

    juce::ThreadPool loadPool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (CutbenchProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CutbenchProcessor)
};
