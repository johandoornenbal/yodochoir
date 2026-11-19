// Exercise the built VST3's public ABI, including JUCE's fallback negotiation.
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <dlfcn.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
using namespace Steinberg;
using namespace Steinberg::Vst;
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
struct Module {
    void* handle=nullptr;
#if defined(__APPLE__)
    CFBundleRef bundle=nullptr;
#endif
    explicit Module(const char* path) {
        handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!handle)throw std::runtime_error(dlerror());
#if defined(__APPLE__)
        const auto root=std::filesystem::path(path).parent_path().parent_path().parent_path().string();
        auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8*>(root.data()),static_cast<CFIndex>(root.size()),true);
        bundle=CFBundleCreate(nullptr,url);CFRelease(url);
        auto entry=reinterpret_cast<bool(*)(CFBundleRef)>(dlsym(handle,"bundleEntry"));
        check(bundle && entry && entry(bundle),"VST3 bundleEntry failed");
#else
        auto entry=reinterpret_cast<bool(*)(void*)>(dlsym(handle,"ModuleEntry"));check(entry && entry(handle),"VST3 ModuleEntry failed");
#endif
    }
    ~Module() {
#if defined(__APPLE__)
        if(auto exit=reinterpret_cast<bool(*)()>(dlsym(handle,"bundleExit")))exit();
        if(bundle)CFRelease(bundle);
#else
        if(auto exit=reinterpret_cast<bool(*)()>(dlsym(handle,"ModuleExit")))exit();
#endif
        dlclose(handle);
    }
};
class Host final : public FUnknown {
public:
    uint32 refs=1;
    tresult PLUGIN_API queryInterface(const TUID id,void** result) override {
        *result=nullptr;if(std::memcmp(id,INLINE_UID_OF(FUnknown),16)!=0)return kNoInterface;
        *result=this;addRef();return kResultOk;
    }
    uint32 PLUGIN_API addRef() override {return ++refs;}
    uint32 PLUGIN_API release() override {return --refs;}
};
void pins(IComponent& component) {
    BusInfo input{},output{};
    check(component.getBusCount(kAudio,kInput)==1 && component.getBusCount(kAudio,kOutput)==1,"Unexpected bus count");
    check(component.getBusInfo(kAudio,kInput,0,input)==kResultOk && input.channelCount==8,"VST3 input pins shrank below eight");
    check(component.getBusInfo(kAudio,kOutput,0,output)==kResultOk && output.channelCount==2,"VST3 output is not stereo");
}
int main(int argc,char** argv) try {
    check(argc==2,"Pass the built VST3 binary");Module module(argv[1]);Host host;
    auto getFactory=reinterpret_cast<IPluginFactory*(*)()>(dlsym(module.handle,"GetPluginFactory"));check(getFactory,"No plugin factory");
    auto* factory=getFactory();check(factory,"Null plugin factory");IComponent* component=nullptr;
    for(int32 i=0;i<factory->countClasses() && !component;++i) {
        PClassInfo info{};if(factory->getClassInfo(i,&info)==kResultOk && std::strcmp(info.category,kVstAudioEffectClass)==0)
            factory->createInstance(info.cid,INLINE_UID_OF(IComponent),reinterpret_cast<void**>(&component));
    }
    check(component,"Cannot create VST3 component");check(component->initialize(&host)==kResultOk,"Cannot initialize VST3 component");
    IAudioProcessor* audio=nullptr;check(component->queryInterface(INLINE_UID_OF(IAudioProcessor),reinterpret_cast<void**>(&audio))==kResultOk && audio,"No VST3 audio processor");
    pins(*component);
    SpeakerArrangement eight=0;check(audio->getBusArrangement(kInput,0,eight)==kResultOk && SpeakerArr::getChannelCount(eight)==8,"No eight-channel arrangement");
    auto output=SpeakerArr::kStereo;
    // Reaper commonly proposes 2-in/2-out, despite the constructor's 8-in default.
    for(const auto proposal:{SpeakerArr::kStereo,SpeakerArr::kMono,SpeakerArr::k40Cine,SpeakerArr::k51}) {
        auto input=proposal;check(audio->setBusArrangements(&input,1,&output,1)!=kResultOk,"VST3 accepted a layout that hides capture pairs");pins(*component);
    }
    check(audio->setBusArrangements(&eight,1,&output,1)==kResultOk,"VST3 rejected eight inputs and stereo output");pins(*component);
    // The global Reaper bus-size menu can also request eight output channels.
    auto outputEight=eight;check(audio->setBusArrangements(&eight,1,&outputEight,1)!=kResultOk,"Unexpected eight-channel output accepted");pins(*component);
    check(audio->setBusArrangements(&eight,1,&output,1)==kResultOk,"Negotiation could not recover after global eight-channel proposal");
    ProcessSetup setup{};setup.processMode=kRealtime;setup.symbolicSampleSize=kSample32;setup.maxSamplesPerBlock=256;setup.sampleRate=96000;
    check(audio->setupProcessing(setup)==kResultOk,"VST3 setup failed");
    check(component->activateBus(kAudio,kInput,0,true)==kResultOk && component->activateBus(kAudio,kOutput,0,true)==kResultOk,"Bus activation failed");
    check(component->setActive(true)==kResultOk,"Component activation failed");pins(*component);
    check(component->setActive(false)==kResultOk,"Component deactivation failed");
    audio->release();component->terminate();component->release();factory->release();check(host.refs==1,"Host reference leaked");
    std::cout<<"Built VST3 retains eight input pins / stereo output after stereo, mono, quad, 5.1 and global eight-channel host proposals\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
