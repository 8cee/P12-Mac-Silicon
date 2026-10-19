#pragma once
#include <algorithm>
#include <cmath>
namespace p12 {
// Stereo state-variable bandpass, matching the GUI's sine-swept Auto Wah.
// Callback owns phase and filter history. No allocation or renderer scheduling.
class AutoWah {
public:
 void configure(double sampleRate,float rate,float depth,float resonance,bool reset=true) noexcept {
  sr=std::max(8000.0,sampleRate);speed=.2+std::clamp(rate,0.f,100.f)*.05;
  range=std::clamp(depth,0.f,100.f)*22.;q=1.+std::clamp(resonance,0.f,100.f)*.14;
  if(reset){phase=0;count=0;for(int c=0;c<2;c++)s1[c]=s2[c]=0;}
 }
 void process(float* l,float* r,int n,float mix=1) noexcept {
  for(int i=0;i<n;i++){
   if((count++&31)==0){
    const double f=std::clamp(400.+range+range*std::sin(phase),40.,sr*.45);
    const double g=std::tan(3.141592653589793*f/sr);k=1./q;
    a1=1./(1.+g*(g+k));a2=g*a1;a3=g*a2;
   }
   phase+=6.283185307179586*speed/sr;if(phase>=6.283185307179586)phase-=6.283185307179586;
   float* channels[2]={l,r};
   for(int c=0;c<2;c++){
    const double dry=channels[c][i],v3=dry-s2[c],v1=a1*s1[c]+a2*v3,v2=s2[c]+a2*s1[c]+a3*v3;
    s1[c]=2.*v1-s1[c];s2[c]=2.*v2-s2[c];
    channels[c][i]=static_cast<float>(dry+(k*v1-dry)*mix);
   }
  }
 }
private:
 double sr=48000,speed=2.2,range=1650,q=6.6,phase=0,a1=0,a2=0,a3=0,k=1;
 double s1[2]{},s2[2]{};unsigned count=0;
};
}
