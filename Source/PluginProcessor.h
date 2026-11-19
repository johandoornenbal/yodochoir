#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "ChoirEngine.h"
#include "ReaperBridge.h"
#include <mutex>
class ChoirProcessor final : public juce::AudioProcessor, private juce::Thread,private juce::Timer {
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
    bool startRecording(int slot);
    bool isArmed() const {return armed.load();}
    ReaperBridge reaper;
    juce::VST3ClientExtensions* getVST3ClientExtensions() override {return &reaper;}
    void requestWrite();
    bool isWriting() const {return writeBusy.load();}
    bool startRecordingAll();
    void finishRecording();
    void cancelRecording();
    bool isRecording() const { return recording.load(); }
    bool captureActive() const { return armed.load() || recording.load() || captureBusy.load(); }
    int recordingSlot() const { return captureSlot.load(); }
    double recordingSeconds() const { return captureSeconds.load(); }
    std::array<std::atomic<float>,2> inputPeaks{};
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
    void releaseResources() override { stop(); finishRecording(); }
    void processBlock(juce::AudioBuffer<float>&,juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&,juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>& b,juce::MidiBuffer&) override { bypass(b); }
    void processBlockBypassed(juce::AudioBuffer<double>& b,juce::MidiBuffer&) override { bypass(b); }
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool isBusesLayoutSupported(const BusesLayout& b) const override { const auto in=b.getMainInputChannelSet(); return (in.isDisabled() || in==juce::AudioChannelSet::mono() || in==juce::AudioChannelSet::stereo() || in==juce::AudioChannelSet::quadraphonic() || in==juce::AudioChannelSet::create5point1() || in==juce::AudioChannelSet::create7point1SDDS() || in==juce::AudioChannelSet::create7point1()) && b.getMainOutputChannelSet()==juce::AudioChannelSet::stereo(); }
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
    struct Capture {
        std::array<std::vector<float>,4> mono; // Allocated and zero-filled off the audio thread.
        std::array<int,4> firstInput{},channels{};
        unsigned mask=0;
        int inputChannels=0;
        size_t frames=0;
        double rate=48000.;
        int slot=0;
        uint64_t serial=0;
        bool hasTimeline=false;
        double startSeconds=0;
    };
    bool beginRecording(unsigned mask,int selectedSlot);
    struct WriteJob {
        ReaperBridge::Context context;
        juce::File file;
        uint64_t revision=0;
        choir::Settings settings;
        double startSeconds=0,length=0;
        bool success=false;
    };
    void timerCallback() override;
    void writeAudio();
    void syncTransport(int frames);
    void run() override;
    void finalizeCapture();
    template<class T> void captureInput(const juce::AudioBuffer<T>&);
    template<class T> void bypass(juce::AudioBuffer<T>& b) {
        finishRecording(); stop();
        if(getTotalNumInputChannels()==0) b.clear();
        else if(getTotalNumInputChannels()==1 && b.getNumChannels()>1) b.copyFrom(1,0,b,0,0,b.getNumSamples());
        for(int c=2;c<b.getNumChannels();++c) b.clear(c,0,b.getNumSamples());
    }
    juce::SpinLock captureLock;
    std::unique_ptr<Capture> capture;
    std::atomic<bool> armed{false},waitForStop{false},transportPlaying{false},transportKnown{false};
    std::atomic<juce::uint32> lastAudioCallback{0};
    double expectedPosition=0; // Audio-thread timeline continuity guard.
    std::mutex writeMutex;
    std::unique_ptr<WriteJob> writeJob;
    std::atomic<bool> writeBusy{false},writePending{false},writeReady{false};
    std::atomic<bool> recording{false},captureBusy{false},capturePending{false},captureInterrupted{false},captureAtLimit{false};
    std::atomic<uint64_t> captureSerial{0};
    std::atomic<int> captureSlot{-1};
    std::atomic<double> captureSeconds{0};
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
    std::atomic<float>* monitor=nullptr;
    std::atomic<uint64_t> sourceRevision{0},requestSerial{0},queuedSerial{0};
    std::atomic<bool> pending{false},playing{false},rewind{false};
    mutable juce::SpinLock renderLock;
    std::shared_ptr<const choir::Render> clip;
    std::atomic<double> hostRate{48000.};
    double cursor=0; // Protected by renderLock or audio-thread ownership.
    std::atomic<float> position{0};
    static bool saveWav(const std::shared_ptr<const choir::Render>&,const juce::File&);
};
