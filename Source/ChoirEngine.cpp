#include "ChoirEngine.h"
#include "kWoodRoomEngine.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace choir {
namespace {
constexpr double pi=3.14159265358979323846;
struct Random {
    uint32_t state;
    double next() { state^=state<<13; state^=state>>17; state^=state<<5; return double(state)/4294967295.; }
};
double read(const std::vector<float>& data,double pos) {
    if(pos<0 || pos>=double(data.size()-1)) return 0.;
    const auto i=static_cast<size_t>(pos); const double f=pos-double(i);
    return data[i]*(1.-f)+data[i+1]*f;
}

}
bool Settings::operator==(const Settings& b) const {
    return singers==b.singers && detune==b.detune && timing==b.timing && width==b.width
        && room==b.room && outputDb==b.outputDb && seed==b.seed
        && sourceRate==b.sourceRate && outputRate==b.outputRate && roomParameters==b.roomParameters
        && pitchEngine==b.pitchEngine && octaveUp==b.octaveUp && octaveDown==b.octaveDown;
}
Waveform waveform(const std::vector<float>& data) {
    Waveform result{};
    for(size_t i=0;i<data.size();++i) {
        const auto bin=std::min(result.size()-1,i*result.size()/data.size());
        result[bin]=std::max(result[bin],std::abs(data[i]));
    }
    return result;
}
std::vector<float> resample(const std::vector<float>& in,double rate,double targetRate) {
    if(in.empty() || !std::isfinite(rate) || rate<8000 || rate>192000 || !std::isfinite(targetRate) || targetRate<8000 || targetRate>192000) throw std::invalid_argument("Unsupported audio sample rate");
    if(double(in.size())/rate>maxSeconds+.001) throw std::invalid_argument("Each take must be 120 seconds or shorter");
    if(rate==targetRate) return in;
    const size_t length=static_cast<size_t>(std::llround(double(in.size())*targetRate/rate));
    std::vector<float> out(length);
    const double step=rate/targetRate,cutoff=std::min(1.,1./step)*.95;
    // Keep the same transition width in destination samples when downsampling.
    const int radius=static_cast<int>(std::ceil(16*std::max(1.,step)));
    for(size_t i=0;i<length;++i) {
        const double pos=double(i)*step; const int base=static_cast<int>(pos);
        double sum=0,weight=0;
        for(int j=base-radius+1;j<=base+radius;++j) {
            const double distance=pos-j,phase=pi*distance*cutoff;
            const double w=(std::abs(phase)<1.e-10?cutoff:std::sin(phase)/(pi*distance))*(.5+.5*std::cos(pi*distance/radius));
            if(j>=0 && j<static_cast<int>(in.size())) { sum+=in[static_cast<size_t>(j)]*w; weight+=w; }
        }
        out[i]=static_cast<float>(weight!=0?sum/weight:0.);
    }
    return out;
}
std::shared_ptr<const Take> demo(int variation) {
    auto take=std::make_shared<Take>(); take->name="Synthetic ah "+std::to_string(variation+1)+" (demo)";
    take->mono.resize(sampleRate*3);
    const double fundamental=130.81278265*std::pow(2.,(variation==0?-1.5:1.5)/1200.);
    for(size_t i=0;i<take->mono.size();++i) {
        const double t=double(i)/sampleRate;
        const double envelope=std::min(1.,t/.07)*std::min(1.,(3.-t)/.18)*(.85+.15*std::sin(2*pi*.6*t));
        double value=0;
        for(int h=1;h<=48;++h) {
            const double hz=fundamental*h;
            const auto band=[hz](double centre,double width) { return std::exp(-.5*std::pow((hz-centre)/width,2)); };
            const double amp=(.3+2.2*band(730,100)+1.5*band(1090,140)+band(2440,220))/h;
            value+=amp*std::sin(2*pi*hz*t+.012*h*std::sin(2*pi*(4.8+variation*.3)*t));
        }
        take->mono[i]=static_cast<float>(value*.13*envelope);
    }
    take->preview=waveform(take->mono);
    return take;
}
std::unique_ptr<Render> render(const std::array<std::shared_ptr<const Take>,slots>& bank,Settings s,
                             const std::function<bool()>& cancel,const std::function<void(float)>& progress) {
    if(!std::isfinite(s.sourceRate) || s.sourceRate<8000 || s.sourceRate>192000
        || !std::isfinite(s.outputRate) || s.outputRate<8000 || s.outputRate>192000)
        throw std::invalid_argument("Unsupported render sample rate");
    for(float v:s.roomParameters) if(!std::isfinite(v) || v<0 || v>1) throw std::invalid_argument("Invalid room parameter");
    const double rate=s.outputRate;
    std::vector<std::shared_ptr<const Take>> sources;
    size_t longest=0;
    for(const auto& t:bank) if(t && !t->mono.empty()) {
        if(cancel && cancel()) return {};
        if(!std::isfinite(t->rate) || t->rate<8000 || t->rate>192000 || double(t->mono.size())/t->rate>maxSeconds+.001) throw std::invalid_argument("Take too long");
        for(float v:t->mono) if(!std::isfinite(v)) throw std::invalid_argument("Non-finite source audio");
        if(t->rate==s.sourceRate && t->rate==rate) sources.push_back(t);
        else {
            auto converted=std::make_shared<Take>();
            converted->name=t->name;converted->hasTimeline=t->hasTimeline;converted->startSeconds=t->startSeconds;
            auto samples=resample(t->mono,t->rate,s.sourceRate);
            if(cancel && cancel()) return {};
            converted->mono=s.sourceRate==rate?std::move(samples):resample(samples,s.sourceRate,rate);converted->rate=rate;
            sources.push_back(std::move(converted));
        }
        longest=std::max(longest,sources.back()->mono.size());
    }
    if(sources.empty()) throw std::invalid_argument("Load at least one vocal take first");
    if(s.singers<1 || s.singers>32 || !std::isfinite(s.detune) || s.detune<0 || s.detune>25
        || !std::isfinite(s.timing) || s.timing<0 || s.timing>60 || !std::isfinite(s.width) || s.width<0 || s.width>1
        || !std::isfinite(s.room) || s.room<0 || s.room>.5f || !std::isfinite(s.outputDb) || s.outputDb<-24 || s.outputDb>0)
        throw std::invalid_argument("Invalid ensemble settings");
    if(s.octaveUp<0 || s.octaveDown<0 || s.octaveUp>s.singers || s.octaveDown>s.singers || s.octaveUp+s.octaveDown>s.singers
        || (s.pitchEngine!=PitchEngine::rubberBand && s.pitchEngine!=PitchEngine::signalsmith))
        throw std::invalid_argument("Octave voice counts must fit within the Singers total");
    // Spread each octave group across the panorama, retaining the existing
    // voice/source order and random sequence for unshifted choir renders.
    std::vector<int> octaves(static_cast<size_t>(s.singers));
    for(int i=0;i<s.octaveUp;++i) octaves[static_cast<size_t>((2*i+1)*s.singers/(2*s.octaveUp))]=12;
    std::vector<size_t> remaining;
    for(size_t i=0;i<octaves.size();++i) if(octaves[i]==0) remaining.push_back(i);
    for(int i=0;i<s.octaveDown;++i) octaves[remaining[static_cast<size_t>((2*i+1)*static_cast<int>(remaining.size())/(2*s.octaveDown))]]=-12;
    std::vector<std::array<std::vector<float>,2>> shifted(sources.size());
    const size_t tail=s.room>0?static_cast<size_t>(std::llround(rate*6)):0;
    const size_t count=longest+static_cast<size_t>(std::ceil(s.timing*.001*rate))+tail;
    auto result=std::make_unique<Render>(); result->settings=s;
    for(const auto& take:sources) if(take->hasTimeline) {
        if(result->hasTimeline && std::abs(result->startSeconds-take->startSeconds)>1./rate) result->mixedStarts=true;
        if(!result->hasTimeline) {result->hasTimeline=true;result->startSeconds=take->startSeconds;}
    }
    result->left.resize(count); result->right.resize(count); result->dry.resize(count);
    std::copy(sources[0]->mono.begin(),sources[0]->mono.end(),result->dry.begin());
    Random rng{static_cast<uint32_t>(s.seed)^0xa5f179b3u};
    const int hop=static_cast<int>(std::llround(rate*.025)),window=hop*2; // 50 ms Hann windows, 50% overlap.
    std::vector<double> hann(static_cast<size_t>(hop));
    for(int i=0;i<hop;++i) hann[static_cast<size_t>(i)]=.5-.5*std::cos(2*pi*i/window);
    for(int v=0;v<s.singers;++v) {
        const size_t index=static_cast<size_t>(v)%sources.size();
        const auto* voiceSource=&sources[index]->mono;
        const int octave=octaves[static_cast<size_t>(v)];
        if(octave!=0) {
            auto& cache=shifted[index][octave>0?0:1];
            if(cache.empty()) cache=shiftOctave(*voiceSource,rate,octave,s.pitchEngine,s.seed+static_cast<int>(index)*31+(octave>0?101:211),cancel);
            if(cache.empty()) return {};
            voiceSource=&cache;
        }
        const auto& source=*voiceSource;
        const double detune=s.singers==1?0.:(rng.next()*2.-1.)*s.detune;
        const double delay=s.singers==1?0.:rng.next()*s.timing*.001*rate;
        const double pan=(s.singers==1?0.:2.*v/(s.singers-1)-1.)*s.width;
        const double gain=1./std::sqrt(double(s.singers));
        const double gainL=std::cos((pan+1.)*pi/4.)*std::sqrt(2.)*gain;
        const double gainR=std::sin((pan+1.)*pi/4.)*std::sqrt(2.)*gain;
        const double phase=rng.next()*2*pi,driftRate=.13+.16*rng.next();
        const double cutoff=5200+5500*rng.next(),alpha=1.-std::exp(-2*pi*cutoff/rate);
        double low=0;
        const size_t end=std::min(count,source.size()+static_cast<size_t>(delay)+window);
        std::vector<double> ratios(end/hop+3);
        for(size_t g=0;g<ratios.size();++g) {
            const double centre=double(g)*hop;
            const double cents=detune+std::min(2.f,s.detune*.2f)*std::sin(2*pi*driftRate*centre/rate+phase);
            ratios[g]=std::pow(2.,cents/1200.);
        }
        for(size_t i=0;i<end;++i) {
            if((i&4095u)==0u && cancel && cancel()) return {};
            double value=0;
            if(s.detune==0 || s.singers==1) value=read(source,double(i)-delay);
            else {
                const int grain=static_cast<int>(i)/hop;
                for(int k=grain-1;k<=grain;++k) {
                    const double start=double(k*hop),local=double(i)-start;
                    const double centre=start+hop;
                    const double ratio=ratios[static_cast<size_t>(k+1)];
                    const double pos=centre-delay+(local-hop)*ratio;
                    const double weight=k==grain?hann[i%hop]:1.-hann[i%hop];
                    value+=read(source,pos)*weight;
                }
            }
            if(s.singers>1) { low+=alpha*(value-low); value=.85*value+.15*low; }
            if(octave!=0) {
                // 300 ms slow-start fade, relative to this singer's timing offset.
                // A -60-to-0 dB-shaped ramp, normalized to exact silence at onset.
                const double age=double(i)-delay;
                if(age<=0) value=0;
                else if(age<rate*.3) value*=std::expm1(std::log(1000.)*age/(rate*.3))/999.;
            }
            result->left[i]+=static_cast<float>(value*gainL);
            result->right[i]+=static_cast<float>(value*gainR);
        }
        if(progress) progress(.85f*static_cast<float>(v+1)/static_cast<float>(s.singers));
    }
    if(s.room>0) {
        auto room=std::make_unique<kWoodRoomEngine>();room->reset(rate);
        for(int i=0;i<6;++i) room->setParameter(i,s.roomParameters[static_cast<size_t>(i)]);
        std::array<float,256> wetL{},wetR{};
        for(size_t offset=0;offset<count;offset+=wetL.size()) {
            if(cancel && cancel()) return {};
            const size_t n=std::min(wetL.size(),count-offset);
            std::copy_n(result->left.data()+offset,n,wetL.data());std::copy_n(result->right.data()+offset,n,wetR.data());
            float* audio[]{wetL.data(),wetR.data()};room->processReplacing(audio,audio,static_cast<int>(n));
            for(size_t i=0;i<n;++i) {
                result->left[offset+i]=result->left[offset+i]*(1.f-s.room)+wetL[i]*s.room;
                result->right[offset+i]=result->right[offset+i]*(1.f-s.room)+wetR[i]*s.room;
            }
        }
    }
    if(progress) progress(.95f);
    float peak=0;
    const size_t fade=std::min(count/2,static_cast<size_t>(rate/50));
    for(size_t i=0;i<count;++i) {
        const float ramp=i>=count-fade?static_cast<float>(count-1-i)/static_cast<float>(fade):1.f;
        result->left[i]*=ramp; result->right[i]*=ramp;
        peak=std::max({peak,std::abs(result->left[i]),std::abs(result->right[i])});
    }
    const float trim=peak>.94f?.94f/peak:1.f;
    result->peakTrimDb=20.f*std::log10(trim);
    const float output=std::pow(10.f,s.outputDb/20.f);
    for(size_t i=0;i<count;++i) { result->left[i]*=trim*output; result->right[i]*=trim*output; result->dry[i]*=output; }
    result->wetWave=waveform(result->left); result->dryWave=waveform(result->dry);
    if(progress) progress(1.f);
    return result;
}
}
