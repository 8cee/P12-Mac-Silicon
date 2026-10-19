#pragma once
// Playback-only pad controls. No exporter/sample storage is read or rewritten.
// Parameter writes are atomic; filter/crusher/reverb history belongs to callback.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
namespace p12 {
class PadControlsDSP {
public:
    void configure(int mode,float hz,float q,float compensation,float reverb,float crush) noexcept {
        mode_.store(std::clamp(mode,0,2));hz_.store(hz);q_.store(q);
        compensation_.store(std::clamp(compensation,0.0f,1.0f));
        send_.store(std::clamp(reverb,0.0f,1.0f));crush_.store(std::clamp(crush,0.0f,1.0f));
    }
    // Device is stopped here. All delay storage is allocated before playback.
    void prepare(double rate) {
        rate_=rate;cachedMode_=-1;tail_=0;held_={};phase_=0;z1_={};z2_={};
        constexpr double lengths[]={.0297,.0371,.0411,.0437};
        for(int ch=0;ch<2;++ch)for(int k=0;k<4;++k){
            auto& lane=lanes_[ch][k];lane.ring.assign(std::max(1,int((lengths[k]+ch*.0013)*rate)),0);lane.cursor=0;lane.damp=0;
        }
    }
    bool hasTail(int track)const noexcept {return tail_>0&&track_==track;}
    void process(float* l,float* r,int n,int track,bool input) noexcept {
        if(!l||!r||n<=0)return;
        if(input)track_=track;
        const int mode=mode_.load();const float hz=hz_.load(),q=q_.load();
        if(mode!=cachedMode_||hz!=cachedHz_||q!=cachedQ_){
            cachedMode_=mode;cachedHz_=hz;cachedQ_=q;
            const double w=2*3.14159265358979323846*std::clamp(double(hz),20.0,rate_*.45)/rate_;
            const double c=std::cos(w),alpha=std::sin(w)/(2*std::clamp(double(q),.3,14.3)),a0=1+alpha;
            b0_=float((mode==2?(1+c):(1-c))/(2*a0));b1_=float((mode==2?-(1+c):(1-c))/a0);b2_=b0_;
            a1_=float(-2*c/a0);a2_=float((1-alpha)/a0);
            if(mode==0)z1_=z2_={};
        }
        const float comp=compensation_.load(),send=send_.load(),crush=crush_.load();
        const bool reverbActive=send>0||tail_>0;
        const float steps=std::pow(2.0f,15-12*crush),reduction=1+15*crush;
        for(int i=0;i<n;++i){
            bool capture=false;if(crush>0){phase_+=1;if(phase_>=reduction){phase_-=reduction;capture=true;}}
            for(int ch=0;ch<2;++ch){float* data=ch?r:l;float x=data[i];
                if(mode){const float y=b0_*x+z1_[ch];z1_[ch]=b1_*x-a1_*y+z2_[ch];z2_[ch]=b2_*x-a2_*y;x=y*comp;}
                if(crush>0){if(capture)held_[ch]=std::round(std::clamp(x,-1.0f,1.0f)*steps)/steps;x=held_[ch];}
                float wet=0;
                // Four damped combs per side, with different stereo lengths.
                // Additive send preserves dry level at every Reverb position.
                if(reverbActive)for(auto& lane:lanes_[ch])if(!lane.ring.empty()){
                    const float delayed=lane.ring[lane.cursor];lane.damp=.3f*delayed+.7f*lane.damp;
                    lane.ring[lane.cursor]=x*send+lane.damp*.78f;
                    wet+=delayed*.25f;if(++lane.cursor==lane.ring.size())lane.cursor=0;
                }
                data[i]=x+wet*.45f;
                if(send>0&&std::abs(x)>1e-6f)tail_=int(rate_*3);
            }
            if(tail_>0)--tail_;
        }
    }
private:
    struct Lane{std::vector<float> ring;size_t cursor=0;float damp=0;};
    std::array<std::array<Lane,4>,2> lanes_;
    std::atomic<int> mode_{0};std::atomic<float> hz_{14000},q_{.3f},compensation_{1},send_{0},crush_{0};
    double rate_=48000;int cachedMode_=-1,track_=0,tail_=0;float cachedHz_=0,cachedQ_=0;
    float b0_=1,b1_=0,b2_=0,a1_=0,a2_=0,phase_=0;
    std::array<float,2> z1_{},z2_{},held_{};
};
}
