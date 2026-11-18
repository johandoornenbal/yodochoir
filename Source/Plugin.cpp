#include "PluginProcessor.h"
#include "Look.h"
#include "OutputMeter.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr std::array<const char*,7> ids{"singers","detune","timing","width","room","output","seed"};
constexpr std::array<const char*,6> names{"Singers","Pitch spread","Timing spread","Stereo width","Room","Output"};
constexpr std::array<float,6> defaults{12,9,25,85,18,-3};
}
juce::AudioProcessorValueTreeState::ParameterLayout ChoirProcessor::layout() {
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    p.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"singers",1},"Singers",4,32,12));
    const std::array<float,5> low{0,0,0,0,-24},high{25,60,100,50,0};
    for(size_t i=1;i<6;++i) p.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ids[i],1},names[i],juce::NormalisableRange<float>(low[i-1],high[i-1],.1f),defaults[i]));
    p.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"seed",1},"Variation seed",0,99999,1977));
    p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"original",1},"Original first take",false));
    p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"loop",1},"Loop preview",false));
    p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"inputMonitor",1},"Monitor routed input",false));
    return p;
}
ChoirProcessor::ChoirProcessor():AudioProcessor(BusesProperties().withInput("Record input",juce::AudioChannelSet::stereo(),true).withOutput("Choir",juce::AudioChannelSet::stereo(),true)),
    Thread("YodoChoir renderer"),state(*this,nullptr,"YodoChoirState",layout()) {
    formats.registerBasicFormats();
    for(size_t i=0;i<ids.size();++i) values[i]=state.getRawParameterValue(ids[i]);
    original=state.getRawParameterValue("original"); loop=state.getRawParameterValue("loop"); monitor=state.getRawParameterValue("inputMonitor");
    startThread(juce::Thread::Priority::low);
}
ChoirProcessor::~ChoirProcessor() { recording=false; captureSerial.fetch_add(1); signalThreadShouldExit(); requestSerial.fetch_add(1); notify(); stopThread(-1); }
choir::Settings ChoirProcessor::settings() const {
    return {juce::roundToInt(values[0]->load()),values[1]->load(),values[2]->load(),values[3]->load()*.01f,
            values[4]->load()*.01f,values[5]->load(),juce::roundToInt(values[6]->load())};
}
void ChoirProcessor::message(juce::String s) { const std::lock_guard<std::mutex> lock(statusMutex); messageText=std::move(s); }
juce::String ChoirProcessor::status() const { const std::lock_guard<std::mutex> lock(statusMutex); return messageText; }
std::array<std::shared_ptr<const choir::Take>,4> ChoirProcessor::takes() const { const std::lock_guard<std::mutex> lock(sourceMutex); return sourceBank; }
void ChoirProcessor::sourcesChanged() { sourceRevision.fetch_add(1); cancelRender(); stop(); }
bool ChoirProcessor::loadTake(const juce::File& file,int slot) {
    if(captureActive()) { message("Finish or cancel recording before changing source takes."); return false; }
    if(slot<0 || slot>=4) return false;
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if(!reader) { message("Cannot read this file. Use WAV, AIFF or FLAC."); return false; }
    if(reader->numChannels<1 || reader->numChannels>2 || !std::isfinite(reader->sampleRate) || reader->sampleRate<8000 || reader->sampleRate>192000
       || reader->lengthInSamples<2 || double(reader->lengthInSamples)/reader->sampleRate>choir::maxSeconds) {
        message("Use a mono/stereo take up to 120 seconds, at 8-192 kHz."); return false;
    }
    try {
        juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels),static_cast<int>(reader->lengthInSamples));
        if(!reader->read(&buffer,0,buffer.getNumSamples(),0,true,true)) { message("The audio file could not be decoded."); return false; }
        std::vector<float> mono(static_cast<size_t>(buffer.getNumSamples()));
        for(int i=0;i<buffer.getNumSamples();++i) {
            double value=0; for(int c=0;c<buffer.getNumChannels();++c) value+=buffer.getSample(c,i)/buffer.getNumChannels();
            mono[static_cast<size_t>(i)]=std::isfinite(value)?static_cast<float>(std::clamp(value,-32.,32.)):0.f;
        }
        auto take=std::make_shared<choir::Take>(); take->name=file.getFileName().toStdString(); take->mono=choir::resample(mono,reader->sampleRate); take->preview=choir::waveform(take->mono);
        { const std::lock_guard<std::mutex> lock(sourceMutex); sourceBank[static_cast<size_t>(slot)]=std::move(take); sourcesChanged(); }
        message("Take loaded. Align matching phrases at their starts, then Render choir.");
        return true;
    } catch(const std::exception& e) { message(juce::String("Import failed: ")+e.what()); return false; }
}
void ChoirProcessor::clearTake(int slot) {
    if(captureActive()) return;
    if(slot<0 || slot>=4) return;
    { const std::lock_guard<std::mutex> lock(sourceMutex); sourceBank[static_cast<size_t>(slot)].reset(); sourcesChanged(); }
    message("Take cleared. Render again to update the choir.");
}
void ChoirProcessor::loadDemo() {
    if(captureActive()) return;
    auto a=choir::demo(0),b=choir::demo(1);
    { const std::lock_guard<std::mutex> lock(sourceMutex); sourceBank={a,b,nullptr,nullptr}; sourcesChanged(); }
    message("Synthetic vowel demo loaded. Render choir to hear the ensemble.");
}
void ChoirProcessor::requestRender() {
    if(captureActive()) { message("Finish or cancel recording before rendering the choir."); return; }
    const auto bank=takes(); if(std::none_of(bank.begin(),bank.end(),[](const auto& t){return bool(t);})) { message("Load at least one vocal take first."); return; }
    stop(); queuedSerial=requestSerial.fetch_add(1)+1; pending=true; message("Queued for rendering..."); notify();
}
void ChoirProcessor::cancelRender() { requestSerial.fetch_add(1); pending=false; message("Render cancelled."); }
bool ChoirProcessor::startRecording(int slot) {
    if(slot<0 || slot>=4) return false;
    if(captureActive()) { message("Finish the current recording first."); return false; }
    const double rate=hostRate.load();
    if(getTotalNumInputChannels()<1) { message("Enable the plugin's mono or stereo input bus to record Reaper sends."); return false; }
    if(!std::isfinite(rate) || rate<8000 || rate>192000) { message("Record at a host sample rate between 8 and 192 kHz."); return false; }
    try {
        auto ready=std::make_unique<Capture>();
        ready->mono.resize(static_cast<size_t>(std::llround(rate*choir::maxSeconds)),0.f);
        ready->rate=rate; ready->slot=slot;
        bool accepted=false;
        {
            const juce::SpinLock::ScopedLockType lock(captureLock);
            if(!capture && !captureBusy.load() && !recording.load()) {
                ready->serial=captureSerial.fetch_add(1)+1;
                capture=std::move(ready); captureSeconds=0; captureSlot=slot;
                captureInterrupted=false; captureAtLimit=false; inputPeaks[0]=0; inputPeaks[1]=0; accepted=true;
            }
        }
        if(!accepted) { message("The previous recording is still being prepared."); return false; }
        stop(); cancelRender(); recording=true;
        message("Recording routed input into take "+juce::String(slot+1)+" (maximum 2:00).");
        return true;
    } catch(const std::exception&) { message("Not enough memory to start a two-minute recording. Previous take preserved."); return false; }
}
void ChoirProcessor::finishRecording() {
    if(recording.exchange(false)) { captureBusy=true; capturePending=true; }
    // The worker polls this flag; this path is safe to call from the audio callback.
}
void ChoirProcessor::cancelRecording() {
    captureSerial.fetch_add(1);
    if(recording.exchange(false)) { captureBusy=true; capturePending=true; }
    message("Recording cancelled. Previous take preserved.");
}
template<class T> void ChoirProcessor::captureInput(const juce::AudioBuffer<T>& b) {
    if(!recording.load()) return;
    const juce::SpinLock::ScopedTryLockType lock(captureLock);
    if(!lock.isLocked()) { if(recording.load()) { captureInterrupted=true; finishRecording(); } return; }
    if(!recording.load() || !capture) return;
    if(capture->rate!=hostRate.load() || capture->serial!=captureSerial.load()) { finishRecording(); return; }
    const int channels=std::min(getTotalNumInputChannels(),b.getNumChannels());
    if(channels<1) { captureInterrupted=true; finishRecording(); return; }
    const size_t count=std::min(static_cast<size_t>(b.getNumSamples()),capture->mono.size()-capture->frames);
    std::array<float,2> peak{};
    for(size_t i=0;i<count;++i) {
        double sum=0;
        for(int c=0;c<channels;++c) {
            const double sample=static_cast<double>(b.getSample(c,static_cast<int>(i)));
            const float value=std::isfinite(sample)?static_cast<float>(std::clamp(sample,-32.,32.)):0.f;
            sum+=value; peak[static_cast<size_t>(std::min(c,1))]=std::max(peak[static_cast<size_t>(std::min(c,1))],std::abs(value));
        }
        capture->mono[capture->frames+i]=static_cast<float>(sum/channels);
    }
    if(channels==1) peak[1]=peak[0];
    for(size_t c=0;c<2;++c) { auto old=inputPeaks[c].load(); while(old<peak[c] && !inputPeaks[c].compare_exchange_weak(old,peak[c])) {} }
    capture->frames+=count; captureSeconds=double(capture->frames)/capture->rate;
    if(capture->frames==capture->mono.size()) { captureAtLimit=true; finishRecording(); }
}
void ChoirProcessor::finalizeCapture() {
    std::unique_ptr<Capture> completed;
    { const juce::SpinLock::ScopedLockType lock(captureLock); completed=std::move(capture); }
    if(completed && completed->serial==captureSerial.load() && !threadShouldExit()) {
        try {
            if(captureInterrupted.load()) message("Input recording was interrupted. Previous take preserved.");
            else if(completed->frames<2) message("No input samples were received. Start Reaper playback and record again. Previous take preserved.");
            else {
                completed->mono.resize(completed->frames);
                auto take=std::make_shared<choir::Take>();
                take->name="Recorded take "+std::to_string(completed->slot+1);
                take->mono=choir::resample(completed->mono,completed->rate);
                take->preview=choir::waveform(take->mono);
                bool committed=false;
                {
                    const std::lock_guard<std::mutex> lock(sourceMutex);
                    if(completed->serial==captureSerial.load() && !threadShouldExit()) {
                        sourceBank[static_cast<size_t>(completed->slot)]=std::move(take); sourcesChanged(); committed=true;
                    }
                }
                if(committed) message((captureAtLimit.load()?"Two-minute limit reached. ":juce::String{})+"Recorded take "+juce::String(completed->slot+1)+" saved. Render choir when ready.");
            }
        } catch(const std::exception& e) { message(juce::String("Recording could not be prepared. Previous take preserved: ")+e.what()); }
    }
    captureSlot=-1;
    captureBusy=false;
}

void ChoirProcessor::run() {
    while(!threadShouldExit()) {
        if(capturePending.exchange(false)) { finalizeCapture(); continue; }
        if(!pending.exchange(false)) { wait(25); continue; }
        const auto serial=queuedSerial.load(); const auto config=settings();
        uint64_t revision=0; std::array<std::shared_ptr<const choir::Take>,4> bank;
        { const std::lock_guard<std::mutex> lock(sourceMutex); bank=sourceBank; revision=sourceRevision.load(); }
        if(serial!=requestSerial.load()) continue;
        busy=true; progress=0; message("Building independent singers...");
        try {
            auto result=choir::render(bank,config,[this,serial] { return threadShouldExit() || requestSerial.load()!=serial; },[this](float p){progress=p;});
            if(result && requestSerial.load()==serial && sourceRevision.load()==revision) {
                if(config==settings()) {
                    result->revision=revision;
                    std::shared_ptr<const choir::Render> ready(std::move(result)),previous;
                    { const juce::SpinLock::ScopedLockType lock(renderLock); previous=std::move(clip); clip=std::move(ready); playing=false; rewind=true; position=0; }
                    message("Choir ready. Preview it, compare Original, or export a stereo WAV.");
                } else message("Settings changed during rendering. Render choir again.");
            }
        } catch(const std::exception& e) { message(juce::String("Render failed: ")+e.what()); }
        busy=false;
    }
}
std::shared_ptr<const choir::Render> ChoirProcessor::rendered() const { const juce::SpinLock::ScopedLockType lock(renderLock); return clip; }
bool ChoirProcessor::current() const { const auto r=rendered(); return r && r->revision==sourceRevision.load() && r->settings==settings(); }
void ChoirProcessor::play() { if(captureActive()) { message("Finish or cancel recording before previewing the choir."); return; } if(current()) { rewind=true; playing=true; } else message("Render choir with the current takes and settings first."); }
void ChoirProcessor::stop() { playing=false; rewind=true; position=0; }
void ChoirProcessor::prepareToPlay(double rate,int) {
    finishRecording(); hostRate=std::isfinite(rate) && rate>0?rate:48000.; stop(); peaks[0]=0; peaks[1]=0;
}
template<class T> void ChoirProcessor::process(juce::AudioBuffer<T>& b,juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals guard;
    captureInput(b); // Read the Reaper sends before clearing/replacing any output.
    const int inputs=getTotalNumInputChannels();
    if(monitor->load()<.5f || inputs==0) b.clear();
    else if(inputs==1 && b.getNumChannels()>1) b.copyFrom(1,0,b,0,0,b.getNumSamples());
    if(monitor->load()>.5f && inputs>0 && !playing.load()) {
        for(size_t c=0;c<2 && c<static_cast<size_t>(b.getNumChannels());++c) {
            const float peak=static_cast<float>(b.getMagnitude(static_cast<int>(c),0,b.getNumSamples()));
            auto old=peaks[c].load(); while(old<peak && !peaks[c].compare_exchange_weak(old,peak)) {}
        }
    }
    if(captureActive()) { playing=false; midi.clear(); return; }
    const juce::SpinLock::ScopedTryLockType lock(renderLock);
    if(!lock.isLocked() || !clip || b.getNumChannels()<2) { midi.clear(); return; }
    if(clip->revision!=sourceRevision.load() || !(clip->settings==settings())) { playing=false; midi.clear(); return; }
    if(rewind.exchange(false)) cursor=0;
    const auto count=clip->left.size(); const double step=choir::sampleRate/hostRate.load();
    std::array<float,2> peak{};
    const auto renderSegment=[&](int start,int end) {
        for(int i=start;i<end;++i) {
            if(!playing.load()) break;
            if(cursor>=double(count-1)) {
                if(loop->load()>.5f) cursor=std::fmod(cursor,double(count));
                else { playing=false; position=0; break; }
            }
            const auto at=std::min(count-1,static_cast<size_t>(cursor)),next=std::min(count-1,at+1); const double fraction=cursor-double(at);
            const bool dry=original->load()>.5f;
            for(size_t c=0;c<2;++c) {
                const auto& samples=dry?clip->dry:c==0?clip->left:clip->right;
                const T value=static_cast<T>(samples[at]*(1.-fraction)+samples[next]*fraction);
                b.setSample(static_cast<int>(c),i,value); peak[c]=std::max(peak[c],std::abs(static_cast<float>(value)));
            }
            cursor+=step;
        }
    };
    int start=0;
    for(const auto event:midi) {
        const int offset=juce::jlimit(start,b.getNumSamples(),event.samplePosition); renderSegment(start,offset); start=offset;
        if(event.numBytes<=3) {
            const auto m=event.getMessage();
            if(m.isNoteOn()) { cursor=0; playing=true; }
            else if(m.isAllNotesOff() || m.isAllSoundOff()) { playing=false; cursor=0; }
        }
    }
    renderSegment(start,b.getNumSamples()); midi.clear();
    if(playing.load()) position=static_cast<float>(cursor/double(count));
    for(size_t c=0;c<2;++c) { auto old=peaks[c].load(); while(old<peak[c] && !peaks[c].compare_exchange_weak(old,peak[c])) {} }
}
void ChoirProcessor::processBlock(juce::AudioBuffer<float>& b,juce::MidiBuffer& m) { process(b,m); }
void ChoirProcessor::processBlock(juce::AudioBuffer<double>& b,juce::MidiBuffer& m) { process(b,m); }
bool ChoirProcessor::exportWav(const juce::File& file) {
    if(captureActive()) { message("Finish recording before exporting the choir."); return false; }
    const auto r=rendered(); if(!r || r->revision!=sourceRevision.load() || !(r->settings==settings())) { message("Render the current settings before exporting."); return false; }
    juce::TemporaryFile temp(file);
    std::unique_ptr<juce::FileOutputStream> stream(temp.getFile().createOutputStream());
    if(!stream) { message("Cannot create the output file."); return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(),choir::sampleRate,2,24,{},0));
    if(!writer) { message("Cannot create the WAV writer."); return false; }
    stream.release();
    const float* audio[]{r->left.data(),r->right.data()};
    if(!writer->writeFromFloatArrays(audio,2,static_cast<int>(r->left.size()))) { message("WAV write failed. Previous file preserved."); return false; }
    writer.reset();
    if(!temp.overwriteTargetFileWithTemporary()) { message("Cannot replace the destination. Previous file preserved."); return false; }
    message("Exported stereo 48 kHz / 24-bit WAV: "+file.getFileName()); return true;
}
void ChoirProcessor::getStateInformation(juce::MemoryBlock& data) {
    auto saved=state.copyState(); saved.setProperty("schema",1,nullptr); saved.setProperty("uiSize",uiSize.load(),nullptr);
    const auto bank=takes();
    for(size_t i=0;i<bank.size();++i) if(bank[i]) {
        juce::ValueTree take{"TAKE"}; take.setProperty("slot",static_cast<int>(i),nullptr); take.setProperty("name",juce::String(bank[i]->name),nullptr);
        juce::MemoryOutputStream pcm;
        for(float v:bank[i]->mono) pcm.writeFloat(v);
        take.setProperty("pcm",juce::var(pcm.getMemoryBlock()),nullptr); saved.appendChild(take,nullptr);
    }
    juce::MemoryOutputStream out(data,false); saved.writeToStream(out);
}
void ChoirProcessor::setStateInformation(const void* bytes,int size) {
    if(!bytes || size<=0 || size>128*1024*1024) return;
    auto saved=juce::ValueTree::readFromData(bytes,static_cast<size_t>(size));
    if(!saved.isValid() || !saved.hasType("YodoChoirState") || static_cast<int>(saved.getProperty("schema",0))!=1) return;
    std::array<std::shared_ptr<const choir::Take>,4> bank{};
    for(int i=saved.getNumChildren();--i>=0;) {
        auto node=saved.getChild(i); if(!node.hasType("TAKE")) continue;
        const int slot=static_cast<int>(node.getProperty("slot",-1)); const auto* pcm=node.getProperty("pcm").getBinaryData();
        if(slot<0 || slot>=4 || bank[static_cast<size_t>(slot)] || !pcm || pcm->getSize()<8 || pcm->getSize()%4!=0
           || pcm->getSize()>static_cast<size_t>(choir::sampleRate*choir::maxSeconds*4)) return;
        auto take=std::make_shared<choir::Take>(); take->name=node.getProperty("name").toString().substring(0,256).toStdString();
        juce::MemoryInputStream input(*pcm,false); take->mono.resize(pcm->getSize()/4);
        for(auto& v:take->mono) { v=input.readFloat(); if(!std::isfinite(v) || std::abs(v)>32.f) return; }
        take->preview=choir::waveform(take->mono);
        bank[static_cast<size_t>(slot)]=std::move(take); saved.removeChild(i,nullptr);
    }
    // Reject invalid render controls before they can start a background job.
    for(auto* raw:getParameters()) if(auto* p=dynamic_cast<juce::RangedAudioParameter*>(raw)) {
        const auto child=saved.getChildWithProperty("id",p->paramID);
        if(child.isValid() && !std::isfinite(static_cast<float>(child.getProperty("value")))) return;
        if(!child.isValid()) {
            juce::ValueTree missing{"PARAM"}; missing.setProperty("id",p->paramID,nullptr);
            missing.setProperty("value",p->convertFrom0to1(p->getDefaultValue()),nullptr); saved.appendChild(missing,nullptr);
        }
    }
    cancelRecording();
    state.replaceState(saved); uiSize=juce::jlimit(1,3,static_cast<int>(saved.getProperty("uiSize",2)));
    { const std::lock_guard<std::mutex> lock(sourceMutex); sourceBank=std::move(bank); sourcesChanged(); }
    message("Session takes restored. Rebuilding choir..."); requestRender();
}

class ChoirPanel final : public juce::Component,public juce::FileDragAndDropTarget,private juce::Timer {
public:
    explicit ChoirPanel(ChoirProcessor& p):processor(p) {
        setLookAndFeel(&look);
        for(size_t i=0;i<4;++i) {
            addAndMakeVisible(load[i]); addAndMakeVisible(clear[i]); addAndMakeVisible(recordButtons[i]);
            load[i].setComponentID("load"+juce::String(static_cast<int>(i))); clear[i].setComponentID("clear"+juce::String(static_cast<int>(i)));
            recordButtons[i].setComponentID("record"+juce::String(static_cast<int>(i)));
            recordButtons[i].setButtonText("REC"); recordButtons[i].setColour(juce::TextButton::buttonColourId,juce::Colour(0xff6b3430));
            recordButtons[i].setColour(juce::TextButton::textColourOffId,style::cream);
            recordButtons[i].setTooltip("Record routed input into this slot, up to two minutes. Stop commits the recording; Cancel capture keeps the previous take.");
            recordButtons[i].onClick=[this,i] { if(processor.isRecording()) processor.finishRecording(); else processor.startRecording(static_cast<int>(i)); refresh(); };
            load[i].onClick=[this,i]{chooseTake(static_cast<int>(i));};
            clear[i].setButtonText("x"); clear[i].onClick=[this,i]{processor.clearTake(static_cast<int>(i));};
        }
        for(size_t i=0;i<6;++i) {
            auto& knob=knobs[i]; addAndMakeVisible(knob); knob.setName(names[i]); knob.setComponentID(ids[i]);
            knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setRotaryParameters(juce::MathConstants<float>::pi*1.25f,juce::MathConstants<float>::pi*2.75f,true);
            knob.setTextBoxStyle(juce::Slider::TextBoxBelow,false,135,24);
            attachments[i]=std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.state,ids[i],knob);
            knob.setDoubleClickReturnValue(true,defaults[i]);
            knob.textFromValueFunction=[i](double value) { return juce::String(value,i==0?0:1)+(i==1?" ct":i==2?" ms":i==3 || i==4?" %":i==5?" dB":""); };
            knob.valueFromTextFunction=[](const juce::String& value){return value.getDoubleValue();}; knob.updateText();
        }
        knobs[0].setTooltip("Virtual singers, distributed across loaded takes. Render again after changing settings.");
        knobs[1].setTooltip("Maximum independent pitch spread in cents, plus slow drift. Duration-preserving granular processing; not a learned singer model.");
        knobs[2].setTooltip("Maximum onset spread in milliseconds. Use aligned recordings of the same phrase.");
        knobs[3].setTooltip("Stereo placement of the singers. 0% centres the dry ensemble; Room can add stereo reflections.");
        knobs[4].setTooltip("Simple stereo room on the rendered ensemble. 0% removes room and its tail.");
        knobs[5].setTooltip("Output gain baked into the next render and exported WAV. Re-render after changing.");
        for(auto* button:{&renderButton,&preview,&stopButton,&ab,&exportButton,&demoButton,&variation,&reset,&loopButton,&monitorButton,&cancelCaptureButton}) {
            addAndMakeVisible(*button); button->setColour(juce::TextButton::buttonColourId,style::ink);
            button->setColour(juce::TextButton::textColourOffId,style::gold);
            button->setColour(juce::TextButton::buttonOnColourId,style::gold); button->setColour(juce::TextButton::textColourOnId,style::ink);
        }
        for(size_t i=0;i<4;++i) for(auto* b:{&load[i],&clear[i]}) { b->setColour(juce::TextButton::buttonColourId,style::ink.brighter(.1f)); b->setColour(juce::TextButton::textColourOffId,style::cream); }
        renderButton.setComponentID("render"); preview.setComponentID("preview"); stopButton.setComponentID("stop");
        ab.setComponentID("original"); exportButton.setComponentID("export"); demoButton.setComponentID("demo"); variation.setComponentID("variation"); reset.setComponentID("reset"); loopButton.setComponentID("loop");
        renderButton.onClick=[this]{if(processor.busy.load()) processor.cancelRender(); else processor.requestRender();};
        preview.onClick=[this]{processor.play();}; stopButton.onClick=[this]{processor.stop();processor.finishRecording();};
        demoButton.onClick=[this]{processor.loadDemo();}; exportButton.onClick=[this]{chooseExport();};
        variation.onClick=[this]{auto* parameter=processor.state.getParameter("seed");
            const auto value=std::fmod(processor.settings().seed+1.,100000.); parameter->beginChangeGesture(); parameter->setValueNotifyingHost(parameter->convertTo0to1(static_cast<float>(value))); parameter->endChangeGesture(); processor.requestRender();};
        reset.onClick=[this]{for(size_t i=0;i<ids.size();++i) { auto* parameter=processor.state.getParameter(ids[i]); parameter->beginChangeGesture(); parameter->setValueNotifyingHost(parameter->getDefaultValue()); parameter->endChangeGesture(); }};
        monitorButton.setComponentID("inputMonitor"); cancelCaptureButton.setComponentID("cancelCapture");
        monitorButton.setClickingTogglesState(true);
        monitorAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.state,"inputMonitor",monitorButton);
        monitorButton.setTooltip("Hear routed input when choir preview is stopped. Off by default. Preview replaces input monitoring.");
        cancelCaptureButton.onClick=[this] {processor.cancelRecording();refresh();};
        ab.setClickingTogglesState(true); loopButton.setClickingTogglesState(true);
        abAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.state,"original",ab);
        loopAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.state,"loop",loopButton);
        ab.setTooltip("Compare the first loaded take with the stereo choir at the same phrase position. This is not LUFS level matching.");
        preview.setTooltip("Preview needs an active audio device/DAW callback. Any MIDI Note On also restarts the phrase at its original pitch.");
        exportButton.setTooltip("Save the current choir as stereo 48 kHz / 24-bit WAV, including its room tail. Original A/B does not change the export.");
        addAndMakeVisible(sizeChoice); sizeChoice.setComponentID("uiSize"); sizeChoice.addItemList({"Small (75%)","Medium (100%)","Large (125%)"},1);
        sizeChoice.setColour(juce::ComboBox::backgroundColourId,style::ink); sizeChoice.setColour(juce::ComboBox::textColourId,style::gold);
        sizeChoice.setSelectedId(processor.uiSize.load(),juce::dontSendNotification);
        sizeChoice.onChange=[this]{processor.uiSize=sizeChoice.getSelectedId(); processor.updateHostDisplay(juce::AudioProcessor::ChangeDetails{}.withNonParameterStateChanged(true)); if(sizeChanged) sizeChanged();};
        addAndMakeVisible(leftMeter); addAndMakeVisible(rightMeter); addAndMakeVisible(statusLabel);
        statusLabel.setColour(juce::Label::textColourId,style::cream.withAlpha(.8f)); statusLabel.setFont(juce::FontOptions(12.f));
        setSize(1100,820); refresh(); startTimerHz(30);
    }
    ~ChoirPanel() override { stopTimer(); setLookAndFeel(nullptr); }
    std::function<void()> sizeChanged;
    bool isInterestedInFileDrag(const juce::StringArray& files) override { for(const auto& f:files) if(juce::File(f).hasFileExtension("wav;aif;aiff;flac")) return true; return false; }
    void filesDropped(const juce::StringArray& files,int,int) override {
        for(const auto& f:files) {
            auto bank=processor.takes(); int slot=0; while(slot<4 && bank[static_cast<size_t>(slot)]) ++slot;
            if(slot==4) break;
            processor.loadTake(juce::File(f),slot);
        }
        refresh();
    }
    void resized() override {
        sizeChoice.setBounds(864,15,190,27); reset.setBounds(864,56,190,29);
        demoButton.setBounds(423,128,143,26); cancelCaptureButton.setBounds(250,128,160,26); monitorButton.setBounds(602,320,190,26);
        for(int i=0;i<4;++i) { load[static_cast<size_t>(i)].setBounds(46,164+i*45,404,36); recordButtons[static_cast<size_t>(i)].setBounds(458,164+i*45,60,36); clear[static_cast<size_t>(i)].setBounds(525,164+i*45,40,36); }
        leftMeter.setBounds(602,151,208,164); rightMeter.setBounds(828,151,208,164);
        for(int i=0;i<6;++i) knobs[static_cast<size_t>(i)].setBounds(39+i*171,526,165,132);
        renderButton.setBounds(46,687,192,36); preview.setBounds(250,687,125,36); stopButton.setBounds(387,687,100,36);
        ab.setBounds(499,687,154,36); variation.setBounds(665,687,155,36); exportButton.setBounds(832,687,204,36);
        loopButton.setBounds(909,473,127,26); statusLabel.setBounds(46,738,994,30);
    }
    void paint(juce::Graphics& g) override {
        using namespace style;
        g.fillAll(cream); g.setColour(ink); g.setFont(juce::FontOptions(11.f));
        g.drawText("YODO AUDIO    /    PHRASE ENSEMBLE",32,15,650,22,juce::Justification::left);
        g.setFont(juce::FontOptions(40.f)); g.drawText("YodoChoir",30,42,300,55,juce::Justification::left);
        g.setFont(juce::FontOptions(12.f)); g.drawText("S T U D I O   /   P R O T O T Y P E  0 2",290,68,500,20,juce::Justification::left);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff343d36),0,108,juce::Colour(0xff19221d),0,780,false));
        g.fillRoundedRectangle(16,108,1068,674,16); g.setColour(gold.withAlpha(.25f)); g.drawRoundedRectangle({17.f,109.f,1066.f,672.f},15,1);
        for(float x:{30.f,1070.f}) for(float y:{122.f,768.f}) {g.setColour(ink.darker());g.fillEllipse(x-4,y-4,8,8);g.setColour(muted);g.drawLine(x-2,y+1,x+2,y-1,1);}
        g.setColour(gold); g.setFont(juce::FontOptions(12.f)); g.drawText("01 / SOURCE TAKES",46,128,330,25,juce::Justification::left);
        g.drawText(processor.captureActive()?"02 / RECORD INPUT":"02 / STEREO ENSEMBLE",602,122,434,25,juce::Justification::centred);
        g.setColour(cream.withAlpha(.55f)); g.setFont(juce::FontOptions(11.f));
        g.drawText("WAV / AIFF / FLAC  /  DROP FILES HERE  /  120 s PER TAKE",46,348,530,22,juce::Justification::left);
        const auto r=processor.rendered();
        g.drawText(processor.captureActive()?juce::String("HOST INPUT / MONO TAKE / 120 s MAX"):r?juce::String(r->settings.singers)+" SINGERS  /  48 kHz  /  PEAK TRIM "+juce::String(r->peakTrimDb,1)+" dB":"LOCAL RENDERING  /  NO VOICE MODEL REQUIRED",602,347,434,26,juce::Justification::centred);
        g.setColour(juce::Colour(0xff111b16)); g.fillRoundedRectangle(46,385,990,83,7);
        g.setColour(gold.withAlpha(.12f)); g.drawHorizontalLine(426,55,1027);
        choir::Waveform wave{};
        if(processor.captureActive()) wave={};
        else if(r) wave=ab.getToggleState()?r->dryWave:r->wetWave;
        else { const auto bank=processor.takes(); for(const auto& take:bank) if(take) {wave=take->preview;break;} }
        const float wavePeak=*std::max_element(wave.begin(),wave.end());
        const float waveScale=wavePeak>0?31.f/wavePeak:0.f; // Display normalization only.
        for(size_t i=0;i<wave.size();++i) {
            const float x=53.f+static_cast<float>(i)*976.f/256.f,h=std::min(33.f,wave[i]*waveScale);
            g.setColour(gold.withAlpha(.7f)); g.drawVerticalLine(juce::roundToInt(x),426-h,426+h);
        }
        if(processor.captureActive()) {
            g.setColour(gold); g.setFont(juce::FontOptions(15.f));
            g.drawText((processor.isRecording()?"RECORDING INPUT / ":"PREPARING TAKE / ")+juce::String(processor.recordingSeconds(),1)+" s",60,404,960,36,juce::Justification::centred);
            g.setColour(juce::Colour(0xffb36557)); g.fillRect(53.f,461.f,976.f*static_cast<float>(processor.recordingSeconds()/choir::maxSeconds),3.f);
        }
        if(processor.isPlaying()) {g.setColour(cream);const float x=53+processor.playbackPosition()*976;g.drawLine(x,391,x,461,1.5f);}
        g.setFont(juce::FontOptions(10.f));g.setColour(cream.withAlpha(.55f));
        g.drawText("03 / RENDER, AUDITION, EXPORT   /   PHRASE DURATION PRESERVED",46,475,840,22,juce::Justification::left);
        g.setColour(gold); g.setFont(juce::FontOptions(11.f));
        for(int i=0;i<6;++i) g.drawText(juce::String(names[static_cast<size_t>(i)]).toUpperCase(),39+i*171,508,165,24,juce::Justification::centred);
        g.setColour(cream.withAlpha(.1f)); g.drawHorizontalLine(671,46,1036);
        if(processor.busy.load()) {g.setColour(gold.withAlpha(.45f));g.fillRect(46.f,729.f,990.f*processor.progress.load(),3.f);}
        g.setColour(ink.withAlpha(.65f));g.setFont(juce::FontOptions(11.f));
        g.drawText("YodoChoir  /  Sample-based ensemble prototype",32,790,700,22,juce::Justification::left);
        g.drawText("LOCAL AUDIO  /  STEREO WAV EXPORT",700,790,354,22,juce::Justification::right);
    }
private:
    void chooseTake(int slot) {
        chooser=std::make_unique<juce::FileChooser>("Choose a vocal take (up to 120 seconds)",juce::File{},"*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync(juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles,
            [safe=juce::Component::SafePointer<ChoirPanel>(this),slot](const juce::FileChooser& fc){if(safe && fc.getResult().existsAsFile()){safe->processor.loadTake(fc.getResult(),slot);safe->refresh();}});
    }
    void chooseExport() {
        chooser=std::make_unique<juce::FileChooser>("Export the rendered choir",juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("YodoChoir.wav"),"*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting,
            [safe=juce::Component::SafePointer<ChoirPanel>(this)](const juce::FileChooser& fc) {
                if(!safe || fc.getResult()==juce::File{}) return;
                const auto chosen=fc.getResult(),target=chosen.withFileExtension("wav");
                if(target!=chosen && target.existsAsFile()) {
                    juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Replace existing WAV?").withMessage(target.getFileName()+" already exists.")
                        .withButton("Replace").withButton("Cancel"),
                        [safe,target](int result){if(safe && result==1) safe->processor.exportWav(target);});
                } else safe->processor.exportWav(target);
            });
    }
    void refresh() {
        const auto bank=processor.takes();
        for(size_t i=0;i<4;++i) {
            const auto label=bank[i]?juce::String(bank[i]->name)+"  /  "+juce::String(double(bank[i]->mono.size())/choir::sampleRate,1)+" s":"Load vocal take "+juce::String(static_cast<int>(i+1))+"...";
            const bool capturing=processor.captureActive(),activeSlot=processor.recordingSlot()==static_cast<int>(i);
            const auto seconds=static_cast<int>(processor.recordingSeconds());
            load[i].setButtonText(capturing && activeSlot?(processor.isRecording()?"Recording ":"Saving recording ")+juce::String(seconds/60)+":"+juce::String(seconds%60).paddedLeft('0',2)+" / 2:00":label);
            load[i].setEnabled(!capturing); clear[i].setEnabled(bool(bank[i]) && !capturing);
            recordButtons[i].setButtonText(capturing && activeSlot && processor.isRecording()?"STOP":"REC");
            recordButtons[i].setEnabled(!capturing || (activeSlot && processor.isRecording()));
        }
        const bool ready=processor.current(),working=processor.busy.load();
        const bool capturing=processor.captureActive();
        preview.setEnabled(ready && !working && !capturing); exportButton.setEnabled(ready && !working && !capturing);
        renderButton.setEnabled(!capturing); variation.setEnabled(!capturing); demoButton.setEnabled(!capturing);
        cancelCaptureButton.setEnabled(capturing);
        renderButton.setButtonText(working?"Cancel render":"RENDER CHOIR");
        ab.setButtonText(ab.getToggleState()?"A: Original take":"B: Choir");
        const auto r=processor.rendered();
        statusLabel.setText(capturing?(processor.isRecording()?"Recording routed input. Press STOP to keep it, or Cancel capture to keep the previous take.":"Preparing the recorded take..."):!working && r && !ready?"Takes or settings changed. Render choir again before preview or export.":processor.status(),juce::dontSendNotification);
        if(sizeChoice.getSelectedId()!=processor.uiSize.load()) {sizeChoice.setSelectedId(processor.uiSize.load(),juce::dontSendNotification);if(sizeChanged)sizeChanged();}
    }
    void timerCallback() override {
        refresh();
        for(size_t c=0;c<2;++c) {const float peak=processor.captureActive()?processor.inputPeaks[c].exchange(0):processor.peaks[c].exchange(0);meterLevels[c]=std::max(peak,meterLevels[c]*.84f);}
        leftMeter.setPeak(meterLevels[0]);rightMeter.setPeak(meterLevels[1]); repaint();
    }
    ChoirProcessor& processor;
    style::Look look;
    std::array<juce::TextButton,4> load,clear,recordButtons;
    std::array<juce::Slider,6> knobs;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,6> attachments;
    juce::TextButton renderButton{"RENDER CHOIR"},preview{"Preview"},stopButton{"Stop"},ab{"B: Choir"},exportButton{"Export WAV..."},demoButton{"Load demo"},variation{"New variation"},reset{"RESET SETTINGS"},loopButton{"Loop preview"},monitorButton{"Monitor input"},cancelCaptureButton{"Cancel capture"};
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> abAttachment,loopAttachment,monitorAttachment;
    juce::ComboBox sizeChoice;
    juce::Label statusLabel;
    OutputMeter leftMeter{"LEFT"},rightMeter{"RIGHT"};
    std::array<float,2> meterLevels{};
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltip{this,600};
};
class ChoirEditor final : public juce::AudioProcessorEditor {
public:
    explicit ChoirEditor(ChoirProcessor& p):AudioProcessorEditor(p),processor(p),panel(p) {addAndMakeVisible(panel);setResizable(false,false);panel.sizeChanged=[this]{applySize();};applySize();}
private:
    void applySize() {const float scale=processor.uiSize.load()==1?.75f:processor.uiSize.load()==3?1.25f:1.f;panel.setTransform(juce::AffineTransform::scale(scale));panel.setBounds(0,0,1100,820);setSize(juce::roundToInt(1100*scale),juce::roundToInt(820*scale));}
    ChoirProcessor& processor;
    ChoirPanel panel;
};
juce::AudioProcessorEditor* ChoirProcessor::createEditor() {return new ChoirEditor(*this);}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {return new ChoirProcessor;}
