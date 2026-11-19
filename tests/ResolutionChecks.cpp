#include "TestTransport.h"
#include "../Source/kWoodRoomEngine.h"
#include <cmath>
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
void set(ChoirProcessor& p,const char* id,float value) {auto* param=p.state.getParameter(id);check(param,"Missing parameter");param->setValueNotifyingHost(param->convertTo0to1(value));}
void pump() {juce::Timer::callPendingTimersSynchronously();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
template<class F> void wait(F f) {const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!f()){check(std::chrono::steady_clock::now()<end,"Timed out");pump();}}
void render(ChoirProcessor& p) {p.requestRender();wait([&]{return p.current() && !p.busy.load();});}
juce::Component* find(juce::Component& c,const juce::String& id) {if(c.getComponentID()==id)return &c;for(auto* child:c.getChildren())if(auto* hit=find(*child,id))return hit;return nullptr;}
void roomReference(double rate) {
    std::array<std::shared_ptr<const choir::Take>,4> bank{};auto pulse=std::make_shared<choir::Take>();pulse->rate=rate;pulse->mono.resize(1000);pulse->mono[10]=.01f;bank[0]=pulse;
    choir::Settings settings{1,0,0,0,.18f,0,1977};settings.sourceRate=settings.outputRate=rate;
    settings.roomParameters={.31f,.82f,.62f,.23f,.47f,.78f};
    const auto result=choir::render(bank,settings);
    auto native=std::make_unique<kWoodRoomEngine>();native->reset(rate);
    for(int i=0;i<6;++i)native->setParameter(i,settings.roomParameters[static_cast<size_t>(i)]);
    std::vector<float> left(result->left.size()),right(left.size());left[10]=right[10]=.01f;
    for(size_t i=0;i<left.size();i+=256){float* audio[]{left.data()+i,right.data()+i};native->processReplacing(audio,audio,static_cast<int>(std::min<size_t>(256,left.size()-i)));}
    check(left.size()==1000+static_cast<size_t>(rate*6),"Room tail duration changed at project rate");
    const size_t fade=static_cast<size_t>(rate/50);
    for(size_t i=0;i<left.size();++i) {
        const float dry=i==10?.01f:0;const float ramp=i>=left.size()-fade?static_cast<float>(left.size()-1-i)/static_cast<float>(fade):1;
        check(std::abs(result->left[i]-(dry*(1-settings.room)+left[i]*settings.room)*ramp)<1.e-7f && std::abs(result->right[i]-(dry*(1-settings.room)+right[i]*settings.room)*ramp)<1.e-7f,"Native room parameters/rate were not forwarded unchanged");
    }
}
int main() try {
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("previews");dir.createDirectory();
    ChoirProcessor p;p.prepareToPlay(96000,960);
    check(p.sampleResolution()==96000 && p.outputResolution()==96000,"Default resolution does not follow project");
    check(p.startRecordingAll(),"Cannot arm 96k capture");
    juce::AudioBuffer<float> audio(8,960);juce::MidiBuffer midi;
    std::vector<float> expected;
    for(int block=0;block<10;++block) {
        for(int i=0;i<960;++i) {
            const float value=.2f*std::sin(2*juce::MathConstants<double>::pi*26000*(block*960+i)/96000.);expected.push_back(value);
            for(int c=0;c<8;++c)audio.setSample(c,i,value);
        }
        hostProcess(p,audio,midi);
    }
    p.finishRecording();wait([&]{return !p.captureActive();});
    for(auto& take:p.takes())check(take && take->rate==96000 && take->mono==expected,"96k recording lost high-rate samples");
    const auto origin=p.takes()[0]->startSeconds;
    set(p,"singers",4);set(p,"detune",0);set(p,"timing",0);set(p,"room",0);
    render(p);const auto full=p.rendered();check(full->left.size()==9600 && full->settings.outputRate==96000,"96k render length/rate mismatch");
    const auto file=dir.getChildFile("resolution-96k.wav");check(p.exportWav(file),"96k export failed");
    juce::AudioFormatManager formats;formats.registerBasicFormats();std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    check(reader && reader->sampleRate==96000 && reader->lengthInSamples==9600 && reader->bitsPerSample==24,"96k WAV header mismatch");reader.reset();
    ChoirProcessor imported;imported.prepareToPlay(96000,960);check(imported.loadTake(file,0) && imported.takes()[0]->rate==96000 && imported.takes()[0]->mono.size()==9600,"96k import forced to 48k");
    set(imported,"sampleMode",1);check(imported.loadTake(file,1) && imported.takes()[1]->rate==48000 && imported.takes()[1]->mono.size()==4800,"48k import option ignored");
    p.play();audio.clear();p.processBlock(audio,midi);for(int i=0;i<960;++i)check(audio.getSample(0,i)==full->left[static_cast<size_t>(i)],"96k preview changed samples/rate");p.stop();
    set(p,"sampleMode",1);check(!p.current(),"Changing sample resolution did not stale render");render(p);
    const auto lowered=p.rendered();double fullEnergy=0,lowEnergy=0;
    for(size_t i=1000;i<7000;++i){fullEnergy+=full->left[i]*full->left[i];lowEnergy+=lowered->left[i]*lowered->left[i];}
    std::cout<<"48k source rejection energy ratio: "<<lowEnergy/fullEnergy<<"\n";
    check(lowered->left.size()==9600 && lowEnergy<fullEnergy*.001,"48k source mode did not remove above-Nyquist input while retaining 96k output");
    check(p.takes()[0]->mono==expected,"Rate change destructively replaced the stored recording");
    set(p,"sampleMode",0);set(p,"outputMode",1);render(p);check(p.rendered()->left.size()==4800 && p.sampleResolution()==96000,"Independent 48k output option failed");
    check(p.exportWav(file),"48k export failed");reader.reset(formats.createReaderFor(file));check(reader && reader->sampleRate==48000 && reader->lengthInSamples==4800,"48k WAV option mismatch");reader.reset();
    set(p,"outputMode",0);p.prepareToPlay(88200,960);check(!p.current() && p.sampleResolution()==88200 && p.outputResolution()==88200,"Project rate change left stale render current");render(p);check(p.rendered()->left.size()==8820,"Project rate change altered duration");
    p.prepareToPlay(96000,960);set(p,"sampleMode",1);check(p.startRecording(3),"Cannot arm 48k capture in 96k project");
    for(int c=0;c<8;++c)for(int i=0;i<960;++i)audio.setSample(c,i,.1f);hostProcess(p,audio,midi);p.finishRecording();wait([&]{return !p.captureActive();});
    check(p.takes()[3]->rate==48000 && p.takes()[3]->mono.size()==480 && p.takes()[3]->startSeconds==origin,"48k capture lost duration or timeline anchor");
    // Actual pane bindings, all six native parameters, persistence and visibility.
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());auto* toggle=dynamic_cast<juce::Button*>(find(*editor,"tweaks"));auto* pane=find(*editor,"tweakPane");
    check(toggle && pane && !pane->isVisible(),"Tweak pane does not start collapsed");toggle->setToggleState(true,juce::dontSendNotification);toggle->onClick();check(pane->isVisible(),"Tweak pane did not open");
    auto* samples=dynamic_cast<juce::ComboBox*>(find(*editor,"sampleResolution"));auto* output=dynamic_cast<juce::ComboBox*>(find(*editor,"outputResolution"));check(samples && output,"Resolution selectors missing");
    samples->setSelectedId(1,juce::sendNotificationSync);output->setSelectedId(2,juce::sendNotificationSync);check(p.sampleResolution()==96000 && p.outputResolution()==48000,"Resolution UI not attached");
    const std::array<const char*,6> ids{"roomRegen","roomDerez","roomFilter","roomEarly","roomPosition","roomWet"};
    for(size_t i=0;i<ids.size();++i){auto* dial=dynamic_cast<juce::Slider*>(find(*editor,ids[i]));check(dial,"Native room dial missing");dial->setValue(20+static_cast<double>(i)*10,juce::sendNotificationSync);check(std::abs(p.settings().roomParameters[i]-(.2f+static_cast<float>(i)*.1f))<1.e-6f,"Room dial not attached");}
    juce::MemoryBlock state;p.getStateInformation(state);ChoirProcessor recalled;recalled.prepareToPlay(96000,960);recalled.setStateInformation(state.getData(),static_cast<int>(state.getSize()));wait([&]{return recalled.current() && !recalled.busy.load();});
    check(recalled.settings()==p.settings() && recalled.takes()[0]->mono==expected && recalled.takes()[0]->rate==96000 && recalled.takes()[3]->rate==48000 && recalled.takes()[0]->startSeconds==origin,"Session lost rates, mixed-rate PCM, room settings or anchor");
    auto* reset=dynamic_cast<juce::Button*>(find(*editor,"resetRoom"));check(reset,"Room reset missing");reset->onClick();const std::array<float,6> defaults{.5f,.5f,.25f,.5f,.75f,1.f};check(p.settings().roomParameters==defaults,"Room reset did not preserve original defaults");
    for(int i=0;i<20;++i)pump();auto stream=dir.getChildFile("ui-tweaks.png").createOutputStream();check(bool(stream),"Cannot save pane screenshot");stream->setPosition(0);stream->truncate();check(juce::PNGImageFormat().writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds(),true),*stream),"Cannot encode pane screenshot");stream.reset();
    toggle->setToggleState(false,juce::dontSendNotification);toggle->onClick();check(!pane->isVisible(),"Tweak pane did not close");
    // Old embedded sources had no per-take rate and no rate/room parameters.
    ChoirProcessor legacy;legacy.loadDemo();juce::MemoryBlock old;legacy.getStateInformation(old);auto tree=juce::ValueTree::readFromData(old.getData(),old.getSize());
    for(int i=tree.getNumChildren();--i>=0;){auto node=tree.getChild(i);if(node.hasType("TAKE"))node.removeProperty("rate",nullptr);else {const auto id=node.getProperty("id").toString();if(id=="sampleMode" || id=="outputMode" || (id.startsWith("room") && id!="room"))tree.removeChild(i,nullptr);}}
    juce::MemoryBlock oldData;{juce::MemoryOutputStream out(oldData,false);tree.writeToStream(out);}recalled.setStateInformation(oldData.getData(),static_cast<int>(oldData.getSize()));wait([&]{return recalled.current() && !recalled.busy.load();});
    check(recalled.takes()[0]->rate==48000 && recalled.takes()[0]->mono.size()==144000 && recalled.outputResolution()==96000 && recalled.settings().roomParameters==defaults,"Legacy state rate/default migration failed");
    roomReference(48000);roomReference(96000);file.deleteFile();
    std::cout<<"Project-rate capture/import/render/preview/export, independent 48k overrides, rate changes, mixed-rate recall, legacy migration, pane bindings and unchanged native room routing passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
