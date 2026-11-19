#include "TestTransport.h"
#include "../Source/ReaperHostInterface.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <map>
#include <cstring>
#include <stdexcept>
class PCM_source {public:juce::String path;};
class MediaItem_Take {public:PCM_source* source=nullptr;std::map<std::string,double> values;~MediaItem_Take(){delete source;}};
class MediaItem {public:std::unique_ptr<MediaItem_Take> take;std::map<std::string,double> values;};
class MediaTrack {public:std::vector<std::unique_ptr<MediaItem>> items;};
class ReaProject {public:juce::File file;bool playing=false;double cursor=999.;};
namespace {
ReaProject owning,other;
MediaTrack track,selected;
bool failPosition=false;
int begins=0,ends=0;
ReaProject* undoProject=nullptr;
ReaProject* projects(int i,char* out,int length) {
    auto* p=i==0?&other:i==1?&owning:nullptr;
    if(p && out && length>0) std::snprintf(out,static_cast<size_t>(length),"%s",p->file.getFullPathName().toRawUTF8());
    return p;
}
bool valid(ReaProject* p,void* ptr,const char*) {return p==&owning && (ptr==&track || ptr==&selected);}
int playState(ReaProject* p) {return p->playing?1:0;}
double editCursor(ReaProject* p) {return p->cursor;}
MediaItem* addItem(MediaTrack* t) {t->items.push_back(std::make_unique<MediaItem>());return t->items.back().get();}
MediaItem_Take* addTake(MediaItem* item) {item->take=std::make_unique<MediaItem_Take>();return item->take.get();}
PCM_source* source(const char* file) {if(!juce::File(juce::String::fromUTF8(file)).existsAsFile()) return nullptr;auto* p=new PCM_source;p->path=file;return p;}
void destroy(PCM_source* p) {delete p;}
void* takeInfo(MediaItem_Take* t,const char* name,void* value) {if(std::strcmp(name,"P_SOURCE")!=0)return nullptr;if(value)t->source=static_cast<PCM_source*>(value);return t->source;}
bool itemValue(MediaItem* i,const char* name,double value) {if(failPosition && std::strcmp(name,"D_POSITION")==0)return false;i->values[name]=value;return true;}
double itemRead(MediaItem* i,const char* name) {return i->values[name];}
bool takeValue(MediaItem_Take* t,const char* name,double value) {t->values[name]=value;return true;}
bool removeItem(MediaTrack* t,MediaItem* item) {for(auto i=t->items.begin();i!=t->items.end();++i)if(i->get()==item){t->items.erase(i);return true;}return false;}
void undoBegin(ReaProject* p) {++begins;undoProject=p;}
void undoEnd(ReaProject* p,const char*,int) {if(p!=undoProject)throw std::runtime_error("Undo used another project");++ends;}
void refresh() {}
void* api(const char* name) {
#define API(key,fn) if(std::strcmp(name,key)==0)return reinterpret_cast<void*>(&fn)
    API("EnumProjects",projects);API("ValidatePtr2",valid);API("GetPlayStateEx",playState);API("GetCursorPositionEx",editCursor);
    API("AddMediaItemToTrack",addItem);API("AddTakeToMediaItem",addTake);API("PCM_Source_CreateFromFile",source);API("PCM_Source_Destroy",destroy);
    API("GetSetMediaItemTakeInfo",takeInfo);API("SetMediaItemInfo_Value",itemValue);API("GetMediaItemInfo_Value",itemRead);API("SetMediaItemTakeInfo_Value",takeValue);
    API("DeleteTrackMediaItem",removeItem);API("Undo_BeginBlock2",undoBegin);API("Undo_EndBlock2",undoEnd);API("UpdateArrange",refresh);
#undef API
    return nullptr;
}
class Host final : public yodochoir::ReaperApplication {
public:
    int refs=1;MediaTrack* parent=&track;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,void** result) override {
        const Steinberg::TUID expected=INLINE_UID(0x79655E36,0x77EE4267,0xA573FEF7,0x4912C27C);
        if(std::memcmp(iid,expected,16)==0){*result=this;addRef();return Steinberg::kResultOk;}
        *result=nullptr;return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {return static_cast<Steinberg::uint32>(++refs);}
    Steinberg::uint32 PLUGIN_API release() override {return static_cast<Steinberg::uint32>(--refs);}
    virtual void* PLUGIN_API getReaperApi(Steinberg::CStringA name) {return api(name);}
    virtual void* PLUGIN_API getReaperParent(Steinberg::uint32 which) {return which==1?static_cast<void*>(parent):which==3?static_cast<void*>(&owning):nullptr;}
    virtual void* PLUGIN_API reaperExtended(Steinberg::uint32,void*,void*,void*) {return nullptr;}
};
void check(bool ok,const char* msg) {if(!ok)throw std::runtime_error(msg);}
void pump() {juce::Timer::callPendingTimersSynchronously();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
template<class Predicate> void wait(Predicate done) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(!done()){check(std::chrono::steady_clock::now()<end,"Timed out waiting for the workflow");pump();}
}
void process(ChoirProcessor& p,TestTransport& head,int samples) {
    juce::AudioBuffer<float> b(8,samples);juce::MidiBuffer midi;
    for(int c=0;c<8;++c) for(int i=0;i<samples;++i)b.setSample(c,i,.025f*static_cast<float>(c+1));
    p.processBlock(b,midi);if(head.moving)head.sample+=samples;
}
}
int main() try {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root=juce::File::getCurrentWorkingDirectory().getChildFile("previews/track-write-test");root.createDirectory();
    owning.file=root.getChildFile("Target.rpp");other.file=root.getChildFile("Other.rpp");owning.file.replaceWithText("test project fixture");other.file.replaceWithText("test project fixture");
    Host host;
    {
        ChoirProcessor p;TestTransport head;head.sample=48000*37+17;p.setPlayHead(&head);p.prepareToPlay(48000,513);
        p.getVST3ClientExtensions()->setIHostApplication(&host);check(p.reaper.available() && host.refs==2,"Native REAPER API discovery/refcount failed");
        check(p.startRecordingAll() && p.isArmed() && !p.isRecording(),"REC did not only arm");
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
            const auto preview=root.getParentDirectory().getChildFile("ui-armed.png");
            auto stream=preview.createOutputStream();check(bool(stream),"Cannot save armed preview");
            juce::PNGImageFormat png;check(png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds(),true),*stream),"Cannot encode armed preview");
        }
        process(p,head,256);process(p,head,256);
        check(p.isArmed() && p.recordingSeconds()==0. && !p.takes()[0],"Stopped transport was captured");
        head.moving=true;owning.playing=true;const double origin=double(head.sample)/48000.;
        process(p,head,97);process(p,head,513);check(p.isRecording() && !p.isArmed(),"Transport Play did not start capture");
        head.moving=false;owning.playing=false;head.sample=0;process(p,head,256);wait([&]{return !p.captureActive();});
        for(const auto& t:p.takes())check(t && t->mono.size()==610 && t->hasTimeline && t->startSeconds==origin,"Transport boundary/start position was not preserved");
        // Writing renders automatically, targets the owning track/project, and ignores the later cursor.
        p.requestWrite();check(p.isWriting(),"Write phase did not start");wait([&]{return !p.isWriting();});
        check(track.items.size()==1 && selected.items.empty(),"Write did not target the plugin's own track");
        auto& item=*track.items[0];const auto r=p.rendered();
        check(std::abs(item.values["D_POSITION"]-origin)<1.e-12 && item.values["D_POSITION"]!=owning.cursor,"Write used the later cursor instead of captured start");
        check(item.values["D_LENGTH"]==double(r->left.size())/48000. && item.values["D_LENGTH"]>6.,"Writing truncated the room tail");
        check(item.values["D_FADEINLEN"]==0 && item.values["D_FADEOUTLEN"]==0 && item.take->values["D_STARTOFFS"]==0 && item.take->values["D_PLAYRATE"]==1,"Host defaults shifted/trimmed the written audio");
        check(juce::File(item.take->source->path).getParentDirectory()==root.getChildFile("YodoChoir"),"WAV is outside the owning project media folder");
        check(begins==1 && ends==1 && undoProject==&owning,"Item write is not one project-specific undo step");
        check(p.state.getRawParameterValue("inputMonitor")->load()==1,"Track audio was left silent after writing");
        juce::MemoryBlock bytes;p.getStateInformation(bytes);
        ChoirProcessor recalled;recalled.setStateInformation(bytes.getData(),static_cast<int>(bytes.getSize()));wait([&]{return recalled.current() && !recalled.busy.load();});
        check(recalled.takes()[0]->hasTimeline && recalled.rendered()->startSeconds==origin,"Session lost the placement anchor");
        // Reaper can stop without sending another process callback. Its API
        // must finalize capture, including in a not-yet-saved project.
        {
            TestTransport suspended;ChoirProcessor stopped;
            stopped.setPlayHead(&suspended);stopped.prepareToPlay(48000,256);
            stopped.getVST3ClientExtensions()->setIHostApplication(&host);
            suspended.moving=true;owning.playing=true;process(stopped,suspended,256);
            owning.playing=false; // No stopped callback: last audio flag is stale.
            check(stopped.startRecordingAll(),"Could not arm after suspended transport");
            suspended.sample=48000*11;owning.playing=true;process(stopped,suspended,256);
            check(stopped.isRecording(),"Stale playing flag prevented the next capture");
            const auto savedFile=owning.file;owning.file=juce::File{};owning.playing=false;
            wait([&]{return !stopped.captureActive();});
            check(stopped.takes()[0] && stopped.takes()[0]->mono.size()==256 && stopped.takes()[0]->startSeconds==11.,"Suspended callback Stop lost the take or anchor");
            stopped.requestWrite();check(!stopped.isWriting(),"Unsaved project accepted a native write");
            owning.file=savedFile;
        }
        // Another write adds a new item; it does not replace an existing print.
        p.requestWrite();wait([&]{return !p.isWriting();});check(track.items.size()==2,"Repeated writing replaced the earlier item");
        failPosition=true;p.requestWrite();wait([&]{return !p.isWriting();});failPosition=false;
        check(track.items.size()==2 && selected.items.empty() && begins==ends,"Failed insertion did not roll back its new item");
        p.requestWrite();host.parent=&selected;wait([&]{return !p.isWriting();});host.parent=&track;
        check(track.items.size()==2 && selected.items.empty(),"Changing track context redirected an in-flight write");
        // Arming while already playing waits for a complete stop/start edge.
        ChoirProcessor edge;TestTransport clock;edge.setPlayHead(&clock);edge.prepareToPlay(48000,256);
        clock.moving=true;process(edge,clock,256);check(edge.startRecordingAll(),"Cannot arm during playback");process(edge,clock,256);
        check(edge.isArmed() && edge.recordingSeconds()==0,"Arming during playback started mid-pass");
        clock.moving=false;process(edge,clock,64);clock.sample=48000*8;clock.moving=true;process(edge,clock,64);
        clock.sample+=10000;process(edge,clock,64);wait([&]{return !edge.captureActive();});
        check(edge.takes()[0]->mono.size()==64 && edge.takes()[0]->startSeconds==8.,"Seek/loop handling combined different timeline regions");
        ChoirProcessor absent;TestTransport missing;missing.valid=false;absent.setPlayHead(&missing);absent.prepareToPlay(48000,256);
        check(absent.startRecordingAll(),"Cannot arm with temporarily missing host position");missing.moving=true;process(absent,missing,256);
        check(absent.isArmed() && absent.recordingSeconds()==0,"Unknown host position was treated as zero");absent.cancelRecording();wait([&]{return !absent.captureActive();});
        // Imported takes have no capture anchor: explicit Write uses the owning project's edit cursor.
        p.loadDemo();owning.cursor=12.75;p.requestWrite();wait([&]{return !p.isWriting();});
        check(track.items.size()==3 && track.items.back()->values["D_POSITION"]==12.75,"Imported take insertion did not use its explicit Write cursor");
        ChoirProcessor unsupported;unsupported.loadDemo();unsupported.requestWrite();check(!unsupported.isWriting(),"Unsupported host started a destructive write");
        unsupported.reaper.connect([](const char*)->void*{return nullptr;},[](unsigned)->void*{return nullptr;});check(!unsupported.reaper.available(),"Missing native API functions accepted");
    }
    check(host.refs==1,"REAPER host interface reference leaked");
    track.items.clear();selected.items.clear();root.deleteRecursively();
    std::cout<<"Arming, play/stop boundaries, timeline anchor/recall, seek guard, native REAPER discovery, exact item placement/tail, owner track/project, cursor independence, undo and rollback passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
