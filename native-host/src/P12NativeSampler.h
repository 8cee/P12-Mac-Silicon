#pragma once
// P-12 Native Audio Engine v0.1 — real audio-thread sample voices.
// This header intentionally uses no JUCE types so its mixing and event rules
// can be tested without Windows, a plugin, or an audio device.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace p12 {
constexpr int kPads = 32;
constexpr int kBeatTracks = 8;
constexpr int kVstTrack0 = kBeatTracks;
constexpr int kTracks = kBeatTracks + 1;
constexpr int kVoices = 128;
constexpr int kQueueSize = 2048;

struct Sample {
    int rate = 48000;
    // Planar stereo samples: never modified once published.
    std::vector<float> left, right;
};
struct Event {
    enum Type { Play, StopPad, StopAll } type = Play;
    int pad = 0, track = 0, delayFrames = 0;
    float velocity = 1.0f, ratio = 1.0f, pan = 0.0f;
    float trimStart = 0.0f, trimEnd = 1.0f;
    // Playback-only SP character. This flag never participates in Rossum
    // .sp12/.s12 encoding; it only selects the native monitor/playback path.
    bool spCharacter = true;
    bool livePadControls = false;
    // Unix epoch millisecond target shared by Chromium and the JUCE host.
    // Zero retains relative-delay compatibility with earlier callers/tests.
    int64_t targetEpochMs = 0;
    int64_t submittedEpochMs = 0; // IPC latency, separate from musical deadlines.
};
// Single-producer, single-consumer: UI command dispatch is serialized on JUCE's
// message thread, consumer is device callback. Never spin/block in callback.
class EventQueue {
public:
    bool push(const Event& e) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto next = (w + 1) % kQueueSize;
        if (next == read_.load(std::memory_order_acquire)) return false;
        data_[w] = e;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(Event& e) noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        e = data_[r];
        read_.store((r + 1) % kQueueSize, std::memory_order_release);
        return true;
    }
private:
    std::array<Event, kQueueSize> data_{};
    std::atomic<int> read_{0}, write_{0};
};

class Sampler {
public:
    Sampler() { for (auto& p : pads_) p.store(nullptr, std::memory_order_relaxed); }
    // Control thread publishes scalar controls; callback alone owns voice state.
    void setPadControls(int pad,float gain,float pan,bool character,float tuneRatio=1.0f) noexcept {
        if(pad<0||pad>=kPads)return;
        controls_[pad].gain.store(std::clamp(gain,0.0f,16.0f),std::memory_order_relaxed);
        controls_[pad].pan.store(std::clamp(pan,-1.0f,1.0f),std::memory_order_relaxed);
        controls_[pad].character.store(character,std::memory_order_relaxed);
        controls_[pad].tuneRatio.store(std::clamp(tuneRatio,.03125f,32.0f),std::memory_order_relaxed);
    }
    // Control thread only. Sample memory is retained until shutdown: pointers
    // read by audio callback remain valid even if a pad is reloaded mid-note.
    bool load(int pad, std::unique_ptr<Sample> s) {
        if (pad < 0 || pad >= kPads || !s || s->rate < 8000 || s->rate > 192000 ||
            s->left.empty() || s->left.size() != s->right.size() ||
            s->left.size() > 192000u * 60u) return false;
        const auto bytes = s->left.size() * sizeof(float) * 2;
        if (bytes > kMaxSampleBytes || retainedBytes_ > kMaxSampleBytes - bytes) return false;
        auto* ptr = s.get();
        owned_.push_back(std::move(s));
        retainedBytes_ += bytes;
        pads_[pad].store(ptr, std::memory_order_release);
        return true;
    }
    // Drop the pad mappings on a project change without invalidating any sample
    // pointer still held by the audio callback. Storage is retained until exit.
    void clearPads() noexcept {
        requestStop();
        for (auto& pad : pads_) pad.store(nullptr, std::memory_order_release);
    }
    bool hasLoadedPad(int pad) const noexcept {
        return pad >= 0 && pad < kPads && pads_[pad].load(std::memory_order_acquire) != nullptr;
    }
    bool submit(const Event& e) noexcept { return events_.push(e); }
    // Safe to request from a second non-audio thread (mode switch) without
    // turning the single-producer event queue into an unsafe MPSC queue.
    void requestStop() noexcept { stopRequested_.store(true,std::memory_order_release); }
    // Audio-callback service used while native mode is OFF. This consumes the
    // stop request and drains future events without starting any new voices,
    // preventing stale Native Audio events from surviving an ON/OFF handoff.
    void serviceStopRequest() noexcept {
        if(!stopRequested_.exchange(false,std::memory_order_acq_rel)) return;
        for(auto& v:voices_) v.active=false;
        pendingStopCount_=0;
        activeRouteMask_.fill(0);
        Event stale;
        while(events_.pop(stale)) {}
    }
    void resetDiagnostics() noexcept {
        rejectedVoices_.store(0,std::memory_order_relaxed);
        lateEvents_.store(0,std::memory_order_relaxed);
        lateStarts_.store(0,std::memory_order_relaxed);
        lateStops_.store(0,std::memory_order_relaxed);
        liveEvents_.store(0,std::memory_order_relaxed);
        liveWorstQueueMs_.store(0,std::memory_order_relaxed);
    }
    // A block-local route mask avoids scanning 128 voices for all 256
    // pad/track combinations when most pads are silent. No heap allocation.
    bool hasActiveRoute(int pad, int track) const noexcept {
        return pad >= 0 && pad < kPads && track >= 0 && track < kTracks &&
            (activeRouteMask_[track] & (uint32_t(1) << pad)) != 0;
    }
    // Called ONCE at the start of each device callback.
    // Events may have a future sample offset: never choke at enqueue time.
    void beginBlock(int64_t callbackEpochMs = 0, double deviceRate = 48000.0) noexcept {
        if(stopRequested_.load(std::memory_order_acquire)) serviceStopRequest();
        Event e;
        while (events_.pop(e)) {
            applyRealtimeEvent(e,callbackEpochMs,deviceRate);
        }
        refreshRoutes();
    }
    // Callback-only direct dispatch: never push into the UI SPSC queue.
    void applyRealtimeEvent(Event e, int64_t callbackEpochMs=0, double deviceRate=48000.0) noexcept {
            if(e.targetEpochMs==0 && e.submittedEpochMs>0 && callbackEpochMs>0){
                liveEvents_.fetch_add(1,std::memory_order_relaxed);
                const auto latency=std::max(int64_t(0),callbackEpochMs-e.submittedEpochMs);
                auto peak=liveWorstQueueMs_.load(std::memory_order_relaxed);
                while(latency>peak&&!liveWorstQueueMs_.compare_exchange_weak(peak,latency,std::memory_order_relaxed)){}
            }
            if (e.targetEpochMs > 0 && callbackEpochMs > 0 && deviceRate >= 8000.0) {
                const auto deltaMs=e.targetEpochMs-callbackEpochMs;
                if (deltaMs < -2) {
                    lateEvents_.fetch_add(1,std::memory_order_relaxed);
                    if(e.type==Event::Play)lateStarts_.fetch_add(1,std::memory_order_relaxed);
                    else lateStops_.fetch_add(1,std::memory_order_relaxed);
                }
                // Deadline is calculated against the AUDIO callback start,
                // not an outdated delay measured at IPC send time.
                e.delayFrames=static_cast<int>(std::clamp(
                    deltaMs*deviceRate/1000.0,0.0,192000.0));
            }
            if (e.type == Event::StopAll) {
                for(auto& v:voices_) v.active=false;
                pendingStopCount_=0;
                return;
            }
            if (e.pad < 0 || e.pad >= kPads) return;
            if (e.type == Event::StopPad) {
                if (e.delayFrames <= 0) {
                    for(auto& v:voices_) if(v.active && v.pad==e.pad) v.active=false;
                } else if (pendingStopCount_ < pendingStops_.size()) {
                    pendingStops_[pendingStopCount_++] = {e.pad, e.delayFrames};
                }
                return;
            }
            if (e.track < 0 || e.track >= kTracks) return;
            auto* src = pads_[e.pad].load(std::memory_order_acquire);
            if (!src) return;
            // Reserve a future voice, but choke the previous hit ONLY at the
            // exact frame the new hit starts, not when JS queues the note.
            auto* dest = static_cast<Voice*>(nullptr);
            for(auto& v:voices_) if(!v.active) {dest=&v;break;}
            if (!dest) { rejectedVoices_.fetch_add(1,std::memory_order_relaxed); return; }
            const auto start = std::clamp(e.trimStart,0.0f,1.0f);
            const auto end = std::clamp(e.trimEnd,start,1.0f);
            dest->src=src;
            dest->pad=e.pad; dest->track=e.track; dest->active=true;
            dest->pos=start * (src->left.size()-1);
            dest->end=std::max(dest->pos+1.0, static_cast<double>(end*src->left.size()));
            dest->rate=std::clamp(static_cast<double>(e.ratio),0.03125,32.0);
            const auto pan=std::clamp(e.pan,-1.0f,1.0f);
            const auto gain=std::clamp(e.velocity,0.0f,2.0f);
            dest->leftGain = gain * std::sqrt(0.5f*(1-pan));
            dest->rightGain = gain * std::sqrt(0.5f*(1+pan));
            dest->delay=std::clamp(e.delayFrames,0,192000);
            dest->fade=0;
            dest->started=false;
            dest->spCharacter=e.spCharacter;
            dest->livePadControls=e.livePadControls;dest->baseGain=gain;dest->basePan=pan;dest->baseRate=dest->rate;
            dest->characterPrimed=false;
            dest->characterPhase=0.0;
            dest->holdL=dest->holdR=0.0f;
            dest->reconL1=dest->reconL2=dest->reconR1=dest->reconR2=0.0f;
            dest->characterInputGain=gain;

    }
    void refreshRoutes() noexcept {
        activeRouteMask_.fill(0);
        for (const auto& v : voices_)
            if (v.active && v.track >= 0 && v.track < kTracks && v.pad >= 0 && v.pad < kPads)
                activeRouteMask_[v.track] |= uint32_t(1) << v.pad;
    }
    // The host calls renderBus(pad,track) to route to PAD VST3 -> TRACK VST3
    // -> MASTER VST3 inside the SAME native device callback.
    // The caller processes each active pad/track bus once per device block.
    // Iterate frames in time order: future retriggers and choke events cannot
    // cut an earlier voice until their actual sample offsets are reached.
    bool renderBus(int pad, int track, float* l, float* r, int n, double deviceRate) noexcept {
        if (!l || !r || n <= 0 || deviceRate < 8000 || pad<0 || pad>=kPads || track<0 || track>=kTracks) return false;
        // Only iterate voices assigned to this bus on each sample. Scanning
        // all 128 slots at 48 kHz would itself create excessive CPU load.
        std::array<Voice*,kVoices> routeVoices{};
        size_t routeCount=0;
        for(auto& v:voices_)
            if(v.active && v.pad==pad && v.track==track && v.src)
                routeVoices[routeCount++]=&v;
        const bool hadVoices=routeCount>0;
        const auto padGain=controls_[pad].gain.load(std::memory_order_relaxed);
        const auto padPan=controls_[pad].pan.load(std::memory_order_relaxed);
        const auto padTune=controls_[pad].tuneRatio.load(std::memory_order_relaxed);
        const auto character=controls_[pad].character.load(std::memory_order_relaxed);
        for(size_t k=0;k<routeCount;++k){auto& v=*routeVoices[k];if(!v.livePadControls)continue;
            const float gain=std::clamp(v.baseGain*padGain,0.0f,32.0f),pan=std::clamp(v.basePan+padPan,-1.0f,1.0f);
            v.leftGain=gain*std::sqrt(.5f*(1-pan));v.rightGain=gain*std::sqrt(.5f*(1+pan));
            v.characterInputGain=gain;v.rate=std::clamp(v.baseRate*padTune,.03125,32.0);
            if(v.spCharacter!=character){v.spCharacter=character;v.characterPrimed=false;v.characterPhase=0;}
        }
        // Check the pending-stop list once per audio block, not once per
        // audio sample. Only stops targeting this exact pad are relevant.
        std::array<int,512> stopOffsets{};
        size_t stopCount=0;
        for(size_t stop=0; stop<pendingStopCount_; ++stop)
            if(pendingStops_[stop].pad==pad && pendingStops_[stop].frames<n)
                stopOffsets[stopCount++]=pendingStops_[stop].frames;
        for (int i=0; i<n; ++i) {
            for (size_t stop=0; stop<stopCount; ++stop) {
                if (stopOffsets[stop]==i) {
                    for (auto& v:voices_)
                        if (v.active && v.pad==pad && v.started) v.active=false;
                }
            }
            // Apply all starts/stops before rendering this exact sample.
            // Otherwise a previous voice may render one extra sample before
            // the scheduled retrigger is processed (audible as a transient).
            for (size_t k=0; k<routeCount; ++k) {
                auto& v=*routeVoices[k];
                if (!v.active) continue;
                if (v.delay>0) { --v.delay; continue; }
                if (!v.started) {
                    for (auto& old:voices_)
                        if (&old!=&v && old.active && old.pad==pad && old.started)
                            old.active=false;
                    v.started=true;
                }
            }
            for (size_t k=0; k<routeCount; ++k) {
                auto& v=*routeVoices[k];
                if (!v.active || !v.started) continue;
                const auto& a=v.src->left; const auto& b=v.src->right;
                if (v.pos>=v.end || v.pos>=a.size()-1) {v.active=false;continue;}
                const float attack=std::min(1.0f,v.fade/std::max(1.0f,static_cast<float>(deviceRate*0.00035)));
                if (v.spCharacter) {
                    // Playback-only SP/eSPi-style character path. The source
                    // sample itself is never rewritten. A virtual SP DAC clock
                    // runs at 26.04 kHz and changes with playback pitch, so
                    // pitch moves produce the characteristic image/alias shift.
                    // No anti-alias stage is used here by design: this is the
                    // audible playback colour, NOT the hardware export encoder.
                    constexpr double spClock=26040.0;
                    const double virtualRate=std::clamp(spClock*v.rate,spClock/32.0,spClock*32.0);
                    const double ticksPerDeviceSample=virtualRate/deviceRate;
                    const double sourcePerSpTick=static_cast<double>(v.src->rate)/spClock;
                    const auto makeVirtualSample=[&]() noexcept {
                        if (v.pos>=v.end || v.pos>=a.size()-1) {v.active=false;return;}
                        const size_t idx=static_cast<size_t>(v.pos);
                        const size_t next=std::min(idx+1,a.size()-1);
                        const float f=static_cast<float>(v.pos-idx);
                        const float rawL=a[idx]+(a[next]-a[idx])*f;
                        const float rawR=b[idx]+(b[next]-b[idx])*f;
                        // Gentle level-dependent analogue-style drive into a
                        // true 12-bit monitor quantiser. No random dither/noise:
                        // character without adding a new hiss source.
                        const float drive=1.025f+0.055f*std::clamp(v.characterInputGain,0.0f,2.0f);
                        v.holdL=quantize12(softClip(rawL*drive))*0.985f;
                        v.holdR=quantize12(softClip(rawR*drive))*0.985f;
                        v.pos+=sourcePerSpTick;
                    };
                    if (!v.characterPrimed) {
                        makeVirtualSample();
                        v.characterPrimed=true;
                        v.reconL1=v.reconL2=v.holdL;
                        v.reconR1=v.reconR2=v.holdR;
                    }
                    v.characterPhase+=ticksPerDeviceSample;
                    // Typical pitch ranges execute 0-2 iterations. The loop is
                    // still bounded for extreme +5 octave diagnostic values.
                    int guard=0;
                    while (v.characterPhase>=1.0 && v.active && guard++<40) {
                        makeVirtualSample();
                        v.characterPhase-=1.0;
                    }
                    // Two gentle reconstruction poles approximate the smooth
                    // analogue output filtering while retaining aliased grit.
                    const float alpha=static_cast<float>(1.0-std::exp(-2.0*3.14159265358979323846*10800.0/deviceRate));
                    v.reconL1+=alpha*(v.holdL-v.reconL1);
                    v.reconL2+=alpha*(v.reconL1-v.reconL2);
                    v.reconR1+=alpha*(v.holdR-v.reconR1);
                    v.reconR2+=alpha*(v.reconR1-v.reconR2);
                    l[i]+=v.reconL2*v.leftGain*attack;
                    r[i]+=v.reconR2*v.rightGain*attack;
                } else {
                    // Hi-Fi engine: retain the proven clean cubic interpolator.
                    const size_t idx=static_cast<size_t>(v.pos);
                    const float f=static_cast<float>(v.pos-idx);
                    const auto cubic=[&](const std::vector<float>& x) noexcept {
                        const auto i0=idx>0?idx-1:idx;
                        const auto i2=std::min(idx+1,x.size()-1);
                        const auto i3=std::min(idx+2,x.size()-1);
                        const float y0=x[i0],y1=x[idx],y2=x[i2],y3=x[i3];
                        const float c0=y1, c1=0.5f*(y2-y0),
                            c2=y0-2.5f*y1+2*y2-0.5f*y3,
                            c3=0.5f*(y3-y0)+1.5f*(y1-y2);
                        return ((c3*f+c2)*f+c1)*f+c0;
                    };
                    l[i]+=cubic(a)*v.leftGain*attack;
                    r[i]+=cubic(b)*v.rightGain*attack;
                    v.pos+=v.rate*v.src->rate/deviceRate;
                }
                ++v.fade;
            }
        }
        return hadVoices;
    }
    // Called ONCE after all pad/track busses have been rendered.
    // Pending stops advance on the audio clock, not the JS event loop.
    void endBlock(int frames) noexcept {
        size_t dest=0;
        for(size_t i=0; i<pendingStopCount_; ++i) {
            if(pendingStops_[i].frames>=frames) {
                auto item=pendingStops_[i];
                item.frames-=frames;
                pendingStops_[dest++]=item;
            }
        }
        pendingStopCount_=dest;
    }
    uint64_t rejectedVoices() const noexcept {
        return rejectedVoices_.load(std::memory_order_relaxed);
    }
    uint64_t liveEvents() const noexcept {return liveEvents_.load(std::memory_order_relaxed);}
    int64_t liveWorstQueueMs() const noexcept {return liveWorstQueueMs_.load(std::memory_order_relaxed);}
    uint64_t lateEvents() const noexcept {
        return lateEvents_.load(std::memory_order_relaxed);
    }
    uint64_t lateStarts() const noexcept {return lateStarts_.load(std::memory_order_relaxed);}
    uint64_t lateStops() const noexcept {return lateStops_.load(std::memory_order_relaxed);}
    int activeVoices() const noexcept {
        int n=0;for(const auto& v:voices_) if(v.active) ++n;return n;
    }
    size_t sampleMemory() const noexcept {return retainedBytes_;}
private:
    static constexpr size_t kMaxSampleBytes=512u*1024u*1024u;
    static float quantize12(float x) noexcept {
        x=std::clamp(x,-1.0f,2047.0f/2048.0f);
        return std::round(x*2048.0f)/2048.0f;
    }
    static float softClip(float x) noexcept {
        // Mild deterministic analogue-style saturation without the large
        // level drop of a hard polynomial clip. Gain stays punchy near 0 dBFS.
        const float a=std::clamp(x,-2.0f,2.0f);
        const float y=a/(1.0f+0.08f*a*a);
        return std::clamp(y,-1.0f,1.0f);
    }
    struct Voice {
        const Sample* src=nullptr;
        int pad=0,track=0,delay=0,fade=0;
        double pos=0,end=0,rate=1,baseRate=1;
        float leftGain=1,rightGain=1;
        bool active=false, started=false;
        bool spCharacter=true, characterPrimed=false,livePadControls=false;
        float baseGain=1,basePan=0;
        double characterPhase=0.0;
        float holdL=0,holdR=0,reconL1=0,reconL2=0,reconR1=0,reconR2=0;
        float characterInputGain=1.0f;
    };
    struct PendingStop { int pad=0,frames=0; };
    std::array<PendingStop,512> pendingStops_{};
    size_t pendingStopCount_=0;
    std::atomic<uint64_t> rejectedVoices_{0}, lateEvents_{0}, lateStarts_{0}, lateStops_{0};
    struct PadControl {std::atomic<float> gain{1},pan{0},tuneRatio{1};std::atomic<bool> character{true};};
    std::array<PadControl,kPads> controls_{};
    std::array<Voice,kVoices> voices_{};
    std::array<uint32_t,kTracks> activeRouteMask_{};
    std::array<std::atomic<const Sample*>,kPads> pads_{};
    std::vector<std::unique_ptr<Sample>> owned_;
    EventQueue events_;
    std::atomic<uint64_t> liveEvents_{0};
    std::atomic<int64_t> liveWorstQueueMs_{0};
    std::atomic<bool> stopRequested_{false};
    size_t retainedBytes_=0;
};
} // namespace p12
