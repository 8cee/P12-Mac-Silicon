#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
namespace p12 {
// Control thread publishes scalars; callback owns envelope and smoothing.
class BusCompressor {
 std::atomic<bool> enabled{false};
 std::atomic<float> threshold{-10},ratio{4},attackMs{10},releaseMs{300},makeupDb{0},mix{100};
 double rate=48000;float reduction=0,wet=0,makeup=1;
public:
 void prepare(double sr) noexcept {rate=std::clamp(sr,8000.0,192000.0);reduction=0;wet=0;makeup=1;}
 void configure(bool on,float t,float r,float a,float rel,float gain,float amount) noexcept {
  const auto safe=[](float x,float fallback,float lo,float hi){return std::clamp(std::isfinite(x)?x:fallback,lo,hi);};
  threshold.store(safe(t,-10,-30,0));ratio.store(safe(r,4,1,10));attackMs.store(safe(a,10,.1f,30));releaseMs.store(safe(rel,300,30,1200));makeupDb.store(safe(gain,0,0,20));mix.store(safe(amount,100,0,100));enabled.store(on);
 }
 void process(float* l,float* r,int n) noexcept {
  if(!enabled.load()){wet=0;reduction=0;return;}
  const float t=threshold.load(),slope=1-1/ratio.load(),targetWet=mix.load()/100,targetMakeup=std::pow(10.f,makeupDb.load()/20);
  const float attack=std::exp(-1.f/float(rate*attackMs.load()/1000)),release=std::exp(-1.f/float(rate*releaseMs.load()/1000)),smooth=std::exp(-1.f/float(rate*.005));
  for(int i=0;i<n;i++){
   const float level=20*std::log10(std::max(1.e-12f,std::max(std::abs(l[i]),std::abs(r[i])))),over=level-t;
   constexpr float knee=2.5f;
   const float target=over<=-knee/2?0:over>=knee/2?slope*over:slope*(over+knee/2)*(over+knee/2)/(2*knee);
   const float c=target>reduction?attack:release;reduction=c*reduction+(1-c)*target;
   wet=smooth*wet+(1-smooth)*targetWet;makeup=smooth*makeup+(1-smooth)*targetMakeup;
   const float gain=(1-wet)+wet*makeup*std::pow(10.f,-reduction/20);l[i]*=gain;r[i]*=gain;
  }
 }
};
}
