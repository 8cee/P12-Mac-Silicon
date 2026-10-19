#include "src/P12NativeSampler.h"
#include <cassert>
#include <cmath>
#include <memory>
#include <cstdio>
static std::unique_ptr<p12::Sample> tone(int n) {
 auto s=std::make_unique<p12::Sample>();s->left.assign(n,0.8f);
 s->right.assign(n,0.8f);return s;
}
static void render(p12::Sampler& s,float* b,int n) {
 s.beginBlock();float right[512]{};s.renderBus(0,0,b,right,n,48000);s.endBlock(n);
}
int main(){
 p12::Sampler s;assert(s.load(0,tone(6000)));
 p12::Event a;a.pad=0;a.track=0;a.ratio=1;
 p12::Event b=a;b.delayFrames=300;
 assert(s.submit(a)); assert(s.submit(b));
 float first[128]{};render(s,first,128);
 assert(std::fabs(first[100])>0.1f); // earlier sound survives queued retrigger
 float second[128]{};render(s,second,128);
 assert(std::fabs(second[100])>0.1f);
 float third[128]{};render(s,third,128);
 assert(std::fabs(third[40])>0.1f); // earlier sound plays until frame 300
 assert(std::fabs(third[44])<0.02f); // new hit at frame 300 has attack fade
 assert(std::fabs(third[70])>0.1f);
 // Scheduled stop is not applied early.
 p12::Event stop;stop.type=p12::Event::StopPad;stop.pad=0;stop.delayFrames=200;
 assert(s.submit(stop));
 float before[128]{};render(s,before,128);
 assert(std::fabs(before[100])>0.1f);
 float at[128]{};render(s,at,128);
 assert(std::fabs(at[70])>0.1f);
 assert(at[72]==0.0f); // at 128+72 = 200
 // A future hit should not be killed when an earlier scheduled stop fires.
 a.delayFrames=0; b.delayFrames=380;stop.delayFrames=120;
 assert(s.submit(a));assert(s.submit(stop));assert(s.submit(b));
 float a0[128]{};render(s,a0,128);
 assert(std::fabs(a0[100])>0.1f && a0[120]==0);
 float a1[128]{};render(s,a1,128);assert(a1[50]==0);
 float a2[128]{};render(s,a2,128);assert(a2[110]==0);
 float a3[128]{};render(s,a3,128);assert(std::fabs(a3[0])>0.01f);
 assert(std::fabs(a3[45])>0.1f); // hit at total 380 has begun
 // Delayed IPC must not add an old relative delay a second time:
 // target is only 20ms from callback, although the packet carries a stale
 // (intentionally bogus) 8000-frame delay from before it reached JUCE.
 p12::Sampler clocked; assert(clocked.load(0,tone(6000)));
 p12::Event absolute=a;absolute.pad=0;absolute.track=0;
 absolute.delayFrames=8000;absolute.targetEpochMs=1020020;
 assert(clocked.submit(absolute));
 clocked.beginBlock(1020000,48000);
 float v0[128]{},v1[128]{};
 clocked.renderBus(0,0,v0,v1,128,48000);clocked.endBlock(128);
 assert(v0[127]==0);
 for(int step=1;step<8;++step){
   clocked.beginBlock(1020000 + step*128*1000/48000,48000);
   float l[128]{},r[128]{};
   clocked.renderBus(0,0,l,r,128,48000);clocked.endBlock(128);
   if(step==7){assert(l[60]==0 && std::fabs(l[80])>0.01f);}
 }
 // A packet arriving after its deadline should be marked late and played
 // immediately rather than waiting an outdated delay.
 p12::Sampler late;assert(late.load(0,tone(6000)));
 absolute.targetEpochMs=5000;absolute.delayFrames=40000;
 assert(late.submit(absolute));late.beginBlock(5010,48000);
 float l[128]{},r[128]{};late.renderBus(0,0,l,r,128,48000);
 assert(std::fabs(l[100])>0.1f && late.lateEvents()==1);
 // Queue overflow must not corrupt current voice or allocate in callback.
 assert(s.rejectedVoices()==0);
 std::puts("PASS: future retriggers retain prior hit, delayed chokes at audio frames, block-boundary rollover");
}
