#pragma once
#include "P12NativeSampler.h"
namespace p12 {
struct SequenceEvent {
    double seconds=0;
    Event sample;
    bool midi=false,click=false,accent=false;
    int routeKind=-1, routeIndex=0, slot=1, channel=1, sourcePad=-1;
    std::array<unsigned char,3> bytes{};
    int byteCount=3;
};
struct SequencePlan { bool immediate=false; uint64_t id=0; double duration=1,bpm=96; std::vector<SequenceEvent> events; };
// Plans are immutable and retained by the control thread. No allocation,
// wall-clock note deadlines, IPC, mutex or renderer dependency in process().
class SequenceClock {
public:
    std::atomic<const SequencePlan*> published{nullptr};
    std::atomic<bool> playing{false};
    std::atomic<double> tempo{96}, position{0};
    std::atomic<uint64_t> planId{0}, generation{0}, loops{0};
    std::atomic<int64_t> startEpochMs{0};
    void start(int64_t epoch=0) noexcept { startEpochMs.store(epoch); generation.fetch_add(1,std::memory_order_release); playing.store(true,std::memory_order_release); }
    void stop() noexcept { playing.store(false); }
    template<class Dispatch> void process(int frames,double rate,int64_t epoch,Dispatch dispatch) noexcept {
        if(!playing.load(std::memory_order_acquire)||rate<8000||frames<=0)return;
        auto g=generation.load(std::memory_order_acquire);
        if(g!=seenGeneration){seenGeneration=g;active=published.load(std::memory_order_acquire);cursor=0;seconds=0;loops.store(0);}
        if(!active||active->duration<=0)return;
        if(auto* next=published.load(std::memory_order_acquire);next&&next!=active&&next->immediate){
            // Edits/mixer changes preserve musical phase; pattern selection
            // remains boundary-only. Neither path issues another START.
            seconds=std::fmod(seconds*next->duration/active->duration,next->duration);
            active=next;cursor=static_cast<size_t>(std::lower_bound(active->events.begin(),active->events.end(),seconds,[](const SequenceEvent& e,double time){return e.seconds<time;})-active->events.begin());
            tempo.store(active->bpm);
        }
        int first=0;
        const auto deadline=startEpochMs.load();
        if(deadline>epoch) first=std::min(frames,int(std::ceil((deadline-epoch)*rate/1000.0)));
        if(first==frames)return;
        const double scale=std::clamp(tempo.load()/active->bpm,0.05,20.0);
        double increment=scale/rate;
        double offset=first;
        while(offset<frames-1e-8){
            const double end=std::min(active->duration,seconds+(frames-offset)*increment);
            while(cursor<active->events.size() && active->events[cursor].seconds<end){
                // STOP/START can arrive while a dense callback is dispatching.
                // Cancel the rest of this generation instead of queuing its notes.
                if(!playing.load(std::memory_order_acquire)||generation.load(std::memory_order_acquire)!=g)return;
                const auto& e=active->events[cursor++];
                const int at=std::clamp(int(std::llround(offset+(e.seconds-seconds)/increment)),0,frames-1);
                dispatch(e,at);
            }
            offset=std::min(double(frames),offset+(end-seconds)/increment);seconds=end;
            if(seconds>=active->duration-1e-9){
                seconds=0;cursor=0;loops.fetch_add(1);
                if(auto* next=published.load(std::memory_order_acquire);next&&next!=active){active=next;tempo.store(active->bpm);increment=1.0/rate;}
            }
        }
        position.store(seconds,std::memory_order_relaxed);planId.store(active->id,std::memory_order_relaxed);
    }
private:
    const SequencePlan* active=nullptr;
    uint64_t seenGeneration=0;
    size_t cursor=0;
    double seconds=0;
};
}
