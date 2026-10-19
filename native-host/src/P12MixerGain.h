#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
namespace p12 {
// Control thread publishes targets; callback alone owns smoothing history.
// Track gain follows its inserts; master gain follows all master inserts.
class MixerGain {
public:
    MixerGain() { for(auto& x:tracks_)x.store(1);previous_.fill(1); }
    void setTrack(int index,float gain) noexcept { if(index>=0&&index<9&&std::isfinite(gain))tracks_[index].store(std::clamp(gain,0.0f,4.0f)); }
    void setMaster(float gain) noexcept { if(std::isfinite(gain))master_.store(std::clamp(gain,0.0f,4.0f)); }
    void processTrack(int index,float* left,float* right,int n) noexcept { if(index>=0&&index<9)apply(left,right,n,tracks_[index].load(),previous_[index]); }
    void processMaster(float* left,float* right,int n) noexcept { apply(left,right,n,master_.load(),masterPrevious_); }
private:
    static void apply(float* l,float* r,int n,float target,float& previous) noexcept {
        if(n<=0)return;
        const float step=(target-previous)/static_cast<float>(n);
        for(int i=0;i<n;i++){const float gain=previous+step*static_cast<float>(i+1);l[i]*=gain;r[i]*=gain;}
        previous=target;
    }
    std::array<std::atomic<float>,9> tracks_;
    std::array<float,9> previous_;
    std::atomic<float> master_{1};float masterPrevious_=1;
};
}
