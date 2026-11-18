#include "ChoirEngine.h"
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
    std::cout<<"Ensemble: deterministic rendering, variation, stereo, duration, headroom, cancellation, solo, extremes and resampling passed\n";
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
