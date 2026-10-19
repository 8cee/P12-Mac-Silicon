#include "src/P12NativeSampler.h"
#include "src/P12NativePadControls.h"
#include "src/P12MixerGain.h"
#include "src/P12AutoWah.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
static bool countAllocations=false;static int allocations=0;
void* operator new(std::size_t n){if(countAllocations)++allocations;if(auto* p=std::malloc(n))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}void operator delete(void* p,std::size_t)noexcept{std::free(p);}
static double tone(p12::PadControlsDSP& dsp,double hz){
 float l[256],r[256];double sum=0;
 for(int block=0;block<80;++block){for(int i=0;i<256;++i)l[i]=r[i]=float(.2*std::sin(2*3.14159265358979323846*hz*(block*256+i)/48000));dsp.process(l,r,256,0,true);if(block>40)for(float x:l)sum+=x*x;}
 return std::sqrt(sum/(39*256));
}
int main(){
 p12::AutoWah wah;wah.configure(48000,35,60,60);
 float wl[256],wr[256];double low=1e9,high=0,difference=0;
 countAllocations=true;
 for(int b=0;b<400;b++){
  for(int i=0;i<256;i++)wl[i]=wr[i]=float(.2*std::sin(6.283185307179586*1500*(b*256+i)/48000));
  wah.process(wl,wr,256,.8f);double sum=0;
  for(int i=0;i<256;i++){assert(std::isfinite(wl[i]));assert(wl[i]==wr[i]);sum+=wl[i]*wl[i];difference+=std::abs(wl[i]-.2*std::sin(6.283185307179586*1500*(b*256+i)/48000));}
  if(b>10){low=std::min(low,sum);high=std::max(high,sum);}
 }
 countAllocations=false;assert(allocations==0);assert(high>low*3);assert(difference>100);
 wah.configure(48000,35,60,60);for(int i=0;i<256;i++)wl[i]=wr[i]=.2f;wah.process(wl,wr,256,0);for(float x:wl)assert(x==.2f);
 std::puts("PASS native Auto Wah: audible sweep, stereo, dry mix, finite output, zero callback allocations");

 p12::PadControlsDSP dry,lp,hp,resLow,resHigh,crusher,reverb;
 for(auto* p:{&dry,&lp,&hp,&resLow,&resHigh,&crusher,&reverb})p->prepare(48000);
 lp.configure(1,200,.3,1,0,0);hp.configure(2,3000,.3,1,0,0);
 resLow.configure(1,1000,.3,1,0,0);resHigh.configure(1,1000,10,1,0,0);
 assert(tone(lp,5000)<tone(dry,5000)*.02);assert(tone(hp,100)<tone(dry,100)*.01);
 assert(tone(resHigh,1000)>tone(resLow,1000)*20);
 lp.configure(0,200,.3,1,0,0);assert(std::abs(tone(lp,5000)-tone(dry,5000))<1e-6);
 float l[256],r[256];for(int i=0;i<256;++i)l[i]=r[i]=.34f;
 crusher.configure(0,14000,.3,1,0,1);crusher.process(l,r,256,0,true);
 assert(std::abs(l[200]-.375f)<1e-6&&l[200]!=.34f);
 reverb.configure(0,14000,.3,1,1,0);
 for(int i=0;i<256;++i){l[i]=r[i]=0;}l[0]=r[0]=1;
 reverb.process(l,r,256,3,true);assert(reverb.hasTail(3)&&!reverb.hasTail(0));
 double tail=0;for(int block=0;block<60;++block){for(int i=0;i<256;++i)l[i]=r[i]=0;reverb.configure(0,14000,.3,1,1,0);reverb.process(l,r,256,3,false);for(float x:l)tail+=x*x;}
 assert(tail>.001); // repeated scalar updates must not erase the reverb tail
 p12::Sampler sampler;auto sample=std::make_unique<p12::Sample>();sample->left.assign(48000,.8);sample->right.assign(48000,.8);assert(sampler.load(0,std::move(sample)));
 sampler.setPadControls(0,.9f,0,false);p12::Event event;event.livePadControls=true;event.pad=event.track=0;assert(sampler.submit(event));
 sampler.beginBlock();for(int i=0;i<256;++i)l[i]=r[i]=0;sampler.renderBus(0,0,l,r,256,48000);sampler.endBlock(256);const float initial=l[200];assert(initial>.4f);
 sampler.setPadControls(0,.45f,0,false);sampler.beginBlock();for(int i=0;i<256;++i)l[i]=r[i]=0;sampler.renderBus(0,0,l,r,256,48000);sampler.endBlock(256);assert(std::abs(l[200]/initial-.5f)<1e-5);
 sampler.setPadControls(0,4,0,false);sampler.beginBlock();for(int i=0;i<256;++i)l[i]=r[i]=0;sampler.renderBus(0,0,l,r,256,48000);sampler.endBlock(256);assert(std::abs(l[200]/initial-4/.9f)<1e-5); // pad boost is not silently capped at 1.5 or 2
 p12::MixerGain mixerGain;
 auto mixLevel=[&](int track,float trackGain,float masterGain){mixerGain.setTrack(track,trackGain);mixerGain.setMaster(masterGain);for(int block=0;block<2;++block){for(int i=0;i<256;++i)l[i]=r[i]=.01f;mixerGain.processTrack(track,l,r,256);mixerGain.processMaster(l,r,256);}return l[200]/.01f;};
 assert(std::abs(mixLevel(0,1,1)-1)<1e-6); // fresh native bus has unity gain
 assert(std::abs(mixLevel(0,std::pow(10.f,6.f/20),1)-std::pow(10.f,6.f/20))<1e-5);
 assert(std::abs(mixLevel(0,std::pow(10.f,12.f/20),1)-std::pow(10.f,12.f/20))<1e-5);
 assert(std::abs(mixLevel(8,1,2)-2)<1e-5); // master applies to the isolated VST bus too
 assert(mixLevel(8,1,0)==0);assert(mixLevel(0,0,1)==0);
 assert(std::abs(mixLevel(1,1,1)-1)<1e-6); // muting beat Track 1 never affects another bus
 sampler.setPadControls(0,.9f,1,false);sampler.beginBlock();for(int i=0;i<256;++i)l[i]=r[i]=0;sampler.renderBus(0,0,l,r,256,48000);sampler.endBlock(256);assert(l[200]==0&&r[200]>.7);
 sampler.setPadControls(0,0,0,true);sampler.beginBlock();for(int i=0;i<256;++i)l[i]=r[i]=0;sampler.renderBus(0,0,l,r,256,48000);sampler.endBlock(256);assert(l[200]==0&&r[200]==0);
 // Track 0 is additional bus 8, never the original beat's first bus.
 assert(p12::kBeatTracks==8&&p12::kVstTrack0==8&&p12::kTracks==9);
 p12::Sampler vstBus;auto pitched=std::make_unique<p12::Sample>();pitched->left.resize(48000);pitched->right.resize(48000);
 for(int i=0;i<48000;++i)pitched->left[i]=pitched->right[i]=float(.6*std::sin(2*3.14159265358979323846*440*i/48000));
 assert(vstBus.load(0,std::move(pitched)));vstBus.setPadControls(0,1,0,false,1);
 p12::Event vstEvent;vstEvent.track=p12::kVstTrack0;vstEvent.livePadControls=true;assert(vstBus.submit(vstEvent));vstBus.beginBlock();
 float busLeft[512]{},busRight[512]{};vstBus.renderBus(0,0,busLeft,busRight,512,48000);for(float x:busLeft)assert(x==0);
 vstBus.renderBus(0,p12::kVstTrack0,busLeft,busRight,512,48000);vstBus.endBlock(512);
 auto crossings=[](const float* data){int n=0;for(int i=40;i<512;++i)if(data[i-1]<0&&data[i]>=0)++n;return n;};
 const int normal=crossings(busLeft);assert(normal>=3);
 vstBus.setPadControls(0,1,0,false,2);vstBus.beginBlock();for(int i=0;i<512;++i)busLeft[i]=busRight[i]=0;
 vstBus.renderBus(0,p12::kVstTrack0,busLeft,busRight,512,48000);vstBus.endBlock(512);assert(crossings(busLeft)>=normal+3);
 countAllocations=true;
 for(int i=0;i<100;i++){mixerGain.setTrack(i%9,1);mixerGain.setMaster(1);mixerGain.processTrack(i%9,l,r,256);mixerGain.processMaster(l,r,256);}
 for(int i=0;i<100;++i){dry.configure(i%3,2000,.3,1,1,.5);dry.process(l,r,256,0,true);sampler.renderBus(0,0,l,r,256,48000);}
 countAllocations=false;assert(allocations==0);
 std::puts("PASS native pad controls: sustained Level/Pan/Mute/Engine/Tune, uncapped pad boost, calibrated live track/master gains, isolated Track 0, LP/HP/bypass, resonance, crusher, persistent reverb tail, zero callback allocations");
}
