#include "../Source/PluginProcessor.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
void pump() {
    juce::Timer::callPendingTimersSynchronously();
#if JUCE_MODAL_LOOPS_PERMITTED
    juce::MessageManager::getInstance()->runDispatchLoopUntil(3);
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
void wait(ChoirProcessor& p) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(!p.current() || p.busy.load()) { if(std::chrono::steady_clock::now()>end) throw std::runtime_error("Render timeout: "+p.status().toStdString()); pump(); }
    for(int i=0;i<12;++i) pump();
}
juce::Component* find(juce::Component& parent,const juce::String& id) {
    if(parent.getComponentID()==id) return &parent;
    for(auto* c:parent.getChildren()) if(auto* found=find(*c,id)) return found;
    return nullptr;
}
void set(ChoirProcessor& p,const char* id,float plain) {
    auto* param=p.state.getParameter(id); check(param!=nullptr,"Missing parameter"); param->setValueNotifyingHost(param->convertTo0to1(plain));
}
void writeMono(const juce::File& file,const std::vector<float>& data,double rate) {
    juce::WavAudioFormat wav; auto stream=file.createOutputStream(); check(bool(stream),"Cannot create fixture");
    stream->setPosition(0); stream->truncate();
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(),rate,1,24,{},0)); check(bool(writer),"Cannot write fixture"); stream.release();
    const float* samples[]{data.data()}; check(writer->writeFromFloatArrays(samples,1,static_cast<int>(data.size())),"Fixture write failed");
}
int main() try {
    juce::ScopedJuceInitialiser_GUI init;
    ChoirProcessor p; p.prepareToPlay(48000,256);
    juce::AudioBuffer<float> b(2,256); juce::MidiBuffer midi;
    b.applyGain(0); p.processBlock(b,midi); check(b.getMagnitude(0,0,256)==0,"Empty plugin makes sound");
    check(p.isBusesLayoutSupported(p.getBusesLayout()),"Own layout rejected");
    check(p.settings().singers==12 && p.settings().detune==9,"Wrong defaults");
    const auto out=juce::File::getCurrentWorkingDirectory().getChildFile("previews"); out.createDirectory();
    std::vector<float> fixture(44100/2); for(size_t i=0;i<fixture.size();++i) fixture[i]=static_cast<float>(.2*std::sin(2*3.141592653589793*440*double(i)/44100.));
    const auto inputFile=out.getChildFile("import-44100.wav"); writeMono(inputFile,fixture,44100);
    check(p.loadTake(inputFile,0),"WAV import failed"); check(p.takes()[0]->mono.size()==24000,"Import changed duration");
    const auto old=p.takes()[0]; check(!p.loadTake(out.getChildFile("missing.wav"),0) && p.takes()[0]==old,"Failed import destroyed a take");
    check(!p.loadTake(inputFile,4),"Invalid slot accepted");
    std::vector<float> longAudio(48000*(choir::maxSeconds+1)); const auto tooLong=out.getChildFile("too-long.wav"); writeMono(tooLong,longAudio,48000);
    check(!p.loadTake(tooLong,0) && p.takes()[0]==old,"Oversize import was accepted or replaced the source"); tooLong.deleteFile();
    p.loadDemo(); p.requestRender(); wait(p); const auto clip=p.rendered(); check(bool(clip),"Missing render");
    // Playback is sample-exact at 48 kHz, and follows MIDI sample offsets.
    p.prepareToPlay(48000,256); p.play(); p.processBlock(b,midi);
    for(int i=0;i<256;++i) check(b.getSample(0,i)==clip->left[static_cast<size_t>(i)],"Preview differs from render");
    p.stop(); p.processBlock(b,midi); check(b.getMagnitude(0,0,256)==0,"Stop failed");
    midi.addEvent(juce::MidiMessage::noteOn(1,60,(juce::uint8)100),17); p.processBlock(b,midi);
    for(int i=0;i<256;++i) check(b.getSample(0,i)==(i<17?0.f:clip->left[static_cast<size_t>(i-17)]),"MIDI trigger not sample accurate");
    p.stop(); set(p,"original",1); p.play(); p.processBlock(b,midi);
    for(int i=0;i<256;++i) check(b.getSample(0,i)==clip->dry[static_cast<size_t>(i)] && b.getSample(1,i)==b.getSample(0,i),"Original preview wrong");
    set(p,"original",0); set(p,"sampleMode",1);set(p,"outputMode",1);p.prepareToPlay(44100,256); p.play(); juce::AudioBuffer<double> d(2,256); p.processBlock(d,midi);
    for(int i=0;i<256;++i) {
        const double pos=i*48000./44100.; const auto at=static_cast<size_t>(pos); const double f=pos-double(at);
        check(std::abs(d.getSample(0,i)-(clip->left[at]*(1-f)+clip->left[at+1]*f))<1.e-12,"Host-rate playback changed duration/pitch");
    }
    p.stop(); check(p.exportWav(out.getChildFile("demo-choir.wav")),"Export failed");
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(out.getChildFile("demo-choir.wav")));
    check(reader && reader->sampleRate==48000 && reader->numChannels==2 && reader->bitsPerSample==24 && reader->lengthInSamples==static_cast<juce::int64>(clip->left.size()),"Export format/length wrong");
    juce::AudioBuffer<float> decoded(2,static_cast<int>(clip->left.size())); check(reader->read(&decoded,0,decoded.getNumSamples(),0,true,true),"Export decode failed");
    for(int i=0;i<decoded.getNumSamples();++i) check(std::abs(decoded.getSample(0,i)-clip->left[static_cast<size_t>(i)])<2.e-7f,"Export differs from preview");
    writeMono(out.getChildFile("demo-source.wav"),p.takes()[0]->mono,48000);
    set(p,"singers",16); check(!p.current() && !p.exportWav(out.getChildFile("stale.wav")),"Stale render exported");
    p.requestRender(); wait(p); check(p.rendered()->settings.singers==16,"Controls not applied");
    p.uiSize=3; juce::MemoryBlock state; p.getStateInformation(state);
    ChoirProcessor restored; restored.setStateInformation(state.getData(),static_cast<int>(state.getSize())); wait(restored);
    check(restored.uiSize.load()==3 && restored.settings().singers==16,"Session lost controls or UI size");
    check(restored.takes()[0]->mono==p.takes()[0]->mono && restored.rendered()->left==p.rendered()->left,"Embedded audio/session render not portable");
    p.clearTake(0); check(bool(restored.takes()[0]),"Restored session shares mutable source bank");
    const char bad[]{1,2,3}; restored.setStateInformation(bad,3); check(restored.settings().singers==16,"Invalid state changed settings");
    // Actual native editor, control bindings, reset and all interface sizes.
    p.loadDemo(); for(const auto& id:{"singers","detune","timing","width","room","output","seed"}) { auto* param=p.state.getParameter(id); param->setValueNotifyingHost(param->getDefaultValue()); }
    p.uiSize=2; p.requestRender(); wait(p);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor()); check(editor->getWidth()==1100 && editor->getHeight()==820,"Wrong editor dimensions");
    auto* dial=dynamic_cast<juce::Slider*>(find(*editor,"detune")); check(dial!=nullptr,"Missing pitch control");
    dial->setValue(13,juce::sendNotificationSync); check(p.settings().detune==13 && !p.current(),"Knob not attached");
    auto* reset=dynamic_cast<juce::Button*>(find(*editor,"reset")); check(reset && reset->onClick,"Reset missing"); reset->onClick(); check(p.settings().detune==9,"Reset failed");
    check(p.current(),"Reset to rendered defaults unnecessarily invalidates render");
    for(int i=0;i<15;++i) pump();
    auto* sizes=dynamic_cast<juce::ComboBox*>(find(*editor,"uiSize")); check(sizes!=nullptr,"Size control missing");
    for(int size:{1,2,3}) {
        sizes->setSelectedId(size,juce::sendNotificationSync); const double scale=size==1?.75:size==3?1.25:1.;
        check(editor->getWidth()==juce::roundToInt(1100*scale) && editor->getHeight()==juce::roundToInt(820*scale),"Scaling failed");
        auto image=editor->createComponentSnapshot(editor->getLocalBounds(),true); auto stream=out.getChildFile("ui-"+juce::String(size)+".png").createOutputStream(); check(bool(stream),"Screenshot failed");
        stream->setPosition(0); stream->truncate(); check(juce::PNGImageFormat().writeImageToStream(image,*stream),"PNG encode failed");
    }
    // A clear during an in-flight render must cancel it and prevent publishing stale audio.
    p.requestRender(); p.clearTake(0); p.clearTake(1);
    for(int i=0;i<100;++i) pump(); check(!p.current() && !p.busy.load(),"Source edit did not cancel rendering");
    std::cout<<"Import, limits, render, MIDI offsets, original/stereo playback, host rates, WAV export, embedded recall, stale-result protection, UI and three previews passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
