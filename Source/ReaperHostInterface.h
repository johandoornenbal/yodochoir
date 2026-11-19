#pragma once
#include <pluginterfaces/base/funknown.h>
namespace yodochoir {
// ABI from Cockos' reaper_vst3_interfaces.h. Keep external linkage: hosts
// implement this interface in another shared library, beyond compiler visibility.
class ReaperApplication : public Steinberg::FUnknown {
public:
    virtual void* PLUGIN_API getReaperApi(Steinberg::CStringA)=0;
    virtual void* PLUGIN_API getReaperParent(Steinberg::uint32)=0;
    virtual void* PLUGIN_API reaperExtended(Steinberg::uint32,void*,void*,void*)=0;
};
}
