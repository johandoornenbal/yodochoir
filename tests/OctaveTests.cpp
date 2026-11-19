#include "ChoirEngine.h"
#include "kWoodRoomEngine.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <algorithm>
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
constexpr double pi=3.14159265358979323846;
double amplitude(const std::vector<float>& x,double rate,double hz) {
    double re=0,im=0;const size_t start=static_cast<size_t>(rate*.4),end=std::min(x.size(),static_cast<size_t>(rate*1.6));
    for(size_t i=start;i<end;++i){re+=x[i]*std::cos(2*pi*hz*i/rate);im+=x[i]*std::sin(2*pi*hz*i/rate);}
    return 2*std::hypot(re,im)/double(end-start);
}
int main() try {
    for(double rate:{48000.,96000.}) for(auto engine:{choir::PitchEngine::rubberBand,choir::PitchEngine::signalsmith}) {
        auto source=std::make_shared<choir::Take>();source->rate=rate;source->hasTimeline=true;source->startSeconds=37.125;
        source->mono.resize(static_cast<size_t>(rate*2));
        for(size_t i=0;i<source->mono.size();++i)source->mono[i]=.07f*static_cast<float>(std::sin(2*pi*220*i/rate));
        for(int shift:{12,-12}) {
            const auto output=choir::shiftOctave(source->mono,rate,shift,engine,2078);
            const double fundamental=shift==12?440:110;
            std::cout<<"rate="<<rate<<" engine="<<static_cast<int>(engine)<<" shift="<<shift<<" target="<<amplitude(output,rate,fundamental)<<" original="<<amplitude(output,rate,220)<<'\n';
            check(output.size()==source->mono.size(),"Octave shift altered phrase duration");
            check(amplitude(output,rate,fundamental)>.005 && amplitude(output,rate,fundamental)>amplitude(output,rate,220)*10,"Octave pitch incorrect, silent or dominated by original pitch");
            check(output==choir::shiftOctave(source->mono,rate,shift,engine,2078),"Octave processing not deterministic");
            std::vector<float> gated=source->mono;
            for(size_t i=0;i<gated.size();++i)if(i<rate*.3 || i>=rate*1.7)gated[i]=0;
            const auto aligned=choir::shiftOctave(gated,rate,shift,engine,2078);
            double quiet=0,loud=0;for(size_t i=static_cast<size_t>(rate*.05);i<rate*.15;++i)quiet+=aligned[i]*aligned[i];
            for(size_t i=static_cast<size_t>(rate*.6);i<rate*.7;++i)loud+=aligned[i]*aligned[i];
            check(loud>1.e-4 && quiet<loud*.01,"Pitch engine start latency was not compensated");
            std::array<std::shared_ptr<const choir::Take>,4> bank{source};
            choir::Settings settings{1,0,0,0,0,0,1977};settings.sourceRate=settings.outputRate=rate;settings.pitchEngine=engine;
            settings.octaveUp=shift==12?1:0;settings.octaveDown=shift==-12?1:0;
            const auto rendered=choir::render(bank,settings);
            const auto expected=choir::shiftOctave(source->mono,rate,shift,engine,1977+(shift==12?101:211));
            check(rendered->left[0]==0 && rendered->right[0]==0,"Octave fade does not start at silence");
            for(size_t i=static_cast<size_t>(rate*.3);i<static_cast<size_t>(rate*1.9);++i)check(std::abs(rendered->left[i]-expected[i])<1.e-6,"Octave fade changed audio after 300 ms");
            // Measure gain over small windows: the requested curve rises slowly
            // at first and accelerates, rather than using a linear amplitude ramp.
            for(const auto point:{std::pair<double,double>{.1,1./111.},std::pair<double,double>{.2,11./111.}}) {
                double originalEnergy=0,fadedEnergy=0;
                const size_t centre=static_cast<size_t>(rate*point.first),radius=static_cast<size_t>(rate*.0002);
                for(size_t i=centre-radius;i<=centre+radius;++i){originalEnergy+=expected[i]*expected[i];fadedEnergy+=rendered->left[i]*rendered->left[i];}
                check(originalEnergy>1.e-12 && std::abs(std::sqrt(fadedEnergy/originalEnergy)-point.second)<.001,"Octave fade is not a 300 ms accelerating envelope");
            }
            check(rendered->left.size()==source->mono.size(),"Fade changed the phrase duration");
            check(rendered->hasTimeline && rendered->startSeconds==37.125,"Octave shift moved timeline anchor");
        }
        int checks=0;check(choir::shiftOctave(source->mono,rate,12,engine,1977,[&]{return ++checks>3;}).empty(),"Octave processing did not cancel between blocks");
        const auto shortOutput=choir::shiftOctave(std::vector<float>(37,.01f),rate,-12,engine,1977);check(shortOutput.size()==37,"Short phrase duration changed");
        std::array<std::shared_ptr<const choir::Take>,4> bank{source};choir::Settings settings;settings.sourceRate=settings.outputRate=rate;settings.room=0;settings.detune=0;settings.timing=0;
        const auto normal=choir::render(bank,settings);settings.pitchEngine=choir::PitchEngine::signalsmith;check(normal->left==choir::render(bank,settings)->left,"Zero octave counts altered existing choir processing");
        settings.pitchEngine=engine;settings.octaveUp=2;settings.octaveDown=2;const auto grouped=choir::render(bank,settings);
        check(amplitude(grouped->left,rate,110)>.003 && amplitude(grouped->left,rate,220)>.003 && amplitude(grouped->left,rate,440)>.003,"Mixed octave groups are missing");
        settings.octaveUp=12;settings.octaveDown=1;bool rejected=false;try{choir::render(bank,settings);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Octave counts exceeded singer budget");
    }
    for(double rate:{44100.,88200.,192000.}) for(auto engine:{choir::PitchEngine::rubberBand,choir::PitchEngine::signalsmith}) for(int shift:{-12,12}) {
        const auto shortResult=choir::shiftOctave(std::vector<float>(1111,.01f),rate,shift,engine,1977);
        check(shortResult.size()==1111,"Short/odd frame count changed at another host rate");
    }
    // Independent native-room processing verifies octaves enter before kWoodRoom.
    for(auto engine:{choir::PitchEngine::rubberBand,choir::PitchEngine::signalsmith}) {
        auto take=std::make_shared<choir::Take>();take->mono.resize(48000);
        for(size_t i=1000;i<18000;++i)take->mono[i]=.01f*static_cast<float>(std::sin(2*pi*180*i/48000));
        std::array<std::shared_ptr<const choir::Take>,4> bank{take};choir::Settings settings{1,0,0,0,.18f,0,1977};settings.pitchEngine=engine;settings.octaveUp=1;
        auto dry=choir::shiftOctave(take->mono,48000,12,engine,2078);dry.back()=0;
        for(size_t i=0;i<14400;++i)dry[i]*=static_cast<float>((std::pow(1000.,double(i)/14400.)-1.)/999.);
        const auto rendered=choir::render(bank,settings);auto room=std::make_unique<kWoodRoomEngine>();room->reset(48000);
        for(int i=0;i<6;++i)room->setParameter(i,settings.roomParameters[static_cast<size_t>(i)]);
        std::vector<float> left(rendered->left.size()),right(left.size());std::copy(dry.begin(),dry.end(),left.begin());right=left;
        for(size_t offset=0;offset<left.size();offset+=256){float* audio[]{left.data()+offset,right.data()+offset};room->processReplacing(audio,audio,static_cast<int>(std::min<size_t>(256,left.size()-offset)));}
        for(size_t i=0;i<left.size();++i){const float original=i<dry.size()?dry[i]:0;const size_t fade=48000/50;const float ramp=i>=left.size()-fade?static_cast<float>(left.size()-1-i)/static_cast<float>(fade):1;
            check(std::abs(rendered->left[i]-(original*.82f+left[i]*.18f)*ramp)<1.e-7f,"Octave voices bypassed or followed the room stage");}
        check(rendered->left.size()==48000+48000*6,"Octave voices changed room-tail duration");
    }
    std::cout<<"Both octave engines: +/-12 pitch, 48/96k, duration, start latency, determinism, cancellation, short phrases, singer budget, zero-count bypass, 300 ms accelerating fades and pre-room routing passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
