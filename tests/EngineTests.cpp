#include "ChoirEngine.h"
#include "kWoodRoomEngine.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() try {
    std::array<std::shared_ptr<const choir::Take>,4> takes{};
    bool failed=false; try { choir::render(takes,{}); } catch(const std::invalid_argument&) { failed=true; }
    check(failed,"Empty bank accepted");
    takes[0]=choir::demo(0); takes[1]=choir::demo(1);
    choir::Settings s; s.singers=8;
    auto a=choir::render(takes,s),b=choir::render(takes,s);
    check(a->left==b->left && a->right==b->right,"Render not deterministic");
    check(a->left.size()>takes[0]->mono.size(),"Missing timing/room tail");
    double stereo=0,energy=0;
    for(size_t i=0;i<a->left.size();++i) {
        check(std::isfinite(a->left[i]) && std::abs(a->left[i])<=.95f,"Invalid output/headroom");
        stereo+=std::abs(a->left[i]-a->right[i]); energy+=a->left[i]*a->left[i];
    }
    check(stereo>1. && energy>1.,"Silent or mono ensemble");
    check(a->left.back()==0 && a->right.back()==0,"Unfaded render end");
    s.seed++; auto varied=choir::render(takes,s); check(varied->left!=a->left,"Variation seed has no effect");
    check(!choir::render(takes,s,[]{return true;}),"Cancellation ignored");
    s.singers=1; s.detune=0; s.timing=0; s.width=0; s.room=0; s.outputDb=0;
    takes[1].reset(); auto solo=choir::render(takes,s);
    for(size_t i=100;i<10000;++i) check(std::abs(solo->left[i]-takes[0]->mono[i])<1.e-6f,"Solo path changed pitch/timing");
    s.singers=32; s.detune=25; s.timing=60; s.width=0; s.room=.5; auto extreme=choir::render(takes,s);
    for(size_t i=0;i<extreme->left.size();++i) check(std::isfinite(extreme->left[i]) && std::abs(extreme->left[i])<1.f,"Extreme settings failed");
    auto mono=choir::render(takes,choir::Settings{8,0,0,0,0,-3,1977});
    check(mono->left==mono->right,"Zero width not mono");
    std::vector<float> sine(44100);
    for(size_t i=0;i<sine.size();++i) sine[i]=static_cast<float>(.5*std::sin(2*3.141592653589793*440*double(i)/44100.));
    auto converted=choir::resample(sine,44100);
    check(converted.size()==48000,"Import duration changed");
    double error=0; for(int i=100;i<47000;++i) error+=std::abs(converted[static_cast<size_t>(i)]-.5*std::sin(2*3.141592653589793*440*i/48000.));
    check(error/46900<.001,"Resampling pitch or quality failure");
    // Independent kWoodRoom return verifies native routing and the external Room blend.
    std::array<std::shared_ptr<const choir::Take>,4> roomBank{};
    auto pulse=std::make_shared<choir::Take>();pulse->mono.resize(1000);pulse->mono[10]=.01f;roomBank[0]=pulse;
    choir::Settings roomSettings{1,0,0,0,.18f,0,1977};auto roomResult=choir::render(roomBank,roomSettings);
    check(roomResult->left.size()==1000+6*choir::sampleRate,"kWoodRoom render tail is not six seconds");
    auto native=std::make_unique<kWoodRoomEngine>();native->reset(choir::sampleRate);
    const std::array<float,6> nativeValues{.5f,.5f,.25f,.5f,.75f,1.f};
    for(int i=0;i<6;++i) native->setParameter(i,nativeValues[static_cast<size_t>(i)]);
    std::vector<float> wl(roomResult->left.size()),wr(wl.size());wl[10]=wr[10]=.01f;
    for(size_t offset=0;offset<wl.size();offset+=256) {
        float* block[]{wl.data()+offset,wr.data()+offset};native->processReplacing(block,block,static_cast<int>(std::min<size_t>(256,wl.size()-offset)));
    }
    for(size_t i=0;i<wl.size();++i) {
        const float dry=i==10?.01f:0.f;const size_t fade=choir::sampleRate/50;
        const float ramp=i>=wl.size()-fade?static_cast<float>(wl.size()-1-i)/static_cast<float>(fade):1.f;
        check(std::abs(roomResult->left[i]-(dry*(1.f-roomSettings.room)+wl[i]*roomSettings.room)*ramp)<1.e-7f
           && std::abs(roomResult->right[i]-(dry*(1.f-roomSettings.room)+wr[i]*roomSettings.room)*ramp)<1.e-7f,"Room stage altered native kWoodRoom output or applied blend incorrectly");
    }
    std::cout<<"Ensemble: deterministic rendering, variation, stereo, duration, headroom, cancellation, solo, extremes and resampling passed\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
