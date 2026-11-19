#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <mutex>
class ReaProject; class MediaTrack; class MediaItem; class MediaItem_Take; class PCM_source;
// Main-thread-only REAPER operations. The native VST3 interface supplies both
// API functions and the owning project/track; selected tracks are never used.
class ReaperBridge final : public juce::VST3ClientExtensions {
public:
    struct Context {
        ReaProject* project=nullptr;
        MediaTrack* track=nullptr;
        juce::File projectFile;
        bool operator==(const Context& b) const {return project==b.project && track==b.track && projectFile==b.projectFile;}
    };
    struct PeakBuild;
    std::shared_ptr<PeakBuild> preparePeaks(const juce::File&,juce::String& error) const;
    bool advancePeaks(const std::shared_ptr<PeakBuild>&) const;
    void setIHostApplication(Steinberg::FUnknown*) override;
    void connect(std::function<void*(const char*)> api,std::function<void*(unsigned)> parent);
    bool available() const;
    bool context(Context&,juce::String& error,bool requireSaved=true) const;
    int playState(const Context&) const;
    double editCursor(const Context&) const;
    bool insert(const Context&,const juce::File&,double start,double length,juce::String& error,const std::shared_ptr<PeakBuild>& peaks={}) const;
private:
    struct Session;
    std::shared_ptr<Session> snapshot() const;
    mutable std::mutex mutex;
    std::shared_ptr<Session> session;
};
