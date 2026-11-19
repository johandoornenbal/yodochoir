#include "ReaperBridge.h"
#include "ReaperHostInterface.h"
#include <cmath>
#include <limits>
#include <utility>
using yodochoir::ReaperApplication;
struct ReaperBridge::Session {
    std::function<void*(unsigned)> parent;
    ReaProject* (*projects)(int,char*,int)=nullptr;
    bool (*valid)(ReaProject*,void*,const char*)=nullptr;
    int (*playing)(ReaProject*)=nullptr;
    double (*cursor)(ReaProject*)=nullptr;
    MediaItem* (*addItem)(MediaTrack*)=nullptr;
    MediaItem_Take* (*addTake)(MediaItem*)=nullptr;
    PCM_source* (*source)(const char*)=nullptr;
    void (*destroy)(PCM_source*)=nullptr;
    int (*peaks)(PCM_source*,int)=nullptr;
    void (*updateItem)(MediaItem*)=nullptr;
    void* (*takeInfo)(MediaItem_Take*,const char*,void*)=nullptr;
    bool (*itemValue)(MediaItem*,const char*,double)=nullptr;
    double (*itemRead)(MediaItem*,const char*)=nullptr;
    bool (*takeValue)(MediaItem_Take*,const char*,double)=nullptr;
    bool (*remove)(MediaTrack*,MediaItem*)=nullptr;
    void (*undoBegin)(ReaProject*)=nullptr;
    void (*undoEnd)(ReaProject*,const char*,int)=nullptr;
    void (*refresh)()=nullptr;
    bool complete() const {return parent && projects && valid && playing && cursor && addItem && addTake && source && destroy && peaks && updateItem && takeInfo && itemValue && itemRead && takeValue && remove && undoBegin && undoEnd && refresh;}
};
// Own the source until peak generation is complete, then transfer it to the
// new take. No host item/source pointer is held across peak-building callbacks.
struct ReaperBridge::PeakBuild {
    std::shared_ptr<Session> api;
    PCM_source* source=nullptr;
    bool building=false;
    ~PeakBuild() {
        if(building) api->peaks(source,2);
        if(source) api->destroy(source);
    }
};
std::shared_ptr<ReaperBridge::PeakBuild> ReaperBridge::preparePeaks(const juce::File& file,juce::String& error) const {
    const auto api=snapshot();if(!api) {error="REAPER peak-building API unavailable. WAV retained for manual import.";return {};}
    auto job=std::make_shared<PeakBuild>();job->api=api;
    job->source=api->source(file.getFullPathName().toRawUTF8());
    if(!job->source) {error="REAPER could not open the WAV to prepare its waveform.";return {};}
    job->building=api->peaks(job->source,0)!=0;
    return job;
}
bool ReaperBridge::advancePeaks(const std::shared_ptr<PeakBuild>& job) const {
    if(!job || !job->building) return true;
    // Reaper APIs stay on the message thread. Yield between short slices rather
    // than blocking the UI for a long 96/192 kHz file or touching the audio callback.
    const double until=juce::Time::getMillisecondCounterHiRes()+5.;
    do {
        if(job->api->peaks(job->source,1)==0) {
            job->api->peaks(job->source,2);job->building=false;return true;
        }
    } while(juce::Time::getMillisecondCounterHiRes()<until);
    return false;
}
void ReaperBridge::setIHostApplication(Steinberg::FUnknown* host) {
    if(!host) {connect({},{});return;}
    const Steinberg::TUID id=INLINE_UID(0x79655E36,0x77EE4267,0xA573FEF7,0x4912C27C);
    void* ptr=nullptr;
    if(host->queryInterface(id,&ptr)!=Steinberg::kResultOk || !ptr) {connect({},{});return;}
    auto owner=std::shared_ptr<ReaperApplication>(static_cast<ReaperApplication*>(ptr),[](auto* p){p->release();});
    connect([owner](const char* name){return owner->getReaperApi(name);},[owner](unsigned n){return owner->getReaperParent(n);});
}
void ReaperBridge::connect(std::function<void*(const char*)> api,std::function<void*(unsigned)> parent) {
    auto next=std::make_shared<Session>();next->parent=std::move(parent);
    if(api) {
#define LOAD(field,name) next->field=reinterpret_cast<decltype(next->field)>(api(name))
        LOAD(projects,"EnumProjects");LOAD(valid,"ValidatePtr2");LOAD(playing,"GetPlayStateEx");LOAD(cursor,"GetCursorPositionEx");
        LOAD(addItem,"AddMediaItemToTrack");LOAD(addTake,"AddTakeToMediaItem");LOAD(source,"PCM_Source_CreateFromFile");LOAD(destroy,"PCM_Source_Destroy");
        LOAD(peaks,"PCM_Source_BuildPeaks");LOAD(updateItem,"UpdateItemInProject");
        LOAD(takeInfo,"GetSetMediaItemTakeInfo");LOAD(itemValue,"SetMediaItemInfo_Value");LOAD(itemRead,"GetMediaItemInfo_Value");LOAD(takeValue,"SetMediaItemTakeInfo_Value");
        LOAD(remove,"DeleteTrackMediaItem");LOAD(undoBegin,"Undo_BeginBlock2");LOAD(undoEnd,"Undo_EndBlock2");LOAD(refresh,"UpdateArrange");
#undef LOAD
    }
    const std::lock_guard<std::mutex> lock(mutex);session=next->complete()?std::move(next):nullptr;
}
std::shared_ptr<ReaperBridge::Session> ReaperBridge::snapshot() const {const std::lock_guard<std::mutex> lock(mutex);return session;}
bool ReaperBridge::available() const {return bool(snapshot());}
bool ReaperBridge::context(Context& out,juce::String& error,bool requireSaved) const {
    const auto s=snapshot();if(!s){error="Write to track requires a native REAPER VST3 instance with its extension API available.";return false;}
    auto* project=static_cast<ReaProject*>(s->parent(3));auto* track=static_cast<MediaTrack*>(s->parent(1));
    if(!project || !track){error="Use YodoChoir as track FX in REAPER to write an item.";return false;}
    for(int i=0;i<1024;++i) {
        std::array<char,16384> file{};auto* p=s->projects(i,file.data(),static_cast<int>(file.size()));if(!p) break;
        if(p==project) {
            if(!s->valid(project,track,"MediaTrack*")){error="The owning REAPER track is no longer available.";return false;}
            if(requireSaved && file[0]==0){error="Save the REAPER project before writing choir audio to its media folder.";return false;}
            out={project,track,file[0]?juce::File(juce::String::fromUTF8(file.data())):juce::File{}};return true;
        }
    }
    error="The owning REAPER project is no longer available.";return false;
}
int ReaperBridge::playState(const Context& c) const {const auto s=snapshot();return s?s->playing(c.project):-1;}
double ReaperBridge::editCursor(const Context& c) const {const auto s=snapshot();return s?s->cursor(c.project):std::numeric_limits<double>::quiet_NaN();}
bool ReaperBridge::insert(const Context& expected,const juce::File& file,double start,double length,juce::String& error,const std::shared_ptr<PeakBuild>& peaks) const {
    Context actual;if(!context(actual,error)) return false;
    if(!(actual==expected)){error="The owning track or project changed during writing. WAV retained for manual import.";return false;}
    const auto s=snapshot();if(!s || !std::isfinite(start) || !std::isfinite(length) || length<=0){error="Invalid item placement.";return false;}
    if((s->playing(actual.project)&7)!=0){error="Stop REAPER transport before placing the choir item. WAV retained for manual import.";return false;}
    if(peaks && (peaks->building || !peaks->source || peaks->api!=s)) {error="Waveform preparation is incomplete or its host changed.";return false;}
    auto* source=peaks?std::exchange(peaks->source,nullptr):s->source(file.getFullPathName().toRawUTF8());if(!source){error="REAPER could not open the rendered WAV.";return false;}
    s->undoBegin(actual.project);
    auto* item=s->addItem(actual.track);auto* take=item?s->addTake(item):nullptr;
    bool attached=false,ok=false;
    if(take) {
        auto* previous=static_cast<PCM_source*>(s->takeInfo(take,"P_SOURCE",nullptr));
        s->takeInfo(take,"P_SOURCE",source);
        attached=s->takeInfo(take,"P_SOURCE",nullptr)==source;
        if(attached && previous && previous!=source) s->destroy(previous);
        if(attached) {
            ok=s->itemValue(item,"D_POSITION",start) && s->itemValue(item,"D_LENGTH",length)
                && s->itemValue(item,"B_LOOPSRC",0) && s->itemValue(item,"C_BEATATTACHMODE",0)
                && s->itemValue(item,"D_FADEINLEN",0) && s->itemValue(item,"D_FADEOUTLEN",0)
                && s->takeValue(take,"D_STARTOFFS",0) && s->takeValue(take,"D_PLAYRATE",1)
                && std::abs(s->itemRead(item,"D_POSITION")-start)<1.e-9;
        }
    }
    if(ok) s->updateItem(item);
    if(!ok && item) s->remove(actual.track,item);
    if(!attached) s->destroy(source);
    s->undoEnd(actual.project,ok?"YodoChoir: write aligned choir item":"YodoChoir: cancelled item write",-1);s->refresh();
    if(!ok) error="REAPER could not complete the item insertion. WAV retained for manual import.";
    return ok;
}
