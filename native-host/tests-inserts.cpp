#include "src/P12InsertDSP.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
static bool audio=false;
void* operator new(std::size_t n){assert(!audio);if(auto p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
int main(){
 const char* names[]={"comp_basic","comp_vca","comp_optical","limiter_peak","gate","deesser","filter_lp","filter_hp","filter_bp","filter_notch","filter_ladder","filter_statevar","filter_formant","eq_tilt","eq_low","eq_high","eq_para","eq_3band","eq_graphic","delay_slap","delay_pingpong","delay_tape","delay_multi","delay_dub","reverb_room","reverb_plate","reverb_hall","reverb_spring","reverb_ambience","harmonic_tube","harmonic_tape","harmonic_fold","harmonic_exciter","harmonic_softclip","harmonic_rectifier","mod_chorus","mod_flanger","mod_dimension","mod_phaser","mod_tremolo","mod_fuzz","mod_wah","mod_autowah","vintage_sampler","vintage_12bit","vintage_8bit","vintage_wow","vintage_vinyl"};
 for(const auto* name:names){
  p12::InsertDSP effect;
  const auto getter=[](const char* key,float fallback){
   const std::string k(key);if(k=="gain"||k=="low"||k=="mid"||k=="high"||k=="lowmid"||k=="highmid")return 9.f;
   if(k=="tilt")return 65.f;
   if(k=="threshold")return -18.f;
   return fallback;
  };
  assert(effect.configure(name,true,48000,getter));assert(effect.active());
  double difference=0;float l[256],r[256];
  for(int block=0;block<400;++block){
   float original[256];for(int i=0;i<256;++i){const double t=(block*256+i)/48000.;original[i]=l[i]=r[i]=float(.55*std::sin(6.28318530718*200*t)+.23*std::sin(6.28318530718*7000*t));}
   audio=true;effect.process(l,r,256);audio=false;
   for(int i=0;i<256;++i){assert(std::isfinite(l[i])&&std::isfinite(r[i]));assert(std::abs(l[i])<30);difference+=std::abs(l[i]-original[i]);}
  }
  if(difference<.01){std::fprintf(stderr,"No effect: %s\n",name);return 1;}
  effect.enabled=false;for(int i=0;i<256;++i)l[i]=r[i]=.2f;
  audio=true;effect.process(l,r,256);audio=false;for(float x:l)assert(x==.2f);
  std::printf("PASS audio change, bypass, finite and allocation-free: %s\n",name);
 }
 // Delay/track processing must retain a tail after the source voice ends.
 p12::InsertDSP echo;echo.configure("delay_slap",true,48000,[](const char*,float fallback){return fallback;});
 float l[256]{},r[256]{};l[0]=r[0]=1;echo.process(l,r,256);double tail=0;
 for(int b=0;b<100;++b){std::fill_n(l,256,0);std::fill_n(r,256,0);echo.process(l,r,256);for(float x:l)tail+=std::abs(x);}
 assert(tail>.1);
}
