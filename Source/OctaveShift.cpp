#include "OctaveShift.h"
#include <rubberband/RubberBandStretcher.h>
#include <signalsmith-stretch.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace choir {
std::vector<float> shiftOctave(const std::vector<float>& input,double rate,int semitones,
                              PitchEngine engine,int seed,const std::function<bool()>& cancelled) {
    if(input.empty() || !std::isfinite(rate) || rate<8000 || rate>192000 || (semitones!=12 && semitones!=-12)
        || (engine!=PitchEngine::rubberBand && engine!=PitchEngine::signalsmith))
        throw std::invalid_argument("Invalid octave shift settings");
    const auto stop=[&]{return cancelled && cancelled();};
    if(stop()) return {};
    std::vector<float> output(input.size());
    constexpr size_t block=4096;
    if(engine==PitchEngine::rubberBand) {
        // Give the spectral engine enough trailing context for very short
        // recordings. This padding is never added to the returned phrase.
        std::vector<float> padded;
        const auto* source=&input;
        const size_t minimum=static_cast<size_t>(std::ceil(rate*.25));
        if(input.size()<minimum) {padded.resize(minimum);std::copy(input.begin(),input.end(),padded.begin());source=&padded;}
        using RB=RubberBand::RubberBandStretcher;
        RB shifter(static_cast<size_t>(std::llround(rate)),1,RB::OptionProcessOffline|RB::OptionEngineFiner|RB::OptionFormantPreserved,1.,semitones==12?2.:.5);
        if(shifter.getEngineVersion()!=3) throw std::runtime_error("Rubber Band R3 engine is unavailable");
        shifter.setExpectedInputDuration(source->size());shifter.setMaxProcessSize(block);
        for(size_t offset=0;offset<source->size();) {
            if(stop()) return {};
            const size_t n=std::min(block,source->size()-offset);const float* samples[]{source->data()+offset};
            shifter.study(samples,n,offset+n==source->size());offset+=n;
        }
        size_t written=0;
        std::array<float,block> scratch{};
        const auto drain=[&] {
            for(int available=shifter.available();available>0;available=shifter.available()) {
                const size_t n=std::min(block,static_cast<size_t>(available));float* samples[]{scratch.data()};
                const size_t got=shifter.retrieve(samples,n);
                if(!got) throw std::runtime_error("Rubber Band produced no output");
                if(written+got>source->size()) throw std::runtime_error("Rubber Band output duration mismatch");
                if(written<output.size())std::copy_n(scratch.data(),std::min(got,output.size()-written),output.data()+written);
                written+=got;
            }
        };
        for(size_t offset=0;offset<source->size();) {
            if(stop()) return {};
            const size_t n=std::min(block,source->size()-offset);const float* samples[]{source->data()+offset};
            shifter.process(samples,n,offset+n==source->size());offset+=n;drain();
        }
        drain();
        if(written!=source->size()) throw std::runtime_error("Rubber Band output duration mismatch");
    } else {
        signalsmith::stretch::SignalsmithStretch<float> shifter(seed);
        shifter.presetDefault(1,static_cast<float>(rate));
        shifter.setTransposeSemitones(static_cast<float>(semitones));
        shifter.setFormantFactor(1.f,true);shifter.setFormantBase(0);
        const size_t latency=static_cast<size_t>(shifter.inputLatency()+shifter.outputLatency());
        std::array<float,block> zeros{},scratch{};
        // Feed enough trailing silence to flush both halves of the latency,
        // discard the initial pre-roll, and retain the original timeline length.
        for(size_t offset=0;offset<input.size()+latency;) {
            if(stop()) return {};
            const size_t n=std::min(block,offset<input.size()?input.size()-offset:input.size()+latency-offset);
            const float* samples[]{offset<input.size()?input.data()+offset:zeros.data()};float* result[]{scratch.data()};
            shifter.process(samples,static_cast<int>(n),result,static_cast<int>(n));
            const size_t begin=std::max(offset,latency),end=std::min(offset+n,latency+input.size());
            if(begin<end)std::copy_n(scratch.data()+begin-offset,end-begin,output.data()+begin-latency);
            offset+=n;
        }
    }
    for(float v:output)if(!std::isfinite(v))throw std::runtime_error("Pitch engine produced non-finite audio");
    return output;
}
}
