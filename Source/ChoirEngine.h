#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace choir {
constexpr int sampleRate=48000, slots=4, maxSeconds=120;
using Waveform=std::array<float,256>;
struct Take {
    std::string name;
    bool hasTimeline=false;
    double startSeconds=0;
    double rate=sampleRate;
    Waveform preview{};
    std::vector<float> mono; // At this take's rate; immutable once published.
};
struct Settings {
    int singers=12;
    float detune=9.f, timing=25.f, width=.85f, room=.18f, outputDb=-3.f;
    int seed=1977;
    double sourceRate=sampleRate, outputRate=sampleRate;
    std::array<float,6> roomParameters{.5f,.5f,.25f,.5f,.75f,1.f};
    bool operator==(const Settings& b) const;
};
struct Render {
    std::vector<float> left,right,dry;
    Waveform wetWave{},dryWave{};
    float peakTrimDb=0;
    Settings settings;
    uint64_t revision=0;
    bool hasTimeline=false,mixedStarts=false;
    double startSeconds=0;
};
std::vector<float> resample(const std::vector<float>& input,double sourceRate,double targetRate=sampleRate);
std::shared_ptr<const Take> demo(int variation);
Waveform waveform(const std::vector<float>& audio);
// Runs off the audio thread. Cancellation and progress are optional callbacks.
std::unique_ptr<Render> render(const std::array<std::shared_ptr<const Take>,slots>& takes,
                             Settings settings,const std::function<bool()>& cancelled={},
                             const std::function<void(float)>& progress={});
}
