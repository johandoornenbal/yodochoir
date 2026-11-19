#include "TestTransport.h"
#include "../Source/OutputMeter.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void pump() {
    juce::Timer::callPendingTimersSynchronously();
#if JUCE_MODAL_LOOPS_PERMITTED
    juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void finished(ChoirProcessor& p) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while(p.captureActive()) { check(std::chrono::steady_clock::now()<end,"Recording finalization timed out"); pump(); }
}
void rendered(ChoirProcessor& p) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while(!p.current() || p.busy.load()) { check(std::chrono::steady_clock::now()<end,"Long render timed out"); pump(); }
}
void stereo(ChoirProcessor& p) { auto b=p.getBusesLayout();b.inputBuses.set(0,juce::AudioChannelSet::stereo());check(p.setBusesLayout(b),"Stereo fallback rejected"); }
void set(ChoirProcessor& p,const char* id,float value) { auto* param=p.state.getParameter(id); check(param,"Missing parameter"); param->setValueNotifyingHost(param->convertTo0to1(value)); }
juce::Component* find(juce::Component& p,const juce::String& id) {
    if(p.getComponentID()==id) return &p;
    for(auto* c:p.getChildren()) if(auto* hit=find(*c,id)) return hit;
    return nullptr;
}
void screenshot(juce::AudioProcessorEditor& editor,const juce::File& path) {
    auto image=editor.createComponentSnapshot(editor.getLocalBounds(),true); auto out=path.createOutputStream(); check(bool(out),"Cannot save recording preview");
    out->setPosition(0); out->truncate(); check(juce::PNGImageFormat().writeImageToStream(image,*out),"Cannot encode recording preview");
}
template<class T> void capturedRate(double rate) {
    ChoirProcessor p; stereo(p); p.prepareToPlay(rate,513); p.loadDemo(); const auto old=p.takes()[0];
    check(p.startRecording(0),"Cannot arm stereo recording");
    check(!p.startRecording(1),"Concurrent capture was accepted");
    std::vector<float> expected;
    juce::MidiBuffer midi;
    for(int block=0;block<15;++block) {
        const int n=std::array<int,5>{1,37,256,513,127}[static_cast<size_t>(block%5)]; juce::AudioBuffer<T> audio(2,n);
        for(int i=0;i<n;++i) {
            const auto index=expected.size();
            const T l=static_cast<T>(.2*std::sin(2*3.141592653589793*220*double(index)/rate));
            const T r=static_cast<T>(.1*std::cos(2*3.141592653589793*330*double(index)/rate));
            audio.setSample(0,i,l); audio.setSample(1,i,r);
            expected.push_back(static_cast<float>((double(static_cast<float>(l))+double(static_cast<float>(r)))*.5));
        }
        midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),0);
        hostProcess(p,audio,midi); check(audio.getMagnitude(0,0,n)==0,"Input leaked with monitoring disabled");
        check(!p.isPlaying(),"MIDI triggered preview during recording");
    }
    check(p.takes()[0]==old,"Unfinished capture replaced the previous take");
    p.finishRecording(); finished(p);
    const auto take=p.takes()[0]; check(take && take->rate==rate && take!=old,"Recording was not committed");
    const auto converted=choir::resample(expected,rate,rate);
    check(take->mono==converted,"Capture changed input samples or host sample rate");
    p.requestRender(); rendered(p); check(p.rendered()->left.size()>=take->mono.size(),"Captured take could not render");
}
void concurrentStops() {
    ChoirProcessor p; stereo(p); p.prepareToPlay(48000,128);
    std::atomic<bool> running{true};
    std::thread audioThread([&] {
        juce::AudioBuffer<float> block(2,128); juce::MidiBuffer midi;
        while(running.load()) {
            for(int c=0;c<2;++c) for(int i=0;i<128;++i) block.setSample(c,i,.2f);
            hostProcess(p,block,midi); std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    try {
        for(int pass=0;pass<12;++pass) {
            const auto old=p.takes()[0]; check(p.startRecording(0),"Concurrent audio callback prevented arming");
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(p.recordingSeconds()==0.) {check(std::chrono::steady_clock::now()<end,"Audio callback did not capture");pump();}
            p.finishRecording(); finished(p);
            check(p.takes()[0] && p.takes()[0]!=old && p.takes()[0]->mono.front()==.2f,"Concurrent Stop discarded a valid recording");
        }
    } catch(...) {running=false;audioThread.join();throw;}
    running=false; audioThread.join();
}
template<class T> void fourPairs(double rate,bool music=false) {
    ChoirProcessor p;
    if(music) {auto layout=p.getBusesLayout();layout.inputBuses.set(0,juce::AudioChannelSet::create7point1());check(p.setBusesLayout(layout),"7.1 Music input rejected");}
    // Match the JUCE VST3 wrapper's host-pin -> process-buffer permutation.
    const std::array<int,8> channel=music?std::array<int,8>{0,1,2,3,6,7,4,5}:std::array<int,8>{0,1,2,3,4,5,6,7};
    p.prepareToPlay(rate,513);
    check(p.getTotalNumInputChannels()==8 && p.getTotalNumOutputChannels()==2,"Default bus must be 8 in / 2 out");
    check(p.startRecordingAll() && p.recordingSlot()==-2,"Cannot arm all four pairs");
    std::array<std::vector<float>,4> expected;
    juce::MidiBuffer midi;
    for(int block=0;block<20;++block) {
        const int n=std::array<int,5>{1,37,256,513,127}[static_cast<size_t>(block%5)];juce::AudioBuffer<T> input(8,n);
        for(int i=0;i<n;++i) {
            const auto frame=expected[0].size();
            for(int pair=0;pair<4;++pair) {
                const T l=static_cast<T>(.1*(2*pair+1)+.01*std::sin(double(frame)*(.02+pair*.01)));
                const T r=static_cast<T>(.1*(2*pair+2)+.01*std::cos(double(frame)*(.03+pair*.01)));
                input.setSample(channel[static_cast<size_t>(pair*2)],i,l);input.setSample(channel[static_cast<size_t>(pair*2+1)],i,r);
                expected[static_cast<size_t>(pair)].push_back(static_cast<float>((double(static_cast<float>(l))+double(static_cast<float>(r)))*.5));
            }
        }
        hostProcess(p,input,midi);
        for(int c=0;c<8;++c) check(input.getMagnitude(c,0,n)==0,"Unused input pins leaked into output");
        for(const auto& t:p.takes()) check(!t,"A parallel capture was committed before Stop");
    }
    p.finishRecording();finished(p);
    for(size_t pair=0;pair<4;++pair) {
        const auto converted=choir::resample(expected[pair],rate,rate);
        check(p.takes()[pair] && p.takes()[pair]->mono==converted,"Input pair leaked into or missed its assigned take");
        check(p.takes()[pair]->mono.size()==p.takes()[0]->mono.size(),"Simultaneous takes are not sample-aligned");
    }
    const auto bank=p.takes();
    check(p.startRecordingAll(),"Cannot start parallel cancellation test");juce::AudioBuffer<T> block(8,512);block.clear();hostProcess(p,block,midi);p.cancelRecording();finished(p);
    check(p.takes()==bank,"Cancelling parallel capture replaced previous takes");
    // Single-slot recording uses that slot's own pair on an eight-input instance.
    check(p.startRecording(3),"Cannot record pair 7/8 independently");
    for(int c=0;c<8;++c) for(int i=0;i<512;++i) block.setSample(channel[static_cast<size_t>(c)],i,static_cast<T>((c+1)*.1));
    hostProcess(p,block,midi);p.finishRecording();finished(p);
    std::vector<float> mono(512,static_cast<float>((double(static_cast<float>(T(.7)))+double(static_cast<float>(T(.8))))*.5));
    check(p.takes()[3]->mono==choir::resample(mono,rate,rate),"Single-slot capture did not use pair 7/8");
    for(size_t pair=0;pair<3;++pair) check(p.takes()[pair]==bank[pair],"Single-slot capture changed another pair");
    auto buses=p.getBusesLayout();buses.inputBuses.set(0,juce::AudioChannelSet::stereo());check(p.setBusesLayout(buses),"Legacy stereo input rejected");
    check(!p.startRecordingAll(),"Record all armed without eight inputs");
}

int main() try {
    juce::ScopedJuceInitialiser_GUI init;
    fourPairs<float>(48000);fourPairs<double>(44100);fourPairs<float>(48000,true);
    concurrentStops();
    capturedRate<float>(44100); capturedRate<float>(48000); capturedRate<double>(96000); capturedRate<double>(192000);
    ChoirProcessor p; stereo(p); p.prepareToPlay(48000,512); p.loadDemo(); const auto old=p.takes()[0];
    check(p.getTotalNumInputChannels()==2,"Default Reaper input is not stereo");
    juce::AudioBuffer<float> audio(2,512); juce::MidiBuffer midi;
    for(int c=0;c<2;++c) for(int i=0;i<512;++i) audio.setSample(c,i,c==0?.25f:.5f);
    set(p,"inputMonitor",1); hostProcess(p,audio,midi);
    check(audio.getSample(0,100)==.25f && audio.getSample(1,100)==.5f,"Idle input monitoring changed samples");
    set(p,"inputMonitor",0); check(p.startRecording(0),"Could not start cancellation test");
    hostProcess(p,audio,midi); p.cancelRecording(); finished(p); check(p.takes()[0]==old,"Cancel destroyed previous take");
    check(p.startRecording(0),"Could not start empty recording"); p.finishRecording(); finished(p);
    check(p.takes()[0]==old,"Empty recording destroyed previous take");
    check(p.startRecording(0),"Could not start rate-change test"); audio.clear(); hostProcess(p,audio,midi); p.prepareToPlay(96000,512); finished(p);
    check(p.takes()[0]->mono.size()==512,"Host rate change changed the captured take's rate");
    p.prepareToPlay(48000,512); check(p.startRecording(0),"Could not start bypass test"); audio.clear(); hostProcess(p,audio,midi);
    for(int c=0;c<2;++c) for(int i=0;i<512;++i) audio.setSample(c,i,.25f);
    p.processBlockBypassed(audio,midi); finished(p); check(p.takes()[0]->mono.size()==512 && audio.getSample(0,0)==.25f,"Bypass did not commit capture and pass input");
    // Backward-compatible no-input layout, and mono sends on a stereo-output track.
    auto layout=p.getBusesLayout(); layout.inputBuses.set(0,juce::AudioChannelSet::disabled());
    check(p.setBusesLayout(layout),"Legacy no-input layout rejected"); check(!p.startRecording(1),"Recording armed with no input bus");
    layout.inputBuses.set(0,juce::AudioChannelSet::mono()); check(p.setBusesLayout(layout),"Mono input rejected");
    check(p.startRecording(1),"Mono recording refused"); set(p,"inputMonitor",1);
    for(int i=0;i<512;++i) {audio.setSample(0,i,.2f);audio.setSample(1,i,.9f);} hostProcess(p,audio,midi);
    check(audio.getSample(1,0)==.2f,"Mono monitoring did not duplicate its input"); p.releaseResources(); finished(p);
    check(p.takes()[1]->mono.size()==512 && p.takes()[1]->mono[100]==.2f,"Mono capture read the unused second input");
    // Old sessions reset the newly-added monitoring parameter to its default off.
    juce::MemoryBlock state; p.getStateInformation(state);
    auto legacy=juce::ValueTree::readFromData(state.getData(),state.getSize()); legacy.removeChild(legacy.getChildWithProperty("id","inputMonitor"),nullptr);
    juce::MemoryBlock legacyData; {juce::MemoryOutputStream output(legacyData,false);legacy.writeToStream(output);}
    p.setStateInformation(legacyData.getData(),static_cast<int>(legacyData.getSize())); rendered(p);
    check(p.state.getRawParameterValue("inputMonitor")->load()==0,"Legacy session enabled live monitoring");
    // Four full 120-second takes share one auto-stop and the larger state bound.
    ChoirProcessor longTakes; longTakes.prepareToPlay(48000,16384);
    juce::AudioBuffer<double> chunk(8,16384);
    check(longTakes.startRecordingAll(),"Could not start two-minute parallel recording");
    size_t sent=0;const size_t limit=static_cast<size_t>(choir::sampleRate*choir::maxSeconds);
    while(longTakes.isArmed() || longTakes.isRecording()) {
        const int n=static_cast<int>(std::min<size_t>(16384,limit+17-sent));chunk.setSize(8,n,false,false,true);
        for(int c=0;c<8;++c) for(int i=0;i<n;++i) chunk.setSample(c,i,.125*(c/2+1));
        hostProcess(longTakes,chunk,midi);sent+=static_cast<size_t>(n);
    }
    finished(longTakes);
    for(size_t slot=0;slot<4;++slot) {
        check(longTakes.recordingSeconds()==120. && longTakes.takes()[slot]->mono.size()==limit,"Shared auto-stop did not preserve exactly 120 seconds in every take");
        check(longTakes.takes()[slot]->mono.front()==.125f*static_cast<float>(slot+1) && longTakes.takes()[slot]->mono.back()==.125f*static_cast<float>(slot+1),"Long parallel recording was truncated or crossed pairs");
    }
    set(longTakes,"singers",4);set(longTakes,"detune",0);set(longTakes,"timing",0);set(longTakes,"room",0);
    longTakes.getStateInformation(state); check(state.getSize()>32*1024*1024,"Long-session fixture did not exceed the previous limit");
    ChoirProcessor restored; restored.setStateInformation(state.getData(),static_cast<int>(state.getSize())); rendered(restored);
    for(size_t slot=0;slot<4;++slot) check(restored.takes()[slot]->mono==longTakes.takes()[slot]->mono,"Long parallel recordings did not survive session recall");
    check(restored.rendered()->left.size()==static_cast<size_t>(48000*120),"Two-minute choir duration changed");
    const auto dir=juce::File::getCurrentWorkingDirectory().getChildFile("previews"); dir.createDirectory();
    const auto wav=dir.getChildFile("two-minute-test.wav"); check(restored.exportWav(wav),"Two-minute WAV export failed");
    juce::AudioFormatManager fm; fm.registerBasicFormats(); std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
    check(reader && reader->lengthInSamples==48000*120 && reader->numChannels==2,"Long export header is incorrect");reader.reset();wav.deleteFile();
    // Native recording controls and an actual screenshot of the armed input path.
    ChoirProcessor ui; ui.prepareToPlay(48000,512); ui.loadDemo();
    std::unique_ptr<juce::AudioProcessorEditor> editor(ui.createEditor());
    auto* record=dynamic_cast<juce::Button*>(find(*editor,"recordAll")); check(record && record->onClick,"Record all control missing");record->onClick();
    check(ui.isArmed() && !ui.isRecording() && ui.recordingSlot()==-2,"Record all did not arm all four slots");
    juce::AudioBuffer<float> pairs(8,512);
    for(int block=0;block<50;++block) {for(int c=0;c<8;++c) for(int i=0;i<512;++i) pairs.setSample(c,i,.4f*std::sin((block*512+i)*(.04f+c*.005f)));hostProcess(ui,pairs,midi);}
    for(int i=0;i<40;++i) pump();
    bool inputVisible=false;
    for(auto* child:editor->getChildComponent(0)->getChildren()) if(auto* meter=dynamic_cast<OutputMeter*>(child)) inputVisible=inputVisible || meter->getLevelDb()>-30.f;
    check(inputVisible,"Recording input meters did not show the routed signal");
    screenshot(*editor,dir.getChildFile("ui-recording.png"));
    auto* cancel=dynamic_cast<juce::Button*>(find(*editor,"cancelCapture")); check(cancel && cancel->onClick,"Cancel capture control missing");cancel->onClick();finished(ui);
    check(!ui.takes()[2] && ui.takes()[0],"UI cancellation modified the take bank");
    std::cout<<"Reaper-style mono/stereo inputs, float/double capture, host rates, monitor, cancellation, bypass, 120-second limit, large-state recall, long WAV export and recording UI passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
