#pragma once
#include "../Source/PluginProcessor.h"
#include <map>
// Deterministic host playhead; production code only consumes the standard JUCE interface.
class TestTransport final : public juce::AudioPlayHead {
public:
    bool valid=true,moving=false;
    int64_t sample=480000;
    double rate=48000.;
    juce::Optional<PositionInfo> getPosition() const override {
        if(!valid) return {};
        PositionInfo p;p.setIsPlaying(moving);p.setTimeInSamples(sample);p.setTimeInSeconds(double(sample)/rate);return p;
    }
};
// Existing capture-data tests automatically supply stop->play and advancing host
// timestamps. Explicit transport-state tests call processBlock directly instead.
template<class T> void hostProcess(ChoirProcessor& p,juce::AudioBuffer<T>& b,juce::MidiBuffer& midi) {
    static std::map<ChoirProcessor*,std::unique_ptr<TestTransport>> hosts;
    auto& host=hosts[&p];if(!host || p.getPlayHead()!=host.get()) {host=std::make_unique<TestTransport>();p.setPlayHead(host.get());}
    if(p.isArmed()) {
        host->moving=false;host->sample=480000;
        juce::AudioBuffer<T> control(b.getNumChannels(),1);juce::MidiBuffer empty;p.processBlock(control,empty);
    }
    host->moving=true;p.processBlock(b,midi);host->sample+=b.getNumSamples();
}
