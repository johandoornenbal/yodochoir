#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "ChoirEngine.h"
#include <mutex>
class ChoirProcessor final : public juce::AudioProcessor, private juce::Thread {
public:
    ChoirProcessor();
    ~ChoirProcessor() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    choir::Settings settings() const;
    bool loadTake(const juce::File&,int slot);
    void clearTake(int slot);
    void loadDemo();
    std::array<std::shared_ptr<const choir::Take>,4> takes() const;
    void requestRender();
    void cancelRender();
    bool current() const;
    bool exportWav(const juce::File&);
    void play();
    void stop();
    juce::String status() const;
    std::shared_ptr<const choir::Render> rendered() const;
    float playbackPosition() const { return position.load(); }
    bool isPlaying() const { return playing.load(); }
    std::atomic<bool> busy{false};
    std::atomic<float> progress{0};
    std::array<std::atomic<float>,2> peaks{};
    std::atomic<int> uiSize{2};
    juce::AudioProcessorValueTreeState state;
    void prepareToPlay(double,int) override;
    void releaseResources() override { stop(); }
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&,juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override { b.clear(); stop(); }
    void processBlockBypassed(juce::AudioBuffer<double>& b,juce::MidiBuffer&) override { b.clear(); stop(); }
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout& b) const override { return b.getMainInputChannelSet().isDisabled() && b.getMainOutputChannelSet()==juce::AudioChannelSet::stereo(); }
    const juce::String getName() const override { return "YodoChoir"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Prototype"; }
    void changeProgramName(int,const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*,int) override;
private:
    void run() override;
    void message(juce::String);
    void sourcesChanged();
    template<class T> void process(juce::AudioBuffer<T>&,juce::MidiBuffer&);
    mutable std::mutex sourceMutex,statusMutex;
    std::array<std::shared_ptr<const choir::Take>,4> sourceBank{};
    juce::String messageText{"Load vocal takes, or try the synthetic demo."};
    juce::AudioFormatManager formats;
    std::array<std::atomic<float>*,7> values{};
    std::atomic<float>* original=nullptr;
    std::atomic<float>* loop=nullptr;
    std::atomic<uint64_t> sourceRevision{0},requestSerial{0},queuedSerial{0};
    std::atomic<bool> pending{false},playing{false},rewind{false};
    mutable juce::SpinLock renderLock;
    std::shared_ptr<const choir::Render> clip;
    double cursor=0,hostRate=48000.; // Protected by renderLock or audio-thread ownership.
    std::atomic<float> position{0};
};
